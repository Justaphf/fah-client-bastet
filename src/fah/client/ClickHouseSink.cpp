/******************************************************************************\

                  This file is part of the Folding@home Client.

          The fah-client runs Folding@home protein folding simulations.
                    Copyright (c) 2001-2026, foldingathome.org
                               All rights reserved.

       This program is free software; you can redistribute it and/or modify
       it under the terms of the GNU General Public License as published by
        the Free Software Foundation; either version 3 of the License, or
                       (at your option) any later version.

         This program is distributed in the hope that it will be useful,
          but WITHOUT ANY WARRANTY; without even the implied warranty of
          MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
                   GNU General Public License for more details.

     You should have received a copy of the GNU General Public License along
     with this program; if not, write to the Free Software Foundation, Inc.,
           51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

                  For information regarding this software email:
                                 Joseph Coffland
                          joseph@cauldrondevelopment.com

\******************************************************************************/

#include "ClickHouseSink.h"

#include "App.h"
#include "Config.h"
#include "GPUResources.h"
#include "Remote.h"
#include "Unit.h"
#include "Units.h"

#include <cbang/Catch.h>
#include <cbang/String.h>
#include <cbang/log/Logger.h>
#include <cbang/json/JSON.h>
#include <cbang/json/BufferWriter.h>
#include <cbang/http/Client.h>
#include <cbang/http/Request.h>
#include <cbang/net/URI.h>
#include <cbang/openssl/Digest.h>
#include <cbang/openssl/SSLContext.h>
#include <cbang/os/SystemUtilities.h>
#include <cbang/time/Time.h>
#include <cbang/time/Timer.h>
#include <cbang/util/WeakCallback.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

using namespace FAH::Client;
using namespace cb;
using namespace std;


namespace {
  const unsigned schema         = 1;
  const unsigned batchPeriod    = 60;  // Seconds a batch collects rows
  const unsigned batchMax       = 600; // Rows
  const unsigned pumpPeriod     = 5;
  const double   drainGap       = 0.5; // Seconds between insert starts
  const unsigned requestTimeout = 30;
  const unsigned testTimeout    = 10;
  const unsigned retryMin       = 5;
  const unsigned retryMax       = 300;
  const unsigned configRetry    = 900;
  const unsigned loadRetry      = 60;  // Reading settings that failed
  const unsigned dedupWindow    = 10000; // Inserts remembered per table
  const unsigned rowMax         = 256 * 1024; // Bytes
  const unsigned textMax        = 300; // Server text kept, in bytes
  const unsigned logMax         = 1 << 20; // Bytes before the log rotates

  const unsigned queueDefault = 259200; // Three days of one GPU
  const unsigned queueMin     = 1000;
  const unsigned queueLimit   = 10000000;
  const unsigned urlMax       = 2048;
  const unsigned hostMax      = 253;
  const unsigned userMax      = 256;
  const unsigned passwordMax  = 1024;
  const unsigned databaseMax  = 64;
  const unsigned caMax        = 65536;

  const char *settingsKey = "clickhouse";
  const char *passwordKey = "clickhouse-password";
  const char *queueTable  = "clickhouse_queue";
  const char *unitsTable  = "clickhouse_units";
  const char *refusal     = "Not available on this connection";
  const char *badReply    = "Unexpected reply from the server";
  const char *logName     = "external-tsdb.log";

  const char *samplesTable = "fah_gpu_samples";
  const char *eventsTable  = "fah_unit_events";

  struct Column {const char *name; const char *type;};

  const vector<Column> sampleColumns = {
    {"time",         "DateTime64(3, 'UTC')"},
    {"machine_id",   "String"},
    {"machine_name", "String"},
    {"gpu_id",       "String"},
    {"gpu_name",     "String"},
    {"gpu_uuid",     "String"},
    {"units",        "Array(String)"},
    {"schema",       "UInt16"},
    {"data",         "JSON"},
  };

  const vector<Column> eventColumns = {
    {"time",         "DateTime64(3, 'UTC')"},
    {"machine_id",   "String"},
    {"machine_name", "String"},
    {"unit_id",      "String"},
    {"event",        "LowCardinality(String)"},
    {"gpus",         "Array(String)"},
    {"project",      "UInt32"},
    {"run",          "UInt32"},
    {"clone",        "UInt32"},
    {"gen",          "UInt32"},
    {"result",       "LowCardinality(String)"},
    {"schema",       "UInt16"},
    {"data",         "JSON"},
  };

  struct Table {
    const char *name;
    const vector<Column> &columns;
    const char *order;
  };

  const Table tables[] = {
    {samplesTable, sampleColumns, "machine_id, gpu_id, time"},
    {eventsTable,  eventColumns,  "machine_id, unit_id, time"},
  };

  const char *labelPaths[][2] = { // Column, unit path
    {"project", "assignment.project"},
    {"run",     "wu.run"},
    {"clone",   "wu.clone"},
    {"gen",     "wu.gen"},
  };

  const char *stepNames[] = {"server", "database", "tables", "insert"};


  string toJSON(const function<void (JSON::Sink &sink)> &cb) {
    JSON::BufferWriter writer(0, true);
    cb(writer);
    writer.flush();
    return writer.toString();
  }


  string getTableName(const string &database, const string &table) {
    return "`" + database + "`.`" + table + "`";
  }


  string getDDL(const string &database, const Table &table) {
    string sql = "CREATE TABLE IF NOT EXISTS " +
      getTableName(database, table.name) + " (";

    for (unsigned i = 0; i < table.columns.size(); i++) {
      if (i) sql += ", ";
      sql += SSTR('`' << table.columns[i].name << "` "
                  << table.columns[i].type);
    }

    return sql + SSTR(") ENGINE = MergeTree PARTITION BY toYYYYMM(time) "
                      "ORDER BY (" << table.order << ") SETTINGS "
                      "non_replicated_deduplication_window = " << dedupWindow);
  }


  string normalizeType(const string &type) {
    string s;
    for (char c: type) if (c != ' ') s += c;
    return s;
  }


  bool hasControl(const string &s) {
    for (char c: s) if ((unsigned char)c < 32 || c == 127) return true;
    return false;
  }


  bool hasSecret(const string &s, const vector<string> &secrets) {
    for (auto &secret: secrets)
      if (!secret.empty() && s.find(secret) != string::npos) return true;
    return false;
  }


  // A mark that holds neither password, one is left for two passwords
  string getMark(const vector<string> &secrets) {
    for (auto m: {"***", "###", "~~~"})
      if (!hasSecret(m, secrets)) return m;
    return "";
  }


  // Each password in the text replaced literally by the mark, in one pass
  // and only until more than max bytes are kept
  string scrub(const string &in, const vector<string> &secrets,
               const string &mark, size_t max = string::npos) {
    vector<string> found;
    for (auto &secret: secrets) if (!secret.empty()) found.push_back(secret);

    vector<size_t> at; // The next occurrence of each
    for (auto &secret: found) at.push_back(in.find(secret));

    string s;
    size_t pos = 0;
    while (pos < in.size() && s.size() < max) {
      size_t start = string::npos;
      for (auto i: at) start = std::min(start, i);

      s.append(in, pos, std::min(start - pos, max - s.size()));
      if (start == string::npos) break;

      // Overlapping occurrences are hidden under one mark
      size_t end = start + 1;
      for (bool more = true; more;) {
        more = false;
        for (unsigned i = 0; i < found.size(); i++)
          while (at[i] < end) {
            end   = std::max(end, at[i] + found[i].size());
            at[i] = in.find(found[i], at[i] + 1);
            more  = true;
          }
      }

      s += mark;
      pos = end;
    }

    // Text next to a mark could spell a password again
    return hasSecret(s, secrets) ? mark : s;
  }


  // Server text shown locally: no control characters, no password, bounded
  string clean(const string &text, const vector<string> &secrets) {
    string in = text;
    for (auto &c: in) if ((unsigned char)c < 32 || c == 127) c = ' ';

    string mark = getMark(secrets);
    string s    = scrub(in, secrets, mark, textMax + 1);

    if (textMax < s.size()) {
      size_t end = textMax;
      while (end && ((unsigned char)s[end] & 0xc0) == 0x80) end--;
      s = s.substr(0, end) + "...";
    }

    // Text next to a mark could spell a password again
    return hasSecret(s, secrets) ? mark : s;
  }


  string getEventToken(const string &event, const string &id) {
    return Digest::hashHex(string(eventsTable) + "\n" + event + "\n" + id,
                           "sha256");
  }


  string getCodeSuffix(unsigned code) {
    return code ? SSTR(" (code " << code << ')') : string();
  }


  // A number sent back by the server, 0 if it could be a password: its text
  // holds one, or one reads as this number by cbang's lenient parse
  unsigned getShown(unsigned n, const vector<string> &secrets) {
    for (auto &secret: secrets) {
      uint32_t value = 0;
      if (!secret.empty() && String::parseU32(secret, value, false) &&
          value == n) return 0;
    }

    return hasSecret(String(n), secrets) ? 0 : n;
  }


  // The same for a status, also 0 if cbang reads a word of a password as
  // it, by name or number.  cbang splits the status line at spaces.
  unsigned getStatusShown(const HTTP::Status &status,
                          const vector<string> &secrets) {
    for (auto &secret: secrets) {
      vector<string> words;
      String::tokenize(secret, words, " ");

      for (auto &word: words)
        try {
          if (HTTP::Status::parse(word, HTTP::Status::HTTP_UNKNOWN) == status)
            return 0;
        } catch (const std::exception &) {}
    }

    return getShown((unsigned)status, secrets);
  }


  // The HTTP status, unless the text already has it
  string getHTTPStatus(const ClickHouse::Response &r,
                       const vector<string> &secrets) {
    if (!r.hasResponse() || !(r.getCode() || r.isOk())) return "";
    unsigned status = getStatusShown(r.getStatus(), secrets);
    return status ? SSTR("HTTP " << status << ", ") : string();
  }


  string formatRows(uint64_t rows) {
    return SSTR(rows << (rows == 1 ? " row" : " rows"));
  }


  string getSettingsText(const ClickHouseSink::Settings &s) {
    return SSTR((s.enabled ? "enabled" : "disabled")
                << ", url " << (s.url.empty() ? "none" : s.url)
                << ", database " << (s.database.empty() ? "none" : s.database)
                << ", user " << (s.user.empty() ? "default" : s.user)
                << ", CA " << (s.ca.empty() ? "none" : "set")
                << ", queue limit " << s.queueMax);
  }


  bool isHTTPS(const string &url) {return String::startsWith(url, "https:");}


  string checkURL(const string &value) {
    string url = String::trim(value);
    if (url.empty()) return url;
    if (urlMax < url.size()) THROW("URL is too long");
    if (url.find("://") == string::npos) url = "https://" + url;

    URI uri;
    try {
      uri.read(url);
    } catch (const Exception &) {THROW("Invalid URL");}

    // cbang only uses TLS for a lower case scheme
    string scheme = String::toLower(uri.getScheme());
    if (scheme != "http" && scheme != "https")
      THROW("URL must start with http:// or https://");
    if (uri.getHost().empty()) THROW("URL has no host");
    if (hostMax < uri.getHost().size()) THROW("URL host name is too long");
    if (!uri.getUser().empty() || !uri.getPass().empty())
      THROW("Enter the user and password in their own fields, not in the "
            "URL");
    if (!uri.getQuery().empty() || !uri.getFragment().empty())
      THROW("URL must not have a query or fragment");

    if (uri.getHost().find_first_of("[]:") != string::npos)
      THROW("IPv6 addresses are not supported in the URL, use a host name");

    uri.setScheme(scheme);
    uri.setHost(String::toLower(uri.getHost()));
    url = uri.toString();

    // Stored and used as written here
    try {
      URI check(url);
    } catch (const Exception &) {THROW("Invalid URL");}

    return url;
  }


  string checkDatabase(const string &name) {
    if (databaseMax < name.size()) THROW("Database name is too long");

    for (unsigned i = 0; i < name.size(); i++) {
      char c = name[i];
      if (!(isalpha((unsigned char)c) || c == '_' ||
            (i && isdigit((unsigned char)c))))
        THROW("Database name must be letters, digits and underscores and "
              "not start with a digit");
    }

    return name;
  }


  SmartPointer<SSLContext> createSSLContext(const string &ca, bool roots) {
    SmartPointer<SSLContext> ctx = new SSLContext;
    if (roots) ctx->loadSystemRootCerts();

    const string begin = "-----BEGIN CERTIFICATE-----";
    const string end   = "-----END CERTIFICATE-----";
    unsigned count = 0;

    for (size_t i = ca.find(begin); i != string::npos; i = ca.find(begin, i)) {
      size_t j = ca.find(end, i);
      if (j == string::npos) THROW("Incomplete certificate");
      j += end.size();
      ctx->addTrustedCAStr(ca.substr(i, j - i));
      count++;
      i = j;
    }

    if (!ca.empty() && !count) THROW("No PEM certificate found");

    return ctx;
  }


  string checkCA(const string &ca) {
    if (caMax < ca.size()) THROW("CA certificate is too long");

    try {
      createSSLContext(ca, false);
    } catch (const Exception &e) {
      THROW("Invalid CA certificate: " << e.getMessage());
    }

    return ca;
  }


  string checkPassword(const JSON::Value &value) {
    if (!value.isString()) THROW("'password' must be a string");
    string password = value.getString();
    if (passwordMax < password.size()) THROW("Password is too long");
    if (hasControl(password))
      THROW("Password must not contain control characters");
    return password;
  }


  bool isVersion(const string &s) {
    // Like 25.7.5.34 or 23.8.8.21.altinitystable
    if (s.empty() || 48 < s.size()) return false;

    size_t i = 0;
    while (i < s.size() && isdigit((unsigned char)s[i])) i++;
    if (!i || i + 1 >= s.size() || s[i] != '.' ||
        !isdigit((unsigned char)s[i + 1])) return false;

    for (char c: s)
      if (!isalnum((unsigned char)c) && !strchr(".-+_", c)) return false;
    return true;
  }


  // The rows of a JSONCompact result, each a list
  JSON::ValuePtr getRows(const ClickHouse::Response &r) {
    JSON::ValuePtr json = r.getJSON();
    if (json.isNull() || !json->isDict() || !json->hasList("data"))
      THROW("No data");

    JSON::ValuePtr rows = json->get("data");
    for (auto &row: *rows) if (!row->isList()) THROW("Not a row");

    return rows;
  }


  string getCell(const JSON::Value &rows, unsigned row, unsigned col) {
    auto &r = *rows.get(row);
    if (r.size() <= col || !r.get(col)->isString()) THROW("Not a string");
    return r.getString(col);
  }


  struct Token {
    char type; // Word, number, string or punctuation
    string text;
    int depth;
  };


  vector<Token> getTokens(const string &s) {
    vector<Token> tokens;
    int depth = 0;

    for (size_t i = 0; i < s.size();) {
      char c = s[i];

      if (isspace((unsigned char)c)) i++;

      else if (!s.compare(i, 2, "--")) {
        i = s.find('\n', i);
        if (i == string::npos) break;

      } else if (!s.compare(i, 2, "/*")) {
        size_t end = s.find("*/", i + 2);
        if (end == string::npos) THROW("Unterminated comment");
        if (s.find("/*", i + 2) < end) THROW("Nested comment");
        i = end + 2;

      } else if (c == '\'' || c == '"' || c == '`') {
        string text;
        for (i++; i < s.size() && s[i] != c; i++) {
          if (s[i] == '\\' && i + 1 < s.size()) i++;
          text += s[i];
        }
        if (s.size() <= i) THROW("Unterminated quote");
        i++;
        tokens.push_back({c == '\'' ? 's' : 'w', text, depth});

      } else if (isalnum((unsigned char)c) || c == '_') {
        size_t start = i;
        while (i < s.size() &&
               (isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '.'))
          i++;
        string text = s.substr(start, i - start);
        bool digits = text.find_first_not_of("0123456789") == string::npos;
        tokens.push_back({digits ? 'n' : 'w', text, depth});

      } else {
        if (c == ')' && --depth < 0) THROW("Unbalanced parenthesis");
        tokens.push_back({'p', string(1, c), depth});
        if (c == '(') depth++;
        i++;
      }
    }

    if (depth) THROW("Unbalanced parenthesis");
    return tokens;
  }


  // The value assigned to a setting in the engine description's SETTINGS
  // clause, -1 if it is not there or not a plain number
  int64_t getEngineSetting(const string &engine, const string &name) {
    vector<Token> t;
    try {
      t = getTokens(engine);
    } catch (const Exception &) {return -1;}

    size_t i = 0;
    while (i < t.size() && !(t[i].type == 'w' && !t[i].depth &&
                             t[i].text == "SETTINGS")) i++;

    // name = value, ...
    for (i++; i + 2 < t.size(); i++) {
      if (t[i].depth || t[i].type != 'w' || t[i + 1].text != "=") return -1;

      if (t[i].text == name) {
        // The value must end the assignment
        auto &value = t[i + 2];
        bool last  = i + 3 == t.size() ||
          (t[i + 3].text == "," && !t[i + 3].depth);
        if (value.type != 'n' || 18 < value.text.size() || !last) return -1;
        return String::parse<int64_t>(value.text, true);
      }

      for (i += 2; i < t.size() && !(t[i].text == "," && !t[i].depth);) i++;
    }

    return -1;
  }


  uint32_t getLabel(const JSON::Value &unit, const string &path) {
    try {
      auto value = unit.select(path, JSON::Null::instancePtr());
      if (value->isNumber() && 0 <= value->getNumber())
        return value->getU32();
    } catch (const Exception &) {}

    return 0;
  }


  SmartPointer<ClickHouse::Client> newClient(
    const SmartPointer<HTTP::Client> &http,
    const ClickHouseSink::Settings &settings, const string &password,
    unsigned timeout) {
    SmartPointer<ClickHouse::Client> client = new ClickHouse::Client(http);
    client->setURL(settings.url);
    client->setUser(settings.user.empty() ? "default" : settings.user);
    client->setPassword(password);
    client->setTimeout(timeout);
    return client;
  }
}


struct ClickHouseSink::Probe : public RefCounted {
  struct Step {
    string name;
    int ok = -1;
    string message;
    unsigned code = 0;
  };

  SmartPointer<HTTP::Client> http; // A test's own or the sink's
  SmartPointer<ClickHouse::Client> client;
  string database;
  string password;
  bool test           = false;
  bool https          = false;
  unsigned generation = 0;

  vector<Step> steps;
  unsigned step     = 0;
  unsigned requests = 0; // One at a time, the last is current
  vector<string> found;
  vector<string> create;
  unsigned created = 0;
  bool unverified  = false;

  result_t result = RESULT_OK;
  bool schema     = false;
  bool ended      = false;
  string version;
  string error;
  unsigned code = 0;

  function<void (Probe &)> done;

  Probe(bool test) : test(test) {
    for (unsigned i = 0; i < (test ? 4 : 3); i++) {
      steps.push_back(Step());
      steps.back().name = stepNames[i];
    }
  }

  void pass(const string &message) {
    steps[step].ok      = 1;
    steps[step].message = message;
    step++;
  }

  void fail(result_t result, const string &error, unsigned code) {
    this->result        = result;
    this->error         = error;
    this->code          = code;
    steps[step].ok      = 0;
    steps[step].message = error;
    steps[step].code    = code;
  }
};


ClickHouseSink::Settings::Settings() : queueMax(queueDefault) {}


string ClickHouseSink::Settings::getBinding() const {
  // Where a password may be sent: scheme, host, port, the path as sent,
  // user and CA, each length prefixed so no field can stand for another
  vector<string> fields;

  if (!url.empty()) {
    URI uri(url);
    fields = {uri.getScheme(), uri.getHost(), String(uri.getPort()),
              uri.getEscapedPath()};
  }

  fields.push_back(user.empty() ? "default" : user);
  fields.push_back(ca);

  string s;
  for (auto &field: fields) s += SSTR(field.size() << ':' << field);
  return Digest::hashHex(s, "sha256");
}


void ClickHouseSink::Settings::read(const JSON::Value &value) {
  // Only the keys given change, all are checked
  if (!value.isDict()) THROW("Settings must be an object");

  for (auto it = value.begin(); it != value.end(); it++) {
    string key = it.key();
    auto &v    = **it;

    auto getString = [&] () {
      if (!v.isString()) THROW("'" << key << "' must be a string");
      return v.getString();
    };

    if (key == "enabled") {
      if (!v.isBoolean()) THROW("'enabled' must be true or false");
      enabled = v.getBoolean();

    } else if (key == "url") url = checkURL(getString());
    else if (key == "database") database = checkDatabase(getString());

    else if (key == "user") {
      user = getString();
      if (userMax < user.size()) THROW("User name is too long");
      if (hasControl(user))
        THROW("User name must not contain control characters");

    } else if (key == "ca") ca = checkCA(getString());

    else if (key == "queue_max") {
      double n = v.isNumber() ? v.getNumber() : NAN;
      if (!(queueMin <= n && n <= queueLimit) || n != floor(n))
        THROW("'queue_max' must be a whole number from " << queueMin
              << " to " << queueLimit);
      queueMax = (unsigned)n;

    } else THROW("Unknown setting '" << key << "'");
  }
}


void ClickHouseSink::Settings::write(JSON::Sink &sink) const {
  sink.beginDict();
  sink.insertBoolean("enabled", enabled);
  sink.insert("url",       url);
  sink.insert("database",  database);
  sink.insert("user",      user);
  sink.insert("ca",        ca);
  sink.insert("queue_max", queueMax);
  sink.endDict();
}


ClickHouseSink::ClickHouseSink(App &app) : app(app) {}
ClickHouseSink::~ClickHouseSink() {TRY_CATCH_ERROR(save());}


bool ClickHouseSink::isActive() const {
  return settings.enabled && settings.isConfigured();
}


void ClickHouseSink::init() {
  load();
  countQueue(); // Shown also while disabled
  apply();
}


void ClickHouseSink::load() {
  settings = Settings();
  password.clear();

  // Rows that could not be read are read again later, invalid ones are not
  string text, value;
  bool hasSettings = false, hasPassword = false;

  try {
    auto &db = app.getDB("config");
    hasSettings = db.has(settingsKey);
    if (hasSettings) {
      text        = db.getString(settingsKey);
      hasPassword = db.has(passwordKey);
      if (hasPassword) value = db.getString(passwordKey);
    }

  } catch (const Exception &e) {
    app.closeDB("config"); // A failed statement stays unusable
    if (!loadFailed) {
      LOG_WARNING("Failed to read the ClickHouse settings");
      note("Failed to read the settings: " + e.getMessage());
    }
    loadFailed = true;
    loadTime   = now();
    return;
  }

  loadFailed = false;
  if (!hasSettings) return;

  try {
    Settings s;
    s.read(*JSON::parse(text));
    settings = s;

  } catch (const Exception &e) {
    LOG_WARNING("Ignoring invalid ClickHouse settings: " << e.getMessage());
    note("Ignoring invalid saved settings: " + e.getMessage());
    return;
  }

  // A password is only used with the settings it was entered for.  Only a
  // row that was read and does not match is deleted.
  bool dropped = false;
  if (hasPassword)
    try {
      auto stored = JSON::parse(value);
      if (stored->getString("binding") != settings.getBinding())
        THROW("Other settings");
      password = stored->getString("password");

    } catch (const Exception &) {
      LOG_WARNING("Dropped the ClickHouse password saved for other settings");
      dropped = true;
      try {
        app.getDB("config").unset(passwordKey);
      } catch (const Exception &) {app.closeDB("config");}
    }

  note("Settings loaded: " + getSettingsText(settings) + ", password " +
       (dropped ? "dropped, saved for other settings" :
        (password.empty() ? "not set" : "set")));
}


void ClickHouseSink::save() {
  // Everything else is queued already
  if (bufferRows) close();

  // A start whose markers could not be read is not known to be new
  if (!pending.empty())
    note(SSTR("Dropped " << pending.size() << " start rows, the unit markers "
              "were not read"));
  rowsDropped += pending.size();
  pending.clear();
}


void ClickHouseSink::finish(const Unit &unit) {
  // Not lost unseen while the settings cannot be read
  if (loadFailed) {
    TRY_CATCH_ERROR(reload());
    if (loadFailed && unit.hasGPUs()) {
      const string &id = unit.getID();
      rowsDropped++;
      note("Dropped the finish row of unit " + id +
           ", the settings were not read");

      // A held start, unless stored before, is lost with it and not replayed
      bool owed = heldUnits.count(id), marked = false;
      for (auto &h: held) if (h.second.units.count(id)) owed = true;
      if (owed)
        try {
          marked = hasTable(unitsTable) && app.getDB(unitsTable).has(id);
        } catch (const Exception &) {app.closeDB(unitsTable);}

      if (owed && !marked && started.insert(id).second) {
        rowsDropped++;
        note("Dropped the start row of unit " + id +
             ", the settings were not read");
      }
    }
  }

  if (!isActive()) return;

  try {
    const string &id = unit.getID();

    // Units that left after a finish that could not be queued
    for (auto it = started.begin(); it != started.end();)
      if (app.getUnits()->findUnit(*it).isNull()) it = started.erase(it);
      else it++;

    bool known = started.count(id);

    // Its start can no longer be put before it
    if (pending.erase(id)) {
      known = true;
      rowsDropped++;
      note("Dropped the start row of unit " + id + ", the unit markers were "
           "not read");
    }

    // Without the markers, a GPU unit may have had a start
    if (!known && (startedLoaded || !unit.hasGPUs())) return;

    // The unit as logged, with its GPU statistics
    event(id, formatEvent("finish", now(), id, unit, *unit.copy(true)),
          false);
    pump();
  } CATCH_ERROR;
}


void ClickHouseSink::command(Remote &remote, const JSON::Value &msg) {
  string cmd = msg.getString("cmd", "");

  // A relayed or non-local remote learns nothing
  if (!remote.isLocal()) {
    string name = remote.getName();
    if (1000 < refused.size()) refused.clear();
    if (refused.insert(name).second)
      LOG_WARNING("Refused ClickHouse commands from " << name
                  << ", only allowed on a local connection");

    return refuse(remote, msg, refusal, "local_only");
  }

  if (loadFailed) TRY_CATCH_ERROR(reload());

  if (cmd == "clickhouse") configure(remote, msg);
  else if (cmd == "clickhouse_test") test(remote, msg);
  else if (cmd == "clickhouse_status") {
    // Where the details are, for this machine only
    auto status = getStatus();
    string path;
    try {
      if (SystemUtilities::isFile(getLogPath())) path = getLogPath();
    } catch (const std::exception &) {}

    if (path.empty()) status->insertNull("log");
    else status->insert("log", path);
    reply(remote, msg, status);

  } else {
    LOG_WARNING("Received unsupported remote command '" << cmd << "'");
    refuse(remote, msg, "Unsupported command", "unsupported");
  }
}


JSON::ValuePtr ClickHouseSink::getStatus() const {
  const char *name = "disabled";
  if (settings.enabled && !settings.isConfigured()) name = "unconfigured";
  else if (isActive())
    switch (state) {
    case STATE_DISABLED:   break;
    case STATE_CONNECTING: name = "connecting";      break;
    case STATE_OK:         name = "ok";              break;
    case STATE_RETRY:      name = "retrying";        break;
    case STATE_CONFIG:     name = "config_error";    break;
    case STATE_SCHEMA:     name = "schema_mismatch"; break;
    }

  auto time = [] (double t) {return Time((uint64_t)t).toString();};

  JSON::ValuePtr d = new JSON::Dict;
  d->insertBoolean("enabled",    settings.enabled);
  d->insertBoolean("configured", settings.isConfigured());
  d->insert("state", name);
  d->insertBoolean("connected", isActive() && state == STATE_OK);
  d->insertBoolean("testing",   testing);

  if (version.empty()) d->insertNull("server_version");
  else d->insert("server_version", version);

  if (lastSuccess) d->insert("last_success", time(lastSuccess));
  else d->insertNull("last_success");

  if (lastError.empty()) {
    d->insertNull("last_error");
    d->insertNull("last_error_code");
    d->insertNull("last_error_time");

  } else {
    d->insert("last_error", lastError);
    if (lastErrorCode) d->insert("last_error_code", lastErrorCode);
    else d->insertNull("last_error_code");
    d->insert("last_error_time", time(lastErrorTime));
  }

  bool waiting = isActive() &&
    (state == STATE_RETRY || state == STATE_CONFIG || state == STATE_SCHEMA);
  if (waiting) d->insert("next_retry", time(retryAt));
  else d->insertNull("next_retry");

  d->insert("rows_sent",      rowsSent);
  d->insert("rows_queued",    rowsQueued + bufferRows);
  d->insert("rows_dropped",   rowsDropped);
  d->insert("rows_truncated", rowsTruncated);
  d->insert("schema",         schema);

  return d;
}


ClickHouseSink::result_t ClickHouseSink::classify(const Response &r) {
  if (r.isOk()) return RESULT_OK;
  if (r.isTimeout() || !r.hasResponse()) return RESULT_RETRY;

  if (r.getCode()) {
    if (r.isDataError()) return RESULT_DATA;

    // Fixed by the settings or on the server, not by sending again soon
    switch (r.getCode()) {
    case 60:  // UNKNOWN_TABLE
    case 62:  // SYNTAX_ERROR
    case 81:  // UNKNOWN_DATABASE
    case 115: // UNKNOWN_SETTING
    case 164: // READONLY
    case 192: // UNKNOWN_USER
    case 193: // WRONG_PASSWORD
    case 194: // REQUIRED_PASSWORD
    case 195: // IP_ADDRESS_NOT_ALLOWED
    case 291: // DATABASE_ACCESS_DENIED
    case 452: // SETTING_CONSTRAINT_VIOLATION
    case 497: // ACCESS_DENIED
    case 516: // AUTHENTICATION_FAILED
      return RESULT_CONFIG;
    }

    return RESULT_RETRY;
  }

  // Not from ClickHouse: a wrong URL, unless a proxy is in trouble
  unsigned status = r.getStatus();
  return status < 500 && status != 408 && status != 429 ?
    RESULT_CONFIG : RESULT_RETRY;
}


string ClickHouseSink::formatTime(double time) {
  // Read as DateTime64(3) without best effort parsing
  uint64_t ms   = 0 < time ? (uint64_t)llround(time * 1000) : 0;
  uint64_t secs = ms / 1000;
  string date   = secs ? Time(secs).toString("%Y-%m-%d %H:%M:%S") :
    "1970-01-01 00:00:00"; // Zero is an invalid Time
  return date + String::printf(".%03u", (unsigned)(ms % 1000));
}


void ClickHouseSink::sample(const GPUSample &s) {
  retryLoad();

  // Kept until the settings say whether they are sent
  if (loadFailed) {
    if (held.size() < batchMax) held.push_back({now(), s});
    else {
      heldDropped++;

      // A unit first seen now still gets its start, one that left does not
      auto &units = *app.getUnits();
      for (auto &id: s.units)
        if (!heldUnits.count(id) && units.findUnit(id).isSet()) {
          for (auto it = heldUnits.begin(); it != heldUnits.end();)
            if (units.findUnit(it->first).isNull()) it = heldUnits.erase(it);
            else it++;
          heldUnits[id] = s.time;
        }
    }

  } else if (isActive())
    try {
      record(s, now());
      pump();
    } CATCH_ERROR;
}


double ClickHouseSink::now() const {return Timer::now();}


const string &ClickHouseSink::getLogPath() {
  if (logPath.empty()) {
    // Beside the client's log
    string log = app.getOptions()["log"].toString("");
    string dir = log.empty() ? SystemUtilities::getcwd() :
      SystemUtilities::dirname(SystemUtilities::absolute(log));
    logPath = SystemUtilities::joinPath(dir, logName);
  }

  return logPath;
}


void ClickHouseSink::note(const string &text) {
  // The details stay in a local file, the shared log reaches remotes
  try {
    string line = Time((uint64_t)now()).toString() + ' ' + text;
    for (auto &c: line) if ((unsigned char)c < 32 || c == 127) c = ' ';
    line += app.getOptions()["log-crlf"].toBoolean() ? "\r\n" : "\n";

    const string &path = getLogPath();

    auto open = [&] {
      // Opening a FIFO or a device could wait
      if (SystemUtilities::exists(path) && !SystemUtilities::isFile(path))
        THROW("Not a file");

      logFile = SystemUtilities::open(path, ios::out | ios::app | ios::binary,
                                      0600);
      SystemUtilities::chmod(path, 0600); // Also if it existed
    };

    if (logFile.isNull()) {
      open();
      logSize = SystemUtilities::getFileSize(path);
    }

    if (logSize && logMax < logSize + line.size()) {
      auto &options = app.getOptions();
      logFile.release();
      SystemUtilities::rotate(
        path, options["log-rotate-dir"],
        (unsigned)options["log-rotate-max"].toInteger(),
        Compression::parse(options["log-rotate-compression"]));
      open();
      logSize = 0;
    }

    logFile->write(line.data(), line.size());
    logFile->flush();
    if (logFile->fail()) THROW("Write failed");
    logSize  += line.size();
    logFailed = false;

  } catch (const std::exception &) {
    logFile.release();
    if (!logFailed) LOG_WARNING("Failed to write the ClickHouse log file");
    logFailed = true;
  }
}


void ClickHouseSink::reload() {
  load();
  if (loadFailed) return;
  apply();

  // Samples that came while the settings were unread
  auto samples = std::move(held);
  auto units   = std::move(heldUnits);
  held.clear();
  heldUnits.clear();
  uint64_t dropped = heldDropped;
  heldDropped = 0;

  if (!isActive()) return;
  if (dropped) {
    rowsDropped += dropped;
    note("Dropped " + formatRows(dropped) + ", the settings were not read");
  }

  // Batches close as they would have when the samples came
  for (auto &h: samples)
    try {
      closeDue(h.first);
      record(h.second, h.first);
      closeDue(h.first);
    } CATCH_ERROR;

  for (auto &u: units)
    if (!started.count(u.first) && !pending.count(u.first))
      TRY_CATCH_ERROR(start(u.first, u.second));

  if (!samples.empty() || !units.empty()) pump();
}


void ClickHouseSink::retryLoad() {
  double now = this->now();
  if (loadFailed && (loadTime + loadRetry <= now || now < loadTime))
    TRY_CATCH_ERROR(reload());
}


void ClickHouseSink::startTimer() {
  timer = app.getEventBase().newEvent(
    [this] {timer->add(pumpPeriod); TRY_CATCH_ERROR(pump());}, 0);
  timer->add(pumpPeriod);
}


void ClickHouseSink::request(HTTP::Client &http, ClickHouse::Client &client,
                             const string &sql,
                             const JSON::ValuePtr &params,
                             const JSON::ValuePtr &settings,
                             const response_cb_t &cb) {
  client.query(sql, params, settings, cb);
}


SmartPointer<HTTP::Client>
ClickHouseSink::newHTTP(const SmartPointer<SSLContext> &ctx) {
  // Its requests name a destination that is local only
  SmartPointer<HTTP::Client> http = new HTTP::Client(app.getEventBase(), ctx);
  http->setQuiet(true);
  http->setFilter(WeakCall(this, [this] (HTTP::Request &req) {filter(req);}));
  return http;
}


void ClickHouseSink::filter(HTTP::Request &req) {
  // The reply as sent, before cbang trims, cuts, splits or decodes it
  const char *key = "X-ClickHouse-Key";
  vector<string> secrets = {req.outHas(key) ? req.outFind(key) : "", password};
  string body = req.getInput();

  echoed = hasSecret(body, secrets);

  // Only error text is shown, a result is parsed and must stay intact
  const char *codeKey = "X-ClickHouse-Exception-Code";
  bool error = req.getResponseCode() != HTTP::Status::HTTP_OK;
  uint32_t code = 0;
  if (req.inHas(codeKey) &&
      (!String::parseU32(req.inFind(codeKey), code, false) || code))
    error = true;
  if (!error || !echoed) return;

  auto &input = req.getInputBuffer();
  input.clear();
  input.add(scrub(body, secrets, getMark(secrets)));
}


void ClickHouseSink::pump() {
  retryLoad();
  if (!isActive()) return;
  double now = this->now();

  // Retried until read, neither is taken as empty
  if (!queueCounted) countQueue();
  if (!startedLoaded) loadStarted();
  closeDue(now);

  if (state == STATE_OK) return sendNext();
  if (state == STATE_CONNECTING) return;
  if (retryAt <= now || now + retryDelay < retryAt) connect();
}


void ClickHouseSink::apply() {
  generation++;
  probing = false;
  client.release();

  if (!isActive()) {
    if (state != STATE_DISABLED) {
      save();
      note(settings.enabled ? "Stopped, no URL or database" : "Stopped");
    }
    state      = STATE_DISABLED;
    failures   = 0;
    inflightID = 0; // Still queued
    version.clear();
    http.release();
    sendEvent.release();

    // Reading the settings is retried also without samples
    if (!loadFailed) timer.release();
    else if (timer.isNull()) startTimer();
    return;
  }

  if (state == STATE_DISABLED) {
    countQueue();
    loadStarted();
    note("Started, " + formatRows(rowsQueued) + " queued");

    startTimer();
    sendEvent =
      app.getEventBase().newEvent([this] {TRY_CATCH_ERROR(pump());}, 0);
  }

  retryDelay = 0;
  trimQueue();
  connect();
}


void ClickHouseSink::record(const GPUSample &s, double time) {
  // A start waits until it is known which ones were already queued
  for (auto &id: s.units)
    if (!started.count(id) && !pending.count(id)) start(id, s.time);

  if (!bufferRows) bufferStart = time;
  buffer += formatSample(s);
  buffer += '\n';
  bufferRows++;
}


void ClickHouseSink::closeDue(double time) {
  if (bufferRows && (batchMax <= bufferRows ||
                     bufferStart + batchPeriod <= time || time < bufferStart))
    close();
}


void ClickHouseSink::start(const string &id, double time) {
  auto unit = app.getUnits()->findUnit(id);
  if (unit.isNull()) return;

  JSON::ValuePtr data = new JSON::Dict;
  for (auto key: {"number", "group", "cpus", "assignment", "wu"})
    if (unit->has(key)) data->insert(key, unit->get(key)->copy(true));
  data->insert("run_time", unit->getRunTime());

  string row = formatEvent("start", time, id, *unit, *data);
  if (!startedLoaded) pending[id] = row; // At most one per unit
  else {
    started.insert(id); // Not again in this run, even if not stored
    event(id, row, true);
  }
}


void ClickHouseSink::event(const string &id, const string &row, bool start) {
  // Queued before its marker changes, so a crash in between repeats the
  // event and loses no marked one.  The server drops the repeat by its token.
  Batch batch;
  batch.table = eventsTable;
  batch.rows  = 1;
  batch.data  = row + "\n";
  batch.token = getEventToken(start ? "start" : "finish", id);

  enqueue(batch);
  if (!batch.id) return;
  if (!start) started.erase(id); // Kept with its marker if not queued
  mark(id, start);
}


void ClickHouseSink::mark(const string &id, bool start) {
  try {
    auto &units = app.getDB(unitsTable);
    if (start) units.set(id, formatTime(now()));
    else units.unset(id);
    unitsFailed = false;

  } catch (const Exception &e) {
    app.closeDB(unitsTable); // A failed statement stays unusable
    if (!unitsFailed) {
      LOG_WARNING("Failed to update ClickHouse units: " << e.getMessage());
      note("Failed to update the unit markers: " + e.getMessage());
    }
    unitsFailed = true;
  }
}


void ClickHouseSink::configure(Remote &remote, const JSON::Value &msg) {
  string error;
  string code;
  bool cleared = false;

  // Not saved over what could not be read
  if (loadFailed) {
    error = "Failed to read the saved settings";
    code  = "storage";

  } else if (msg.has("config") || msg.has("password")) {
    Settings next = settings;
    string pass   = password;

    try {
      cleared = read(msg, next, pass);
    } catch (const Exception &e) {
      error = e.getMessage();
      code  = "invalid";
    }

    if (error.empty())
      try {
        store(next, pass);
        settings = next;
        password = pass;

        bool given = msg.has("password");
        note("Settings saved: " + getSettingsText(settings) + ", password " +
             (cleared ? "dropped, the settings changed" :
              (pass.empty() ? (given ? "cleared" : "not set") :
               (given ? "set" : "kept"))));

        LOG_INFO(1, (isActive() ? "Sending" : "Not sending")
                 << " GPU telemetry to ClickHouse");
        apply();

      } catch (const Exception &e) {
        error   = "Failed to save the settings: " + e.getMessage();
        code    = "storage";
        cleared = false;
        note(clean(error, {password, pass}));

        // Whatever part was written is what is used
        load();
        apply();
      }
  }

  auto body = getSettings();
  if (cleared) body->insertBoolean("password_cleared", true);

  if (!error.empty()) {
    body->insert("error", clean(error, {password}));
    body->insert("code",  code);
  }

  reply(remote, msg, body);
}


void ClickHouseSink::test(Remote &remote, const JSON::Value &msg) {
  if (testing)
    return refuse(remote, msg, "A connection test is already running",
                  "busy");

  Settings s  = settings;
  string pass = password;
  SmartPointer<HTTP::Client> http;

  try {
    read(msg, s, pass);
  } catch (const Exception &e) {
    return refuse(remote, msg, clean(e.getMessage(), {pass}), "invalid");
  }

  if (!s.isConfigured())
    return refuse(remote, msg, "Enter the URL and database first",
                  "unconfigured");

  try {
    http = newHTTP(createSSLContext(s.ca, true));
  } catch (const Exception &e) {
    return refuse(remote, msg, e.getMessage(), "invalid");
  }

  // The reply needs only these, the request may hold a password
  JSON::ValuePtr request = new JSON::Dict;
  request->insert("cmd", "clickhouse_test");
  if (msg.has("id")) request->insert("id", msg.get("id"));

  ProbePtr p  = new Probe(true);
  p->http     = http;
  p->client   = newClient(http, s, pass, testTimeout);
  p->database = s.database;
  p->password = pass;
  p->https    = isHTTPS(s.url);
  SmartPointer<Remote>::Weak ref(&remote);

  p->done = [this, ref, request] (Probe &p) {
    testing = false;

    for (auto &step: p.steps)
      note("Test step " + step.name + ": " +
           (step.ok < 0 ? "not run" : (step.ok ? "ok" : "failed")) +
           (step.message.empty() ? "" : ", " + step.message));

    SmartPointer<Remote> remote = ref;
    if (remote.isNull()) return;

    JSON::ValuePtr steps = new JSON::List;
    for (auto &step: p.steps) {
      JSON::ValuePtr d = new JSON::Dict;
      d->insert("name", step.name);
      if (step.ok < 0) d->insertNull("ok");
      else d->insertBoolean("ok", step.ok);
      d->insert("message", step.message);
      if (step.code) d->insert("code", step.code);
      steps->append(d);
    }

    JSON::ValuePtr body = new JSON::Dict;
    body->insertBoolean("ok", p.result == RESULT_OK);
    body->insert("steps", steps);
    reply(*remote, *request, body);
  };

  note("Test started: " + getSettingsText(s) + ", password " +
       (pass.empty() ? "none" : (msg.has("password") ? "given" : "stored")));

  testing = true;
  probe(p);
}


bool ClickHouseSink::read(const JSON::Value &msg, Settings &settings,
                          string &password) const {
  // True if the password was dropped.  It is never sent where it was not
  // entered for.
  string binding = settings.getBinding();
  if (msg.has("config")) settings.read(*msg.get("config"));

  if (msg.has("password")) {
    password = checkPassword(*msg.get("password"));
    return false;
  }

  if (password.empty() || settings.getBinding() == binding) return false;
  password.clear();
  return true;
}


void ClickHouseSink::store(const Settings &next, const string &pass) {
  // The password is saved with what it is for and removed first when it
  // changes, so an interrupted save never pairs it with other settings
  string binding = next.getBinding();
  bool keep = pass == password && binding == settings.getBinding();

  try {
    auto &db = app.getDB("config");
    if (!keep) db.unset(passwordKey);
    db.set(settingsKey, toJSON([&] (JSON::Sink &s) {next.write(s);}));

    if (!keep && !pass.empty())
      db.set(passwordKey, toJSON([&] (JSON::Sink &s) {
        s.beginDict();
        s.insert("binding",  binding);
        s.insert("password", pass);
        s.endDict();
      }));

  } catch (const Exception &) {
    app.closeDB("config"); // A failed statement stays unusable
    throw;
  }
}


void ClickHouseSink::reply(Remote &remote, const JSON::Value &msg,
                           const JSON::ValuePtr &body) {
  JSON::ValuePtr changes = new JSON::List;
  changes->append(msg.getString("cmd", ""));
  changes->append(body);

  // Echoed, so a reply can be matched to its request
  auto id = msg.get("id", JSON::Null::instancePtr());
  if (id->isInteger() || id->isString()) changes->append(id);

  remote.sendChanges(changes);
}


void ClickHouseSink::refuse(Remote &remote, const JSON::Value &msg,
                            const string &error, const string &code) {
  JSON::ValuePtr body = new JSON::Dict;

  // Shaped like a test result
  if (msg.getString("cmd", "") == "clickhouse_test") {
    body->insertBoolean("ok", false);
    body->insert("steps", new JSON::List);
  }

  body->insert("error", error);
  body->insert("code",  code);
  reply(remote, msg, body);
}


JSON::ValuePtr ClickHouseSink::getSettings() const {
  JSON::ValuePtr d = new JSON::Dict;
  d->insert("config", JSON::parse(
              toJSON([&] (JSON::Sink &s) {settings.write(s);})));
  d->insertBoolean("password_set", !password.empty());
  d->insert("status", getStatus());
  return d;
}


string ClickHouseSink::describe(const Response &r, const string &password,
                                bool https) const {
  vector<string> secrets = {password, this->password};
  auto shown = [&] (unsigned n) {return n ? String(n) : string("***");};
  unsigned status = getStatusShown(r.getStatus(), secrets);

  string text;

  if (!r.hasResponse()) {
    text = "ClickHouse: " + r.getError();

    // A reply cbang's HTTP client cannot read, as from ClickHouse's native port
    string invalid = Event::ConnectionError::getDescription(
      Event::ConnectionError::CONN_ERR_BAD_RESPONSE);

    if (String::endsWith(r.getError(), invalid))
      text += " (check that the URL is the HTTP interface,"
        " not the native port)";
    else if (https && !r.isTimeout()) text += " (with https, check the CA)";

  } else if (r.getCode()) {
    // Without a body the text is the status' name
    bool hide = !status && r.getError() == r.getStatus().toString();

    text = "ClickHouse:" + shown(getShown(r.getCode(), secrets)) + ": " +
      (hide ? string("***") : r.getError());

  } else text = "HTTP " + shown(status) +
    " from a server that is not ClickHouse";

  return clean(text, secrets);
}


string ClickHouseSink::formatSample(const GPUSample &s) const {
  auto &config = *app.getConfig();
  auto data    = GPUResource::toJSON(s.measurement);

  string name, uuid;
  auto &gpus = app.getGPUs();
  if (gpus.has(s.device) && gpus.get(s.device)->isDict()) {
    auto &gpu = *gpus.get(s.device);
    name = gpu.getString("description", "");
    uuid = gpu.getString("uuid", "");
  }

  return toJSON([&] (JSON::Sink &sink) {
    sink.beginDict();
    sink.insert("time", formatTime(s.time));
    writeMachine(sink);
    sink.insert("gpu_id",   s.device);
    sink.insert("gpu_name", name);
    sink.insert("gpu_uuid", uuid);

    sink.insertList("units", true);
    for (auto &id: s.units) sink.append(id);
    sink.endList();

    sink.insert("schema", schema);

    sink.insertDict("data");
    for (auto it = data->begin(); it != data->end(); it++)
      sink.insert(it.key(), **it);
    sink.insert("weight",      s.weightSeconds);
    sink.insert("interval",    config.getGPUTelemetryInterval());
    sink.insert("energy_rate", config.getNumber("energy_rate", 0));
    sink.endDict();

    sink.endDict();
  });
}


string ClickHouseSink::formatEvent(const string &event, double time,
                                   const string &id, const JSON::Value &unit,
                                   const JSON::Value &data) {
  auto format = [&] (const JSON::Value &data) {
    return toJSON([&] (JSON::Sink &sink) {
      sink.beginDict();
      sink.insert("time", formatTime(time));
      writeMachine(sink);
      sink.insert("unit_id", id);
      sink.insert("event",   event);

      sink.insertList("gpus", true);
      if (unit.hasList("gpus"))
        for (auto &gpu: *unit.get("gpus"))
          if (gpu->isString()) sink.append(gpu->getString());
      sink.endList();

      for (auto &l: labelPaths) sink.insert(l[0], getLabel(unit, l[1]));
      sink.insert("result", unit.getString("result", ""));
      sink.insert("schema", schema);
      sink.insert("data",   data);
      sink.endDict();
    });
  };

  string row = format(data);
  if (row.size() <= rowMax) return row;

  // Sent without its payload
  rowsTruncated++;
  JSON::Dict marker;
  marker.insert("truncated", (uint64_t)row.size());
  return format(marker);
}


void ClickHouseSink::writeMachine(JSON::Sink &sink) const {
  sink.insert("machine_id",   app.selectString("info.id", ""));
  sink.insert("machine_name", app.selectString("info.mach_name", ""));
}


void ClickHouseSink::close() {
  // The token lets the server drop a batch sent again after a lost reply
  Batch batch;
  batch.table = samplesTable;
  batch.rows  = bufferRows;
  batch.data.swap(buffer);
  batch.token = Digest::hashHex(batch.table + "\n" + batch.data, "sha256");
  bufferRows  = 0;

  enqueue(batch);
}


void ClickHouseSink::sendNext() {
  if (inflightID || probing || state != STATE_OK || client.isNull()) return;

  // Paced, so a backlog does not flood the server with small inserts
  double now = this->now();
  if (now < nextSend && nextSend <= now + drainGap) {
    if (sendEvent.isSet()) sendEvent->add(nextSend - now);
    return;
  }

  // Strictly oldest first
  if (delivered) {
    if (!unqueue(delivered)) return;
    delivered = 0;
  }

  Batch batch;
  if (!dequeue(batch)) return;

  JSON::ValuePtr s = new JSON::Dict;
  s->insert("insert_deduplicate", 1);
  s->insert("insert_deduplication_token", batch.token);

  string sql = "INSERT INTO " + getTableName(settings.database, batch.table) +
    " FORMAT JSONEachRow\n" + batch.data;

  inflight     = batch;
  nextSend     = now + drainGap;
  uint64_t id  = inflightID = ++requests;
  unsigned gen = generation;
  string pass  = password;
  bool https   = isHTTPS(settings.url);

  // The batch stays queued whatever happens to the reply
  send(*http, *client, sql, 0, s, guard(
         [this, id, gen, pass, https] (const Response &r) {
           // Not if taken back by a disable
           if (id == inflightID) inserted(gen, pass, https, r);
         }, 0));
}


void ClickHouseSink::inserted(unsigned gen, const string &pass, bool https,
                              const Response &r) {
  inflightID = 0;

  Batch batch;
  swap(batch, inflight);

  // ClickHouse answers an insert with an empty body
  auto result = classify(r);
  if (result == RESULT_OK && r.getJSON().isSet()) result = RESULT_CONFIG;

  string error = result == RESULT_OK ? string() : describe(r, pass, https);
  if (result == RESULT_CONFIG && r.isOk()) error = badReply;
  unsigned code = getShown(r.getCode(), {pass, password});

  if (result != RESULT_OK)
    note(SSTR((result == RESULT_DATA ? "Dropped " : "Failed to send ")
              << formatRows(batch.rows) << " for " << batch.table << ": "
              << getHTTPStatus(r, {pass, password}) << error));

  switch (result) {
  case RESULT_OK:
    rowsSent   += batch.rows;
    lastSuccess = now();
    dataWarned  = false;
    if (!unqueue(batch.id)) delivered = batch.id;
    if (gen == generation) recovered();
    break;

  case RESULT_DATA:
    // Sending it again would fail the same way
    rowsDropped += batch.rows;
    if (!unqueue(batch.id)) delivered = batch.id;
    if (gen != generation) break; // Not news about the current settings

    setError(error, code);
    if (!dataWarned)
      LOG_WARNING("ClickHouse rejected " << batch.rows << " rows, dropped "
                  "them" << getCodeSuffix(code));
    dataWarned = true;
    break;

  default:
    if (gen == generation && state == STATE_OK)
      fail(result, false, error, code);
    break;
  }

  pump();
}


void ClickHouseSink::send(HTTP::Client &http, ClickHouse::Client &client,
                          const string &sql,
                          const JSON::ValuePtr &params,
                          const JSON::ValuePtr &settings,
                          const response_cb_t &cb) {
  echoed = false; // Until the filter sees the reply

  // The text, which may name the host, goes only to the local status
  try {
    request(http, client, sql, params, settings, cb);
  } catch (const Exception &e) {
    cb(Response("Request failed: " + e.getMessage()));
  }
}


ClickHouseSink::response_cb_t ClickHouseSink::guard(
  const response_cb_t &cb, const function<void ()> &failed) {
  // A reply is checked as it is used, one that does not fit fails the step
  return WeakCall(this, [cb, failed] (const Response &r) {
    try {
      cb(r);
    } catch (const std::exception &) {
      if (failed) TRY_CATCH_ERROR(failed());
    }
  });
}


void ClickHouseSink::connect() {
  if (client.isNull())
    try {
      if (http.isNull()) http = newHTTP(0);
      http->setSSLContext(createSSLContext(settings.ca, true));
      client = newClient(http, settings, password, requestTimeout);

    } catch (const Exception &e) {
      return fail(RESULT_CONFIG, false, clean(e.getMessage(), {}), 0);
    }

  ProbePtr p    = new Probe(false);
  p->client     = client;
  p->database   = settings.database;
  p->password   = password;
  p->https      = isHTTPS(settings.url);
  p->http       = http;
  p->generation = generation;
  p->done       = [this] (Probe &p) {connected(p);};

  state   = STATE_CONNECTING;
  probing = true;
  probe(p);
}


void ClickHouseSink::connected(Probe &p) {
  if (p.generation != generation || !probing) return;
  probing = false;

  if (p.result != RESULT_OK) return fail(p.result, p.schema, p.error, p.code);

  version = p.version;
  state   = STATE_OK;
  if (warned == STATE_OK) LOG_INFO(1, "Connected to ClickHouse");
  note(version.empty() ? "Connected" : "Connected to ClickHouse " + version);

  pump();
}


void ClickHouseSink::recovered() {
  // Only an insert shows the server works again
  retryDelay = 0;
  if (warned != STATE_OK) LOG_INFO(1, "ClickHouse is reachable again");
  warned = STATE_OK;

  if (failures)
    note(SSTR("Reachable again after " << failures << " failed "
              << (failures == 1 ? "try, " : "tries, ")
              << formatRows(rowsQueued) << " waiting"));
  failures = 0;
}


void ClickHouseSink::fail(result_t result, bool schema, const string &error,
                          unsigned code) {
  state_t next = result == RESULT_RETRY ? STATE_RETRY :
    (schema ? STATE_SCHEMA : STATE_CONFIG);

  // Errors only the settings or the server can fix are retried slowly
  if (next != STATE_RETRY) retryDelay = configRetry;
  else retryDelay = retryDelay ? std::min(2 * retryDelay, retryMax) : retryMin;

  retryAt = now() + retryDelay;
  setError(error, code);
  failures++;

  // The shared log says what happened, the details stay local
  const char *what = next == STATE_RETRY ? "unavailable" :
    (next == STATE_SCHEMA ? "tables are not compatible" :
     "settings or permissions are wrong");
  if (warned != next)
    LOG_WARNING("ClickHouse " << what << ", queueing GPU telemetry"
                << getCodeSuffix(code));
  note(SSTR("ClickHouse " << what << ": " << error << ", next try at "
            << Time((uint64_t)retryAt).toString()));

  warned = state = next;
}


void ClickHouseSink::setError(const string &error, unsigned code) {
  lastError     = error;
  lastErrorCode = code;
  lastErrorTime = now();
}


ClickHouseSink::response_cb_t
ClickHouseSink::next(const ProbePtr &p, const response_cb_t &cb) {
  unsigned id = ++p->requests;

  return guard(
    [this, p, cb, id] (const Response &r) {
      // An ended or superseded probe, or a repeated reply, stops here
      if (p->ended || id != p->requests) return;
      if (!p->test && (!probing || p->generation != generation)) return;
      if (!probeFailed(p, r)) cb(r);
    },
    [this, p] {
      if (p->ended) return;
      p->fail(RESULT_CONFIG, badReply, 0);
      probeEnd(p);
    });
}


void ClickHouseSink::probe(const ProbePtr &p) {
  send(*p->http, *p->client, "SELECT version()", 0, 0, next(p,
    [this, p] (const Response &r) {
      auto rows = getRows(r);
      string v  = rows->size() == 1 ? getCell(*rows, 0, 0) : "";

      if (!isVersion(v)) {
        p->fail(RESULT_CONFIG, "The server did not report a ClickHouse "
                "version", 0);
        return probeEnd(p);
      }

      // Not shown or kept if the reply held a password
      if (echoed || clean(v, {p->password, password}) != v) v.clear();

      p->version = v;
      p->pass(v.empty() ? string("ClickHouse") : "ClickHouse " + v);
      probeDatabase(p);
    }));
}


void ClickHouseSink::probeDatabase(const ProbePtr &p) {
  JSON::ValuePtr params = new JSON::Dict;
  params->insert("db", p->database);

  send(*p->http, *p->client, "SELECT name FROM system.databases WHERE "
       "name = {db:String}", params, 0, next(p,
    [this, p] (const Response &r) {
      auto rows = getRows(r);

      // Creating it would need more than the writer should have
      if (!rows->size() || getCell(*rows, 0, 0) != p->database) {
        p->fail(RESULT_CONFIG, "Database " + p->database + " does not "
                "exist or this user cannot access it", 0);
        return probeEnd(p);
      }

      p->pass("Database " + p->database + " found");
      probeTables(p);
    }));
}


void ClickHouseSink::probeTables(const ProbePtr &p) {
  JSON::ValuePtr params = new JSON::Dict;
  params->insert("db", p->database);

  string sql = SSTR("SELECT table, name, type FROM system.columns WHERE "
                    "database = {db:String} AND table IN ('" << samplesTable
                    << "', '" << eventsTable << "')");

  send(*p->http, *p->client, sql, params, 0, next(p,
    [this, p] (const Response &r) {
      auto rows = getRows(r);

      map<string, map<string, string>> found; // Column types by table
      for (unsigned i = 0; i < rows->size(); i++)
        found[getCell(*rows, i, 0)][getCell(*rows, i, 1)] =
          getCell(*rows, i, 2);

      // Columns may be added, never changed
      vector<string> errors;
      for (auto &table: tables) {
        auto it = found.find(table.name);
        if (it == found.end()) {
          p->create.push_back(table.name);
          continue;
        }

        string error;
        for (auto &col: table.columns) {
          auto c = it->second.find(col.name);
          if (c == it->second.end())
            error = SSTR("column " << col.name << " is missing");
          else if (normalizeType(c->second) != normalizeType(col.type))
            error = SSTR("column " << col.name << " is "
                         << clean(echoed ? string("***") : c->second,
                                  {p->password, password})
                         << ", expected " << col.type);
          if (!error.empty()) break;
        }

        if (error.empty()) p->found.push_back(table.name);
        else errors.push_back(SSTR("Table " << table.name << ": " << error));
      }

      if (!errors.empty()) {
        p->fail(RESULT_CONFIG, String::join(errors, "; "), 0);
        p->schema = true;
        return probeEnd(p);
      }

      if (p->found.empty()) probeCreate(p);
      else probeEngines(p);
    }));
}


void ClickHouseSink::probeEngines(const ProbePtr &p) {
  JSON::ValuePtr params = new JSON::Dict;
  params->insert("db", p->database);

  string sql = SSTR("SELECT name, engine, engine_full FROM system.tables "
                    "WHERE database = {db:String} AND name IN ('"
                    << samplesTable << "', '" << eventsTable << "')");

  // Without the right to read it, a working setup is not failed
  unsigned id = ++p->requests;
  send(*p->http, *p->client, sql, params, 0, guard(
    [this, p, id] (const Response &r) {
      if (p->ended || id != p->requests) return;
      if (!p->test && (!probing || p->generation != generation)) return;

      auto result = classify(r);
      if (result == RESULT_RETRY) {probeFailed(p, r); return;}
      if (result != RESULT_OK) {
        p->unverified = true;
        return probeCreate(p);
      }

      auto rows = getRows(r);
      vector<string> errors;
      std::set<string> checked;

      for (unsigned i = 0; i < rows->size(); i++) {
        string name   = getCell(*rows, i, 0);
        string engine = getCell(*rows, i, 1);
        string full   = getCell(*rows, i, 2);
        if (name != samplesTable && name != eventsTable) continue;
        checked.insert(name);

        // Replicated tables deduplicate by default.  Without the setting
        // the server's default applies, which is not known here.
        int64_t window =
          getEngineSetting(full, "non_replicated_deduplication_window");

        if (!String::endsWith(engine, "MergeTree"))
          errors.push_back("Table " + name + " is not a MergeTree table");
        else if (String::startsWith(engine, "Replicated") ||
                 String::startsWith(engine, "Shared")) continue;
        else if (window < 0) p->unverified = true;
        else if (!window)
          errors.push_back(SSTR("Table " << name << " does not deduplicate "
                                "inserts, run ALTER TABLE " << name
                                << " MODIFY SETTING "
                                "non_replicated_deduplication_window = "
                                << dedupWindow));
      }

      for (auto &name: p->found)
        if (!checked.count(name)) p->unverified = true;

      if (!errors.empty()) {
        p->fail(RESULT_CONFIG, String::join(errors, "; "), 0);
        p->schema = true;
        return probeEnd(p);
      }

      probeCreate(p);
    },
    [this, p] {
      if (p->ended) return;
      p->fail(RESULT_CONFIG, badReply, 0);
      probeEnd(p);
    }));
}


void ClickHouseSink::probeCreate(const ProbePtr &p) {
  if (p->created == p->create.size()) {
    vector<string> parts;
    if (!p->create.empty())
      parts.push_back("Created " + String::join(p->create, ", "));
    if (!p->found.empty())
      parts.push_back("Found " + String::join(p->found, ", ") +
                      (p->unverified ? " (deduplication not verified)" : ""));
    p->pass(String::join(parts, "; "));

    if (p->test) return probeInsert(p, 0);
    return probeEnd(p);
  }

  const Table *table = 0;
  for (auto &t: tables)
    if (p->create[p->created] == t.name) table = &t;

  send(*p->http, *p->client, getDDL(p->database, *table), 0, 0, next(p,
    [this, p] (const Response &) {
      p->created++;
      probeCreate(p);
    }));
}


void ClickHouseSink::probeInsert(const ProbePtr &p, unsigned table) {
  if (table == 2) {
    p->pass("Inserts allowed, no rows written");
    return probeEnd(p);
  }

  // Checks the privilege, but the select returns no row to insert
  string sql = "INSERT INTO " + getTableName(p->database, tables[table].name)
    + " (`time`) SELECT now64(3) WHERE 0";

  send(*p->http, *p->client, sql, 0, 0, next(p,
    [this, p, table] (const Response &) {probeInsert(p, table + 1);}));
}


bool ClickHouseSink::probeFailed(const ProbePtr &p, const Response &r) {
  auto result = classify(r);
  if (result == RESULT_OK) return false;

  // Before any insert, only an outage is worth retrying soon
  if (result != RESULT_RETRY) result = RESULT_CONFIG;
  vector<string> secrets = {p->password, password};
  p->fail(result, describe(r, p->password, p->https),
          getShown(r.getCode(), secrets));
  note(SSTR((p->test ? "Test" : "Check") << " request failed, step "
            << p->steps[p->step].name << ": " << getHTTPStatus(r, secrets)
            << p->error));
  probeEnd(p);

  return true;
}


void ClickHouseSink::probeEnd(const ProbePtr &p) {
  if (p->ended) return;
  p->ended = true;
  if (p->done) p->done(*p);
}


bool ClickHouseSink::hasTable(const char *name) {
  int64_t count = 0;
  app.getDatabase().execute(
    SSTR("SELECT count(*) FROM sqlite_master WHERE name = '" << name << "'"),
    count);
  return 0 < count;
}


void ClickHouseSink::createQueue() {
  if (queueReady) return;

  app.getDatabase().execute(
    SSTR("CREATE TABLE IF NOT EXISTS " << queueTable << " ("
         "id INTEGER PRIMARY KEY AUTOINCREMENT, dest TEXT NOT NULL, "
         "token TEXT NOT NULL, rows INTEGER NOT NULL, data TEXT NOT NULL)"));
  queueReady = true;
}


void ClickHouseSink::insertBatch(Batch &batch) {
  // The id is chosen here, so one statement both stores and numbers it
  auto stmt = app.getDatabase().compile(
    SSTR("INSERT INTO " << queueTable << " (id, dest, token, rows, data) "
         "VALUES (?, ?, ?, ?, ?)"));
  stmt->parameter(0).bind(nextID);
  stmt->parameter(1).bind(batch.table);
  stmt->parameter(2).bind(batch.token);
  stmt->parameter(3).bind((int64_t)batch.rows);
  stmt->parameter(4).bind(batch.data);
  stmt->execute();
  batch.id = nextID++;
}


void ClickHouseSink::enqueue(Batch &batch) {
  batch.id = 0;

  // While the oldest rows cannot be removed, newer ones are not added
  if (trimFailed && settings.queueMax <= rowsQueued) trimQueue();
  if (trimFailed && settings.queueMax <= rowsQueued) {
    rowsDropped += batch.rows;
    note(SSTR("Dropped " << formatRows(batch.rows) << ", the full queue could "
              "not be trimmed"));
    return;
  }

  try {
    if (!queueCounted) countQueue();
    if (!queueCounted) THROW("Queue not counted");

    createQueue();
    insertBatch(batch);

    rowsQueued += batch.rows;
    queueFailed = false;
    trimQueue();

  } catch (const Exception &e) {
    batch.id     = 0;
    rowsDropped += batch.rows;
    queueReady   = false;
    if (!queueFailed)
      LOG_WARNING("Failed to queue ClickHouse rows: " << e.getMessage());
    queueFailed = true;
    note(SSTR("Failed to queue " << formatRows(batch.rows) << ", dropped: "
              << e.getMessage()));
  }
}


bool ClickHouseSink::dequeue(Batch &batch) {
  if (!queueCounted || !rowsQueued) return false;

  try {
    auto stmt = app.getDatabase().compile(
      SSTR("SELECT id, dest, token, rows, data FROM " << queueTable
           << " ORDER BY id LIMIT 1"));

    if (!stmt->next()) {
      rowsQueued  = 0;
      queueWarned = false;
      return false;
    }

    batch.id    = stmt->column(0).toInteger();
    batch.table = stmt->column(1).toString();
    batch.token = stmt->column(2).toString();
    batch.rows  = (unsigned)stmt->column(3).toInteger();
    batch.data  = stmt->column(4).toString();
    stmt.release();

    if (batch.table == samplesTable || batch.table == eventsTable) return true;

    // Not ours
    if (unqueue(batch.id)) rowsDropped += batch.rows;

  } catch (const Exception &e) {
    if (!queueFailed) {
      LOG_WARNING("Failed to read ClickHouse queue: " << e.getMessage());
      note("Failed to read the queue: " + e.getMessage());
    }
    queueFailed = true;
  }

  return false;
}


bool ClickHouseSink::unqueue(int64_t id) {
  try {
    auto &db = app.getDatabase();
    auto get = db.compile(SSTR("SELECT rows FROM " << queueTable
                               << " WHERE id = ?"));
    get->parameter(0).bind(id);
    uint64_t rows = get->next() ? get->column(0).toInteger() : 0;
    get.release();

    auto del = db.compile(SSTR("DELETE FROM " << queueTable
                               << " WHERE id = ?"));
    del->parameter(0).bind(id);
    del->execute();

    rowsQueued -= std::min(rows, rowsQueued);
    if (!rowsQueued) queueWarned = false;
    return true;

  } catch (const Exception &e) {
    if (!queueFailed) {
      LOG_WARNING("Failed to update ClickHouse queue: " << e.getMessage());
      note("Failed to update the queue: " + e.getMessage());
    }
    queueFailed = true;
  }

  return false;
}


void ClickHouseSink::countQueue() {
  queueCounted = false;

  try {
    queueReady = hasTable(queueTable);
    uint64_t rows = 0;
    int64_t last  = 0;

    if (queueReady) {
      auto stmt = app.getDatabase().compile(
        SSTR("SELECT coalesce(sum(rows), 0), coalesce(max(id), 0) FROM "
             << queueTable));
      if (stmt->next()) {
        rows = stmt->column(0).toInteger();
        last = stmt->column(1).toInteger();
      }
    }

    rowsQueued   = rows;
    nextID       = last + 1;
    queueCounted = true;
    queueFailed  = false;
    trimQueue();

  } catch (const Exception &e) {
    if (!queueFailed) {
      LOG_WARNING("Failed to read ClickHouse queue: " << e.getMessage());
      note("Failed to read the queue: " + e.getMessage());
    }
    queueFailed = true;
  }
}


void ClickHouseSink::trimQueue() {
  // Not before the saved limit is known
  if (loadFailed || rowsQueued <= settings.queueMax) return;

  // Oldest first in one delete, never a batch being sent or just sent
  try {
    auto &db       = app.getDatabase();
    int64_t keep[] = {inflightID ? inflight.id : 0, delivered};
    uint64_t excess = rowsQueued - settings.queueMax;
    uint64_t found  = 0;
    int64_t cut     = 0;

    {
      auto stmt = db.compile(SSTR("SELECT id, rows FROM " << queueTable
                                  << " WHERE id NOT IN (?, ?) ORDER BY id"));
      stmt->parameter(0).bind(keep[0]);
      stmt->parameter(1).bind(keep[1]);

      while (found < excess && stmt->next()) {
        cut    = stmt->column(0).toInteger();
        found += stmt->column(1).toInteger();
      }
    }

    if (!cut) return;

    auto del = db.compile(SSTR("DELETE FROM " << queueTable
                               << " WHERE id <= ? AND id NOT IN (?, ?)"));
    del->parameter(0).bind(cut);
    del->parameter(1).bind(keep[0]);
    del->parameter(2).bind(keep[1]);
    del->execute();

    rowsQueued  -= std::min(found, rowsQueued);
    rowsDropped += found;
    trimFailed   = false;
    note("Queue full, dropped the oldest " + formatRows(found));

    if (!queueWarned)
      LOG_WARNING("ClickHouse queue is full, dropped the oldest " << found
                  << " rows");
    queueWarned = true;

  } catch (const Exception &e) {
    if (!trimFailed) {
      LOG_WARNING("Failed to trim ClickHouse queue, dropping new rows: "
                  << e.getMessage());
      note("Failed to trim the queue: " + e.getMessage());
    }
    trimFailed = true;
  }
}


void ClickHouseSink::loadStarted() {
  startedLoaded = false;
  std::set<string> queued; // Start rows without a marker

  try {
    auto &units = *app.getUnits();

    // Started in this run, even if its marker could not be written
    std::set<string> found;
    for (auto &id: started)
      if (units.findUnit(id).isSet()) found.insert(id);

    if (hasTable(unitsTable)) {
      // Units gone while not sending will not finish here
      vector<string> gone;
      auto &db = app.getDB(unitsTable);

      db.foreach([&] (const string &id, const string &) {
        if (units.findUnit(id).isSet()) found.insert(id);
        else gone.push_back(id);
      }, ~0U);

      for (auto &id: gone) db.unset(id);
    }

    bool unmarked = false;
    for (unsigned i = 0; i < units.size(); i++)
      if (!found.count(units.getUnit(i)->getID())) unmarked = true;

    // A crash between a start's two writes leaves its row queued unmarked
    if (unmarked && hasTable(queueTable)) {
      std::set<string> tokens;
      auto stmt = app.getDatabase().compile(
        SSTR("SELECT token FROM " << queueTable << " WHERE dest = '"
             << eventsTable << "'"));
      while (stmt->next()) tokens.insert(stmt->column(0).toString());

      for (unsigned i = 0; i < units.size(); i++) {
        string id = units.getUnit(i)->getID();
        if (!found.count(id) && tokens.count(getEventToken("start", id)) &&
            !tokens.count(getEventToken("finish", id)))
          queued.insert(id);
      }
    }

    started = found;
    started.insert(queued.begin(), queued.end());
    startedLoaded = true;
    unitsFailed   = false;

  } catch (const Exception &e) {
    app.closeDB(unitsTable); // A failed statement stays unusable
    if (!unitsFailed) {
      LOG_WARNING("Failed to read ClickHouse units: " << e.getMessage());
      note("Failed to read the unit markers: " + e.getMessage());
    }
    unitsFailed = true;
    return;
  }

  for (auto &id: queued) mark(id, true);

  // Starts seen meanwhile, with the time they were seen, unless sent before
  auto waiting = std::move(pending);
  pending.clear();

  for (auto &p: waiting)
    if (started.insert(p.first).second) event(p.first, p.second, true);
}
