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

#include "terminus_manager.hpp"

#include <phosphor-logging/lg2.hpp>

namespace pldm
{
namespace platform_mc
{

requester::Coroutine PlatformManager::initTerminus()
{
    for (auto& [tid, terminus] : termini)
    {
        if (terminus->doesSupport(PLDM_PLATFORM))
        {
            uint16_t terminusMaxBufferSize = terminus->maxBufferSize;
            auto rc = co_await eventMessageBufferSize(
                tid, terminus->maxBufferSize, terminusMaxBufferSize);
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
                terminus->synchronyConfigurationSupported,
                numberEventClassReturned, eventClass);
            if (rc)
            {
                lg2::error("failed to send eventMessageSupported, rc={RC}.",
                           "RC", rc);
                terminus->synchronyConfigurationSupported = 0;
            }

            if (!terminus->initalized)
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
                    terminus->updateAssociations();
                    terminus->initalized = true;
                }
            }
            co_await initEventReceiver(tid);
        }
    }
    co_return PLDM_SUCCESS;
}

requester::Coroutine PlatformManager::initEventReceiver(tid_t tid)
{
    if (termini.find(tid) == termini.end())
    {
        co_return PLDM_SUCCESS;
    }

    auto& terminus = termini[tid];
    uint8_t rc = PLDM_SUCCESS;
    if (terminus->synchronyConfigurationSupported &
        (1 << PLDM_EVENT_MESSAGE_GLOBAL_ENABLE_ASYNC))
    {
        rc = co_await setEventReceiver(tid,
                                       PLDM_EVENT_MESSAGE_GLOBAL_ENABLE_ASYNC,
                                       terminusManager.getLocalEid());
        if (rc)
        {
            auto mctpInfo = terminusManager.toMctpInfo(tid);
            if (!mctpInfo)
            {
                lg2::error(
                    "Failed to send setEventReceiver to tid:{TID}, rc={RC}. "
                    "No match for tid:{TID} in mctpInfo.",
                    "TID", tid, "RC", rc);
            }
            else
            {
                auto destEid = std::get<0>(mctpInfo.value());
                lg2::error(
                    "failed to send setEventReceiver to tid:{TID}, rc={RC}, localEid:{EID}, destEid:{DESTEID}",
                    "TID", tid, "RC", rc, "EID", terminusManager.getLocalEid(),
                    "DESTEID", destEid);
            }
        }
    }
    co_return rc;
}

requester::Coroutine
    PlatformManager::getPDRs(std::shared_ptr<Terminus> terminus)
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
        rc =
            co_await getPDR(tid, recordHndl, 0, PLDM_GET_FIRSTPART, recvBufSize,
                            0, nextRecordHndl, nextDataTransferHndl,
                            transferFlag, responseCnt, recvBuf, transferCrc);

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
            uint32_t receivedRecordSize = 0;
            auto pdrHdr = reinterpret_cast<pldm_pdr_hdr*>(recvBuf.data());
            uint16_t recordChgNum = le16toh(pdrHdr->record_change_num);
            std::vector<uint8_t> receivedPdr(recvBuf.begin(),
                                             recvBuf.begin() + responseCnt);
            do
            {
                rc = co_await getPDR(tid, recordHndl, nextDataTransferHndl,
                                     PLDM_GET_NEXTPART, recvBufSize,
                                     recordChgNum, nextRecordHndl,
                                     nextDataTransferHndl, transferFlag,
                                     responseCnt, recvBuf, transferCrc);
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

requester::Coroutine PlatformManager::getPDR(
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

    uint8_t completionCode;
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

requester::Coroutine PlatformManager::getPDRRepositoryInfo(
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

requester::Coroutine PlatformManager::eventMessageBufferSize(
    tid_t tid, uint16_t receiverMaxBufferSize, uint16_t& terminusBufferSize)
{
    Request request(sizeof(pldm_msg_hdr) +
                    PLDM_EVENT_MESSAGE_BUFFER_SIZE_REQ_BYTES);
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

    uint8_t completionCode;
    rc = decode_event_message_buffer_size_resp(
        responseMsg, responseLen, &completionCode, &terminusBufferSize);
    if (rc)
    {
        co_return rc;
    }
    co_return completionCode;
}

requester::Coroutine PlatformManager::setEventReceiver(
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

    uint8_t completionCode;
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

requester::Coroutine PlatformManager::eventMessageSupported(
    tid_t tid, uint8_t formatVersion, uint8_t& synchronyConfiguration,
    uint8_t& synchronyConfigurationSupported, uint8_t& numberEventClassReturned,
    std::vector<uint8_t>& eventClass)
{
    Request request(sizeof(pldm_msg_hdr) +
                    PLDM_EVENT_MESSAGE_SUPPORTED_REQ_BYTES);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    auto rc = encode_event_message_supported_req(0, formatVersion, requestMsg);
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
    uint8_t* pEventClass = 0;
    rc = decode_event_message_supported_resp(
        responseMsg, responseLen, &completionCode, &synchronyConfiguration,
        &synchronyConfigurationSupported, &numberEventClassReturned,
        &pEventClass);
    if (rc)
    {
        co_return rc;
    }

    eventClass.insert(eventClass.end(), pEventClass,
                      pEventClass + numberEventClassReturned);
    co_return completionCode;
}
} // namespace platform_mc
} // namespace pldm
