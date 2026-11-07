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

#include "GPUStats.h"

#include <cbang/String.h>
#include <cbang/json/JSON.h>
#include <cbang/hw/GPUMeasurement.h>

#include <cmath>

using namespace FAH::Client;
using namespace cb;
using namespace std;


namespace {
  // Time in each state shows fluctuations an average would hide
  bool isStepped(const string &name) {
    return name == "pstate" || name == "pcie_gen" || name == "pcie_width";
  }


  // Rates integrated over time into totals, e.g. watts into joules
  const struct {
    const char *name;
    double GPUMeasurement::*rate;
  } integrals[] = {
    {"energy_joules", &GPUMeasurement::power},
  };
}


void GPUStats::add(const GPUMeasurement &m, double weight) {
  if (weight <= 0) return;

  seconds += weight;
  samples++;

  if (m.gpuClock)            add("gpu_clock",  m.gpuClock,       weight);
  if (m.memClock)            add("mem_clock",  m.memClock,       weight);
  if (m.power)               add("power",      (int)lround(m.power), weight);
  if (m.temperature)         add("temp",       m.temperature,    weight);
  if (0 <= m.utilization)    add("gpu_util",   m.utilization,    weight);
  if (0 <= m.memUtilization) add("mem_util",   m.memUtilization, weight);
  if (0 <= m.pstate)         add("pstate",     m.pstate,         weight);
  if (m.pcieGen)             add("pcie_gen",   m.pcieGen,        weight);
  if (m.pcieWidth)           add("pcie_width", m.pcieWidth,      weight);

  for (unsigned i = 0; i < m.fans.size(); i++)
    add("fan" + String(i), m.fans[i], weight);

  for (auto &i: integrals)
    if (m.*i.rate) totals[i.name] += m.*i.rate * weight;
}


double GPUStats::getEnergyWh() const {
  auto it = totals.find("energy_joules");
  return it == totals.end() ? 0 : it->second / 3600;
}


double GPUStats::getTotal(const histogram_t &h) {
  double total = 0;
  for (auto &e: h) total += e.second;
  return total;
}


double GPUStats::getMean(const histogram_t &h) {
  double sum = 0;
  for (auto &e: h) sum += e.first * e.second;
  return sum / getTotal(h);
}


int GPUStats::getPercentile(const histogram_t &h, double p) {
  // The smallest value whose cumulative weight reaches p of the total.  The
  // tolerance keeps exact boundaries from being missed due to rounding.
  double target = p * getTotal(h) * (1 - 1e-9);
  double sum = 0;

  for (auto &e: h) {
    sum += e.second;
    if (target <= sum) return e.first;
  }

  return h.empty() ? 0 : h.rbegin()->first;
}


JSON::ValuePtr GPUStats::getSummary() const {
  JSON::ValuePtr d = new JSON::Dict;
  d->insert("seconds", seconds);
  d->insert("samples", samples);

  for (auto &p: histograms) {
    auto &h = p.second;
    if (h.empty()) continue;

    JSON::ValuePtr s = new JSON::Dict;

    if (isStepped(p.first)) {
      // Every state is kept, a rare one is what reveals a fluctuation
      double total = getTotal(h);
      for (auto &e: h) s->insert(String(e.first), e.second / total);

    } else {
      s->insert("min",    h.begin()->first);
      s->insert("max",    h.rbegin()->first);
      s->insert("mean",   getMean(h));
      s->insert("median", getPercentile(h, 0.5));
      s->insert("p95",    getPercentile(h, 0.95));
    }

    d->insert(p.first, s);
  }

  for (auto &p: totals) d->insert(p.first, p.second);

  if (totals.count("energy_joules")) d->insert("energy_wh", getEnergyWh());

  return d;
}


void GPUStats::read(const JSON::Value &value) {
  seconds = value.getNumber("seconds", 0);
  samples = value.getU64("samples", 0);
  histograms.clear();
  totals.clear();

  for (auto &i: integrals)
    if (value.hasNumber(i.name)) totals[i.name] = value.getNumber(i.name);

  for (auto it = value.begin(); it != value.end(); it++) {
    if (!(*it)->isDict()) continue;

    auto &h = histograms[it.key()];
    for (auto e = (*it)->begin(); e != (*it)->end(); e++)
      h[String::parseS32(e.key(), true)] = (*e)->getNumber();
  }
}


void GPUStats::write(JSON::Sink &sink) const {
  sink.beginDict();
  sink.insert("seconds", seconds);
  sink.insert("samples", samples);

  for (auto &p: histograms) {
    sink.insertDict(p.first, true);
    for (auto &e: p.second) sink.insert(String(e.first), e.second);
    sink.endDict();
  }

  for (auto &p: totals) sink.insert(p.first, p.second);

  sink.endDict();
}


void GPUStats::add(const string &name, int value, double weight) {
  histograms[name][value] += weight;
}
