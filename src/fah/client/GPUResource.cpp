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

#include "GPUResource.h"
#include "Config.h"

#include <cbang/Catch.h>
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
    insert(name, d);

  } else if (has(name)) erase(name);
}

void GPUResource::setRealTimeMeasurements(const cb::GPUMeasurement &meas) {
  char buf[12];
  try {
    toPcieGenString(buf, meas.maxPCIeLinkGenDevice);
    insert("pcie_link_device", buf);
    toPcieLinkString(buf, meas.maxPCIeLinkGen, meas.maxPCIeLinkWidth);
    insert("pcie_link_system", buf);
    toPcieLinkString(buf, meas.currPCIeLinkGen, meas.currPCIeLinkWidth);
    insert("pcie_link_current", buf);
    insert("max_gpu_clock", meas.gpuFreqLimit_MHz);
    insert("max_mem_clock", meas.memFreqLimit_MHz);
    insert("cur_gpu_clock", meas.gpuFreq_MHz);
    insert("cur_mem_clock", meas.memFreq_MHz);
    insert("cur_temp", meas.gpuTemp_C);
    toPStateString(buf, meas.pstate);
    insert("pstate", buf);
    insert("cur_gpu_power", meas.currPower_Watts);
    insert("max_gpu_power", meas.maxPower_Watts);
    insert("gpu_fans", meas.fanCount);
    if(meas.fanCount > 0)
    {
      insert("fan0_pct", meas.fan0Speed_pct);
      if (meas.fanCount > 1) insert("fan1_pct", meas.fan1Speed_pct);
      if (meas.fanCount > 2) insert("fan2_pct", meas.fan2Speed_pct);
    }
  } CATCH_WARNING;
}

bool GPUResource::isComputeDeviceSupported(
  const std::string &type, const Config &config) const {
  return has(type) && config.isComputeDeviceEnabled(type);
}


bool GPUResource::isSupported(const Config &config) const {
  return getBoolean("supported", false) && config.isGPUEnabled(getID()) &&
    (isComputeDeviceSupported("cuda",   config) ||
     isComputeDeviceSupported("hip",    config) ||
     isComputeDeviceSupported("opencl", config));
}


void GPUResource::writeRequest(JSON::Sink &sink, const Config &config) const {
  sink.beginDict();

  sink.insert("gpu",    getString("type"));
  sink.insert("vendor", getU16("vendor"));
  sink.insert("device", getU16("device"));

  if (isComputeDeviceSupported("cuda", config))
    sink.insert("cuda", *get("cuda"));
  if (isComputeDeviceSupported("hip", config))
    sink.insert("hip", *get("hip"));
  if (isComputeDeviceSupported("opencl", config))
    sink.insert("opencl", *get("opencl"));

  sink.endDict();
}
