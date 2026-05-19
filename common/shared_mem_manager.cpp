/*
 * SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "common/shared_mem_manager.hpp"

#include <phosphor-logging/lg2.hpp>

#include <chrono>

PHOSPHOR_LOG2_USING;

namespace pldm
{
namespace shmem_utils
{

std::vector<tal::TelemetryData> SharedMemoryManager::telemetryData;

bool SharedMemoryManager::initTALNamespace()
{
    telemetryData.reserve(kReserveCapacity);
    bool ok = tal::TelemetryAggregator::namespaceInit(
        tal::ProcessType::Producer, "pldmd");
    if (ok)
    {
        info("Initialized tal from pldmd");
    }
    return ok;
}

void SharedMemoryManager::cacheTALData(
    const std::string& objPath, const std::string& iface,
    const std::string& prop, const std::vector<uint8_t>& smbusData,
    const nv::sensor_aggregation::DbusVariantType& propValue,
    const std::string& associatedEntity, int rc)
{
    telemetryData.push_back(tal::TelemetryData{
        objPath, iface, prop, smbusData, rc, propValue, associatedEntity, 0u});
}

void SharedMemoryManager::updateAggregateTelemetryOnTAL()
{
    if (telemetryData.empty())
    {
        return;
    }
    uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    for (auto& td : telemetryData)
    {
        td.timestamp = now;
    }
    tal::TelemetryAggregator::updateAggregateTelemetry(telemetryData);
    telemetryData.clear();
}

} // namespace shmem_utils
} // namespace pldm
