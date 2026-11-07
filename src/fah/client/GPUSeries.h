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

#include <map>
#include <string>
#include <vector>
#include <cstdint>


namespace cb {
  struct GPUMeasurement;
  namespace JSON {
    class Value;
    class Sink;
  }
}


namespace FAH {
  namespace Client {
    // One device's telemetry downsampled into wall clock buckets.  A bucket
    // without samples has no row, so pauses and restarts show as gaps.
    class GPUSeries {
    public:
      struct Range {
        double min    = 0;
        double max    = 0;
        double sum    = 0;
        double weight = 0;

        void add(const Range &r);
      };

      using states_t = std::map<int, double>; // State to seconds

      struct Bucket {
        double seconds = 0;
        std::vector<Range> values;
        std::vector<states_t> states;

        void add(const Bucket &b);
      };

      static const unsigned maxBuckets = 1440;

    protected:
      unsigned bucket;
      unsigned limit;
      std::vector<std::string> fields;
      std::vector<std::string> states;
      std::map<uint64_t, Bucket> buckets; // By start time

    public:
      GPUSeries(unsigned bucket = 60, unsigned limit = maxBuckets);

      unsigned getBucket() const {return bucket;}
      bool empty() const {return buckets.empty();}
      uint64_t getEnd() const;

      void add(double time, const cb::GPUMeasurement &m, double weight);
      void coarsen(unsigned target);

      static int getMode(const states_t &s);

      void read(const cb::JSON::Value &value);
      void write(cb::JSON::Sink &sink) const {write(sink, false);}
      void writeSummary(cb::JSON::Sink &sink) const {write(sink, true);}

    protected:
      void add(Bucket &b, const cb::GPUMeasurement &m, double weight);
      void merge(unsigned length);
      void shrink();
      void write(cb::JSON::Sink &sink, bool summary) const;
      static unsigned getColumn(
        std::vector<std::string> &names, const std::string &name);
      void addValue(Bucket &b, const std::string &name, double value,
                    double weight);
      void addState(Bucket &b, const std::string &name, int value,
                    double weight);
    };
  }
}
