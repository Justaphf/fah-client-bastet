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

#include "GPUSeriesSink.h"

#include "App.h"
#include "Config.h"
#include "Units.h"
#include "Unit.h"

#include <cbang/Catch.h>
#include <cbang/log/Logger.h>
#include <cbang/json/JSON.h>
#include <cbang/json/Builder.h>
#include <cbang/json/BufferWriter.h>
#include <cbang/time/Time.h>

#include <set>
#include <vector>

using namespace FAH::Client;
using namespace cb;
using namespace std;


namespace {
  const unsigned retryDelay = 60;
  const unsigned coarsenMax = 32; // Series read per prune
}


GPUSeriesSink::GPUSeriesSink(App &app) : app(app) {}


GPUSeriesSink::~GPUSeriesSink() {
  // A last chance for an unsaved ended unit.  The DB outlives the sink but
  // the config does not, so pruning is left to the next start.
  if (pending) TRY_CATCH_ERROR(flush(false));
}


JSON::ValuePtr GPUSeriesSink::get(const string &id) const {
  auto it = units.find(id);
  if (it != units.end())
    return JSON::build(
      [&] (JSON::Sink &sink) {write(it->second.devices, sink, true);});

  auto &db = app.getDB("wu_series", true);
  if (db.has(id))
    try {
      auto devices = read(*db.getJSON(id));
      mark(id, devices);
      return JSON::build(
        [&] (JSON::Sink &sink) {write(devices, sink, true);});
    } CATCH_ERROR;

  return JSON::Null::instancePtr();
}


bool GPUSeriesSink::save(const string &id) {
  bool ok = store(id);
  flush();
  return ok;
}


void GPUSeriesSink::end(const string &id) {
  auto it = units.find(id);
  if (it == units.end()) return;

  it->second.ended = true;
  flush();
}


void GPUSeriesSink::sample(const GPUSample &s) {
  // Retried from here, a failed save may have been the last unit's
  if (pending && lastFailure + retryDelay <= Time::now()) flush();

  auto &config = *app.getConfig();
  if (!config.getGPUTelemetryHistory()) return;

  // Recording before the saved series is loaded would overwrite it
  if (!loaded)
    try {
      load();
    } catch (const Exception &e) {
      if (!loadFailed) LOG_ERROR("Failed to load GPU series: " << e);
      loadFailed = true;
      return;
    }

  for (auto &id: s.units) {
    auto &entry = units[id];
    auto it     = entry.devices.find(s.device);

    if (it == entry.devices.end()) {
      GPUSeries series(config.getGPUTelemetryBucket());
      it = entry.devices.insert(devices_t::value_type(s.device, series)).first;
    }

    it->second.add(s.time, s.measurement, s.weightSeconds);
    entry.dirty = true;
  }
}


bool GPUSeriesSink::store(const string &id) {
  auto it = units.find(id);
  if (it == units.end() || !it->second.dirty) return true;

  try {
    write(id, it->second.devices);
    it->second.dirty = false;
    saveFailed       = false;
    return true;

  } catch (const std::exception &e) {
    // Retried until it succeeds
    if (!saveFailed) LOG_WARNING("Failed to save GPU series: " << e.what());
    saveFailed = true;
  }

  return false;
}


void GPUSeriesSink::flush(bool clean) {
  // An ended unit's series stays in memory until it is saved
  bool removed = false;
  pending = false;

  for (auto it = units.begin(); it != units.end();)
    if (!it->second.ended) it++;
    else if (store(it->first)) {
      it      = units.erase(it);
      removed = true;

    } else {
      pending = true;
      it++;
    }

  if (pending) lastFailure = Time::now();
  if (removed && clean) TRY_CATCH_ERROR(prune());
}


void GPUSeriesSink::load() {
  auto &db = app.getDB("wu_series", true);

  stored.clear();
  db.foreach([this] (const string &id, const string &) {stored.insert(id);},
             ~0U);

  // Continue the series of units that were in progress at shutdown
  for (auto &v: *app.getUnits()) {
    auto &id = v.cast<Unit>()->getID();
    if (!db.has(id)) continue;

    try {
      auto devices = read(*db.getJSON(id));
      mark(id, devices);
      units[id].devices = devices;

    } catch (const std::exception &e) {
      LOG_WARNING("Dropped GPU series of unit " << id << ": " << e.what());
    }
  }

  loaded = true;
  TRY_CATCH_ERROR(prune());
}


void GPUSeriesSink::prune() {
  // Units still in the client are kept.  Of the rest, the most recently
  // ended are kept if they are among the newest WU log entries.  The log
  // orders them since a series' own write time changes when it is coarsened.
  unsigned keep = app.getConfig()->getGPUTelemetrySeriesMax();
  auto &db      = app.getDB("wu_series", true);
  auto &unitsDB = app.getDB("units");
  auto &logDB   = app.getDB("wu_log", true);

  // The window follows the log, which is never pruned.  The config bounds
  // keep, so the sum cannot wrap.
  unsigned limit = keep + app.getUnits()->size() + 1;
  vector<pair<string, string>> logged; // Newest first
  logDB.foreach(
    [&] (const string &id, const string &entry) {
      if (stored.count(id)) logged.push_back(make_pair(id, entry));
    }, limit);

  vector<pair<string, string>> ended; // Newest first
  set<string> kept;
  for (auto &p: logged)
    if (ended.size() < keep && !unitsDB.has(p.first)) {
      ended.push_back(p);
      kept.insert(p.first);
    }

  for (auto it = stored.begin(); it != stored.end();)
    if (!kept.count(*it) && !unitsDB.has(*it)) {
      db.unset(*it);
      coarse.erase(*it);
      it = stored.erase(it);

    } else it++;

  coarsen(ended);
}


void GPUSeriesSink::coarsen(const vector<pair<string, string>> &ended) {
  auto tiers = app.getConfig()->getGPUTelemetryAging();
  if (tiers.empty()) return;

  auto &db           = app.getDB("wu_series", true);
  uint64_t now       = Time::now();
  unsigned maxBucket = 0;
  unsigned reads     = 0;
  unsigned pending   = 0;
  unsigned count     = 0;

  for (auto &tier: tiers) maxBucket = std::max(maxBucket, tier.second);

  auto getTarget = [&] (uint64_t endTime) {
    uint64_t age    = endTime < now ? now - endTime : 0;
    unsigned days   = (unsigned)(age / Time::SEC_PER_DAY);
    unsigned target = 0;
    for (auto &tier: tiers)
      if (tier.first <= days) target = std::max(target, tier.second);
    return target;
  };

  for (auto &unit: ended) {
    auto &id    = unit.first;
    bool parsed = false;

    try {
      auto it     = coarse.find(id);
      bool marked = it != coarse.end();
      if (marked && maxBucket <= it->second) continue;

      // The logged end time spares parsing a series too young to coarsen
      auto entry  = JSON::parse(unit.second);
      bool logged = entry->hasString("end_time");
      unsigned target =
        logged ? getTarget(Time::parse(entry->getString("end_time"))) : 0;
      if (logged && (!target || (marked && target <= it->second))) continue;

      // The rest waits for the next prune
      if (coarsenMax <= reads) {
        pending++;
        continue;
      }

      reads++;
      auto devices = read(*db.getJSON(id));
      mark(id, devices);
      parsed = true;

      if (!logged) {
        uint64_t endTime = 0;
        for (auto &p: devices) endTime = std::max(endTime, p.second.getEnd());
        if (endTime) target = getTarget(endTime);
      }

      bool changed = false;
      for (auto &p: devices)
        if (p.second.getBucket() < target) {
          p.second.coarsen(target);
          changed = true;
        }

      if (changed) {
        write(id, devices);
        count++;
      }

    } catch (const std::exception &e) {
      LOG_WARNING("Failed to coarsen GPU series of unit " << id << ": "
                  << e.what());
      if (!parsed) coarse[id] = ~0U;
    }
  }

  if (count) LOG_INFO(3, "Coarsened " << count << " GPU series");
  if (pending) LOG_INFO(3, pending << " GPU series pending coarsening");
}


void GPUSeriesSink::mark(const string &id, const devices_t &devices) const {
  unsigned bucket = ~0U;
  for (auto &p: devices) bucket = std::min(bucket, p.second.getBucket());
  coarse[id] = bucket;
}


void GPUSeriesSink::write(const string &id, const devices_t &devices) {
  JSON::BufferWriter writer(0, true);
  write(devices, writer, false);
  writer.flush();

  app.getDB("wu_series", true).set(id, writer.toString());
  stored.insert(id);
  mark(id, devices);
}


GPUSeriesSink::devices_t GPUSeriesSink::read(const JSON::Value &value) {
  devices_t devices;

  for (auto it = value.begin(); it != value.end(); it++)
    devices[it.key()].read(**it);

  return devices;
}


void GPUSeriesSink::write(const devices_t &devices, JSON::Sink &sink,
                          bool summary) {
  sink.beginDict();

  for (auto &p: devices) {
    sink.beginInsert(p.first);
    if (summary) p.second.writeSummary(sink);
    else p.second.write(sink);
  }

  sink.endDict();
}
