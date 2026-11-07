// Feeds timed, weighted GPU telemetry samples read from stdin to
// FAH::Client::GPUSeries and prints the series summary.  Before each sample
// index listed in "reload" the series is checkpointed and read back into a
// new object, as on a client restart.  Also prints whether that matches
// feeding the samples without reloads and whether a checkpoint reads back
// unchanged.  "limit" sets the series' bucket limit, "load" is a checkpoint
// read before the samples and "checkpoint" also prints the final checkpoint.

#include <fah/client/GPUSeries.h>

#include <cbang/Exception.h>
#include <cbang/hw/GPUMeasurement.h>
#include <cbang/json/JSON.h>
#include <cbang/json/BufferWriter.h>

#include <iostream>
#include <set>

using namespace FAH::Client;
using namespace cb;
using namespace std;


namespace {
  string toString(const GPUSeries &series, bool summary = true) {
    JSON::BufferWriter writer;
    if (summary) series.writeSummary(writer);
    else series.write(writer);
    writer.flush();
    return writer.toString();
  }


  GPUSeries reload(const GPUSeries &series, unsigned limit) {
    GPUSeries restored(series.getBucket(), limit);
    restored.read(*JSON::Reader::parse(toString(series, false)));
    return restored;
  }
}


int main(int argc, char *argv[]) {
  try {
    auto input = JSON::Reader(cin).parse();
    unsigned bucket = input->getU32("bucket");
    unsigned limit  = input->getU32("limit", GPUSeries::maxBuckets);
    GPUSeries series(bucket, limit);
    GPUSeries uninterrupted(bucket, limit);

    if (input->hasDict("load")) {
      series.read(*input->get("load"));
      uninterrupted.read(*input->get("load"));
    }

    set<unsigned> reloads;
    if (input->hasList("reload"))
      for (auto &i: *input->get("reload")) reloads.insert(i->getU32());

    auto &samples = *input->get("samples");
    for (unsigned i = 0; i < samples.size(); i++) {
      auto &s = *samples.get(i);

      GPUMeasurement m;
      m.gpuClock       = s.getU32("gpu_clock",  0);
      m.memClock       = s.getU32("mem_clock",  0);
      m.power          = s.getNumber("power",   0);
      m.temperature    = s.getU32("temp",       0);
      m.utilization    = s.getS32("gpu_util",   -1);
      m.memUtilization = s.getS32("mem_util",   -1);
      m.pstate         = s.getS32("pstate",     -1);
      m.pcieGen        = s.getU32("pcie_gen",   0);
      m.pcieWidth      = s.getU32("pcie_width", 0);

      if (s.hasList("fans"))
        for (auto &fan: *s.get("fans")) m.fans.push_back(fan->getU32());

      if (reloads.count(i)) series = reload(series, limit);

      series.add(s.getNumber("time"), m, s.getNumber("weight"));
      uninterrupted.add(s.getNumber("time"), m, s.getNumber("weight"));
    }

    if (input->hasList("coarsen"))
      for (auto &v: *input->get("coarsen")) {
        unsigned target = v->getU32();
        string before   = toString(series, false);

        series.coarsen(target);
        uninterrupted.coarsen(target);

        bool merged = toString(series, false) != before;
        cout << "coarsen=" << target << ' '
             << (merged ? "merged" : "unchanged") << '\n';
      }

    string result = toString(series);
    bool same     = result == toString(uninterrupted);
    bool restored = toString(reload(series, limit)) == result;

    cout << "series=" << result << '\n';
    if (input->getBoolean("checkpoint", false))
      cout << "checkpoint=" << toString(series, false) << '\n';
    cout << "uninterrupted=" << (same ? "same" : "different") << '\n';
    cout << "restored=" << (restored ? "same" : "different") << '\n';

    return 0;

  } catch (const Exception &e) {cerr << e << '\n';}

  return 1;
}
