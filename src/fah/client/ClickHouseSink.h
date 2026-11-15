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

#pragma once

#include "GPUSampleSink.h"

#include <cbang/SmartPointer.h>
#include <cbang/json/Value.h>
#include <cbang/event/Event.h>
#include <cbang/db/clickhouse/Client.h>

#include <map>
#include <set>
#include <string>
#include <vector>
#include <iostream>
#include <functional>
#include <cstdint>


namespace cb {
  class SSLContext;
  namespace HTTP {class Client; class Request;}
}


namespace FAH {
  namespace Client {
    class App;
    class Unit;
    class Remote;

    // Writes every GPU sample and Work Unit start and finish to ClickHouse.
    // The settings belong to this machine, so they are kept out of the config
    // which remotes share, and the password is never sent to any remote.
    // Every closed batch waits in a bounded database queue until sent.
    class ClickHouseSink :
      public GPUSampleSink, public virtual cb::RefCounted {
    public:
      typedef cb::ClickHouse::Response Response;
      typedef std::function<void (const Response &)> response_cb_t;

      enum result_t {RESULT_OK, RESULT_RETRY, RESULT_DATA, RESULT_CONFIG};

      struct Settings {
        bool enabled = false;
        std::string url;
        std::string database;
        std::string user;
        std::string ca;
        unsigned queueMax;

        Settings();

        bool isConfigured() const {return !url.empty() && !database.empty();}
        std::string getBinding() const;
        void read(const cb::JSON::Value &value);
        void write(cb::JSON::Sink &sink) const;
      };

    protected:
      enum state_t {
        STATE_DISABLED, STATE_CONNECTING, STATE_OK, STATE_RETRY,
        STATE_CONFIG, STATE_SCHEMA,
      };

      struct Batch {
        int64_t id = 0; // In the queue
        std::string table;
        std::string token;
        unsigned rows = 0;
        std::string data; // JSONEachRow
      };

      struct Probe;
      typedef cb::SmartPointer<Probe> ProbePtr;

      App &app;
      Settings settings;
      std::string password;

      cb::SmartPointer<cb::HTTP::Client> http;
      cb::SmartPointer<cb::ClickHouse::Client> client;
      cb::Event::EventPtr timer;
      cb::Event::EventPtr sendEvent;

      state_t state       = STATE_DISABLED;
      state_t warned      = STATE_OK; // Last failure state logged
      unsigned generation = 0;        // Of the settings
      uint64_t requests   = 0;
      bool probing        = false;
      bool testing        = false;
      bool echoed         = false; // The last reply held a password
      double retryAt      = 0;
      unsigned retryDelay = 0;
      double nextSend     = 0;

      std::string buffer; // Open sample batch
      unsigned bufferRows = 0;
      double bufferStart  = 0;

      Batch inflight;
      uint64_t inflightID = 0; // Request, 0 if none
      int64_t delivered   = 0; // Sent but still in the queue
      std::set<std::string> started; // Unit IDs
      std::map<std::string, std::string> pending; // Start rows by unit ID

      std::string version;
      std::string lastError;
      unsigned lastErrorCode = 0;
      double lastErrorTime   = 0;
      double lastSuccess     = 0;
      uint64_t rowsSent      = 0;
      uint64_t rowsDropped   = 0;
      uint64_t rowsTruncated = 0;
      uint64_t rowsQueued    = 0; // In the database
      bool queueReady        = false;
      bool queueCounted      = false;
      bool startedLoaded     = false;
      bool trimFailed        = false;
      bool unitsFailed       = false;
      int64_t nextID         = 1;   // Of the next queued batch
      bool dataWarned        = false;
      bool queueWarned       = false;
      bool queueFailed       = false;
      bool loadFailed        = false; // Settings not read
      double loadTime        = 0;
      // Until the settings are read, with the time each came
      std::vector<std::pair<double, GPUSample>> held;
      uint64_t heldDropped   = 0;     // Beyond one batch
      std::map<std::string, double> heldUnits; // First seen beyond it
      unsigned failures      = 0;   // Failed tries since the last insert
      std::set<std::string> refused; // Remote names

      std::string logPath;
      cb::SmartPointer<std::iostream> logFile;
      uint64_t logSize = 0;
      bool logFailed   = false;

    public:
      ClickHouseSink(App &app);
      virtual ~ClickHouseSink();

      bool isActive() const;
      void init();
      void load();
      void save();
      void finish(const Unit &unit);
      void command(Remote &remote, const cb::JSON::Value &msg);
      cb::JSON::ValuePtr getStatus() const;

      static result_t classify(const Response &r);
      static std::string formatTime(double time);

      // From GPUSampleSink
      void sample(const GPUSample &s) override;

    protected:
      virtual double now() const;
      virtual void request(cb::HTTP::Client &http,
                           cb::ClickHouse::Client &client,
                           const std::string &sql,
                           const cb::JSON::ValuePtr &params,
                           const cb::JSON::ValuePtr &settings,
                           const response_cb_t &cb);
      cb::SmartPointer<cb::HTTP::Client>
      newHTTP(const cb::SmartPointer<cb::SSLContext> &ctx);
      void filter(cb::HTTP::Request &req);

      const std::string &getLogPath();
      void note(const std::string &text);

      void pump();
      void reload();
      void retryLoad();
      void startTimer();
      void apply();
      void record(const GPUSample &s, double time);
      void closeDue(double time);
      void start(const std::string &id, double time);
      void event(const std::string &id, const std::string &row, bool start);
      void mark(const std::string &id, bool start);

      void configure(Remote &remote, const cb::JSON::Value &msg);
      void test(Remote &remote, const cb::JSON::Value &msg);
      bool read(const cb::JSON::Value &msg, Settings &settings,
                std::string &password) const;
      void store(const Settings &next, const std::string &pass);
      void reply(Remote &remote, const cb::JSON::Value &msg,
                 const cb::JSON::ValuePtr &body);
      void refuse(Remote &remote, const cb::JSON::Value &msg,
                  const std::string &error, const std::string &code);
      cb::JSON::ValuePtr getSettings() const;
      std::string describe(const Response &r, const std::string &password,
                           bool https) const;

      std::string formatSample(const GPUSample &s) const;
      std::string formatEvent(const std::string &event, double time,
                              const std::string &id,
                              const cb::JSON::Value &unit,
                              const cb::JSON::Value &data);
      void writeMachine(cb::JSON::Sink &sink) const;

      void close();
      void sendNext();
      void inserted(unsigned generation, const std::string &password,
                    bool https, const Response &r);
      void send(cb::HTTP::Client &http, cb::ClickHouse::Client &client,
                const std::string &sql,
                const cb::JSON::ValuePtr &params,
                const cb::JSON::ValuePtr &settings, const response_cb_t &cb);
      response_cb_t guard(const response_cb_t &cb,
                          const std::function<void ()> &failed);

      void connect();
      void connected(Probe &p);
      void recovered();
      void fail(result_t result, bool schema, const std::string &error,
                unsigned code);
      void setError(const std::string &error, unsigned code);

      response_cb_t next(const ProbePtr &p, const response_cb_t &cb);
      void probe(const ProbePtr &p);
      void probeDatabase(const ProbePtr &p);
      void probeTables(const ProbePtr &p);
      void probeEngines(const ProbePtr &p);
      void probeCreate(const ProbePtr &p);
      void probeInsert(const ProbePtr &p, unsigned table);
      bool probeFailed(const ProbePtr &p, const Response &r);
      void probeEnd(const ProbePtr &p);

      bool hasTable(const char *name);
      void createQueue();
      void insertBatch(Batch &batch);
      void enqueue(Batch &batch);
      bool dequeue(Batch &batch);
      bool unqueue(int64_t id);
      void countQueue();
      void trimQueue();
      void loadStarted();
    };
  }
}
