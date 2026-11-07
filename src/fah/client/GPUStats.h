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

#include <cbang/json/Value.h>

#include <map>
#include <string>
#include <cstdint>


namespace cb {
  struct GPUMeasurement;
  namespace JSON {class Sink;}
}


namespace FAH {
  namespace Client {
    // Time weighted statistics of one device's telemetry.  Readings are
    // small integers (power rounded to whole watts), so a histogram of value
    // to seconds is compact and gives exact percentiles.
    class GPUStats {
    public:
      using histogram_t = std::map<int, double>;

    protected:
      double   seconds = 0;
      uint64_t samples = 0;
      std::map<std::string, histogram_t> histograms;
      std::map<std::string, double> totals;

    public:
      bool empty() const {return !samples;}
      double getSeconds() const {return seconds;}
      uint64_t getSamples() const {return samples;}
      double getEnergyWh() const;

      void add(const cb::GPUMeasurement &m, double weight);

      static double getTotal(const histogram_t &h);
      static double getMean(const histogram_t &h);
      static int getPercentile(const histogram_t &h, double p);

      cb::JSON::ValuePtr getSummary() const;
      void read(const cb::JSON::Value &value);
      void write(cb::JSON::Sink &sink) const;

    protected:
      void add(const std::string &name, int value, double weight);
    };
  }
}
