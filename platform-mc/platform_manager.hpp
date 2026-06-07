/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
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

#include "libpldm/platform.h"
#include "libpldm/pldm.h"

#include "terminus.hpp"
#include "terminus_manager.hpp"

namespace pldm
{

namespace platform_mc
{

/**
 * @brief PlatformManager
 *
 * PlatformManager class manages Terminus for fetching PDRs and initializing
 * sensors.
 */
class PlatformManager
{
  public:
    PlatformManager() = delete;
    PlatformManager(const PlatformManager&) = delete;
    PlatformManager(PlatformManager&&) = delete;
    PlatformManager& operator=(const PlatformManager&) = delete;
    PlatformManager& operator=(PlatformManager&&) = delete;
    ~PlatformManager() = default;

    explicit PlatformManager(
        TerminusManager& terminusManager,
        std::map<tid_t, std::shared_ptr<Terminus>>& termini) :
        terminusManager(terminusManager), termini(termini)
    {}

    /** @brief Initialize terminus which supports PLDM Type 2
     *  @return coroutine return_value - PLDM completion code
     */
    exec::task<int> initTerminus();

    /** @brief Re-initialize a single terminus (by tid).
     *
     *  Scoped variant of initTerminus() used by the PDRRepositoryChgEvent
     *  full-rebuild path so a rediscovery touches only the target terminus and
     *  cannot re-create objects for other termini.
     *
     *  @param[in] tid - Terminus ID
     *  @return coroutine return_value - PLDM completion code
     */
    exec::task<int> initTerminus(tid_t tid);

    /** @brief Initialize terminus Event Receiver setting
     *  @param[in] tid - Terminus ID
     *  @return coroutine return_value - PLDM completion code
     */
    exec::task<int> initEventReceiver(tid_t tid);

    /** @brief Fetch a single PDR by record handle into pdrOut.
     *
     *  Unlike getPDRs(), this does NOT clear the terminus PDR repository — it
     *  is used to incrementally pull PDRs that a PDRRepositoryChgEvent reported
     *  as added/modified. Handles multi-part (PLDM_GET_NEXTPART) transfers.
     *
     *  @param[in]  tid - Terminus ID
     *  @param[in]  recordHandle - PDR record handle to fetch
     *  @param[out] pdrOut - assembled raw PDR bytes (cleared then filled)
     *  @return coroutine return_value - PLDM completion code
     */
    exec::task<int> fetchSinglePdr(tid_t tid, uint32_t recordHandle,
                                   std::vector<uint8_t>& pdrOut);

  private:
    /** @brief Initialize a single terminus object (shared body of the two
     *         public initTerminus() entry points).
     *
     *  @param[in] tid - Terminus ID
     *  @param[in] terminus - The terminus to initialize
     *  @return coroutine return_value - PLDM completion code
     */
    exec::task<int> initTerminusImpl(tid_t tid,
                                     std::shared_ptr<Terminus> terminus);

    /** @brief Fetch all PDRs from terminus.
     *
     *  @param[in] terminus - The terminus object to store fetched PDRs
     *  @return coroutine return_value - PLDM completion code
     */
    exec::task<int> getPDRs(std::shared_ptr<Terminus> terminus);

    /** @brief Fetch PDR from terminus
     *
     *  @param[in] tid - Destination TID
     *  @param[in] recordHndl - Record handle
     *  @param[in] dataTransferHndl - Data transfer handle
     *  @param[in] transferOpFlag - Transfer Operation Flag
     *  @param[in] requstCnt - Request Count of data
     *  @param[in] recordChgNum - Record change number
     *  @param[out] nextRecordHndl - Next record handle
     *  @param[out] nextDataTransferHndl - Next data transfer handle
     *  @param[out] transferFlag - Transfer flag
     *  @param[out] responseCnt - Response count of record data
     *  @param[out] recordData - Returned record data
     *  @param[out] transferCrc - CRC value when record data is last part of PDR
     *  @return coroutine return_value - PLDM completion code
     */
    exec::task<int> getPDR(
        tid_t tid, uint32_t recordHndl, uint32_t dataTransferHndl,
        uint8_t transferOpFlag, uint16_t requestCnt, uint16_t recordChgNum,
        uint32_t& nextRecordHndl, uint32_t& nextDataTransferHndl,
        uint8_t& transferFlag, uint16_t& responseCnt,
        std::vector<uint8_t>& recordData, uint8_t& transferCrc);

    /** @brief get PDR repository information.
     *
     *  @param[in] terminus - The terminus object to store fetched PDRs
     *  @return coroutine return_value - PLDM completion code
     */
    exec::task<int> getPDRRepositoryInfo(
        tid_t tid, uint8_t& repositoryState, uint32_t& recordCount,
        uint32_t& repositorySize, uint32_t& largestRecordSize);

    /** @brief Send setEventReceiver command to destination EID.
     *
     *  @param[in] tid - Destination TID
     *  @param[in] eventMessageGlobalEnable - Enable/disable event message
     * generation from the terminus
     *  @param[in] eventReceiverEid - The EID of eventReceiver that terminus
     * should send event message to
     *  @return coroutine return_value - PLDM completion code
     */
    exec::task<int> setEventReceiver(
        tid_t tid, pldm_event_message_global_enable eventMessageGlobalEnable,
        mctp_eid_t eventReceiverEid);

    /** @brief  send eventMessageBufferSize
     *  @param[in] tid - Destination TID
     *  @param[in] receiverMaxBufferSize
     *  @param[out] terminusBufferSize
     *  @return coroutine return_value - PLDM completion code
     */
    exec::task<int> eventMessageBufferSize(tid_t tid,
                                           uint16_t receiverMaxBufferSize,
                                           uint16_t& terminusBufferSize);

    /** @brief  send eventMessageSupprted
     *  @param[in] tid - Destination TID
     *  @param[in] formatVersion - version of the event format
     *  @param[out] synchronyConfiguration - messaging style most recently
     * configured via the setEventReceiver command
     *  @param[out] synchronyConfigurationSupported - event messaging styles
     * supported by the terminus
     *  @param[out] numerEventClassReturned - number of eventClass enumerated
     * bytes
     *  @param[out] eventClass - vector of eventClass the device can generate
     *  @return coroutine return_value - PLDM completion code
     */
    exec::task<int> eventMessageSupported(
        tid_t tid, uint8_t formatVersion, uint8_t& synchronyConfiguration,
        bitfield8_t& synchronyConfigurationSupported,
        uint8_t& numerEventClassReturned, std::vector<uint8_t>& eventClass);

    /** reference of TerminusManager for sending PLDM request to terminus*/
    TerminusManager& terminusManager;

    /** @brief Managed termini list */
    std::map<tid_t, std::shared_ptr<Terminus>>& termini;
};
} // namespace platform_mc
} // namespace pldm
