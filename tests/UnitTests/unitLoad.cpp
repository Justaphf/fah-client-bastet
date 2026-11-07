// Drives FAH::Client::Unit's DB-loaded constructor and reports key
// invariants of the rehydrated Unit to stdout for the test harness to
// diff.  Read a saved-unit JSON blob from stdin; one blob per run.  With
// "gpu_stats" in the blob the restored GPU statistics are reported, then the
// unit is saved and the saved record loaded again.

#include <fah/client/App.h>
#include <fah/client/Unit.h>

#include <cbang/ApplicationMain.h>
#include <cbang/SmartPointer.h>
#include <cbang/json/Reader.h>

#include <iostream>


namespace FAH {
  namespace Client {
    class UnitLoadTest : public App {
    public:
      void run() override {
        setup();

        auto blob = cb::JSON::Reader(std::cin).parse();
        cb::SmartPointer<Unit> u = new Unit(*this, blob);

        std::cout << "state="    << u->getState()             << '\n';
        std::cout << "id="       << u->getID()                << '\n';
        std::cout << "wu="       << u->getU64("number")       << '\n';
        std::cout << "group="    << u->getString("group")     << '\n';
        std::cout << "cpus="     << u->getU32("cpus")         << '\n';
        std::cout << "run_time=" << u->getU64("run_time", 0)  << '\n';

        if (!blob->has("gpu_stats")) return;

        auto &stats = *blob->get("gpu_stats");
        if (stats.isDict())
          for (auto it = stats.begin(); it != stats.end(); it++)
            std::cout << "energy_wh." << it.key() << '='
                      << u->getGPUEnergyWh(it.key()) << '\n';

        u->save();
        std::cout << "gpu_stats=" << getStats(*u) << '\n';

        auto record = getDB("units").getJSON(u->getID());
        std::cout << "saved=" << getStats(*record) << '\n';

        // The saved state holds the summary too.  A re-save shows the restore
        cb::SmartPointer<Unit> r = new Unit(*this, record);
        r->save();
        auto again = getStats(*getDB("units").getJSON(r->getID()));
        std::cout << "reloaded="
                  << (again == getStats(*record) ? "same" : "different")
                  << '\n';
      }


      static std::string getStats(const cb::JSON::Value &v) {
        return v.has("gpu_stats") ? v.get("gpu_stats")->toString() : "none";
      }
    };
  }
}


int main(int argc, char *argv[]) {
  return cb::doApplication<FAH::Client::UnitLoadTest>(argc, argv);
}
