/******************************************************************************\

                  This file is part of the Folding@home Client.

          The fah-client runs Folding@home protein folding simulations.
                    Copyright (c) 2001-2024, foldingathome.org
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

#include "GPUResource.h"

#include <cbang/String.h>
#include <cbang/json/JSON.h>
#include <cbang/hw/GPUVendor.h>

using namespace FAH::Client;
using namespace cb;
using namespace std;

namespace {
  string getGPUVendorName(uint16_t id) {
    return String::toLower(GPUVendor((GPUVendor::enum_t)id).toString());
  }
}


void GPUResource::setPCI(const PCIDevice &pci) {
  insert("vendor", pci.getVendorID());
  insert("device", pci.getDeviceID());
  insert("type",   getGPUVendorName(pci.getVendorID()));
}


void GPUResource::set(const string &name, const ComputeDevice &cd) {
  if (cd.vendorID != -1) {
    if (!hasU32("vendor"))  insert("vendor", cd.vendorID);
    if (!hasString("type")) insert("type",   getGPUVendorName(cd.vendorID));
  }

  if (!cd.name.empty() && !hasString("description"))
    insert("description", cd.name);

  if (!cd.uuid.empty() && !hasString("uuid")) insert("uuid", cd.uuid);

  if (cd.isValid()) {
    JSON::ValuePtr d = new JSON::Dict;
    d->insert("platform", cd.platformIndex);
    d->insert("device",   cd.deviceIndex);
    d->insert("compute",  cd.computeVersion.toString());
    d->insert("driver",   cd.driverVersion.toString());
    if (name == "nvml") d->insert("UUID", cd.name);
    insert(name, d);

  } else if (has(name)) erase(name);
}

void toPStateString(char* buf, uint8_t state) { sprintf(buf, "P%u", state); }
void toPcieGenString(char* buf, uint8_t gen) { sprintf(buf, "Gen%u", gen); }
void toPcieLinkString(char* buf, uint8_t gen, uint8_t width) { sprintf(buf, "Gen%ux%u", gen, width); }

void GPUResource::setMeasurements(const cb::GPUMeasurement &meas) {
  char buf[12];
  toPcieGenString(buf, meas.maxPCIeLinkGenDevice);
  insert("pcie_link_device", buf);
  toPcieLinkString(buf, meas.maxPCIeLinkGen, meas.maxPCIeLinkWidth);
  insert("pcie_link_system", buf);
  insert("max_gpu_clock", meas.gpuFreqLimit_MHz);
  insert("max_mem_clock", meas.memFreqLimit_MHz);
  insert("cur_gpu_clock", meas.gpuFreq_MHz);
  insert("cur_mem_clock", meas.memFreq_MHz);
  insert("cur_temp", meas.gpuTemp_C);
  toPStateString(buf, meas.pstate);
  insert("pstate", buf);
}

void GPUResource::writeRequest(JSON::Sink &sink) const {
  sink.beginDict();

  sink.insert("gpu",    getString("type"));
  sink.insert("vendor", getU16("vendor"));
  sink.insert("device", getU16("device"));

  if (has("cuda"))   sink.insert("cuda",   *get("cuda"));
  if (has("opencl")) sink.insert("opencl", *get("opencl"));

  sink.endDict();
}
