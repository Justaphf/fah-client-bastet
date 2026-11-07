// Feeds weighted GPU telemetry samples read from stdin to
// FAH::Client::GPUStats and prints the summary, the sum of each stepped
// field's fractions, the checkpoint and whether the checkpoint restores the
// same summary.  With an "interval" the samples are timed instead and
// weighted as the client's sampler does; the weights are printed.  With
// "live" the energy after each sample and whether the last one equals the
// summary are printed too.

#include <fah/client/GPUStats.h>
#include <fah/client/GPUResources.h>

#include <cbang/String.h>
#include <cbang/Exception.h>
#include <cbang/hw/GPUMeasurement.h>
#include <cbang/json/JSON.h>
#include <cbang/json/BufferWriter.h>

#include <iostream>
#include <vector>

using namespace FAH::Client;
using namespace cb;
using namespace std;


int main(int argc, char *argv[]) {
  try {
    auto input = JSON::Reader(cin).parse();
    unsigned interval = input->getU32("interval", 0);
    bool live = input->getBoolean("live", false);
    double last = 0;
    vector<double> weights;
    vector<double> energy;
    GPUStats stats;

    for (auto &s: *input->get("samples")) {
      GPUMeasurement m;
      m.gpuClock       = s->getU32("gpu_clock",  0);
      m.memClock       = s->getU32("mem_clock",  0);
      m.power          = s->getNumber("power",   0);
      m.temperature    = s->getU32("temp",       0);
      m.utilization    = s->getS32("gpu_util",   -1);
      m.memUtilization = s->getS32("mem_util",   -1);
      m.pstate         = s->getS32("pstate",     -1);
      m.pcieGen        = s->getU32("pcie_gen",   0);
      m.pcieWidth      = s->getU32("pcie_width", 0);

      if (s->hasList("fans"))
        for (auto &fan: *s->get("fans")) m.fans.push_back(fan->getU32());

      double weight;
      if (interval) {
        double now = s->getNumber("time");
        weight = GPUResources::getSampleWeight(last, now, interval);
        last   = now;
        weights.push_back(weight);

      } else weight = s->getNumber("weight");

      stats.add(m, weight);
      if (live) energy.push_back(stats.getEnergyWh());
    }

    if (interval) {
      cout << "weights=";
      for (unsigned i = 0; i < weights.size(); i++)
        cout << (i ? " " : "") << String(weights[i]);
      cout << '\n';
    }

    auto summary = stats.getSummary();
    cout << "summary=" << *summary << '\n';

    if (live) {
      cout << "live=";
      for (unsigned i = 0; i < energy.size(); i++)
        cout << (i ? " " : "") << String(energy[i], 6);
      cout << '\n';

      // The live value at a save must equal the saved statistics
      bool same = !energy.empty() &&
        energy.back() == summary->getNumber("energy_wh", 0);
      cout << "live.saved=" << (same ? "same" : "different") << '\n';
    }

    for (auto name: {"pstate", "pcie_gen", "pcie_width"})
      if (summary->hasDict(name)) {
        double sum = 0;
        for (auto &f: *summary->get(name)) sum += f->getNumber();
        cout << name << ".sum=" << String(sum, 3) << '\n';
      }

    JSON::BufferWriter writer;
    stats.write(writer);
    writer.flush();
    cout << "checkpoint=" << writer.toString() << '\n';

    GPUStats restored;
    restored.read(*JSON::Reader::parse(writer.toString()));
    // The checkpoint holds doubles to the writer's precision
    bool same = restored.getSummary()->toString() == summary->toString();
    cout << "restored=" << (same ? "same" : "different") << '\n';

    return 0;

  } catch (const Exception &e) {cerr << e << '\n';}

  return 1;
}
