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

#include "GPUHistory.h"

#include "App.h"
#include "Config.h"
#include "Units.h"
#include "Unit.h"

#include <cbang/Catch.h>
#include <cbang/String.h>
#include <cbang/log/Logger.h>
#include <cbang/json/JSON.h>
#include <cbang/json/Builder.h>
#include <cbang/json/BufferWriter.h>
#include <cbang/time/Time.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace FAH::Client;
using namespace cb;
using namespace std;


namespace {
  const unsigned saveDelay  = 5 * Time::SEC_PER_MIN;
  const unsigned saveMin    = 60; // Between saves for units
  const unsigned retryDelay = 60;
  const unsigned coarsenMax = 8;  // Days coarsened per prune
  const unsigned repairMax  = 8;  // Days indexed per prune
  const unsigned repairStep = 2;  // Days indexed per periodic save
  const unsigned cacheMax   = 8;  // Decoded stored days

  const unsigned pointsDefault = 1500; // Rows of a range
  const unsigned pointsMin     = 10;
  const unsigned pointsMax     = 5000;
  const unsigned fullMax       = 2 * Time::SEC_PER_DAY; // Range read in full
  const unsigned buildMax      = 8; // Rollups built per range
  const unsigned viewMax       = 2; // Kept per GPU
  const uint64_t rangeMax      = 366 * Time::SEC_PER_DAY; // Newest kept

  const char *chunksTable = "gpu_history";
  const char *unitsTable  = "gpu_history_units";
  const char *daysTable   = "gpu_history_days";
  const char *rollupTable = "gpu_history_rollup";
  const char *completeKey = "complete"; // Every chunk has metadata

  const char *labelPaths[][2] = { // Index label, unit path
    {"number",  "number"},
    {"group",   "group"},
    {"project", "assignment.project"},
    {"run",     "wu.run"},
    {"clone",   "wu.clone"},
    {"gen",     "wu.gen"},
  };


  string toJSON(const function<void (JSON::Sink &sink)> &cb) {
    JSON::BufferWriter writer(0, true);
    cb(writer);
    writer.flush();
    return writer.toString();
  }


  // Newest day first
  void sortDue(vector<pair<int64_t, string>> &due) {
    sort(due.begin(), due.end(),
         [] (const pair<int64_t, string> &a, const pair<int64_t, string> &b) {
           return a.first != b.first ? b.first < a.first : a.second < b.second;
         });
  }
}


void GPUHistory::Meta::read(const JSON::Value &value) {
  bucket  = value.getU32("bucket", 0);
  rollup  = value.getBoolean("rollup", false);
  hasTags = value.hasList("tags");

  tags.clear();
  if (hasTags)
    for (auto &tag: *value.get("tags")) tags.insert(tag->getString());
}


void GPUHistory::Meta::write(JSON::Sink &sink) const {
  sink.beginDict();
  sink.insert("bucket", bucket);
  sink.insertBoolean("rollup", rollup);

  sink.insertList("tags", true);
  for (auto &tag: tags) sink.append(tag);
  sink.endList();

  sink.endDict();
}


void GPUHistory::Entry::read(const JSON::Value &value) {
  gpus.clear();
  for (auto &gpu: *value.get("gpus")) gpus.insert(gpu->getString());

  first  = value.getU64("first");
  last   = value.getU64("last");
  finish = value.hasNumber("finish") ? value.getU64("finish") : 0;
  result = value.hasString("result") ? value.getString("result") : "";
  labels = new JSON::Dict;

  for (auto &l: labelPaths)
    if (value.has(l[0])) labels->insert(l[0], value.get(l[0])->copy(true));
}


void GPUHistory::Entry::write(JSON::Sink &sink) const {
  sink.beginDict();

  sink.insertList("gpus", true);
  for (auto &gpu: gpus) sink.append(gpu);
  sink.endList();

  sink.insert("first", first);
  sink.insert("last",  last);

  // Null while the unit is in the client
  if (finish) sink.insert("finish", finish);
  else sink.insertNull("finish");

  if (finish && !result.empty()) sink.insert("result", result);
  else sink.insertNull("result");

  if (labels.isSet())
    for (auto it = labels->begin(); it != labels->end(); it++)
      sink.insert(it.key(), **it);

  sink.endDict();
}


GPUHistory::GPUHistory(App &app) : app(app) {}


GPUHistory::~GPUHistory() {
  // The DB outlives the history but the config does not, so no pruning
  TRY_CATCH_ERROR(flush());
}


JSON::ValuePtr GPUHistory::get(const string &id) {
  try {
    return getSeries(id);
  } CATCH_ERROR;

  return JSON::Null::instancePtr();
}


JSON::ValuePtr GPUHistory::get(const string &gpu, double from, double to,
                               double points) {
  try {
    double now = this->now();
    if (!isfinite(from) || !isfinite(to) || from < 0 || to <= from ||
        now <= from) return JSON::Null::instancePtr();

    unsigned n = pointsDefault;
    if (isfinite(points) && 0 < points)
      n = (unsigned)std::min(std::max(points, (double)pointsMin),
                             (double)pointsMax);

    return getRange(gpu, (uint64_t)from, (uint64_t)ceil(std::min(to, now)), n);

  } catch (const std::exception &e) {
    LOG_ERROR("Failed to read GPU history of " << gpu << ": " << e.what());
    reset();
  }

  return JSON::Null::instancePtr();
}


void GPUHistory::getDays(const string &gpu, uint64_t first, uint64_t last,
                         bool rollup, const day_cb_t &cb) {
  TRY_CATCH_ERROR(readDays(gpu, first, last, rollup, cb));
}


void GPUHistory::save() {
  // Units save often, the periodic save covers the rest
  uint64_t now = this->now();
  if (now < lastFlush) lastFlush = now; // Clock set back
  if (lastFlush + saveMin <= now) flush();
}


void GPUHistory::end(const string &id, const string &result) {
  TRY_CATCH_ERROR(finish(id, result));
}


unsigned GPUHistory::getDayBucket(unsigned length, unsigned multiple) {
  // The shortest multiple at least as long that divides a day, so a bucket
  // never crosses midnight
  if (!multiple) multiple = 1;
  unsigned first = (length + multiple - 1) / multiple * multiple;
  if (!first) first = multiple;

  for (unsigned d = first; d <= Time::SEC_PER_DAY; d += multiple)
    if (Time::SEC_PER_DAY % d == 0) return d;

  return first;
}


void GPUHistory::sample(const GPUSample &s) {TRY_CATCH_ERROR(record(s));}
uint64_t GPUHistory::now() const {return Time::now();}


void GPUHistory::record(const GPUSample &s) {
  uint64_t now = this->now();
  if (!lastFlush || now < lastFlush) lastFlush = now;

  // Also covers idle time, which no unit saves
  if (dirty &&
      lastFlush + (saveFailed ? retryDelay : saveDelay) <= now && flush() &&
      loaded)
    try {
      repair(repairStep);
    } catch (const std::exception &e) {
      LOG_WARNING("Failed to index GPU history: " << e.what());
      reset();
    }

  auto &config = *app.getConfig();
  if (!config.getGPUTelemetryHistory()) return;
  if (s.units.empty() && !config.getGPUTelemetryContinuous()) return;
  if (s.weightSeconds <= 0 || s.time < s.weightSeconds || !load()) return;

  int64_t today = now / Time::SEC_PER_DAY;
  if (lastPrune != today) prune();

  int64_t oldest = today - config.getGPUTelemetryHistoryDays();
  string tag     = String::join(s.units, ",");
  double start   = s.time - s.weightSeconds;
  if (s.time <= (double)oldest * Time::SEC_PER_DAY) return;

  // Entries first, so no sample is stored without one.  If one cannot be
  // read now, the sample is dropped as for an unreadable day, and so are
  // the entries made for it.
  vector<string> created;
  auto drop = [&] {for (auto &id: created) entries.erase(id);};

  vector<Entry *> found;
  for (auto &id: s.units) {
    if (!entries.count(id)) created.push_back(id);
    auto entry = getEntry(id);
    if (!entry) return drop();
    found.push_back(entry);
  }

  // A sample stands for the time before it.  Buckets divide a day, so
  // splitting at UTC midnight keeps each bucket in the day it starts.
  double first = -1, last = 0;
  for (double t = start; t < s.time;) {
    uint64_t day = (uint64_t)t / Time::SEC_PER_DAY;
    double end   = std::min(s.time, (double)((day + 1) * Time::SEC_PER_DAY));

    if (oldest <= (int64_t)day && add(s, day, tag, end, end - t)) {
      if (first < 0) first = t;
      last = end;
    }

    t = end;
  }

  if (first < 0) return drop(); // Older than the history or unreadable

  uint64_t begin = (uint64_t)first;
  uint64_t end   = (uint64_t)ceil(last);

  for (auto entry: found) {
    entry->gpus.insert(s.device);
    if (!entry->first || begin < entry->first) entry->first = begin;
    if (entry->last < end) entry->last = end;
    entry->dirty = true;
    dirty = true;
  }
}


void GPUHistory::finish(const string &id, const string &result) {
  // A unit without samples, e.g. on the CPU, leaves the history untouched.
  // With the history off and not loaded, an earlier entry gets its finish
  // from the WU log.
  if (!entries.count(id) && !finishes.count(id)) {
    if (!loaded && !app.getConfig()->getGPUTelemetryHistory()) return;
    Entry entry;
    if (!readEntry(id, entry)) return;
  }

  // Only a finish sets these, a pause leaves a gap.  Kept until the entry
  // can be read.
  auto &done  = finishes[id];
  done.finish = now();
  done.result = result;
  label(done, id);
  dirty = true;

  flush();
  if (load()) prune();
}


JSON::ValuePtr GPUHistory::getSeries(const string &id) {
  Entry entry;
  if (id.empty() || !open() || !findEntry(id, entry))
    return JSON::Null::instancePtr();

  map<string, GPUSeries> devices; // By GPU ID

  for (auto &gpu: entry.gpus) {
    vector<GPUSeries> parts;

    for (auto &key: getKeys(gpu, entry.first / Time::SEC_PER_DAY,
                            entry.last / Time::SEC_PER_DAY)) {
      // Unsaved chunks are newer than stored ones
      const tags_t *tags = 0;
      auto it = chunks.find(key);

      if (it != chunks.end()) tags = &it->second.tags;
      else {
        auto &meta = days[key];
        if (meta.hasTags && none_of(meta.tags.begin(), meta.tags.end(),
                                    [&] (const string &tag) {
                                      return hasUnit(tag, id);
                                    })) continue;

        if (!(tags = getStored(key))) continue;
      }

      for (auto &p: *tags)
        if (hasUnit(p.first, id) && !p.second.empty())
          parts.push_back(p.second);
    }

    if (parts.empty()) continue;

    // In time order, so a field's column is where it first appeared
    stable_sort(parts.begin(), parts.end(),
                [] (const GPUSeries &a, const GPUSeries &b) {
                  return a.getStart() < b.getStart();
                });

    // Chunks of different age may differ in bucket length
    unsigned bucket = parts[0].getBucket();
    for (auto &p: parts)
      bucket = GPUTimeline::getMultiple(bucket, p.getBucket());

    auto &series = devices.insert(make_pair(gpu, GPUSeries(bucket))).first
      ->second;
    for (auto &p: parts) series.add(p);
  }

  if (devices.empty()) return JSON::Null::instancePtr();

  return JSON::build(
    [&] (JSON::Sink &sink) {
      sink.beginDict();

      for (auto &p: devices) {
        sink.beginInsert(p.first);
        p.second.writeSummary(sink);
      }

      sink.endDict();
    });
}


JSON::ValuePtr GPUHistory::getRange(const string &gpu, uint64_t from,
                                    uint64_t to, unsigned points) {
  if (gpu.empty() || !open()) return JSON::Null::instancePtr();

  string prefix = gpu + "/";
  auto isGPU = [&] (const string &key) {
    return !key.compare(0, prefix.size(), prefix);
  };

  auto d = days.lower_bound(prefix);
  auto c = chunks.lower_bound(prefix);
  if (!(d != days.end() && isGPU(d->first)) &&
      !(c != chunks.end() && isGPU(c->first)))
    return JSON::Null::instancePtr();

  auto &config    = *app.getConfig();
  uint64_t now    = this->now();
  uint64_t today  = now / Time::SEC_PER_DAY;
  uint64_t keep   = config.getGPUTelemetryHistoryDays();
  uint64_t oldest = (keep < today ? today - keep : 0) * Time::SEC_PER_DAY;

  to   = std::min(to, now);
  from = std::max(std::max(from, oldest), to < rangeMax ? 0 : to - rangeMax);
  if (to <= from) return JSON::Null::instancePtr();

  // Day rollups unless the range is short and its rows finer than them
  uint64_t span = to - from;
  uint64_t need = (span + points - 2) / (points - 1);

  auto getLength = [&] (unsigned base) {
    const uint64_t day = Time::SEC_PER_DAY;
    if (day < need) return (unsigned)((need + day - 1) / day * day);
    return getDayBucket(std::max((unsigned)need, base), base);
  };

  unsigned base   = config.getGPUTelemetryBucket();
  unsigned bucket = getLength(base);
  bool full       = span <= fullMax && bucket < rollupBucket;
  if (!full) bucket = getLength(getDayBucket(rollupBucket, base));

  // Whole stored days are kept, edge and unsaved days read every time
  vector<string> inside;
  vector<string> edges;

  for (auto &key: getKeys(gpu, from / Time::SEC_PER_DAY,
                          (to - 1) / Time::SEC_PER_DAY)) {
    int64_t day = getDay(key);
    if (day < 0) continue;

    uint64_t start = day * Time::SEC_PER_DAY;
    if (!chunks.count(key) && from <= start &&
        start + Time::SEC_PER_DAY <= to) inside.push_back(key);
    else edges.push_back(key);
  }

  // Days not read now are reported missing and read with the next request
  unsigned builds = buildMax;
  bool missing    = false;

  // Two views per GPU, so alternating row lengths do not rebuild them.  A
  // view grows by the days it lacks.
  auto &recent = views[gpu];
  auto it = find_if(recent.begin(), recent.end(), [&] (const View &v) {
    return v.full == full && v.bucket == bucket;
  });

  if (it != recent.end()) recent.splice(recent.begin(), recent, it);
  else {
    recent.push_front(View());
    if (viewMax < recent.size()) recent.pop_back();
  }

  auto &view = recent.front();
  bool valid = view.full == full && view.bucket == bucket;
  for (auto &key: view.keys)
    if (!valid) break;
    else valid = binary_search(inside.begin(), inside.end(), key) &&
           days[key].changed <= view.changes;

  if (!valid) {
    view.full     = full;
    view.bucket   = bucket;
    view.keys.clear();
    view.timeline = GPUTimeline(0, ~(uint64_t)0, bucket);
  }

  vector<string> added;
  for (auto &key: inside) {
    if (binary_search(view.keys.begin(), view.keys.end(), key)) continue;

    tags_t buffer;
    auto tags = readRange(key, full, buffer, builds);
    if (!tags) missing = true;
    else {
      view.timeline.add(*tags);
      added.push_back(key);
    }
  }

  if (!added.empty()) {
    view.keys.insert(view.keys.end(), added.begin(), added.end());
    sort(view.keys.begin(), view.keys.end());
  }
  view.changes = changes;

  GPUTimeline timeline(from, to, bucket);
  timeline.add(view.timeline);

  for (auto &key: edges) {
    tags_t buffer;
    auto tags = readRange(key, full, buffer, builds);
    if (tags) timeline.add(*tags);
    else missing = true;
  }

  timeline.finish();

  // Widened to the stored buckets counted
  if (!timeline.empty()) {
    from = std::min(from, timeline.getFirst());
    to   = std::min(std::max(to, timeline.getLast()), now);
  }

  auto rows = JSON::build(
    [&] (JSON::Sink &sink) {timeline.getRows().writeSummary(sink);});

  return JSON::build(
    [&] (JSON::Sink &sink) {
      sink.beginDict();
      sink.insert("from", from);
      sink.insert("to",   to);
      if (missing) sink.insertBoolean("incomplete", true);

      for (auto it = rows->begin(); it != rows->end(); it++)
        sink.insert(it.key(), **it);

      writeUnits(sink, gpu, from, to, timeline);

      sink.insertDict("stats");
      sink.beginInsert("all");
      timeline.getAll().write(sink);
      sink.beginInsert("active");
      timeline.getActive().write(sink);
      sink.endDict();

      sink.endDict();
    });
}


const GPUHistory::tags_t *GPUHistory::readRange(const string &key, bool full,
                                                tags_t &buffer,
                                                unsigned &builds) {
  // Unsaved chunks are newer than stored ones
  auto it = chunks.find(key);
  if (it != chunks.end()) {
    if (full) return &it->second.tags;
    buffer = getRollup(it->second.tags);
    return &buffer;
  }

  if (full) return getStored(key);

  auto &meta  = days[key];
  bool rollup = meta.rollup;
  if (rollup && readTags(rollupTable, key, buffer) && !buffer.empty())
    return &buffer;

  // A day that coarse is its own rollup, a missing one is built and stored
  // for a few days per request
  bool coarse = meta.hasTags && rollupBucket <= meta.bucket;
  auto cached = cache.find(key);

  if (cached != cache.end()) buffer = cached->second.tags;
  else if (!coarse && !builds) return 0;
  else if (!readTags(chunksTable, key, buffer)) return 0;
  else if (!coarse) {
    builds--;
    complete(key, buffer, rollup);
  }

  buffer = getRollup(buffer);
  return &buffer;
}


void GPUHistory::writeUnits(JSON::Sink &sink, const string &gpu,
                            uint64_t from, uint64_t to,
                            const GPUTimeline &timeline) {
  readIndex();

  map<string, const Entry *> found;
  for (auto &p: index) found[p.first] = &p.second;
  for (auto &p: entries) found[p.first] = &p.second;

  uint64_t now = this->now();
  vector<pair<uint64_t, string>> order;
  map<string, uint64_t> ends; // 0 while in the client

  for (auto &p: found) {
    auto &entry = *p.second;
    if (!entry.gpus.count(gpu)) continue;

    // A unit gone without a finish ended after its last sample
    uint64_t end = entry.finish;
    if (!end && app.getUnits()->findUnit(p.first).isNull()) end = entry.last;

    // Listed while any of its samples lies in the range
    if (entry.first < to && from <= std::max(entry.last, end ? end : now)) {
      order.push_back(make_pair(entry.first, p.first));
      ends[p.first] = end;
    }
  }

  sort(order.begin(), order.end());

  sink.insertList("units");

  for (auto &o: order) {
    auto &id    = o.second;
    auto &entry = *found[id];
    uint64_t end = ends[id];

    sink.appendDict();
    sink.insert("id", id);
    sink.insert("start", entry.first);

    if (end) sink.insert("end", end);
    else {
      sink.insertNull("end");
      end = now;
    }

    if (entry.finish && !entry.result.empty())
      sink.insert("result", entry.result);
    else sink.insertNull("result");

    // Coarse rows may reach past the unit's samples and the range
    sink.insertList("runs");
    for (auto &run: timeline.getRuns(id)) {
      uint64_t start = std::max(std::max(run.first, entry.first), from);
      uint64_t stop  =
        std::min(std::min(std::min(run.second, end), entry.last), to);
      if (stop <= start) continue;

      sink.appendList(true);
      sink.append(start);
      sink.append(stop);
      sink.endList();
    }
    sink.endList();

    if (entry.labels.isSet())
      for (auto it = entry.labels->begin(); it != entry.labels->end(); it++)
        sink.insert(it.key(), **it);

    sink.endDict();
  }

  sink.endList();
}


void GPUHistory::readIndex() {
  if (indexed) return;

  map<string, Entry> found;
  app.getDB(unitsTable).foreach(
    [&] (const string &id, const string &value) {
      try {
        Entry entry;
        entry.read(*JSON::parse(value));
        found[id] = entry;
      } CATCH_DEBUG(3);
    }, ~0U);

  index.swap(found);
  indexed = true;
}


void GPUHistory::readDays(const string &gpu, uint64_t first, uint64_t last,
                          bool rollup, const day_cb_t &cb) {
  if (!load()) return;
  unsigned builds = buildMax;

  for (auto &key: getKeys(gpu, first, last)) {
    int64_t day = getDay(key);
    if (day < 0) continue;

    auto it = chunks.find(key);
    if (it != chunks.end()) {
      if (rollup) cb(day, getRollup(it->second.tags));
      else cb(day, it->second.tags);
      continue;
    }

    tags_t tags;
    if (rollup && days[key].rollup && readTags(rollupTable, key, tags) &&
        !tags.empty()) {
      cb(day, tags);
      continue;
    }

    // Long ranges should not displace the cache
    const tags_t *stored = 0;
    auto cached = cache.find(key);
    if (cached != cache.end()) stored = &cached->second.tags;
    else if (!rollup) stored = getStored(key);
    else if (readTags(chunksTable, key, tags)) {
      // Stores a missing rollup, for a few days per call
      if (builds && getBucket(tags) < rollupBucket) {
        builds--;
        complete(key, tags, days[key].rollup);
      }
      stored = &tags;
    }
    if (!stored) continue;

    if (rollup) cb(day, getRollup(*stored));
    else cb(day, *stored);
  }
}


set<string> GPUHistory::getKeys(const string &gpu, uint64_t first,
                                uint64_t last) const {
  string begin = getKey(gpu, first);
  string end   = getKey(gpu, last);
  std::set<string> keys;

  for (auto it = days.lower_bound(begin);
       it != days.end() && it->first <= end; it++) keys.insert(it->first);
  for (auto it = chunks.lower_bound(begin);
       it != chunks.end() && it->first <= end; it++) keys.insert(it->first);

  return keys;
}


bool GPUHistory::load() {
  if (loaded) return true;

  try {
    bool complete = false;
    map<string, Meta> found;

    app.getDB(daysTable).foreach(
      [&] (const string &key, const string &value) {
        if (key == completeKey) complete = true;
        else
          try {
            found[key].read(*JSON::parse(value));
          } catch (const std::exception &) {found[key] = Meta();}
      }, ~0U);

    // Chunks written by another program are indexed later
    if (!complete) {
      vector<string> missing;
      app.getDB(chunksTable).foreach(
        [&] (const string &key, const string &) {
          if (!found.count(key) && 0 <= getDay(key)) missing.push_back(key);
        }, ~0U);

      auto &db = app.getDB(daysTable);
      for (auto &key: missing) {
        db.set(key, string("{}"));
        found[key] = Meta();
      }

      db.set(completeKey, string("true"));
      if (!missing.empty())
        LOG_INFO(3, "Found " << missing.size() << " GPU history days");
    }

    days.swap(found);
    loaded = true;

  } catch (const std::exception &e) {
    if (!loadFailed) LOG_ERROR("Failed to load GPU history: " << e.what());
    loadFailed = true;
    reset();
  }

  return loaded;
}


bool GPUHistory::open() {
  if (!load()) return false;

  // Finishes lost in a crash, also when not recording.  Retention and aging
  // are left to recording.
  uint64_t now  = this->now();
  int64_t today = now / Time::SEC_PER_DAY;
  if (now < recoverFailed) recoverFailed = 0; // Clock set back

  if (recovered != today && !days.empty() &&
      (!recoverFailed || recoverFailed + retryDelay <= now)) {
    recoverFailed = now;

    try {
      if (recover()) {
        recovered     = today;
        recoverFailed = 0;
      }
    } catch (const std::exception &e) {
      LOG_WARNING("Failed to recover GPU history finishes: " << e.what());
      reset();
    }
  }

  return true;
}


bool GPUHistory::flush() {
  if (!dirty) return true;
  lastFlush = now();

  // What fails stays pending and is retried, the rest is written
  bool failed = false;
  auto fail = [&] (const std::exception &e) {
    if (!saveFailed && !failed)
      LOG_WARNING("Failed to save GPU history: " << e.what());
    failed = true;
    reset();
  };

  for (auto it = finishes.begin(); it != finishes.end();) {
    auto e = entries.find(it->first);

    if (e == entries.end()) {
      Entry entry;
      int found = readEntry(it->first, entry);
      if (found < 0) {failed = true; it++; continue;}
      if (!found) {it = finishes.erase(it); continue;} // No samples
      e = entries.insert(make_pair(it->first, entry)).first;
    }

    auto &done   = it->second;
    auto &entry  = e->second;
    entry.finish = std::max(done.finish, entry.last); // Not before samples
    entry.result = done.result;
    if (done.labels.isSet()) entry.labels = done.labels;
    entry.dirty  = true;
    it = finishes.erase(it);
  }

  // Entries first, so stored samples always have a current one
  for (auto &p: entries)
    if (p.second.dirty)
      try {
        auto &entry = p.second;
        app.getDB(unitsTable).set(p.first, toJSON(
          [&] (JSON::Sink &sink) {entry.write(sink);}));
        entry.dirty = false;
        if (indexed) index[p.first] = entry;
      } catch (const std::exception &e) {fail(e);}

  // A chunk waits for the entries of its units
  auto isHeld = [&] (const tags_t &tags) {
    for (auto &e: entries)
      if (e.second.dirty)
        for (auto &p: tags)
          if (hasUnit(p.first, e.first)) return true;
    return false;
  };

  for (auto &p: chunks)
    if (p.second.dirty) {
      if (isHeld(p.second.tags)) {failed = true; continue;}

      try {
        store(p.first, p.second.tags);
        p.second.dirty = false;
      } catch (const std::exception &e) {fail(e);}
    }

  saveFailed = failed;
  if (!failed) dirty = false;

  // Only what new samples continue stays in memory
  for (auto it = chunks.begin(); it != chunks.end();) {
    auto c = current.find(it->first.substr(0, it->first.rfind('/')));
    if (!it->second.dirty && (c == current.end() || c->second != it->first))
      it = chunks.erase(it);
    else it++;
  }

  for (auto it = entries.begin(); it != entries.end();)
    if (!it->second.dirty && it->second.finish) it = entries.erase(it);
    else it++;

  return !failed;
}


void GPUHistory::store(const string &key, const tags_t &tags, bool chunk) {
  Meta meta;
  meta.bucket  = getBucket(tags);
  meta.rollup  = meta.bucket < rollupBucket;
  meta.hasTags = true;
  for (auto &p: tags) meta.tags.insert(p.first);

  // Unknown until all is written, so the next start never misses a stored
  // chunk and an interrupted write is indexed again from what was stored
  auto &unknown   = days[key];
  unknown         = Meta();
  unknown.changed = ++changes;
  cache.erase(key);

  auto &daysDB = app.getDB(daysTable);
  daysDB.set(key, string("{}"));

  if (chunk)
    app.getDB(chunksTable)
      .set(key, toJSON([&] (JSON::Sink &sink) {write(tags, sink);}));

  auto &rollupDB = app.getDB(rollupTable);
  if (!meta.rollup) rollupDB.unset(key);
  else {
    auto series = getRollup(tags);
    rollupDB.set(key, toJSON([&] (JSON::Sink &sink) {write(series, sink);}));
  }

  daysDB.set(key, toJSON([&] (JSON::Sink &sink) {meta.write(sink);}));
  meta.changed = ++changes;
  days[key]    = meta;
}


void GPUHistory::prune() {
  int64_t today = now() / Time::SEC_PER_DAY;
  lastPrune     = today;

  try {
    expire(today);
    coarsen(today);
    repair(repairMax);

  } catch (const std::exception &e) {
    LOG_ERROR("Failed to prune GPU history: " << e.what());
    reset();
  }
}


void GPUHistory::expire(int64_t today) {
  // One boundary, the start of the oldest day kept, for days and units
  unsigned keep   = app.getConfig()->getGPUTelemetryHistoryDays();
  int64_t oldest  = today - keep;
  uint64_t cutoff = 0 < oldest ? oldest * Time::SEC_PER_DAY : 0;
  bool full       = scanned != (int64_t)cutoff;
  scanned         = -1; // Until complete

  // Before any read, which may fail with the index.  A unit's samples are
  // taken before its finish, so they end by the second after it.
  for (auto it = finishes.begin(); it != finishes.end();)
    if (it->second.finish < cutoff) it = finishes.erase(it);
    else it++;

  auto isOld = [&] (const string &key) {
    int64_t day = getDay(key);
    return 0 <= day && day < oldest;
  };

  auto &chunksDB = app.getDB(chunksTable);
  auto &rollupDB = app.getDB(rollupTable);
  auto &daysDB   = app.getDB(daysTable);

  for (auto it = days.begin(); it != days.end();)
    if (isOld(it->first)) {
      chunksDB.unset(it->first);
      rollupDB.unset(it->first);
      daysDB.unset(it->first); // Last, so an interrupted prune is redone
      cache.erase(it->first);
      it = days.erase(it);

    } else it++;

  for (auto it = chunks.begin(); it != chunks.end();)
    if (isOld(it->first)) it = chunks.erase(it);
    else it++;

  auto &unitsDB = app.getDB(unitsTable);
  vector<string> expired;
  map<string, Entry> open;
  set<string> check;
  check.swap(watched);

  auto scan = [&] (const string &id, const string &value) {
    if (entries.count(id)) return;

    try {
      Entry entry;
      entry.read(*JSON::parse(value));

      if (isOpen(id, entry)) open[id] = entry;
      else if (isExpired(id, entry, cutoff)) expired.push_back(id);
      else if (!entry.finish && app.getUnits()->findUnit(id).isSet())
        watched.insert(id);
    } CATCH_DEBUG(3);
  };

  // Read in full when the cutoff changed or the last pass failed.  Else only
  // an entry whose unit was in the client can newly expire or be open.
  if (full) unitsDB.foreach(scan, ~0U);
  else
    for (auto &id: check)
      if (entries.count(id)) watched.insert(id);
      else if (unitsDB.has(id)) scan(id, unitsDB.getString(id));

  for (auto it = entries.begin(); it != entries.end();)
    if (isExpired(it->first, it->second, cutoff)) {
      expired.push_back(it->first);
      it = entries.erase(it);

    } else it++;

  for (auto &id: expired) {
    unitsDB.unset(id);
    index.erase(id);
  }

  if (finishLogged(open, cutoff, true)) {
    scanned = cutoff;
    if (full) recovered = today; // Did what recover() does
  }
}


bool GPUHistory::recover() {
  // As in expire(), but nothing is removed
  map<string, Entry> open;

  app.getDB(unitsTable).foreach(
    [&] (const string &id, const string &value) {
      if (entries.count(id)) return;

      try {
        Entry entry;
        entry.read(*JSON::parse(value));
        if (isOpen(id, entry)) open[id] = entry;
      } CATCH_DEBUG(3);
    }, ~0U);

  return finishLogged(open, 0, false);
}


bool GPUHistory::finishLogged(map<string, Entry> &open, uint64_t cutoff,
                              bool remove) {
  // The WU log has their finish, looked up once per start.  Expired ones
  // are removed only if asked.  False if the WU log failed.
  auto &unitsDB = app.getDB(unitsTable);

  for (auto &p: open) {
    auto &id    = p.first;
    auto &entry = p.second;

    int found = readLog(id, entry);
    if (found < 0) return false;
    if (!found) unlogged.insert(id);

    if (remove && isExpired(id, entry, cutoff)) {
      unitsDB.unset(id);
      index.erase(id);

    } else if (found) {
      unitsDB.set(id, toJSON(
        [&] (JSON::Sink &sink) {entry.write(sink);}));
      if (indexed) index[id] = entry;
    }
  }

  return true;
}


bool GPUHistory::isOpen(const string &id, const Entry &entry) const {
  // Gone without a finish, e.g. lost in a crash
  return !entry.finish && !finishes.count(id) && !unlogged.count(id) &&
    app.getUnits()->findUnit(id).isNull();
}


bool GPUHistory::isExpired(const string &id, const Entry &entry,
                           uint64_t cutoff) const {
  // Units still in the client are kept.  Its samples end by its last, which
  // a stored finish may precede.
  uint64_t time = std::max(entry.finish, entry.last);
  return time <= cutoff &&
    (entry.finish || app.getUnits()->findUnit(id).isNull());
}


void GPUHistory::coarsen(int64_t today) {
  auto tiers = app.getConfig()->getGPUTelemetryAging();
  if (tiers.empty()) return;

  // A tier applies once the day is that many whole days old
  auto getTarget = [&] (int64_t age) {
    unsigned target = 0;
    for (auto &tier: tiers)
      if ((int64_t)tier.first < age) target = std::max(target, tier.second);
    return target;
  };

  // Newest first, so a restart does not hold back newly due days
  vector<pair<int64_t, string>> due;
  for (auto &p: days) {
    int64_t day = getDay(p.first);
    if (day < 0 || !p.second.bucket || chunks.count(p.first)) continue;
    if (p.second.bucket < getTarget(today - day))
      due.push_back(make_pair(day, p.first));
  }

  sortDue(due);

  unsigned count = 0;
  unsigned reads = 0;

  for (auto &d: due) {
    if (coarsenMax <= count || 4 * coarsenMax <= reads) break;
    reads++;

    tags_t tags;
    if (!readTags(chunksTable, d.second, tags)) continue;

    unsigned target = getTarget(today - d.first);
    bool changed    = false;

    for (auto &p: tags)
      if (p.second.getBucket() < target) {
        p.second.coarsen(getDayBucket(target, p.second.getBucket()));
        changed = true;
      }

    store(d.second, tags, changed);
    if (changed) count++;
  }

  if (count) LOG_INFO(3, "Coarsened " << count << " GPU history days");
  if (reads < due.size())
    LOG_INFO(3, due.size() - reads << " GPU history days pending coarsening");
}


void GPUHistory::repair(unsigned max) {
  // Metadata and rollups of days stored without them
  vector<pair<int64_t, string>> due;
  for (auto &p: days) {
    auto &meta = p.second;
    if (chunks.count(p.first)) continue;
    if (!meta.bucket || !meta.hasTags ||
        (meta.bucket < rollupBucket && !meta.rollup))
      due.push_back(make_pair(getDay(p.first), p.first));
  }

  sortDue(due);

  for (unsigned i = 0; i < due.size() && i < max; i++) {
    tags_t tags;
    if (readTags(chunksTable, due[i].second, tags))
      store(due[i].second, tags, false);
  }
}


void GPUHistory::reset() {
  // A failed statement is not reset and stays unusable, so the tables are
  // opened again before the next try
  for (auto table: {chunksTable, unitsTable, daysTable, rollupTable})
    app.closeDB(table);
}


bool GPUHistory::add(const GPUSample &s, uint64_t day, const string &tag,
                     double end, double weight) {
  string key = getKey(s.device, day);
  auto chunk = getChunk(key);
  if (!chunk) return false;

  auto it = chunk->tags.find(tag);
  if (it == chunk->tags.end()) {
    GPUSeries series(app.getConfig()->getGPUTelemetryBucket());
    it = chunk->tags.insert(tags_t::value_type(tag, series)).first;
  }

  it->second.add(end, s.measurement, weight);
  chunk->dirty = true;
  dirty        = true;
  current[s.device] = key;
  return true;
}


GPUHistory::Chunk *GPUHistory::getChunk(const string &key) {
  auto it = chunks.find(key);
  if (it != chunks.end()) return &it->second;

  // Continue a stored day, e.g. after a restart or a clock change.  If it
  // cannot be read now, the sample is dropped rather than the day.
  tags_t tags;
  auto cached = cache.find(key);
  if (cached != cache.end()) tags.swap(cached->second.tags);
  else if (!readTags(chunksTable, key, tags)) return 0;
  cache.erase(key);

  auto &chunk = chunks[key];
  chunk.tags.swap(tags);
  return &chunk;
}


const GPUHistory::tags_t *GPUHistory::getStored(const string &key) {
  auto it = cache.find(key);

  if (it == cache.end()) {
    tags_t tags;
    if (!readTags(chunksTable, key, tags)) return 0;
    complete(key, tags);

    if (cacheMax <= cache.size()) {
      auto lru = cache.begin();
      for (auto c = cache.begin(); c != cache.end(); c++)
        if (c->second.used < lru->second.used) lru = c;
      cache.erase(lru);
    }

    it = cache.insert(make_pair(key, Cached())).first;
    it->second.tags.swap(tags);
  }

  it->second.used = ++uses;
  return &it->second.tags;
}


void GPUHistory::complete(const string &key, const tags_t &tags,
                          bool force) {
  auto &meta = days[key];
  if (force || !meta.bucket || !meta.hasTags ||
      (meta.bucket < rollupBucket && !meta.rollup))
    try {
      store(key, tags, false);
    } catch (const std::exception &e) {
      LOG_WARNING("Failed to index GPU history " << key << ": " << e.what());
      reset();
    }
}


bool GPUHistory::readTags(const char *table, const string &key,
                          tags_t &tags) {
  // False only if the DB failed, a malformed value reads as empty
  string value;

  try {
    auto &db = app.getDB(table);
    if (!db.has(key)) return true;
    value = db.getString(key);

  } catch (const std::exception &e) {
    if (!readFailed)
      LOG_WARNING("Failed to read GPU history " << key << ": " << e.what());
    readFailed = true;
    reset();
    return false;
  }

  readFailed = false;

  try {
    tags = read(*JSON::parse(value), key);
  } catch (const std::exception &e) {
    LOG_WARNING("Dropped GPU history " << key << ": " << e.what());
  }

  return true;
}


GPUHistory::Entry *GPUHistory::getEntry(const string &id) {
  auto it = entries.find(id);
  if (it != entries.end()) return &it->second;

  Entry entry;
  if (readEntry(id, entry) < 0) return 0; // Retried with the next sample
  if (entry.labels.isNull() || !entry.labels->size()) label(entry, id);

  return &entries.insert(make_pair(id, entry)).first->second;
}


bool GPUHistory::findEntry(const string &id, Entry &entry) {
  auto it = entries.find(id);
  if (it == entries.end()) return 0 < readEntry(id, entry);
  entry = it->second;
  return true;
}


int GPUHistory::readEntry(const string &id, Entry &entry) {
  // 1 if read, 0 if missing or malformed, -1 if the DB failed
  string value;

  try {
    auto &db = app.getDB(unitsTable);
    if (!db.has(id)) return 0;
    value = db.getString(id);

  } catch (const std::exception &e) {
    if (!readFailed)
      LOG_WARNING("Failed to read GPU history index of unit " << id << ": "
                  << e.what());
    readFailed = true;
    reset();
    return -1;
  }

  try {
    entry.read(*JSON::parse(value));
    return 1;

  } catch (const std::exception &e) {
    LOG_WARNING("Dropped GPU history index of unit " << id << ": "
                << e.what());
  }

  entry = Entry();
  return 0;
}


int GPUHistory::readLog(const string &id, Entry &entry) {
  // 1 if the unit's WU log row has its end, 0 if not, -1 if the DB failed
  string value;

  try {
    auto &db = app.getDB("wu_log", true);
    if (!db.has(id)) return 0;
    value = db.getString(id);

  } catch (const std::exception &e) {
    LOG_WARNING("Failed to read WU log of unit " << id << ": " << e.what());
    app.closeDB("wu_log");
    return -1;
  }

  try {
    auto unit     = JSON::parse(value);
    uint64_t time = Time::parse(unit->getString("end_time"));
    if (!time) return 0;

    // Not before its samples nor in the future, e.g. after a clock change
    entry.finish = std::max(std::min(time, now()), entry.last);
    entry.result = unit->getString("result", "");

    Entry labeled;
    label(labeled, *unit);
    if (labeled.labels->size()) entry.labels = labeled.labels;
    return 1;

  } catch (const std::exception &e) {
    LOG_DEBUG(3, "No end in WU log of unit " << id << ": " << e.what());
  }

  return 0;
}


void GPUHistory::label(Entry &entry, const string &id) const {
  auto unit = app.getUnits()->findUnit(id);
  if (unit.isSet()) label(entry, *unit);
}


void GPUHistory::label(Entry &entry, const JSON::Value &unit) {
  entry.labels = new JSON::Dict;
  for (auto &l: labelPaths)
    if (unit.exists(l[1]))
      entry.labels->insert(l[0], unit.select(l[1])->copy(true));
}


string GPUHistory::getKey(const string &gpu, uint64_t day) {
  return gpu + "/" + Time(day * Time::SEC_PER_DAY).toString("%Y%m%d");
}


int64_t GPUHistory::getDay(const string &key) {
  // Days since 1970 of the key's YYYYMMDD, -1 if invalid
  size_t slash = key.rfind('/');
  if (slash == string::npos) return -1;

  string date = key.substr(slash + 1);
  if (date.size() != 8 || date.find_first_not_of("0123456789") != string::npos)
    return -1;

  int y      = stoi(date.substr(0, 4));
  unsigned m = stoi(date.substr(4, 2));
  unsigned d = stoi(date.substr(6, 2));
  if (y < 1970 || m < 1 || 12 < m || d < 1 || 31 < d) return -1;

  y -= m <= 2;
  unsigned era = y / 400;
  unsigned yoe = y - era * 400;
  unsigned doy = (153 * (2 < m ? m - 3 : m + 9) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t day  = (int64_t)era * 146097 + doe - 719468;

  // Rejects dates like the 31st of February
  return getKey("", day) == "/" + date ? day : -1;
}


unsigned GPUHistory::getBucket(const tags_t &tags) {
  unsigned bucket = ~0U;
  for (auto &p: tags) bucket = std::min(bucket, p.second.getBucket());
  return bucket;
}


GPUHistory::tags_t GPUHistory::getRollup(const tags_t &tags) {
  tags_t rollup = tags;

  for (auto &p: rollup)
    if (p.second.getBucket() < rollupBucket)
      p.second.coarsen(getDayBucket(rollupBucket, p.second.getBucket()));

  return rollup;
}


bool GPUHistory::hasUnit(const string &tag, const string &id) {
  for (size_t start = 0;;) {
    size_t end = tag.find(',', start);
    size_t len = end == string::npos ? string::npos : end - start;
    if (!tag.compare(start, len, id)) return true;
    if (end == string::npos) return false;
    start = end + 1;
  }
}


GPUHistory::tags_t GPUHistory::read(const JSON::Value &value,
                                    const string &key) {
  if (!value.isDict()) THROW("Not a dict");

  // An invalid tag costs only its own series.  Buckets divide a day and
  // start in the key's day.
  const uint64_t secs = Time::SEC_PER_DAY;
  int64_t day = getDay(key);
  tags_t tags;

  for (auto it = value.begin(); it != value.end(); it++)
    try {
      // Checked as stored, before the row limit merges buckets
      GPUSeries series(60, ~0U);
      series.read(**it);

      unsigned len = series.getBucket();
      if (secs % len) THROW("Invalid bucket length " << len);

      for (auto &b: series.getBuckets())
        if (b.first % len || (0 <= day && b.first / secs != (uint64_t)day))
          THROW("Invalid bucket start " << b.first);

      series.setLimit(GPUSeries::maxBuckets);
      tags.insert(tags_t::value_type(it.key(), series));

    } catch (const std::exception &e) {
      LOG_WARNING("Dropped GPU history " << key << " tag '" << it.key()
                  << "': " << e.what());
    }

  return tags;
}


void GPUHistory::write(const tags_t &tags, JSON::Sink &sink) {
  sink.beginDict();

  for (auto &p: tags) {
    sink.beginInsert(p.first);
    p.second.write(sink);
  }

  sink.endDict();
}
