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

#include <cbang/json/Observable.h>
#include <cbang/hw/ComputeDevice.h>
#include <cbang/hw/PCIInfo.h>
#include <cbang/hw/GPUMeasurement.h>

namespace FAH {
  namespace Client {
    class Config;

    inline void toPStateString(char* buf, uint8_t state) { sprintf(buf, "P%u", state); }
    inline void toPcieGenString(char* buf, uint8_t gen) { sprintf(buf, "Gen%u", gen); }
    inline void toPcieLinkString(char* buf, uint8_t gen, uint8_t width) { sprintf(buf, "Gen%ux%u", gen, width); }

    class GPUResource : public cb::JSON::ObservableDict {
      std::string id;

    public:
      GPUResource(const std::string &id) : id(id) {}

      const std::string &getID() const {return id;}

      void setPCI(const cb::PCIDevice &pci);

      using cb::JSON::ObservableDict::set;
      void set(const std::string &name, const cb::ComputeDevice &cd);

      void setRealTimeMeasurements(const cb::GPUMeasurement &meas);

      bool isComputeDeviceSupported(
        const std::string &type, const Config &config) const;
      bool isSupported(const Config &config) const;
      void writeRequest(cb::JSON::Sink &sink, const Config &config) const;
    };
  }
}
