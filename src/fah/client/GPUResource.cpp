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

#include <cbang/Exception.h>
#include <cbang/String.h>
#include <cbang/json/JSON.h>
#include <cbang/log/Logger.h>
#include <cbang/hw/GPUVendor.h>
#include <cbang/hw/NVMLLibrary.h>

using namespace FAH::Client;
using namespace cb;
using namespace std;

namespace {
  string getGPUVendorName(uint16_t id) {
    return String::toLower(GPUVendor((GPUVendor::enum_t)id).toString());
  }


  template <typename T>
  void insertNonZero(JSON::Value &d, const string &key, T value) {
    if (value) d.insert(key, value);
  }
}


GPUResource::GPUResource(const GPUDevice &gpu) : id("gpu:" + gpu.getID()) {
  if (gpu.getVendorID()) {
    insert("vendor", gpu.getVendorID());
    insert("type",   getGPUVendorName(gpu.getVendorID()));
  }

  if (gpu.getDeviceID()) insert("device", gpu.getDeviceID());

  if (!gpu.getDescription().empty())
    insert("description", gpu.getDescription());

  string uuid = gpu.getUUID();
  if (!uuid.empty()) insert("uuid", uuid);

  insertBoolean("supported", gpu.isSupported());
  if (gpu.getNVML().isValid()) insertBoolean("nvml", true);

  insertComputeDevice("cuda",   gpu.getCUDA());
  insertComputeDevice("hip",    gpu.getHIP());
  insertComputeDevice("opencl", gpu.getOpenCL());
}


JSON::ValuePtr GPUResource::readTelemetry(GPUMeasurement &m) {
  if (!getBoolean("nvml", false) || !hasString("uuid")) return 0;

  try {
    m = NVMLLibrary::instance().getMeasurement(getString("uuid"));

    JSON::ValuePtr d = new JSON::Dict;
    insertNonZero(*d, "gpu_clock",           m.gpuClock);
    insertNonZero(*d, "gpu_clock_max",       m.gpuClockMax);
    insertNonZero(*d, "mem_clock",           m.memClock);
    insertNonZero(*d, "mem_clock_max",       m.memClockMax);
    insertNonZero(*d, "power",               m.power);
    insertNonZero(*d, "power_limit",         m.powerLimit);
    insertNonZero(*d, "temp",                m.temperature);
    if (0 <= m.utilization)    d->insert("gpu_util", m.utilization);
    if (0 <= m.memUtilization) d->insert("mem_util", m.memUtilization);
    if (0 <= m.pstate)         d->insert("pstate",   m.pstate);
    insertNonZero(*d, "pcie_gen",            m.pcieGen);
    insertNonZero(*d, "pcie_gen_max",        m.pcieGenMax);
    insertNonZero(*d, "pcie_gen_device_max", m.pcieGenDeviceMax);
    insertNonZero(*d, "pcie_width",          m.pcieWidth);
    insertNonZero(*d, "pcie_width_max",      m.pcieWidthMax);

    auto fans = d->createList();
    for (auto speed: m.fans) fans->append(speed);
    d->insert("fans", fans);

    telemetryFailed = false;
    return d;

  } catch (const Exception &e) {
    // Warn once, then stay quiet until a read succeeds
    if (!telemetryFailed)
      LOG_WARNING("Failed to read telemetry for " << getID() << ": "
                  << e.getMessage());

    telemetryFailed = true;
    clearTelemetry();
  }

  return 0;
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


void GPUResource::insertComputeDevice(
  const string &name, const ComputeDevice &cd) {
  if (!cd.isValid()) return;

  JSON::ValuePtr d = new JSON::Dict;
  d->insert("platform", cd.platformIndex);
  d->insert("device",   cd.deviceIndex);
  d->insert("compute",  cd.computeVersion.toString());
  d->insert("driver",   cd.driverVersion.toString());
  insert(name, d);
}
