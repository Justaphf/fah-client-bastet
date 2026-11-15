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

#include "GPUSeries.h"
#include "GPUStats.h"

#include <map>
#include <set>
#include <string>
#include <vector>
#include <cstdint>


namespace FAH {
  namespace Client {
    // One GPU's history over a time range from stored buckets: rows of all
    // recorded time at one bucket length, statistics of all and of unit
    // time, and the rows in which each unit had samples.  A stored bucket
    // that overlaps the range counts whole.
    class GPUTimeline {
    public:
      using tags_t = std::map<std::string, GPUSeries>;
      using runs_t = std::vector<std::pair<uint64_t, uint64_t>>;

      struct Stats {
        struct Field {
          GPUSeries::Range range;
          GPUStats::histogram_t means; // Seconds by rounded bucket mean
        };

        double seconds = 0;
        std::map<std::string, Field> fields;
        std::map<std::string, GPUSeries::states_t> states;

        void add(const GPUSeries &series, const GPUSeries::Bucket &b);
        void add(const Stats &stats);
        void write(cb::JSON::Sink &sink) const;
      };

    protected:
      uint64_t begin;
      uint64_t end;
      unsigned bucket;
      unsigned length;  // Common multiple of all added bucket lengths
      unsigned longest; // Of all added bucket lengths
      uint64_t first = ~(uint64_t)0;
      uint64_t last  = 0;

      GPUSeries rows;
      Stats all;
      Stats active;
      std::map<std::string, std::set<uint64_t>> units; // Row starts by ID

    public:
      GPUTimeline(uint64_t begin = 0, uint64_t end = 0, unsigned bucket = 60);

      bool empty() const {return rows.empty();}
      uint64_t getFirst() const {return first;}
      uint64_t getLast() const {return last;}
      const GPUSeries &getRows() const {return rows;}
      const Stats &getAll() const {return all;}
      const Stats &getActive() const {return active;}
      runs_t getRuns(const std::string &id) const;

      void add(const tags_t &tags);
      void add(const GPUTimeline &timeline);
      void finish();

      static unsigned getMultiple(unsigned a, unsigned b);
      static unsigned getDayMultiple(unsigned a, unsigned b);
    };
  }
}
