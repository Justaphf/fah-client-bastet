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

#include "GPUTimeline.h"

#include <cbang/String.h>
#include <cbang/json/Sink.h>
#include <cbang/time/Time.h>

#include <algorithm>
#include <cmath>

using namespace FAH::Client;
using namespace cb;
using namespace std;


namespace {
  vector<string> getIDs(const string &tag) {
    vector<string> ids;

    for (size_t start = 0; !tag.empty();) {
      size_t end = tag.find(',', start);
      ids.push_back(tag.substr(start, end == string::npos ? end : end - start));
      if (end == string::npos) break;
      start = end + 1;
    }

    return ids;
  }
}


void GPUTimeline::Stats::add(const GPUSeries &series,
                             const GPUSeries::Bucket &b) {
  seconds += b.seconds;

  auto &names = series.getFields();
  for (unsigned i = 0; i < b.values.size() && i < names.size(); i++) {
    auto &r = b.values[i];
    if (!r.weight) continue;

    auto &f = fields[names[i]];
    f.range.add(r);
    f.means[(int)lround(r.sum / r.weight)] += r.weight;
  }

  auto &states = series.getStates();
  for (unsigned i = 0; i < b.states.size() && i < states.size(); i++)
    for (auto &e: b.states[i]) this->states[states[i]][e.first] += e.second;
}


void GPUTimeline::Stats::add(const Stats &stats) {
  seconds += stats.seconds;

  for (auto &p: stats.fields) {
    auto &f = fields[p.first];
    f.range.add(p.second.range);
    for (auto &e: p.second.means) f.means[e.first] += e.second;
  }

  for (auto &p: stats.states)
    for (auto &e: p.second) states[p.first][e.first] += e.second;
}


void GPUTimeline::Stats::write(JSON::Sink &sink) const {
  // As a unit's GPU statistics, but percentiles are of bucket means
  sink.beginDict();
  sink.insert("seconds", seconds);

  for (auto &p: fields) {
    auto &r = p.second.range;
    if (!r.weight) continue;

    sink.insertDict(p.first, true);
    sink.insert("min",    r.min);
    sink.insert("max",    r.max);
    sink.insert("mean",   r.sum / r.weight);
    sink.insert("median", GPUStats::getPercentile(p.second.means, 0.5));
    sink.insert("p95",    GPUStats::getPercentile(p.second.means, 0.95));
    sink.endDict();
  }

  for (auto &p: states) {
    double total = 0;
    for (auto &e: p.second) total += e.second;
    if (!total) continue;

    sink.insertDict(p.first, true);
    for (auto &e: p.second) sink.insert(String(e.first), e.second / total);
    sink.endDict();
  }

  auto power = fields.find("power");
  if (power != fields.end() && power->second.range.weight) {
    sink.insert("energy_joules", power->second.range.sum);
    sink.insert("energy_wh",     power->second.range.sum / 3600);
  }

  sink.endDict();
}


GPUTimeline::GPUTimeline(uint64_t begin, uint64_t end, unsigned bucket) :
  begin(begin), end(end), bucket(bucket), length(bucket), longest(bucket),
  rows(bucket, ~0U) {}


GPUTimeline::runs_t GPUTimeline::getRuns(const string &id) const {
  runs_t runs;

  auto it = units.find(id);
  if (it == units.end()) return runs;

  for (auto start: it->second)
    if (!runs.empty() && runs.back().second == start)
      runs.back().second = start + bucket;
    else runs.push_back(make_pair(start, start + bucket));

  return runs;
}


void GPUTimeline::add(const tags_t &tags) {
  for (auto &p: tags) {
    auto &series = p.second;
    unsigned len = series.getBucket();
    auto &buckets = series.getBuckets();
    auto ids = getIDs(p.first);
    bool added = false;

    for (auto it = buckets.lower_bound(begin < len ? 0 : begin - len + 1);
         it != buckets.end() && it->first < end; it++) {
      all.add(series, it->second);
      if (!ids.empty()) active.add(series, it->second);

      first = std::min(first, it->first);
      last  = std::max(last, it->first + len);

      uint64_t row = it->first - it->first % bucket;
      for (auto &id: ids) units[id].insert(row);
      added = true;
    }

    if (!added) continue;
    rows.add(series, begin, end);
    length  = getMultiple(length, len);
    longest = std::max(longest, len);
  }
}


void GPUTimeline::add(const GPUTimeline &timeline) {
  if (timeline.empty()) return;

  rows.add(timeline.rows);
  all.add(timeline.all);
  active.add(timeline.active);

  for (auto &p: timeline.units)
    units[p.first].insert(p.second.begin(), p.second.end());

  first   = std::min(first, timeline.first);
  last    = std::max(last, timeline.last);
  length  = getMultiple(length, timeline.length);
  longest = std::max(longest, timeline.longest);
}


void GPUTimeline::finish() {
  // Rows of a coarser stored bucket come out at a length dividing all,
  // unless that is more than four times the longest, then at the shortest
  // that divides a day, where a stored bucket counts in the row it starts in
  unsigned target = length;
  if ((uint64_t)longest * 4 < length) target = getDayMultiple(bucket, longest);
  if (target == bucket) return;
  bucket = target;
  rows.coarsen(bucket);

  for (auto &p: units) {
    set<uint64_t> starts;
    for (auto start: p.second) starts.insert(start - start % bucket);
    p.second.swap(starts);
  }
}


unsigned GPUTimeline::getMultiple(unsigned a, unsigned b) {
  // The least common multiple, unless too long, then the shortest multiple
  // of a at least the longer that divides a day
  if (!a || !b) return std::max(std::max(a, b), 1U);

  uint64_t x = a, y = b;
  while (y) {uint64_t t = x % y; x = y; y = t;}

  uint64_t lcm = a / x * b;
  if (lcm <= ~0U) return (unsigned)lcm;

  return getDayMultiple(a, std::max(a, b));
}


unsigned GPUTimeline::getDayMultiple(unsigned a, unsigned b) {
  // The shortest multiple of a at least b that divides a day, else the
  // shortest at least b that fits
  if (!a) return std::max(b, 1U);

  const uint64_t day = Time::SEC_PER_DAY;
  uint64_t next = ((uint64_t)b + a - 1) / a * a;
  if (!next) next = a;
  for (uint64_t d = next; d <= day; d += a)
    if (day % d == 0) return (unsigned)d;

  return (unsigned)(next <= ~0U ? next : ~0U / a * a);
}
