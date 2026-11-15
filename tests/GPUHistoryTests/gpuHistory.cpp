// Feeds timed GPU telemetry samples, attributed to units, from stdin to
// FAH::Client::GPUHistory and prints what it stores and the unit series and
// GPU ranges it answers.  The input is a list of steps:
//
//   {"sample": {"time", "weight", "now", "gpu", "units", measurements}}
//   {"repeat": count, "step": seconds, "sample": {...}}
//   {"config": {...}}         {"unit": unit state}
//   {"remove": unit ID}
//   {"now": seconds}          {"prune": true}
//   {"flush": true}           {"save": true}
//   {"end": unit ID, "result": result}
//   {"restart": true}         {"crash": true}
//   {"set": [table, key, value]}
//   {"multiple": [length, length]}
//   {"sql": statement}        {"dump": true or table}
//   {"tables": true}
//   {"series": unit ID, "brief": bool, "compare": bool}
//   {"days": GPU ID, "from": seconds, "to": seconds, "rollup": bool}
//   {"range": GPU ID, "from": seconds, "to": seconds, "points": rows,
//    "brief": bool}
//
// A crash is a restart that saves nothing.  A sample sets the clock to its
// "now" or time.  A series is compared, unless "compare" is false, with one
// series per unit and GPU fed all samples attributed to the unit.  "brief"
// prints only bucket length and row count.  "tables" lists the history's
// tables.  A step with "timed": label prints its duration to stderr.

#include <fah/client/App.h>
#include <fah/client/Config.h>
#include <fah/client/Unit.h>
#include <fah/client/Units.h>
#include <fah/client/GPUHistory.h>

#include <cbang/ApplicationMain.h>
#include <cbang/SmartPointer.h>
#include <cbang/hw/GPUMeasurement.h>
#include <cbang/json/JSON.h>
#include <cbang/json/Builder.h>
#include <cbang/time/Time.h>
#include <cbang/time/Timer.h>

#include <iostream>
#include <map>
#include <cmath>

using namespace FAH::Client;
using namespace cb;
using namespace std;


namespace FAH {
  namespace Client {
    class TestHistory : public GPUHistory {
    public:
      uint64_t clock = 0;

      TestHistory(App &app) : GPUHistory(app) {}

      using GPUHistory::flush;
      using GPUHistory::load;
      using GPUHistory::prune;
      uint64_t now() const override {return clock;}
      void crash() {dirty = false;} // Nothing left for the destructor
    };


    class GPUHistoryTest : public App {
      map<string, map<string, GPUSeries>> perUnit; // By unit and GPU

    public:
      void run() override {
        setup();

        auto &config = *getConfig();
        config.insertBoolean("gpu_telemetry_history", true);

        SmartPointer<TestHistory> history = new TestHistory(*this);
        auto steps = JSON::Reader(cin).parse();

        for (auto &step: *steps) {
          double start = Timer::now();

          if (step->has("sample")) {
            unsigned count = step->getU32("repeat", 1);
            double delta   = step->getNumber("step", 0);

            for (unsigned i = 0; i < count; i++)
              sample(*history, *step->get("sample"), i * delta);
          }

          if (step->has("config")) {
            auto &values = *step->get("config");
            for (auto it = values.begin(); it != values.end(); it++)
              config.insert(it.key(), *it);
          }

          if (step->has("unit")) {
            auto blob = JSON::build([&] (JSON::Sink &sink) {
              sink.beginDict();
              sink.insert("state", *step->get("unit"));
              sink.insertDict("data");
              sink.endDict();
              sink.endDict();
            });
            getUnits()->add(new Unit(*this, blob));
          }

          if (step->has("remove"))
            getUnits()->removeUnit(step->getString("remove"));

          if (step->has("now")) history->clock = step->getU64("now");
          if (step->has("prune") && history->load()) history->prune();
          if (step->has("flush")) history->flush();
          if (step->has("save")) history->save();
          if (step->has("sql")) db.execute(step->getString("sql"));

          if (step->has("end"))
            history->end(step->getString("end"), step->getString("result"));

          if (step->has("crash")) history->crash();

          if (step->has("restart") || step->has("crash")) {
            uint64_t clock = history->clock;
            history.release();
            history = new TestHistory(*this);
            history->clock = clock;
            cout << (step->has("crash") ? "crash\n" : "restart\n");
          }

          if (step->has("set")) {
            auto &row = *step->get("set");
            string table = row.getString(0);
            getDB(table, table == "wu_log")
              .set(row.getString(1), row.getString(2));
          }

          if (step->has("series")) {
            string id   = step->getString("series");
            auto series = history->get(id);

            if (!step->getBoolean("brief", false))
              cout << "series " << id << '=' << series->toString(0, true)
                   << '\n';

            else if (series->isDict())
              for (auto it = series->begin(); it != series->end(); it++)
                cout << "series " << id << ' ' << it.key() << " bucket="
                     << (*it)->getU32("bucket") << " rows="
                     << (*it)->get("rows")->size() << '\n';

            if (step->getBoolean("compare", true))
              cout << "per-unit " << id << '='
                   << (series->toString(0, true) == getPerUnit(id) ?
                       "same" : "different") << '\n';
          }

          if (step->has("days")) {
            string gpu    = step->getString("days");
            bool rollup   = step->getBoolean("rollup", false);
            uint64_t from = step->getU64("from") / Time::SEC_PER_DAY;
            uint64_t to   = step->getU64("to") / Time::SEC_PER_DAY;

            history->getDays(gpu, from, to, rollup,
              [&] (uint64_t day, const GPUHistory::tags_t &tags) {
                cout << "days " << gpu << ' '
                     << Time(day * Time::SEC_PER_DAY).toString("%Y%m%d");
                for (auto &p: tags)
                  cout << " '" << p.first << "'=" << p.second.getBucket()
                       << '/' << p.second.size();
                cout << '\n';
              });
          }

          if (step->has("range")) {
            string gpu = step->getString("range");
            auto range = history->get(
              gpu, step->getNumber("from", NAN), step->getNumber("to", NAN),
              step->getNumber("points", 0));

            if (step->getBoolean("brief", false) && range->isDict())
              cout << "range " << gpu << " bucket=" << range->getU32("bucket")
                   << " rows=" << range->get("rows")->size() << " units="
                   << range->get("units")->size()
                   << (range->has("incomplete") ? " incomplete" : "") << '\n';
            else cout << "range " << gpu << '=' << range->toString(0, true)
                      << '\n';
          }

          if (step->has("multiple")) {
            auto &args = *step->get("multiple");
            unsigned a = args.getU32(0), b = args.getU32(1);
            cout << "multiple " << a << ' ' << b << '='
                 << GPUTimeline::getMultiple(a, b) << '\n';
          }

          if (step->has("tables")) {
            string names;
            db.execute("SELECT group_concat(name, ' ') FROM (SELECT name "
                       "FROM sqlite_master WHERE type = 'table' AND name "
                       "LIKE 'gpu_history%' ORDER BY name)", names);
            cout << "tables" << (names.empty() ? "" : " ") << names << '\n';
          }

          if (step->has("dump")) {
            cout << "dump\n";
            if (step->hasString("dump")) dump(step->getString("dump"));
            else
              for (auto table: {"gpu_history", "gpu_history_days",
                                "gpu_history_rollup", "gpu_history_units"})
                dump(table);
          }

          if (step->has("timed"))
            cerr << "time " << step->getString("timed") << ' '
                 << (unsigned)((Timer::now() - start) * 1000) << " ms\n";
        }
      }


      void sample(GPUHistory &history, const JSON::Value &s, double offset) {
        GPUSample sample;
        sample.time          = s.getNumber("time") + offset;
        sample.weightSeconds = s.getNumber("weight");
        sample.device        = s.getString("gpu", "gpu:01:00:00");

        auto &m          = sample.measurement;
        m.gpuClock       = s.getU32("gpu_clock",  0);
        m.memClock       = s.getU32("mem_clock",  0);
        m.power          = s.getNumber("power",   0);
        m.temperature    = s.getU32("temp",       0);
        m.utilization    = s.getS32("gpu_util",   -1);
        m.memUtilization = s.getS32("mem_util",   -1);
        m.pstate         = s.getS32("pstate",     -1);
        m.pcieGen        = s.getU32("pcie_gen",   0);
        m.pcieWidth      = s.getU32("pcie_width", 0);

        if (s.hasList("units"))
          for (auto &id: *s.get("units")) sample.units.insert(id->getString());

        static_cast<TestHistory &>(history).clock =
          s.getU64("now", (uint64_t)sample.time);
        history.sample(sample);

        auto &config = *getConfig();
        if (!config.getGPUTelemetryHistory()) return;

        for (auto &id: sample.units) {
          auto &devices = perUnit[id];
          auto it       = devices.find(sample.device);

          if (it == devices.end())
            it = devices.insert(make_pair(sample.device,
              GPUSeries(config.getGPUTelemetryBucket()))).first;

          it->second.add(sample.time, m, sample.weightSeconds);
        }
      }


      string getPerUnit(const string &id) {
        auto it = perUnit.find(id);
        if (it == perUnit.end()) return "null";

        return JSON::build([&] (JSON::Sink &sink) {
          sink.beginDict();

          for (auto &p: it->second) {
            sink.beginInsert(p.first);
            p.second.writeSummary(sink);
          }

          sink.endDict();
        })->toString(0, true);
      }


      void dump(const string &table) {
        map<string, string> rows;
        getDB(table).foreach(
          [&] (const string &key, const string &value) {rows[key] = value;},
          ~0U);

        for (auto &p: rows)
          cout << table << ' ' << p.first << '=' << p.second << '\n';
      }
    };
  }
}


int main(int argc, char *argv[]) {
  return doApplication<FAH::Client::GPUHistoryTest>(argc, argv);
}
