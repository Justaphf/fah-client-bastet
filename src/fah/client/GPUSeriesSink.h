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

#include <cbang/json/Value.h>

#include <map>
#include <set>
#include <string>
#include <vector>


namespace FAH {
  namespace Client {
    class App;

    // Keeps a time series per unit and device in memory while the unit is
    // in the client and in the "wu_series" table after it ends
    class GPUSeriesSink : public GPUSampleSink {
      App &app;

      using devices_t = std::map<std::string, GPUSeries>; // By GPU ID

      struct Entry {
        devices_t devices;
        bool dirty = false;
        bool ended = false;
      };

      bool loaded     = false;
      bool loadFailed = false;
      bool saveFailed = false;
      bool pending    = false; // Ended units not yet saved
      uint64_t lastFailure = 0;
      std::map<std::string, Entry> units; // By unit ID
      std::set<std::string> stored; // Unit IDs in "wu_series"

      // Smallest bucket of each stored series as last read or written
      mutable std::map<std::string, unsigned> coarse; // By unit ID

    public:
      GPUSeriesSink(App &app);
      ~GPUSeriesSink();

      cb::JSON::ValuePtr get(const std::string &id) const;
      bool save(const std::string &id);
      void end(const std::string &id);

      // From GPUSampleSink
      void sample(const GPUSample &s) override;

    protected:
      bool store(const std::string &id);
      void flush(bool clean = true);
      void load();
      void prune();
      void coarsen(
        const std::vector<std::pair<std::string, std::string>> &ended);
      void mark(const std::string &id, const devices_t &devices) const;
      void write(const std::string &id, const devices_t &devices);
      static devices_t read(const cb::JSON::Value &value);
      static void write(const devices_t &devices, cb::JSON::Sink &sink,
                        bool summary);
    };
  }
}
