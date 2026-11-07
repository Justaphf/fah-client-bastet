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

#include "GPUSeries.h"

#include <cbang/String.h>
#include <cbang/Exception.h>
#include <cbang/json/JSON.h>
#include <cbang/hw/GPUMeasurement.h>

using namespace FAH::Client;
using namespace cb;
using namespace std;


void GPUSeries::Range::add(const Range &r) {
  if (!r.weight) return;
  if (!weight || r.min < min) min = r.min;
  if (!weight || max < r.max) max = r.max;
  sum    += r.sum;
  weight += r.weight;
}


void GPUSeries::Bucket::add(const Bucket &b) {
  seconds += b.seconds;

  if (values.size() < b.values.size()) values.resize(b.values.size());
  for (unsigned i = 0; i < b.values.size(); i++) values[i].add(b.values[i]);

  if (states.size() < b.states.size()) states.resize(b.states.size());
  for (unsigned i = 0; i < b.states.size(); i++)
    for (auto &e: b.states[i]) states[i][e.first] += e.second;
}


GPUSeries::GPUSeries(unsigned bucket, unsigned limit) :
  bucket(bucket), limit(limit ? limit : 1) {}


uint64_t GPUSeries::getEnd() const {
  return buckets.empty() ? 0 : buckets.rbegin()->first + bucket;
}


void GPUSeries::add(double time, const GPUMeasurement &m, double weight) {
  if (weight <= 0 || time < weight) return;

  // A sample stands for the time before it, which may span buckets
  for (double start = time - weight; start < time;) {
    uint64_t begin = (uint64_t)start - (uint64_t)start % bucket;
    double end     = std::min(time, (double)(begin + bucket));

    add(buckets[begin], m, end - start);
    start = end;
  }

  shrink();
}


void GPUSeries::coarsen(unsigned target) {
  // Only a whole multiple of the current length merges exactly
  if (bucket < target) merge((target + bucket - 1) / bucket * bucket);
}


int GPUSeries::getMode(const states_t &s) {
  if (s.empty()) return -1;

  // On a tie the lowest state wins
  auto mode = s.begin();

  for (auto it = s.begin(); it != s.end(); it++)
    if (mode->second < it->second) mode = it;

  return mode->first;
}


void GPUSeries::read(const JSON::Value &value) {
  bucket = value.getU32("bucket");
  if (!bucket) THROW("Invalid GPU series bucket length");

  fields.clear();
  states.clear();
  buckets.clear();

  for (auto &name: *value.get("fields")) fields.push_back(name->getString());
  for (auto &name: *value.get("states")) states.push_back(name->getString());

  for (auto &row: *value.get("rows")) {
    if (row->size() != 2 + fields.size() + states.size())
      THROW("Invalid GPU series row: " << row->toString(0, true));

    auto &b   = buckets[row->getU64(0)];
    b.seconds = row->getNumber(1);
    b.values.resize(fields.size());
    b.states.resize(states.size());
    unsigned col = 2;

    for (unsigned i = 0; i < fields.size(); i++) {
      auto &v = *row->get(col++);
      if (v.isNull()) continue;
      if (!v.isList() || v.size() != 4)
        THROW("Invalid GPU series row: " << row->toString(0, true));

      auto &r  = b.values[i];
      r.min    = v.getNumber(0);
      r.max    = v.getNumber(1);
      r.sum    = v.getNumber(2);
      r.weight = v.getNumber(3);
    }

    for (unsigned i = 0; i < states.size(); i++) {
      auto &v = *row->get(col++);
      if (v.isNull()) continue;
      if (!v.isDict())
        THROW("Invalid GPU series row: " << row->toString(0, true));

      for (auto e = v.begin(); e != v.end(); e++)
        b.states[i][String::parseS32(e.key(), true)] = (*e)->getNumber();
    }
  }

  shrink();
}


void GPUSeries::add(Bucket &b, const GPUMeasurement &m, double weight) {
  b.seconds += weight;

  if (m.gpuClock)            addValue(b, "gpu_clock", m.gpuClock,       weight);
  if (m.memClock)            addValue(b, "mem_clock", m.memClock,       weight);
  if (m.power)               addValue(b, "power",     m.power,          weight);
  if (m.temperature)         addValue(b, "temp",      m.temperature,    weight);
  if (0 <= m.utilization)    addValue(b, "gpu_util",  m.utilization,    weight);
  if (0 <= m.memUtilization) addValue(b, "mem_util",  m.memUtilization, weight);

  for (unsigned i = 0; i < m.fans.size(); i++)
    addValue(b, "fan" + String(i), m.fans[i], weight);

  if (0 <= m.pstate) addState(b, "pstate",     m.pstate,    weight);
  if (m.pcieGen)     addState(b, "pcie_gen",   m.pcieGen,   weight);
  if (m.pcieWidth)   addState(b, "pcie_width", m.pcieWidth, weight);
}


void GPUSeries::merge(unsigned length) {
  bucket = length;

  map<uint64_t, Bucket> merged;
  for (auto &p: buckets) merged[p.first - p.first % bucket].add(p.second);
  buckets.swap(merged);
}


void GPUSeries::shrink() {
  // Halving the resolution keeps a long unit's whole run in bounded space
  while (limit < buckets.size()) merge(bucket * 2);
}


void GPUSeries::write(JSON::Sink &sink, bool summary) const {
  sink.beginDict();
  sink.insert("bucket", bucket);

  sink.insertList("fields", true);
  for (auto &name: fields) sink.append(name);
  sink.endList();

  sink.insertList("states", true);
  for (auto &name: states) sink.append(name);
  sink.endList();

  // Rows rather than a dict per bucket keep a long WU small.  A summary row
  // has each field's [min, max, mean] and each state's mode, a checkpoint
  // the accumulators, so a reopened or merged bucket stays exact.
  sink.insertList("rows");

  for (auto &p: buckets) {
    auto &b = p.second;

    sink.appendList(true);
    sink.append(p.first);
    sink.append(b.seconds);

    for (unsigned i = 0; i < fields.size(); i++)
      if (i < b.values.size() && b.values[i].weight) {
        auto &r = b.values[i];
        sink.appendList(true);
        sink.append(r.min);
        sink.append(r.max);

        if (summary) sink.append(r.sum / r.weight);
        else {
          sink.append(r.sum);
          sink.append(r.weight);
        }

        sink.endList();

      } else sink.appendNull();

    for (unsigned i = 0; i < states.size(); i++)
      if (i < b.states.size() && !b.states[i].empty()) {
        if (summary) sink.append(getMode(b.states[i]));
        else {
          sink.appendDict(true);
          for (auto &e: b.states[i]) sink.insert(String(e.first), e.second);
          sink.endDict();
        }

      } else sink.appendNull();

    sink.endList();
  }

  sink.endList();
  sink.endDict();
}


unsigned GPUSeries::getColumn(vector<string> &names, const string &name) {
  unsigned i = 0;
  while (i < names.size() && names[i] != name) i++;
  if (i == names.size()) names.push_back(name);
  return i;
}


void GPUSeries::addValue(Bucket &b, const string &name, double value,
                         double weight) {
  unsigned i = getColumn(fields, name);
  if (b.values.size() <= i) b.values.resize(i + 1);
  b.values[i].add({value, value, value * weight, weight});
}


void GPUSeries::addState(Bucket &b, const string &name, int value,
                         double weight) {
  unsigned i = getColumn(states, name);
  if (b.states.size() <= i) b.states.resize(i + 1);
  b.states[i][value] += weight;
}
