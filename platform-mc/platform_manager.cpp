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
#include "platform_manager.hpp"

#include "common/start_lifetime_as.hpp"
#include "common/types.hpp"
#include "manager.hpp"
#include "terminus_manager.hpp"

#include <phosphor-logging/lg2.hpp>

#include <memory>

PHOSPHOR_LOG2_USING;

namespace pldm
{
namespace platform_mc
{

exec::task<int> PlatformManager::initTerminus()
{
    /* Snapshot TIDs before iterating. The termini map can be modified (entries
     * erased) by removeMctpTerminus() while this coroutine is suspended at a
     * co_await point, which would invalidate range-for iterators and references
     * into the map, causing use-after-free. */
    std::vector<pldm_tid_t> tids;
    for (auto& [tid, _] : termini)
    {
        tids.push_back(tid);
    }

    for (const auto tid : tids)
    {
        // termini[tid] would auto-insert if the TID was erased after the
        // snapshot above.
        if (!termini.contains(tid))
        {
            continue;
        }

        /* Take a local shared_ptr copy so the Terminus object stays alive even
         * if the map entry is erased while this coroutine is suspended. */
        auto terminus = termini[tid];

        if (terminus->initalized)
        {
            continue;
        }

        /* Get Fru */
        uint16_t totalTableRecords = 0;
        if (terminus->doesSupport(PLDM_FRU, PLDM_GET_FRU_RECORD_TABLE_METADATA))
        {
            auto rc =
                co_await getFRURecordTableMetadata(tid, &totalTableRecords);
            if (rc)
            {
                lg2::error(
                    "Failed to get FRU Metadata for terminus {TID}, error {ERROR}",
                    "TID", tid, "ERROR", rc);
            }
            if (!totalTableRecords)
            {
                lg2::info("Fru record table meta data has 0 records");
            }
        }

        if (!termini.contains(tid))
        {
            continue;
        }

        std::vector<uint8_t> fruData{};
        if ((totalTableRecords != 0) &&
            terminus->doesSupport(PLDM_FRU, PLDM_GET_FRU_RECORD_TABLE))
        {
            auto rc =
                co_await getFRURecordTables(tid, totalTableRecords, fruData);
            if (rc)
            {
                lg2::error(
                    "Failed to get Fru Record table for terminus {TID}, error {ERROR}",
                    "TID", tid, "ERROR", rc);
            }
        }

        if (!termini.contains(tid))
        {
            continue;
        }

        if (terminus->doesSupport(PLDM_PLATFORM, PLDM_GET_PDR))
        {
            auto rc = co_await getPDRs(terminus);
            if (rc)
            {
                lg2::error(
                    "Failed to fetch PDRs for terminus with TID: {TID}, error: {ERROR}",
                    "TID", tid, "ERROR", rc);
                continue; // Continue to next terminus
            }

            if (!termini.contains(tid))
            {
                continue;
            }

            terminus->parseTerminusPDRs();
        }

        /**
         * Need terminus name from PDRs before updating Inventory object with
         * Fru data
         */
        if (fruData.size())
        {
            updateInventoryWithFru(tid, fruData.data(), fruData.size());
        }

        uint16_t terminusMaxBufferSize = terminus->maxBufferSize;
        if (!terminus->doesSupport(PLDM_PLATFORM,
                                   PLDM_EVENT_MESSAGE_BUFFER_SIZE))
        {
            terminusMaxBufferSize = PLDM_PLATFORM_DEFAULT_MESSAGE_BUFFER_SIZE;
        }
        else
        {
            /* Get maxBufferSize use PLDM command eventMessageBufferSize */
            auto rc = co_await eventMessageBufferSize(
                tid, terminus->maxBufferSize, terminusMaxBufferSize);
            if (rc != PLDM_SUCCESS)
            {
                lg2::error(
                    "Failed to get message buffer size for terminus with TID: {TID}, error: {ERROR}",
                    "TID", tid, "ERROR", rc);
                terminusMaxBufferSize =
                    PLDM_PLATFORM_DEFAULT_MESSAGE_BUFFER_SIZE;
            }
        }

        if (!termini.contains(tid))
        {
            continue;
        }

        terminus->maxBufferSize =
            std::min(terminus->maxBufferSize, terminusMaxBufferSize);

        auto rc = co_await configEventReceiver(tid);

        if (!termini.contains(tid))
        {
            continue;
        }

        if (rc)
        {
            lg2::error(
                "Failed to config event receiver for terminus with TID: {TID}, error: {ERROR}",
                "TID", tid, "ERROR", rc);
        }

        terminus->initalized = true;
        if (manager)
        {
            manager->startSensorPolling(tid);
        }
        else
        {
            lg2::error(
                "Cannot start sensor polling for TID: {TID} because the manager is not initialized.",
                "TID", tid);
        }
    }
    co_return PLDM_SUCCESS;
}

exec::task<int> PlatformManager::initTerminus(tid_t tid)
{
    // Re-initialize a single terminus. Used by the PDRRepositoryChgEvent
    // full-rebuild path so a rediscovery for one terminus does NOT re-create
    // objects for other termini that a concurrent rebuild left
    // initalized=false (which races on D-Bus object creation → FileExists).
    auto it = termini.find(tid);
    if (it == termini.end())
    {
        lg2::error("initTerminus: terminus tid={TID} not found for re-init",
                   "TID", tid);
        co_return PLDM_ERROR;
    }
    co_return co_await initTerminusImpl(tid, it->second);
}

/* Take a local shared_ptr copy so the Terminus object stays alive even
 * if the map entry is erased while this coroutine is suspended. */
auto terminus = termini[tid];
if (!terminus->doesSupport(PLDM_PLATFORM, PLDM_EVENT_MESSAGE_SUPPORTED))
{
    uint16_t terminusMaxBufferSize = terminus->maxBufferSize;
    auto rc = co_await eventMessageBufferSize(tid, terminus->maxBufferSize,
                                              terminusMaxBufferSize);
    if (!rc)
    {
        terminus->maxBufferSize =
            std::min(terminus->maxBufferSize, terminusMaxBufferSize);
    }

    uint8_t synchronyConfiguration = 0;
    uint8_t numberEventClassReturned = 0;
    std::vector<uint8_t> eventClass{};
    rc = co_await eventMessageSupported(
        tid, 1, synchronyConfiguration,
        terminus->synchronyConfigurationSupported, numberEventClassReturned,
        eventClass);
    if (rc)
    {
        lg2::error("tid={TID} eventMessageSupported failed rc={RC}, "
                   "setEventReceiver will be skipped.",
                   "TID", tid, "RC", rc);
        terminus->synchronyConfigurationSupported.byte = 0;
    }

    if (!termini.contains(tid))
    {
        co_return PLDM_ERROR;
    }

    if (!terminus->doesSupport(PLDM_PLATFORM, PLDM_SET_EVENT_RECEIVER))
    {
        lg2::error("Terminus {TID} does not support Event", "TID", tid);
        co_return PLDM_ERROR;
    }

    /**
     *  Set Event receiver base on synchronyConfigurationSupported data
     *  use PLDM command SetEventReceiver
     */
    pldm_event_message_global_enable eventMessageGlobalEnable =
        PLDM_EVENT_MESSAGE_GLOBAL_DISABLE;
    uint16_t heartbeatTimer = 0;

    /* Use PLDM_EVENT_MESSAGE_GLOBAL_ENABLE_ASYNC_KEEP_ALIVE when
     * for eventMessageGlobalEnable when the terminus supports that type
     */
    if (terminus->synchronyConfigurationSupported.byte &
        (1 << PLDM_EVENT_MESSAGE_GLOBAL_ENABLE_ASYNC_KEEP_ALIVE))
    {
        heartbeatTimer = HEARTBEAT_TIMEOUT;
        eventMessageGlobalEnable =
            PLDM_EVENT_MESSAGE_GLOBAL_ENABLE_ASYNC_KEEP_ALIVE;
    }
    /* Use PLDM_EVENT_MESSAGE_GLOBAL_ENABLE_ASYNC when
     * for eventMessageGlobalEnable when the terminus does not support
     * PLDM_EVENT_MESSAGE_GLOBAL_ENABLE_ASYNC_KEEP_ALIVE
     * and supports PLDM_EVENT_MESSAGE_GLOBAL_ENABLE_ASYNC type
     */
    else if (terminus->synchronyConfigurationSupported.byte &
             (1 << PLDM_EVENT_MESSAGE_GLOBAL_ENABLE_ASYNC))
    {
        eventMessageGlobalEnable = PLDM_EVENT_MESSAGE_GLOBAL_ENABLE_ASYNC;
    }
    /* Only use PLDM_EVENT_MESSAGE_GLOBAL_ENABLE_POLLING
     * for eventMessageGlobalEnable when the terminus only supports
     * this type
     */
    else if (terminus->synchronyConfigurationSupported.byte &
             (1 << PLDM_EVENT_MESSAGE_GLOBAL_ENABLE_POLLING))
    {
        eventMessageGlobalEnable = PLDM_EVENT_MESSAGE_GLOBAL_ENABLE_POLLING;
    }

    if (eventMessageGlobalEnable != PLDM_EVENT_MESSAGE_GLOBAL_DISABLE)
    {
        auto rc = co_await setEventReceiver(tid, eventMessageGlobalEnable,
                                            PLDM_TRANSPORT_PROTOCOL_TYPE_MCTP,
                                            heartbeatTimer);
        if (rc != PLDM_SUCCESS)
        {
            rc = co_await getPDRs(terminus);
            if (!rc)
            {
                terminus->parsePDRs();
                // look for Platform Configuration PDIs like SensorAuxName
                // etc.
                co_await terminus->scanInventories();
                // update Sensor Objects with information from Platform
                // Configuration PDIs
                co_await terminus->updateAssociations();
                terminus->initalized = true;
                terminus->applyPendingRefresh();
            }
        }
        co_await initEventReceiver(tid);
    }
    co_return PLDM_SUCCESS;
}

exec::task<int> PlatformManager::initEventReceiver(tid_t tid)
{
    if (termini.find(tid) == termini.end())
    {
        co_return PLDM_SUCCESS;
    }

    if (!termini.contains(tid))
    {
        co_return PLDM_ERROR;
    }

    co_return PLDM_SUCCESS;
}

exec::task<int> PlatformManager::getPDRs(std::shared_ptr<Terminus> terminus)
{
    tid_t tid = terminus->getTid();

    uint8_t repositoryState = 0;
    uint32_t recordCount = 0;
    uint32_t repositorySize = 0;
    uint32_t largestRecordSize = 0;
    auto rc = co_await getPDRRepositoryInfo(tid, repositoryState, recordCount,
                                            repositorySize, largestRecordSize);
    if (rc)
    {
        lg2::error(
            "getPDRRepositoryInfo failed and set default value to repositoryState, recordCount and largestRecordSize, rc={RC} tid={TID}.",
            "RC", rc, "TID", tid);
        repositoryState = PLDM_AVAILABLE;
        recordCount = std::numeric_limits<uint32_t>::max();
        largestRecordSize = std::numeric_limits<uint32_t>::max();
    }
    else
    {
        if (recordCount < std::numeric_limits<uint32_t>::max())
        {
            recordCount++;
        }
        if (largestRecordSize < std::numeric_limits<uint32_t>::max())
        {
            largestRecordSize++;
        }
    }

    if (repositoryState != PLDM_AVAILABLE)
    {
        co_return PLDM_ERROR_NOT_READY;
    }

    uint32_t recordHndl = 0;
    uint32_t nextRecordHndl = 0;
    uint32_t nextDataTransferHndl = 0;
    uint8_t transferFlag = 0;
    uint16_t responseCnt = 0;
    constexpr uint16_t recvBufSize = 1024;
    std::vector<uint8_t> recvBuf(recvBufSize);
    uint8_t transferCrc = 0;

    terminus->pdrs.clear();
    uint32_t receivedRecordCount = 0;

    do
    {
        rc = co_await getPDR(tid, recordHndl, 0, PLDM_GET_FIRSTPART,
                             recvBufSize, 0, nextRecordHndl,
                             nextDataTransferHndl, transferFlag, responseCnt,
                             recvBuf, transferCrc);

        if (rc)
        {
            co_return rc;
        }

        if (transferFlag == PLDM_START || transferFlag == PLDM_START_AND_END)
        {
            // single-part transfer
            terminus->pdrs.emplace_back(std::vector<uint8_t>(
                recvBuf.begin(), recvBuf.begin() + responseCnt));
            recordHndl = nextRecordHndl;
        }
        else
        {
            // multipart transfer
            uint32_t receivedRecordSize = responseCnt;
            auto pdrHdr = std::start_lifetime_as<pldm_pdr_hdr>(recvBuf.data());
            uint16_t recordChgNum = le16toh(pdrHdr->record_change_num);
            std::vector<uint8_t> receivedPdr(recvBuf.begin(),
                                             recvBuf.begin() + responseCnt);
            do
            {
                rc = co_await getPDR(
                    tid, recordHndl, nextDataTransferHndl, PLDM_GET_NEXTPART,
                    recvBufSize, recordChgNum, nextRecordHndl,
                    nextDataTransferHndl, transferFlag, responseCnt, recvBuf,
                    transferCrc);
                if (rc)
                {
                    co_return rc;
                }

                receivedPdr.insert(receivedPdr.end(), recvBuf.begin(),
                                   recvBuf.begin() + responseCnt);
                receivedRecordSize += responseCnt;

                if (transferFlag == PLDM_END)
                {
                    terminus->pdrs.emplace_back(receivedPdr);
                    recordHndl = nextRecordHndl;
                }
            } while (nextDataTransferHndl != 0 &&
                     receivedRecordSize < largestRecordSize);
        }
        receivedRecordCount++;
    } while (nextRecordHndl != 0 && receivedRecordCount < recordCount);

    co_return PLDM_SUCCESS;
}

exec::task<int> PlatformManager::getPDR(
    tid_t tid, uint32_t recordHndl, uint32_t dataTransferHndl,
    uint8_t transferOpFlag, uint16_t requestCnt, uint16_t recordChgNum,
    uint32_t& nextRecordHndl, uint32_t& nextDataTransferHndl,
    uint8_t& transferFlag, uint16_t& responseCnt,
    std::vector<uint8_t>& recordData, uint8_t& transferCrc)
{
    Request request(sizeof(pldm_msg_hdr) + PLDM_GET_PDR_REQ_BYTES);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    auto rc = encode_get_pdr_req(0, recordHndl, dataTransferHndl,
                                 transferOpFlag, requestCnt, recordChgNum,
                                 requestMsg, PLDM_GET_PDR_REQ_BYTES);
    if (rc)
    {
        co_return rc;
    }

    const pldm_msg* responseMsg = NULL;
    size_t responseLen = 0;
    rc = co_await terminusManager.SendRecvPldmMsg(tid, request, &responseMsg,
                                                  &responseLen);
    if (rc)
    {
        co_return rc;
    }

    uint8_t completionCode = 0;
    rc = decode_get_pdr_resp(responseMsg, responseLen, &completionCode,
                             &nextRecordHndl, &nextDataTransferHndl,
                             &transferFlag, &responseCnt, recordData.data(),
                             recordData.size(), &transferCrc);
    if (rc)
    {
        co_return rc;
    }
    co_return completionCode;
}

exec::task<int> PlatformManager::fetchSinglePdr(
    tid_t tid, uint32_t recordHandle, std::vector<uint8_t>& pdrOut)
{
    pdrOut.clear();

    uint32_t nextRecordHndl = 0;
    uint32_t nextDataTransferHndl = 0;
    uint8_t transferFlag = 0;
    uint16_t responseCnt = 0;
    constexpr uint16_t recvBufSize = 1024;
    std::vector<uint8_t> recvBuf(recvBufSize);
    uint8_t transferCrc = 0;

    auto rc =
        co_await getPDR(tid, recordHandle, 0, PLDM_GET_FIRSTPART, recvBufSize,
                        0, nextRecordHndl, nextDataTransferHndl, transferFlag,
                        responseCnt, recvBuf, transferCrc);
    if (rc)
    {
        co_return rc;
    }

    pdrOut.insert(pdrOut.end(), recvBuf.begin(), recvBuf.begin() + responseCnt);

    // Multi-part transfer: pull the remaining parts of this single record.
    if (transferFlag != PLDM_START_AND_END && transferFlag != PLDM_END)
    {
        auto pdrHdr = reinterpret_cast<pldm_pdr_hdr*>(recvBuf.data());
        uint16_t recordChgNum = le16toh(pdrHdr->record_change_num);
        while (nextDataTransferHndl != 0)
        {
            rc = co_await getPDR(
                tid, recordHandle, nextDataTransferHndl, PLDM_GET_NEXTPART,
                recvBufSize, recordChgNum, nextRecordHndl, nextDataTransferHndl,
                transferFlag, responseCnt, recvBuf, transferCrc);
            if (rc)
            {
                co_return rc;
            }
            pdrOut.insert(pdrOut.end(), recvBuf.begin(),
                          recvBuf.begin() + responseCnt);
            if (transferFlag == PLDM_END)
            {
                break;
            }
        }
    }

    co_return PLDM_SUCCESS;
}

exec::task<int> PlatformManager::getPDRRepositoryInfo(
    tid_t tid, uint8_t& repositoryState, uint32_t& recordCount,
    uint32_t& repositorySize, uint32_t& largestRecordSize)
{
    Request request(sizeof(pldm_msg_hdr));
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    auto rc = encode_pldm_header_only(PLDM_REQUEST, 0, PLDM_PLATFORM,
                                      PLDM_GET_PDR_REPOSITORY_INFO, requestMsg);
    if (rc)
    {
        co_return rc;
    }

    const pldm_msg* responseMsg = NULL;
    size_t responseLen = 0;
    rc = co_await terminusManager.SendRecvPldmMsg(tid, request, &responseMsg,
                                                  &responseLen);
    if (rc)
    {
        co_return rc;
    }

    uint8_t completionCode = 0;
    uint8_t updateTime[PLDM_TIMESTAMP104_SIZE] = {0};
    uint8_t oemUpdateTime[PLDM_TIMESTAMP104_SIZE] = {0};
    uint8_t dataTransferHandleTimeout = 0;

    rc = decode_get_pdr_repository_info_resp(
        responseMsg, responseLen, &completionCode, &repositoryState, updateTime,
        oemUpdateTime, &recordCount, &repositorySize, &largestRecordSize,
        &dataTransferHandleTimeout);
    if (rc)
    {
        co_return rc;
    }
    co_return completionCode;
}

exec::task<int> PlatformManager::eventMessageBufferSize(
    tid_t tid, uint16_t receiverMaxBufferSize, uint16_t& terminusBufferSize)
{
    Request request(
        sizeof(pldm_msg_hdr) + PLDM_EVENT_MESSAGE_BUFFER_SIZE_REQ_BYTES);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    auto rc = encode_event_message_buffer_size_req(0, receiverMaxBufferSize,
                                                   requestMsg);
    if (rc)
    {
        co_return rc;
    }

    const pldm_msg* responseMsg = NULL;
    size_t responseLen = 0;
    rc = co_await terminusManager.SendRecvPldmMsg(tid, request, &responseMsg,
                                                  &responseLen);
    if (rc)
    {
        co_return rc;
    }

    uint8_t completionCode = 0;
    rc = decode_event_message_buffer_size_resp(
        responseMsg, responseLen, &completionCode, &terminusBufferSize);
    if (rc)
    {
        co_return rc;
    }
    co_return completionCode;
}

exec::task<int> PlatformManager::setEventReceiver(
    tid_t tid, pldm_event_message_global_enable eventMessageGlobalEnable,
    mctp_eid_t eventReceiverEid)
{
    auto requestSize = sizeof(pldm_msg_hdr) + PLDM_SET_EVENT_RECEIVER_REQ_BYTES;
#ifdef OMIT_HEARTBEAT
    if (eventMessageGlobalEnable !=
        PLDM_EVENT_MESSAGE_GLOBAL_ENABLE_ASYNC_KEEP_ALIVE)
    {
        requestSize -= PLDM_HEARTBEAT_BYTES;
    }
#endif
    Request request(requestSize);

    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    auto rc = encode_set_event_receiver_req(0, eventMessageGlobalEnable, 0x0,
                                            eventReceiverEid, 0x0, requestMsg);
    if (rc)
    {
        lg2::error(
            "failed to encode_set_event_receiver_req. tid:{TID}, rc={RC}.",
            "TID", tid, "RC", rc);
        co_return rc;
    }

    const pldm_msg* responseMsg = NULL;
    size_t responseLen = 0;
    rc = co_await terminusManager.SendRecvPldmMsg(tid, request, &responseMsg,
                                                  &responseLen);
    if (rc)
    {
        lg2::error("failed to SendRecvPldmMsg to tid:{TID}, rc={RC}.", "TID",
                   tid, "RC", rc);
        co_return rc;
    }

    uint8_t completionCode = 0;
    rc = decode_set_event_receiver_resp(responseMsg, responseLen,
                                        &completionCode);
    if (rc)
    {
        lg2::error(
            "failed to decode_set_event_receiver_resp. tid:{TID}, rc={RC}.",
            "TID", tid, "RC", rc);
        co_return rc;
    }
    co_return completionCode;
}

exec::task<int> PlatformManager::eventMessageSupported(
    tid_t tid, uint8_t formatVersion, uint8_t& synchronyConfiguration,
    bitfield8_t& synchronyConfigurationSupported,
    uint8_t& numberEventClassReturned, std::vector<uint8_t>& eventClass)
{
    Request request(
        sizeof(pldm_msg_hdr) + PLDM_EVENT_MESSAGE_SUPPORTED_REQ_BYTES);
    auto requestMsg = new (request.data()) pldm_msg;
    auto rc = encode_event_message_supported_req(0, formatVersion, requestMsg);
    if (rc)
    {
        lg2::error(
            "Failed to encode request EventMessageSupported for terminus ID {TID}, error {RC} ",
            "TID", tid, "RC", rc);
        co_return rc;
    }

    const pldm_msg* responseMsg = nullptr;
    size_t responseLen = 0;
    rc = co_await terminusManager.SendRecvPldmMsg(tid, request, &responseMsg,
                                                  &responseLen);
    if (rc)
    {
        lg2::error(
            "Failed to send EventMessageSupported message for terminus {TID}, error {RC}",
            "TID", tid, "RC", rc);
        co_return rc;
    }

    uint8_t completionCode = 0;
    uint8_t eventClassCount = static_cast<uint8_t>(responseLen) -
                              PLDM_EVENT_MESSAGE_SUPPORTED_MIN_RESP_BYTES;
    eventClass.resize(eventClassCount);

    rc = decode_event_message_supported_resp(
        responseMsg, responseLen, &completionCode, &synchronyConfiguration,
        &synchronyConfigurationSupported, &numberEventClassReturned,
        eventClass.data(), eventClassCount);
    if (rc)
    {
        lg2::error(
            "Failed to decode response EventMessageSupported for terminus ID {TID}, error {RC} ",
            "TID", tid, "RC", rc);
        co_return rc;
    }

    if (completionCode != PLDM_SUCCESS)
    {
        lg2::error(
            "Error : EventMessageSupported for terminus ID {TID}, complete code {CC}.",
            "TID", tid, "CC", completionCode);
        co_return completionCode;
    }

    co_return completionCode;
}

exec::task<int> PlatformManager::getFRURecordTableMetadata(pldm_tid_t tid,
                                                           uint16_t* total)
{
    Request request(
        sizeof(pldm_msg_hdr) + PLDM_GET_FRU_RECORD_TABLE_METADATA_REQ_BYTES);
    auto requestMsg = new (request.data()) pldm_msg;

    auto rc = encode_get_fru_record_table_metadata_req(
        0, requestMsg, PLDM_GET_FRU_RECORD_TABLE_METADATA_REQ_BYTES);
    if (rc)
    {
        lg2::error(
            "Failed to encode request GetFRURecordTableMetadata for terminus ID {TID}, error {RC} ",
            "TID", tid, "RC", rc);
        co_return rc;
    }

    const pldm_msg* responseMsg = nullptr;
    size_t responseLen = 0;

    rc = co_await terminusManager.sendRecvPldmMsg(tid, request, &responseMsg,
                                                  &responseLen);
    if (rc)
    {
        lg2::error(
            "Failed to send GetFRURecordTableMetadata message for terminus {TID}, error {RC}",
            "TID", tid, "RC", rc);
        co_return rc;
    }

    uint8_t completionCode = 0;
    if (responseMsg == nullptr || !responseLen)
    {
        lg2::error(
            "No response data for GetFRURecordTableMetadata for terminus {TID}",
            "TID", tid);
        co_return rc;
    }

    uint8_t fru_data_major_version = 0, fru_data_minor_version = 0;
    uint32_t fru_table_maximum_size = 0, fru_table_length = 0;
    uint16_t total_record_set_identifiers = 0;
    uint32_t checksum = 0;
    rc = decode_get_fru_record_table_metadata_resp(
        responseMsg, responseLen, &completionCode, &fru_data_major_version,
        &fru_data_minor_version, &fru_table_maximum_size, &fru_table_length,
        &total_record_set_identifiers, total, &checksum);

    if (rc)
    {
        lg2::error(
            "Failed to decode response GetFRURecordTableMetadata for terminus ID {TID}, error {RC} ",
            "TID", tid, "RC", rc);
        co_return rc;
    }

    if (completionCode != PLDM_SUCCESS)
    {
        lg2::error(
            "Error : GetFRURecordTableMetadata for terminus ID {TID}, complete code {CC}.",
            "TID", tid, "CC", completionCode);
        co_return completionCode;
    }

    co_return rc;
}

exec::task<int> PlatformManager::getFRURecordTable(
    pldm_tid_t tid, const uint32_t dataTransferHndl,
    const uint8_t transferOpFlag, uint32_t* nextDataTransferHndl,
    uint8_t* transferFlag, size_t* responseCnt,
    std::vector<uint8_t>& recordData)
{
    Request request(sizeof(pldm_msg_hdr) + PLDM_GET_FRU_RECORD_TABLE_REQ_BYTES);
    auto requestMsg = new (request.data()) pldm_msg;

    auto rc = encode_get_fru_record_table_req(
        0, dataTransferHndl, transferOpFlag, requestMsg,
        PLDM_GET_FRU_RECORD_TABLE_REQ_BYTES);
    if (rc != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to encode request GetFRURecordTable for terminus ID {TID}, error {RC} ",
            "TID", tid, "RC", rc);
        co_return rc;
    }

    const pldm_msg* responseMsg = nullptr;
    size_t responseLen = 0;

    rc = co_await terminusManager.sendRecvPldmMsg(tid, request, &responseMsg,
                                                  &responseLen);
    if (rc)
    {
        lg2::error(
            "Failed to send GetFRURecordTable message for terminus {TID}, error {RC}",
            "TID", tid, "RC", rc);
        co_return rc;
    }

    uint8_t completionCode = 0;
    if (responseMsg == nullptr || !responseLen)
    {
        lg2::error("No response data for GetFRURecordTable for terminus {TID}",
                   "TID", tid);
        co_return rc;
    }

    auto responsePtr = reinterpret_cast<const struct pldm_msg*>(responseMsg);
    rc = decode_get_fru_record_table_resp(
        responsePtr, responseLen, &completionCode, nextDataTransferHndl,
        transferFlag, recordData.data(), responseCnt);

    if (rc)
    {
        lg2::error(
            "Failed to decode response GetFRURecordTable for terminus ID {TID}, error {RC} ",
            "TID", tid, "RC", rc);
        co_return rc;
    }

    if (completionCode != PLDM_SUCCESS)
    {
        lg2::error(
            "Error : GetFRURecordTable for terminus ID {TID}, complete code {CC}.",
            "TID", tid, "CC", completionCode);
        co_return completionCode;
    }

    co_return rc;
}

void PlatformManager::updateInventoryWithFru(
    pldm_tid_t tid, const uint8_t* fruData, const size_t fruLen)
{
    if (tid == PLDM_TID_RESERVED || !termini.contains(tid) || !termini[tid])
    {
        lg2::error("Invalid terminus {TID}", "TID", tid);
        return;
    }

    termini[tid]->updateInventoryWithFru(fruData, fruLen);
}

exec::task<int> PlatformManager::getFRURecordTables(
    pldm_tid_t tid, const uint16_t& totalTableRecords,
    std::vector<uint8_t>& fruData)
{
    if (!totalTableRecords)
    {
        lg2::info("Fru record table has 0 records");
        co_return PLDM_ERROR;
    }

    uint32_t dataTransferHndl = 0;
    uint32_t nextDataTransferHndl = 0;
    uint8_t transferFlag = 0;
    uint8_t transferOpFlag = PLDM_GET_FIRSTPART;
    size_t responseCnt = 0;
    std::vector<uint8_t> recvBuf(PLDM_PLATFORM_GETPDR_MAX_RECORD_BYTES);

    size_t fruLength = 0;
    std::vector<uint8_t> receivedFru(0);
    do
    {
        auto rc = co_await getFRURecordTable(
            tid, dataTransferHndl, transferOpFlag, &nextDataTransferHndl,
            &transferFlag, &responseCnt, recvBuf);

        if (rc)
        {
            lg2::error(
                "Failed to get Fru Record Data for terminus {TID}, error: {RC}, first part of data handle {RECORD}",
                "TID", tid, "RC", rc, "RECORD", dataTransferHndl);
            co_return rc;
        }

        receivedFru.insert(receivedFru.end(), recvBuf.begin(),
                           recvBuf.begin() + responseCnt);
        fruLength += responseCnt;
        if (transferFlag == PLDM_PLATFORM_TRANSFER_START_AND_END ||
            transferFlag == PLDM_PLATFORM_TRANSFER_END)
        {
            break;
        }

        // multipart transfer
        dataTransferHndl = nextDataTransferHndl;
        transferOpFlag = PLDM_GET_NEXTPART;

    } while (nextDataTransferHndl != 0);

    if (fruLength != receivedFru.size())
    {
        lg2::error(
            "Size of Fru Record Data {SIZE} for terminus {TID} is different the responded size {RSPSIZE}.",
            "SIZE", receivedFru.size(), "RSPSIZE", fruLength);
        co_return PLDM_ERROR_INVALID_LENGTH;
    }

    fruData = receivedFru;

    co_return PLDM_SUCCESS;
}

} // namespace platform_mc
} // namespace pldm
