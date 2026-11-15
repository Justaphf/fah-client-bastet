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
#include "GPUSeries.h"
#include "GPUTimeline.h"

#include <cbang/json/Value.h>

#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>


namespace FAH {
  namespace Client {
    class App;

    // One timeline per GPU in UTC day chunks of the "gpu_history" table.  A
    // chunk holds a series per tag, the IDs of the units a sample was
    // attributed to or empty for idle time.  A unit's series is the slice of
    // all tags with its ID, "gpu_history_units" indexes the units.  Each day
    // also has metadata and, unless already that coarse, a rollup, which
    // ranges too long for full detail are read from.
    class GPUHistory : public GPUSampleSink {
    public:
      using tags_t = std::map<std::string, GPUSeries>; // By tag
      using day_cb_t =
        std::function<void (uint64_t day, const tags_t &tags)>;

      static const unsigned rollupBucket = 1800;

    protected:
      App &app;

      struct Chunk {
        tags_t tags;
        bool dirty = false;
      };

      struct Meta {
        unsigned bucket = 0; // Smallest of the day, 0 if unknown
        bool rollup     = false;
        bool hasTags    = false;
        std::set<std::string> tags;
        uint64_t changed = 0; // Not stored

        void read(const cb::JSON::Value &value);
        void write(cb::JSON::Sink &sink) const;
      };

      struct Entry {
        std::set<std::string> gpus;
        uint64_t first  = 0;
        uint64_t last   = 0;
        uint64_t finish = 0;
        std::string result;
        cb::JSON::ValuePtr labels;
        bool dirty = false;

        void read(const cb::JSON::Value &value);
        void write(cb::JSON::Sink &sink) const;
      };

      struct Cached {
        tags_t tags;
        uint64_t used = 0;
      };

      // The whole stored days of a GPU's last range
      struct View {
        bool full       = false;
        unsigned bucket = 0;
        std::vector<std::string> keys;
        uint64_t changes = 0;
        GPUTimeline timeline;
      };

      bool loaded     = false;
      bool loadFailed = false;
      bool readFailed = false;
      bool saveFailed = false;
      bool dirty      = false;
      uint64_t lastFlush = 0;
      int64_t lastPrune  = -1; // Day
      int64_t recovered  = -1; // Day of the last finish recovery
      uint64_t recoverFailed = 0; // Time of the last failed one
      int64_t scanned    = -1; // Cutoff of the last complete index scan

      std::map<std::string, Chunk> chunks;  // Recording, by key
      std::map<std::string, Entry> entries; // By unit ID
      std::map<std::string, Entry> finishes; // Not yet in an entry
      std::set<std::string> unlogged; // Gone without a finish or WU log row
      std::set<std::string> watched; // Stored unfinished, unit in the client
      std::map<std::string, std::string> current; // Last chunk key by GPU ID
      std::map<std::string, Meta> days;     // Stored chunks by key
      std::map<std::string, Cached> cache;  // Decoded stored chunks by key
      uint64_t uses    = 0;
      uint64_t changes = 0; // Of stored days

      std::map<std::string, std::list<View>> views; // By GPU ID, newest first
      std::map<std::string, Entry> index;   // All stored entries, once read
      bool indexed = false;

    public:
      GPUHistory(App &app);
      virtual ~GPUHistory();

      cb::JSON::ValuePtr get(const std::string &id);
      cb::JSON::ValuePtr get(const std::string &gpu, double from, double to,
                             double points);
      // The callback must not call into the history
      void getDays(const std::string &gpu, uint64_t first, uint64_t last,
                   bool rollup, const day_cb_t &cb);
      void save();
      void end(const std::string &id, const std::string &result);

      static unsigned getDayBucket(unsigned length, unsigned multiple);

      // From GPUSampleSink
      void sample(const GPUSample &s) override;

    protected:
      virtual uint64_t now() const;

      void record(const GPUSample &s);
      void finish(const std::string &id, const std::string &result);
      cb::JSON::ValuePtr getSeries(const std::string &id);
      cb::JSON::ValuePtr getRange(const std::string &gpu, uint64_t from,
                                  uint64_t to, unsigned points);
      const tags_t *readRange(const std::string &key, bool full,
                              tags_t &buffer, unsigned &builds);
      void writeUnits(cb::JSON::Sink &sink, const std::string &gpu,
                      uint64_t from, uint64_t to,
                      const GPUTimeline &timeline);
      void readIndex();
      void readDays(const std::string &gpu, uint64_t first, uint64_t last,
                    bool rollup, const day_cb_t &cb);
      std::set<std::string> getKeys(const std::string &gpu, uint64_t first,
                                    uint64_t last) const;

      bool load();
      bool open();
      bool flush();
      void store(const std::string &key, const tags_t &tags,
                 bool chunk = true);
      void prune();
      void expire(int64_t today);
      bool recover();
      bool finishLogged(std::map<std::string, Entry> &open, uint64_t cutoff,
                        bool remove);
      bool isOpen(const std::string &id, const Entry &entry) const;
      bool isExpired(const std::string &id, const Entry &entry,
                     uint64_t cutoff) const;
      void coarsen(int64_t today);
      void repair(unsigned max);
      void reset();

      bool add(const GPUSample &s, uint64_t day, const std::string &tag,
               double end, double weight);
      Chunk *getChunk(const std::string &key);
      const tags_t *getStored(const std::string &key);
      void complete(const std::string &key, const tags_t &tags,
                    bool force = false);
      bool readTags(const char *table, const std::string &key, tags_t &tags);
      Entry *getEntry(const std::string &id);
      bool findEntry(const std::string &id, Entry &entry);
      int readEntry(const std::string &id, Entry &entry);
      int readLog(const std::string &id, Entry &entry);
      void label(Entry &entry, const std::string &id) const;
      static void label(Entry &entry, const cb::JSON::Value &unit);

      static std::string getKey(const std::string &gpu, uint64_t day);
      static int64_t getDay(const std::string &key);
      static unsigned getBucket(const tags_t &tags);
      static tags_t getRollup(const tags_t &tags);
      static bool hasUnit(const std::string &tag, const std::string &id);
      static tags_t read(const cb::JSON::Value &value,
                         const std::string &key);
      static void write(const tags_t &tags, cb::JSON::Sink &sink);
    };
  }
}
