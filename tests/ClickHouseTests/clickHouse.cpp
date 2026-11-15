// Drives FAH::Client::ClickHouseSink from steps on stdin with a scripted
// clock and scripted ClickHouse responses.  Prints every request the sink
// makes, every reply a remote gets and every log line about ClickHouse.
// The input is a list of steps:
//
//   {"now": seconds}
//   {"sample": {"time", "weight", "gpu", "units", measurements},
//    "repeat": count, "step": seconds}
//   {"unit": unit state, "fill": n}  adds a unit, with "fill" a wu.blob of
//                             n bytes
//   {"finish": unit ID, "result": result}
//   {"gpu": {"id", "description", "uuid"}}
//   {"config": {...}}         client config values
//   {"local": msg}            command from a loopback websocket
//   {"remote": msg, "peer": address or null, "name": remote name,
//    "headers": {...}}        with the headers of its websocket upgrade
//   {"app": msg, "peer": address}  through Remote::onMessage to the app's
//                             own sink
//   {"respond": {"status", "code", "body": text or JSON, "times": n} or
//    {"transport": text} or {"timeout": true}, "at": index, "twice": bool}
//                             answers the oldest open request, or the one
//                             at index, twice to model a repeated delivery;
//                             "again": response delivers a second one.  The
//                             reply first goes through the filter of the
//                             HTTP client the request was sent with.
//   {"pump": true}            {"status": true}
//   {"queue": true or "full"} {"tables": true}  {"dump": name value table}
//   {"stored": true}          {"restart": true}   {"crash": true}
//                             with "keep": true open requests stay open
//   {"set": [table, key, value]}  {"unset": [table, key]}  {"sql": statement}
//   {"classify": [responses]} {"gate": [addresses]} {"node": true}
//   {"time": [seconds]}
//   {"leak": password}        searches all output, the sink's log file, logs
//                             and the database for it and for what a cut of
//                             the server's error text, the split status line
//                             or JSON decoding could leave of it
//   {"scan": [texts]}         searches the log for each
//   {"throw": text}           the next request throws text
//   {"file": true}            prints the sink's own log file
//   {"rotated": true}         prints the rotated copies of that file
//   {"pad": bytes}            adds about that many bytes to that file
//   {"block": true}           puts a directory where that file goes
//   {"remove": unit ID}       the unit leaves the client
//   {"timer": true}           whether the sink's pump timer is set
//   {"held": true}            samples held while the settings are unread
//   {"kept": true}            units first seen when no more could be held
//   {"started": true}         the units the sink knows as started
//
// A request prints the password's length, never the password.  A crash is
// a restart that saves nothing.  The log file's path is printed as <cwd>/
// followed by its name when it is in the working directory.

#include <fah/client/App.h>
#include <fah/client/Config.h>
#include <fah/client/Unit.h>
#include <fah/client/Units.h>
#include <fah/client/Remote.h>
#include <fah/client/NodeRemote.h>
#include <fah/client/GPUResources.h>
#include <fah/client/ClickHouseSink.h>

#include <cbang/ApplicationMain.h>
#include <cbang/Catch.h>
#include <cbang/SmartPointer.h>
#include <cbang/String.h>
#include <cbang/log/Logger.h>
#include <cbang/log/LogLineListener.h>
#include <cbang/json/JSON.h>
#include <cbang/json/Builder.h>
#include <cbang/http/Client.h>
#include <cbang/http/Request.h>
#include <cbang/http/RequestParams.h>
#include <cbang/http/Headers.h>
#include <cbang/db/Statement.h>
#include <cbang/os/SystemUtilities.h>

#include <iostream>
#include <deque>
#include <vector>
#include <functional>

using namespace FAH::Client;
using namespace cb;
using namespace std;


namespace FAH {
  namespace Client {
    typedef function<void (const HTTP::Client::callback_t &,
                           ClickHouse::Client &, const string &,
                           const JSON::ValuePtr &, const JSON::ValuePtr &,
                           const ClickHouseSink::response_cb_t &)> request_t;


    class TestSink : public ClickHouseSink {
    public:
      double clock = 0;
      string throwText;
      request_t cb;
      function<void (const string &)> note;

      TestSink(App &app) : ClickHouseSink(app) {}

      using ClickHouseSink::pump;
      double now() const override {return clock;}
      void crash() {buffer.clear(); bufferRows = 0;}
      bool hasTimer() const {return timer.isSet();}
      unsigned getHeld() const {return held.size();}
      const std::set<string> &getStarted() const {return started;}
      const map<string, double> &getKept() const {return heldUnits;}

      void request(HTTP::Client &http, ClickHouse::Client &client,
                   const string &sql,
                   const JSON::ValuePtr &params,
                   const JSON::ValuePtr &settings,
                   const response_cb_t &cb) override {
        // Its requests must stay out of the log
        if (!http.isQuiet()) note("HTTP client logs its requests");

        if (!throwText.empty()) {
          string text = throwText;
          throwText.clear();
          THROW(text);
        }

        this->cb(http.getFilter(), client, sql, params, settings, cb);
      }
    };


    class TestRemote : public Remote {
      string name;
      SockAddr peer;
      function<void (const string &)> print;

    public:
      TestRemote(App &app, const string &name, const SockAddr &peer,
                 function<void (const string &)> print) :
        Remote(app), name(name), peer(peer), print(print) {}

      string getName() const override {return name;}
      SockAddr getPeerAddr() const override {return peer;}
      void close() override {}

      void send(const JSON::ValuePtr &msg) override {
        print("reply " + name + ' ' + msg->toString(0, true));
      }
    };


    class LogCapture : public LogLineListener {
    public:
      vector<string> lines;
      function<void (const string &)> print;

      void writeln(const char *s) override {
        string line = String::trimRight(s);
        lines.push_back(line);
        if (print && line.find("ClickHouse") != string::npos)
          print("log " + line);
      }
    };


    class ClickHouseTest : public App {
      SmartPointer<TestSink> sink;
      SmartPointer<LogCapture> capture = new LogCapture;
      // Each with the filter of the HTTP client it was sent with
      struct Open {
        HTTP::Client::callback_t filter;
        ClickHouseSink::response_cb_t cb;
        unsigned timeout;
      };
      deque<Open> pending;
      vector<SmartPointer<Remote>> remotes;
      string output;
      string logPath;   // Where the sink's file should be
      string logJSON;   // The same as a JSON string, as a pattern

    public:
      void print(const string &text) {
        string line =
          String::replace(text, logJSON, "\"<cwd>/external-tsdb.log\"");
        cout << line << '\n';
        output += line + '\n';
      }


      void run() override {
        logPath = SystemUtilities::joinPath(SystemUtilities::getcwd(),
                                            "external-tsdb.log");
        logJSON = String::escapeRE(JSON::String(logPath).toString());

        // Log lines are printed, so without the time
        auto &log = Logger::instance();
        log.setLogTime(false);
        log.setLogPrefix(false);
        log.setLogColor(false);
        capture->print = [this] (const string &s) {print(s);};
        log.addListener(capture);

        setup();
        get("info")->insert("id",        "machine-id-1");
        get("info")->insert("mach_name", "test-machine");
        newSink();

        auto steps = JSON::Reader(cin).parse();
        for (auto &step: *steps) TRY_CATCH_ERROR(run(*step));

        Logger::instance().removeListener(capture);
      }


      void newSink(bool keep = false) {
        double clock = sink.isSet() ? sink->clock : 0;
        sink.release();
        if (!keep) pending.clear();

        sink = new TestSink(*this);
        sink->clock = clock;
        sink->note  = [this] (const string &s) {print(s);};
        sink->cb = [this] (const HTTP::Client::callback_t &filter,
                           ClickHouse::Client &client, const string &sql,
                           const JSON::ValuePtr &params,
                           const JSON::ValuePtr &settings,
                           const ClickHouseSink::response_cb_t &cb) {
          request(client, sql, params, settings, cb);

          // The request as cbang's ClickHouse client sends it
          string key = client.getPassword();
          if (filter)
            pending.back().filter = [filter, key] (HTTP::Request &req) {
              if (!key.empty()) req.outSet("X-ClickHouse-Key", key);
              filter(req);
            };
        };
        sink->init();
      }


      void request(ClickHouse::Client &client, const string &sql,
                   const JSON::ValuePtr &params,
                   const JSON::ValuePtr &settings,
                   const ClickHouseSink::response_cb_t &cb) {
        string pass = client.getPassword();
        string line = "request url=" + client.getURL() + " user=" +
          client.getUser() + " password=" +
          (pass.empty() ? "none" : String((unsigned)pass.size()) + "-chars") +
          " timeout=" + String(client.getTimeout());
        if (params.isSet()) line += " params=" + params->toString(0, true);
        if (settings.isSet())
          line += " settings=" + settings->toString(0, true);
        print(line);

        printLines(sql);
        pending.push_back({0, cb, client.getTimeout()});
      }


      void printLines(const string &text) {
        // Long batches show only their ends
        vector<string> lines;
        String::tokenize(text, lines, "\n");

        for (unsigned i = 0; i < lines.size(); i++)
          if (i < 4 || i == lines.size() - 1 || lines.size() < 12)
            print("  " + lines[i]);
          else if (i == 4)
            print("  ... " + String((unsigned)lines.size() - 5) + " lines");
      }


      SmartPointer<Remote> getRemote(const string &name,
                                     const JSON::ValuePtr &peer,
                                     const JSON::ValuePtr &headers = 0) {
        SockAddr addr;
        if (peer.isSet() && peer->isString())
          addr = SockAddr::parse(peer->getString());

        SmartPointer<Remote> remote = new TestRemote(
          *this, name, addr, [this] (const string &s) {print(s);});
        remotes.push_back(remote);

        if (headers.isSet()) {
          HTTP::RequestParams params;
          params.uri  = URI("http://localhost/");
          params.hdrs = new HTTP::Headers;
          for (auto it = headers->begin(); it != headers->end(); it++)
            params.hdrs->set(it.key(), (*it)->getString());
          remote->setForwarded(*SmartPtr(new HTTP::Request(params)));
        }

        return remote;
      }


      static ClickHouse::Response getResponse(
        const JSON::Value &r, const HTTP::Client::callback_t &filter = 0,
        unsigned timeout = 30) {
        if (r.has("transport"))
          return ClickHouse::Response(
            "Connection failed: " + r.getString("transport"));

        if (r.getBoolean("timeout", false))
          return ClickHouse::Response(
            "Timed out after " + String(timeout) + "s", true);

        HTTP::RequestParams params;
        params.uri  = URI("http://localhost/");
        params.hdrs = new HTTP::Headers;

        SmartPointer<HTTP::Request> req = new HTTP::Request(params);
        req->parseResponseLine(
          "HTTP/1.1 " + (r.has("status") ? r.getAsString("status") : "200") +
          " Test");
        if (r.has("code"))
          req->inSet("X-ClickHouse-Exception-Code", r.getAsString("code"));
        if (r.hasString("body"))
          for (unsigned i = 0; i < r.getU32("times", 1); i++)
            req->getInputBuffer().add(r.getString("body"));
        else if (r.has("body"))
          req->getInputBuffer().add(r.get("body")->toString(0, true));

        if (filter) filter(*req);
        return ClickHouse::Response(*req);
      }


      void run(const JSON::Value &step) {
        if (step.has("now")) sink->clock = step.getNumber("now");
        if (step.has("throw")) sink->throwText = step.getString("throw");

        if (step.has("config")) {
          auto &values = *step.get("config");
          for (auto it = values.begin(); it != values.end(); it++)
            getConfig()->insert(it.key(), *it);
        }

        if (step.has("gpu")) {
          auto &gpu = *step.get("gpu");
          getGPUs().insert(gpu.getString("id"), gpu.copy(true));
        }

        if (step.has("unit")) {
          auto state = step.get("unit")->copy(true);
          if (step.has("fill")) {
            JSON::ValuePtr wu = new JSON::Dict;
            wu->insert("blob", string(step.getU32("fill"), 'y'));
            state->insert("wu", wu);
          }

          auto blob = JSON::build([&] (JSON::Sink &s) {
            s.beginDict();
            s.insert("state", *state);
            s.insertDict("data");
            s.endDict();
            s.endDict();
          });
          getUnits()->add(new Unit(*this, blob));
        }

        if (step.has("sample")) {
          unsigned count = step.getU32("repeat", 1);
          double delta   = step.getNumber("step", 0);

          for (unsigned i = 0; i < count; i++)
            sample(*step.get("sample"), i * delta);
        }

        if (step.has("finish")) {
          auto unit = getUnits()->findUnit(step.getString("finish"));
          unit->insert("result", step.getString("result", "dumped"));
          sink->finish(*unit);
        }

        if (step.has("local")) {
          auto remote = getRemote("local", new JSON::String("127.0.0.1"));
          sink->command(*remote, *step.get("local"));
        }

        if (step.has("remote")) {
          auto remote = getRemote(step.getString("name", "remote"),
                                  step.get("peer", JSON::Null::instancePtr()),
                                  step.has("headers") ? step.get("headers") :
                                  JSON::ValuePtr());
          sink->command(*remote, *step.get("remote"));
        }

        if (step.has("app")) {
          auto remote = getRemote(step.getString("name", "app"),
                                  step.get("peer", JSON::Null::instancePtr()));
          remote->onMessage(step.get("app"));
        }

        if (step.has("respond")) {
          unsigned at = step.getU32("at", 0);
          if (pending.size() <= at) print("respond nothing open");
          else {
            auto open = pending[at];
            pending.erase(pending.begin() + at);
            auto response =
              getResponse(*step.get("respond"), open.filter, open.timeout);
            open.cb(response);
            if (step.getBoolean("twice", false)) open.cb(response);
            if (step.has("again"))
              open.cb(getResponse(*step.get("again"), open.filter,
                                  open.timeout));
          }
        }

        if (step.has("pump")) sink->pump();

        if (step.has("status"))
          print("status " + sink->getStatus()->toString(0, true));

        if (step.has("classify")) {
          const char *names[] = {"ok", "retry", "data", "config"};
          for (auto &r: *step.get("classify"))
            print("classify " + r->toString(0, true) + " = " +
                  names[ClickHouseSink::classify(getResponse(*r))]);
        }

        if (step.has("gate"))
          for (auto &addr: *step.get("gate"))
            print("gate " + addr->toString(0, true) + " local=" +
                  (getRemote("gate", addr)->isLocal() ? "true" : "false"));

        if (step.has("node")) {
          SmartPointer<Remote> node = new NodeRemote(*this, getAccount(), "s");
          print(string("node local=") + (node->isLocal() ? "true" : "false"));
        }

        if (step.has("time"))
          for (auto &t: *step.get("time"))
            print("time " + t->toString(0, true) + " = " +
                  ClickHouseSink::formatTime(t->getNumber()));

        if (step.has("queue")) {
          bool full = step.hasString("queue");
          int64_t exists = 0;
          db.execute("SELECT count(*) FROM sqlite_master WHERE name = "
                     "'clickhouse_queue'", exists);
          unsigned count = 0;

          if (exists) {
            auto stmt = db.compile("SELECT id, dest, token, rows, data FROM "
                                   "clickhouse_queue ORDER BY id");
            for (; stmt->next(); count++) {
              print("queue " + stmt->column(0).toString() + ' ' +
                    stmt->column(1).toString() + " rows=" +
                    stmt->column(3).toString() + " token=" +
                    stmt->column(2).toString());

              if (full) printLines(stmt->column(4).toString());
            }
          }

          if (!count) print("queue empty");
        }

        if (step.has("dump")) {
          string table = step.getString("dump");
          int64_t exists = 0;
          db.execute("SELECT count(*) FROM sqlite_master WHERE name = '" +
                     table + "'", exists);

          if (!exists) print("dump " + table + " none");
          else {
            auto stmt = db.compile("SELECT name, value FROM \"" + table +
                                   "\" ORDER BY name");
            unsigned count = 0;
            for (; stmt->next(); count++)
              print("dump " + table + ' ' + stmt->column(0).toString() +
                    '=' + stmt->column(1).toString());
            if (!count) print("dump " + table + " empty");
          }
        }

        if (step.has("tables")) {
          string names;
          db.execute("SELECT group_concat(name, ' ') FROM (SELECT name "
                     "FROM sqlite_master WHERE type = 'table' AND name "
                     "LIKE 'clickhouse%' ORDER BY name)", names);
          print("tables" + (names.empty() ? "" : " " + names));
        }

        if (step.has("stored")) {
          auto &config = getDB("config");
          print("stored settings=" +
                config.getString("clickhouse", "none") + " password=" +
                (config.has("clickhouse-password") ? "set" : "unset"));
        }

        if (step.has("set")) {
          auto &row = *step.get("set");
          getDB(row.getString(0)).set(row.getString(1), row.getString(2));
        }

        if (step.has("unset")) {
          auto &row = *step.get("unset");
          getDB(row.getString(0)).unset(row.getString(1));
        }

        if (step.has("sql")) db.execute(step.getString("sql"));

        if (step.has("crash")) sink->crash();

        if (step.has("restart") || step.has("crash")) {
          newSink(step.getBoolean("keep", false));
          print(step.has("crash") ? "crash" : "restart");
        }

        if (step.has("remove"))
          getUnits()->removeUnit(step.getString("remove"));
        if (step.has("timer"))
          print(string("timer ") + (sink->hasTimer() ? "set" : "none"));
        if (step.has("held")) print("held " + String(sink->getHeld()));

        if (step.has("started")) {
          string ids;
          for (auto &id: sink->getStarted()) ids += ' ' + id;
          print("started" + (ids.empty() ? string(" none") : ids));
        }

        if (step.has("kept")) {
          string ids;
          for (auto &it: sink->getKept()) ids += ' ' + it.first;
          print("kept" + (ids.empty() ? string(" none") : ids));
        }

        if (step.has("pad")) {
          auto f = SystemUtilities::open(logPath, ios::out | ios::app |
                                         ios::binary);
          string line = string(99, 'x') + '\n';
          for (unsigned n = 0; n < step.getU32("pad"); n += 100) *f << line;
        }

        if (step.has("block")) SystemUtilities::ensureDirectory(logPath);
        if (step.has("file")) printFile(logPath, "file");

        if (step.has("rotated")) {
          vector<string> paths;
          if (SystemUtilities::exists("logs"))
            SystemUtilities::listDirectory(paths, "logs", "external-tsdb.*");
          print("rotated " + String((unsigned)paths.size()) + " files");
          for (auto &path: paths) printFile(path, "rotated");
        }

        if (step.has("leak")) leak(step.getString("leak"));

        if (step.has("scan"))
          for (auto &text: *step.get("scan")) {
            bool found = false;
            for (auto &line: capture->lines)
              if (line.find(text->getString()) != string::npos) found = true;
            print("scan " + text->toString(0, true) + " log=" +
                  (found ? "found" : "none"));
          }
      }


      void printFile(const string &path, const string &name) {
        if (!SystemUtilities::isFile(path)) return print(name + " none");

        // Padding shows as a count
        vector<string> lines;
        String::tokenize(SystemUtilities::read(path), lines, "\r\n");
        lines.push_back("");
        unsigned pad = 0;

        for (auto &line: lines)
          if (line == string(99, 'x')) pad++;
          else {
            if (pad) print(name + " padding " + String(pad) + " lines");
            if (!line.empty()) print(name + ' ' + line);
            pad = 0;
          }
      }


      void sample(const JSON::Value &s, double offset) {
        GPUSample sample;
        sample.time          = s.getNumber("time") + offset;
        sample.weightSeconds = s.getNumber("weight", 1);
        sample.device        = s.getString("gpu", "gpu:01:00:00");

        auto &m          = sample.measurement;
        m.gpuClock       = s.getU32("gpu_clock",  0);
        m.memClock       = s.getU32("mem_clock",  0);
        m.power          = s.getNumber("power",   0);
        m.powerLimit     = s.getNumber("power_limit", 0);
        m.temperature    = s.getU32("temp",       0);
        m.utilization    = s.getS32("gpu_util",   -1);
        m.memUtilization = s.getS32("mem_util",   -1);
        m.pstate         = s.getS32("pstate",     -1);
        m.pcieGen        = s.getU32("pcie_gen",   0);
        m.pcieWidth      = s.getU32("pcie_width", 0);
        if (s.hasList("fans"))
          for (auto &f: *s.get("fans")) m.fans.push_back(f->getU32());

        if (s.hasList("units"))
          for (auto &id: *s.get("units")) sample.units.insert(id->getString());

        sink->clock = s.getNumber("now", sample.time);
        sink->sample(sample);
      }


      void leak(const string &password) {
        // Also what trimming the server's error text or cutting it at
        // ClickHouse's prefix or suffix leaves of the password
        const string head = "DB::Exception: ", tail = " (version ";
        vector<size_t> starts = {0}, ends = {password.size()};
        for (size_t i = 1; i < password.size(); i++) {
          string a = password.substr(0, i), b = password.substr(i);
          if (String::trim(a).empty() || String::endsWith(a, head) ||
              String::endsWith(head, a)) starts.push_back(i);
          if (String::trim(b).empty() || String::startsWith(b, tail) ||
              String::startsWith(tail, b)) ends.push_back(i);
        }

        vector<string> parts;
        for (auto i: starts)
          for (auto j: ends)
            if (i < j && !String::trim(password.substr(i, j - i)).empty())
              parts.push_back(password.substr(i, j - i));

        // The name of a status read from one of its words
        vector<string> words;
        String::tokenize(password, words, " ");
        for (auto &word: words) {
          HTTP::Status status =
            HTTP::Status::parse(word, HTTP::Status::HTTP_UNKNOWN);
          if (HTTP::Status::isValid(status) && status)
            parts.push_back(status.toString());
        }

        // A result cell holding it or a part a quote split off, decoded,
        // except parts so short that they occur by chance
        vector<string> pieces;
        String::tokenize(password, pieces, "\"");
        pieces.push_back(password);
        for (auto &piece: pieces)
          try {
            string s = JSON::parse("\"" + piece + "\"")->getString();
            for (auto &c: s) if ((unsigned char)c < 32 || c == 127) c = ' ';
            if (2 < String::trim(s).size()) parts.push_back(s);
          } catch (const Exception &) {}

        auto has = [&] (const string &s) {
          for (auto &part: parts)
            if (s.find(part) != string::npos) return true;
          return false;
        };

        bool inLog = false;
        for (auto &line: capture->lines) if (has(line)) inLog = true;

        // Everything stored except the password itself and values that are
        // random or hashes, which can hold a short text by chance
        bool inDB = false;
        vector<string> tables;
        auto list = db.compile("SELECT name FROM sqlite_master WHERE "
                               "type = 'table'");
        while (list->next()) tables.push_back(list->column(0).toString());

        for (auto &table: tables) {
          string cols = table == "clickhouse_queue" ? "dest, rows, data" : "*";
          auto stmt = db.compile("SELECT " + cols + " FROM \"" + table + "\"");

          while (stmt->next()) {
            vector<string> values;
            for (unsigned i = 0; i < stmt->columns(); i++)
              values.push_back(stmt->column(i).toString());

            if (table == "config") {
              if (values[0] == "key" || values[0] == "machine-id") continue;

              if (values[0] == "clickhouse-password")
                try {
                  auto row = JSON::parse(values[1]);
                  if (row->isDict())
                    for (auto name: {"password", "binding"})
                      if (row->has(name)) row->erase(name);
                  values[1] = row->toString(0, true);
                } catch (const Exception &) {}
            }

            for (auto &value: values) if (has(value)) inDB = true;
          }
        }

        // Stored with what it is for
        auto &config = getDB("config");
        bool stored  = false;
        if (config.has("clickhouse-password"))
          try {
            stored = JSON::parse(config.getString("clickhouse-password"))
              ->getString("password", "") == password;
          } catch (const Exception &) {}

        // The sink's own log file counts as output
        bool shown = has(output) || (SystemUtilities::isFile(logPath) &&
                                     has(SystemUtilities::read(logPath)));

        print(string("leak output=") + (shown ? "found" : "none") +
              " log=" + (inLog ? "found" : "none") + " db=" +
              (inDB ? "found" : "none") + " logged=" +
              (capture->lines.empty() ? "nothing" : "lines") +
              " stored=" + (stored ? "yes" : "no"));
      }
    };
  }
}


int main(int argc, char *argv[]) {
  return doApplication<FAH::Client::ClickHouseTest>(argc, argv);
}
