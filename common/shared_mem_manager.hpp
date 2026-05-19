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

#pragma once

#include <tal.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pldm
{
namespace shmem_utils
{

/** @class SharedMemoryManager
 *
 *  Batches per-poll-cycle TAL telemetry writes so pldmd issues one
 *  tal::TelemetryAggregator::updateAggregateTelemetry call per terminus poll
 *  cycle instead of N individual updateTelemetry calls. Mirrors
 *  nsm_shmem_utils::SharedMemoryManager from nsmd commit 3cb9d9e2.
 *
 *  Threading: pldmd runs single-threaded sd-event coroutines; the batch is
 *  process-global with no locking. Coroutines from different termini may
 *  interleave cacheTALData calls but cannot race because they yield only at
 *  co_await points.
 */
class SharedMemoryManager
{
  public:
    SharedMemoryManager() = delete;
    SharedMemoryManager(const SharedMemoryManager&) = delete;
    SharedMemoryManager& operator=(const SharedMemoryManager&) = delete;
    ~SharedMemoryManager() = delete;

    /** @brief Initialize the TAL namespace for pldmd as a Producer and
     *         reserve batch capacity. Logs success once.
     *
     *  @return true if TAL initialization succeeded.
     */
    static bool initTALNamespace();

    /** @brief Append one telemetry record to the per-cycle batch.
     *
     *  Timestamp is left at 0 here and stamped uniformly at flush time so
     *  all records from a single poll cycle share the same timestamp.
     */
    static void cacheTALData(
        const std::string& objPath, const std::string& iface,
        const std::string& prop, const std::vector<uint8_t>& smbusData,
        const nv::sensor_aggregation::DbusVariantType& propValue,
        const std::string& associatedEntity = {}, int rc = 0);

    /** @brief Stamp queued records with the current steady-clock ms,
     *         hand them off to TAL as a single batch, and clear the buffer.
     *         No-op when the batch is empty.
     */
    static void updateAggregateTelemetryOnTAL();

  private:
    static constexpr size_t kReserveCapacity = 64;
    static std::vector<tal::TelemetryData> telemetryData;
};

} // namespace shmem_utils
} // namespace pldm
