/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION &
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
#include "mockup_responder.hpp"

#include "libpldm/entity.h"
#include "libpldm/pdr.h"
#include "libpldm/platform.h"

#include "common/types.hpp"
#include "common/utils.hpp"
#include "libpldmresponder/base.hpp"
#include "libpldmresponder/pdr_utils.hpp"
#include "pdr_json_parser.hpp"
#include "pldmd/handler.hpp"
#include "sensor_to_dbus.hpp"

#include <sys/socket.h>

#include <phosphor-logging/lg2.hpp>
#include <phosphor-logging/log.hpp>

#include <chrono>
#include <ctime>
#include <iostream>
#include <thread>

#define MCTP_DEMUX_PREFIX 3

const uint8_t MCTP_TAG_PLDM = 0;
const uint8_t MCTP_MSG_TAG_RESP = MCTP_TAG_PLDM;
const uint8_t MCTP_MSG_TYPE_PLDM = 1;
const uint8_t MCTP_TAG_OWNER_REQ = 0x01;
const uint8_t MCTP_MSG_TAG_REQ = (MCTP_TAG_OWNER_REQ << 3) | MCTP_TAG_PLDM;
const uint8_t UUID_LENGTH = 16;

int socketFD;

using namespace pldm::utils;
using namespace phosphor::logging;
using namespace pldm::responder::base;
using Type = uint8_t;

typedef struct pldm_pdr
{
    uint32_t record_count;
    uint32_t size;
    pldm_pdr_record* first;
    pldm_pdr_record* last;
} pldm_pdr;

namespace MockupResponder
{

static const std::map<uint8_t, std::vector<uint8_t>> capabilities{
    {PLDM_BASE,
     {PLDM_GET_TID, PLDM_GET_PLDM_VERSION, PLDM_GET_PLDM_TYPES,
      PLDM_GET_PLDM_COMMANDS}},
    {PLDM_PLATFORM,
     {PLDM_GET_PDR, PLDM_SET_STATE_EFFECTER_STATES, PLDM_SET_EVENT_RECEIVER,
      PLDM_GET_SENSOR_READING, PLDM_GET_STATE_SENSOR_READINGS,
      PLDM_SET_NUMERIC_EFFECTER_VALUE, PLDM_GET_NUMERIC_EFFECTER_VALUE,
      PLDM_PLATFORM_EVENT_MESSAGE}},
    {PLDM_BIOS,
     {PLDM_GET_DATE_TIME, PLDM_SET_DATE_TIME, PLDM_GET_BIOS_TABLE,
      PLDM_GET_BIOS_ATTRIBUTE_CURRENT_VALUE_BY_HANDLE,
      PLDM_SET_BIOS_ATTRIBUTE_CURRENT_VALUE, PLDM_SET_BIOS_TABLE}},
    {PLDM_FRU,
     {PLDM_GET_FRU_RECORD_TABLE_METADATA, PLDM_GET_FRU_RECORD_TABLE,
      PLDM_GET_FRU_RECORD_BY_OPTION}}};

MockupResponder::MockupResponder(bool verbose, sdeventplus::Event& event,
                                 sdbusplus::asio::object_server& server,
                                 uint8_t eid, std::string pdrPath,
                                 uint16_t terminusMaxBufferSize,
                                 uint8_t* uuidValue) :
    event(event),
    verbose(verbose), mockEid(eid), server(server), eventReceiverEid(0),
    jsonParser(verbose, server),
    mockTerminusMaxBufferSize(terminusMaxBufferSize)
{
    std::memcpy(this->mockUUID, uuidValue, sizeof(this->mockUUID));
    std::ostringstream uuidStream;
    for (int i = 0; i < 16; ++i)
    {
        uuidStream << std::hex << std::setw(2) << std::setfill('0')
                   << static_cast<int>(mockUUID[i]) << " ";
    }
    lg2::info("MockupResponder initialized with UUID: {UUID}", "UUID",
              uuidStream.str());
    this->sockFd = initSocket();
    socketFD = this->sockFd;
    readJsonPdrs(pdrPath);
}

void MockupResponder::readJsonPdrs(std::string& path)
{
    Json json = pldm::responder::pdr_utils::readJson(path);
    this->pdrRepo =
        (pldm_pdr*)jsonParser.parse(json, (::pldm_pdr*)this->pdrRepo);
    if (verbose)
    {
        lg2::info("Finished parsing JSON PDRs.");
    }
}

int MockupResponder::initSocket()
{
    if (verbose)
    {
        lg2::info("connect to Mockup EID({EID})", "EID", mockEid);
    }

    auto fd = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd == -1)
    {
        lg2::error("Socket creation failed, errno = {ERROR}.", "ERROR", errno);
        return -1;
    }

    struct sockaddr_un addr = {};

    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, MCTP_SOCKET_PATH, sizeof(MCTP_SOCKET_PATH) - 1);

    if (::connect(fd, (struct sockaddr*)&addr,
                  sizeof(MCTP_SOCKET_PATH) + sizeof(addr.sun_family) - 1) == -1)
    {
        lg2::error(
            "connect() error to mctp-demux-daemon, errno = {ERROR}, {STRERROR}",
            "ERROR", errno, "STRERROR", std::strerror(errno));
        close(fd);
        return -1;
    }

    uint8_t prefix = MCTP_MSG_EMU_PREFIX;
    uint8_t type = MCTP_MSG_TYPE_PLDM;

    ssize_t ret = ::write(fd, &prefix, sizeof(uint8_t));
    if (ret < 0)
    {
        lg2::error(
            "Failed to write mockup prefix code to socket, errno = {ERROR}, {STRERROR}",
            "ERROR", errno, "STRERROR", strerror(errno));
        close(fd);
        return -1;
    }

    ret = ::write(fd, &type, sizeof(uint8_t));
    if (ret < 0)
    {
        lg2::error(
            "Failed to write VDM type code to socket, errno = {ERROR}, {STRERROR}",
            "ERROR", errno, "STRERROR", strerror(errno));
        close(fd);
        return -1;
    }

    ret = ::write(fd, &mockEid, sizeof(uint8_t));
    if (ret == -1)
    {
        lg2::error("Failed to write eid to socket, errno = {ERROR}, {STRERROR}",
                   "ERROR", errno, "STRERROR", strerror(errno));
        close(fd);
        return -1;
    }

    auto callback = [this](sdeventplus::source::IO& io, int fd,
                           uint32_t revents) mutable {
        lg2::info("Received message");

        if (!(revents & EPOLLIN))
        {
            return;
        }

        ssize_t peekedLength = recv(fd, nullptr, 0, MSG_PEEK | MSG_TRUNC);
        if (peekedLength == 0)
        {
            lg2::info("Socket closed, exiting event loop.");
            io.get_event().exit(0);
        }
        else if (peekedLength <= -1)
        {
            lg2::error("recv system call failed, errno = {ERROR}.", "ERROR",
                       errno);
            return;
        }

        std::vector<uint8_t> requestMsg(peekedLength);
        auto recvDataLength =
            recv(fd, static_cast<void*>(requestMsg.data()), peekedLength, 0);
        if (recvDataLength != peekedLength)
        {
            lg2::error("Failure to read peeked length packet. peekedLength="
                       "{PEEKEDLENGTH} recvDataLength={RECVDATALENGTH}",
                       "PEEKEDLENGTH", peekedLength, "RECVDATALENGTH",
                       recvDataLength);
            return;
        }

        if (requestMsg[2] != MCTP_MSG_TYPE_PLDM)
        {
            lg2::error("Received non PLDM message type={TYPE}", "TYPE",
                       requestMsg[1]);
            return;
        }

        struct iovec iov[2]{};
        msghdr msg{};

        auto response = processRxMsg(requestMsg);
        if (response.has_value())
        {
            constexpr uint8_t tagOwnerBitPos = 3;
            constexpr uint8_t tagOwnerMask = ~(1 << tagOwnerBitPos);
            // Set tag owner bit to 0 for PLDM responses
            requestMsg[0] = requestMsg[0] & tagOwnerMask;
            iov[0].iov_base = &requestMsg[0];
            iov[0].iov_len = sizeof(requestMsg[0]) + sizeof(requestMsg[1]) +
                             sizeof(requestMsg[2]);
            iov[1].iov_base = (*response).data();
            iov[1].iov_len = (*response).size();

            msg.msg_iov = iov;
            msg.msg_iovlen = sizeof(iov) / sizeof(iov[0]);

            if (verbose)
            {
                printBuffer(Rx, requestMsg);
                printBuffer(Tx, *response);
            }

            ssize_t result = sendmsg(fd, &msg, 0);
            if (result < 0)
            {
                lg2::error("sendmsg system call failed, errno: {ERROR}.",
                           "ERROR", errno);
            }
            lg2::info("Response sent.");
        }
    };

    io = std::make_unique<sdeventplus::source::IO>(event, fd, EPOLLIN,
                                                   std::move(callback));
    return fd;
}

Response MockupResponder::getPLDMTypes(const pldm_msg* request,
                                       [[maybe_unused]] size_t payloadLength)
{
    lg2::info("GetPLDMTypes");

    std::array<bitfield8_t, 8> types{};
    for (const auto& type : capabilities)
    {
        auto index = type.first / 8;
        auto bit = type.first - (index * 8);
        types[index].byte |= 1 << bit;
    }

    Response response(sizeof(pldm_msg_hdr) + PLDM_GET_TYPES_RESP_BYTES, 0);
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    auto rc = encode_get_types_resp(request->hdr.instance_id, PLDM_SUCCESS,
                                    types.data(), responsePtr);
    if (rc != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to encode getPLDMTypes response, instance_id: {ID}, RC: {RC}",
            "ID", request->hdr.instance_id, "RC", rc);
        return CmdHandler::ccOnlyResponse(request, rc);
    }
    return response;
}

Response MockupResponder::getPLDMCommands(const pldm_msg* request,
                                          size_t payloadLength)
{
    lg2::info("GetPLDMCommands");

    ver32_t version{};
    Type type;

    Response response(sizeof(pldm_msg_hdr) + PLDM_GET_COMMANDS_RESP_BYTES, 0);
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());

    auto rc = decode_get_commands_req(request, payloadLength, &type, &version);

    if (rc != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to decode getCommands request, instance_id: {ID}, RC: {RC}",
            "ID", request->hdr.instance_id, "RC", rc);
        return CmdHandler::ccOnlyResponse(request, rc);
    }

    std::array<bitfield8_t, 32> cmds{};
    if (capabilities.find(type) == capabilities.end())
    {
        return CmdHandler::ccOnlyResponse(request,
                                          PLDM_ERROR_INVALID_PLDM_TYPE);
    }

    for (const auto& cmd : capabilities.at(type))
    {
        auto index = cmd / 8;
        auto bit = cmd % 8;
        cmds[index].byte |= 1 << bit;
    }

    rc = encode_get_commands_resp(request->hdr.instance_id, PLDM_SUCCESS,
                                  cmds.data(), responsePtr);
    if (rc != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to encode getCommands response, instance_id: {ID}, RC: {RC}",
            "ID", request->hdr.instance_id, "RC", rc);
        return CmdHandler::ccOnlyResponse(request, rc);
    }

    return response;
}

Response MockupResponder::getPLDMVersion(const pldm_msg* request,
                                         size_t payloadLength)
{

    lg2::info("GetPLDMVersion");

    uint32_t transferHandle;
    Type type;
    uint8_t transferFlag;
    const std::map<uint8_t, ver32_t> versions{
        {PLDM_BASE, {0x00, 0xF0, 0xF0, 0xF1}},
        {PLDM_PLATFORM, {0x00, 0xF0, 0xF2, 0xF1}},
        {PLDM_BIOS, {0x00, 0xF0, 0xF0, 0xF1}},
        {PLDM_FRU, {0x00, 0xF0, 0xF0, 0xF1}},
#ifdef OEM_IBM
        {PLDM_OEM, {0x00, 0xF0, 0xF0, 0xF1}},
#endif
    };

    Response response(sizeof(pldm_msg_hdr) + PLDM_GET_VERSION_RESP_BYTES, 0);
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());

    auto rc = decode_get_version_req(request, payloadLength, &transferHandle,
                                     &transferFlag, &type);
    if (rc != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to decode getVersion request, instance_id: {ID}, RC: {RC}",
            "ID", request->hdr.instance_id, "RC", rc);
        return CmdHandler::ccOnlyResponse(request, rc);
    }

    ver32_t version{};
    auto search = versions.find(type);
    if (search == versions.end())
    {
        lg2::error(
            "PLDM type not found for getVersion request, instance_id: {ID}, Type: {TYPE}",
            "ID", request->hdr.instance_id, "TYPE", type);
        return CmdHandler::ccOnlyResponse(request,
                                          PLDM_ERROR_INVALID_PLDM_TYPE);
    }

    memcpy(&version, &(search->second), sizeof(version));

    rc = encode_get_version_resp(request->hdr.instance_id, PLDM_SUCCESS, 0,
                                 PLDM_START_AND_END, &version,
                                 sizeof(pldm_version), responsePtr);
    if (rc != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to encode getVersion response, instance_id: {ID}, RC: {RC}",
            "ID", request->hdr.instance_id, "RC", rc);
        return CmdHandler::ccOnlyResponse(request, rc);
    }

    return response;
}

Response MockupResponder::getTID(const pldm_msg* request,
                                 [[maybe_unused]] size_t payloadLength,
                                 MockupResponder& responder)
{
    lg2::info("GetTID");

    uint8_t tid = responder.getTid();

    Response response(sizeof(pldm_msg_hdr) + PLDM_GET_TID_RESP_BYTES, 0);
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    auto rc = encode_get_tid_resp(request->hdr.instance_id, PLDM_SUCCESS, tid,
                                  responsePtr);
    if (rc != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to encode getTID response, instance_id: {ID}, RC: {RC}",
            "ID", request->hdr.instance_id, "RC", rc);
        return CmdHandler::ccOnlyResponse(request, rc);
    }

    return response;
}

Response MockupResponder::setTID(const pldm_msg* request, size_t payloadLength,
                                 MockupResponder& responder)
{
    lg2::info("SetTID");
    uint8_t tid;

    auto rc = decode_set_tid_req(request, payloadLength, &tid);
    if (rc != PLDM_SUCCESS)
    {
        lg2::error("Failed to decode setTID request, RC: {RC}", "RC", rc);
    }
    else
    {
        responder.setTid(tid);
    }

    return CmdHandler::ccOnlyResponse(request, rc);
}

Response MockupResponder::getTerminusUID(const pldm_msg* request,
                                         MockupResponder& responder)
{
    lg2::info("GetTerminusUID");

    // eid 31's UUID hardcoded in mctp_ctrl_emu.json
    // uint8_t uuidValue[16] = {0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    //                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11};
    uint8_t* mockUpUUID = responder.getUUID();

    Response response(sizeof(pldm_msg_hdr) + PLDM_GET_TERMINUS_UID_RESP_BYTES,
                      0);

    auto* msg = reinterpret_cast<pldm_msg*>(response.data());

    auto rc = encode_get_terminus_uid_resp(
        request->hdr.instance_id, PLDM_SUCCESS, mockUpUUID, UUID_LENGTH, msg);

    if (rc != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to encode getTerminusUID response, instance_id: {ID}, RC:{RC}",
            "ID", request->hdr.instance_id, "RC", rc);
        return CmdHandler::ccOnlyResponse(request, rc);
    }

    return response;
}

Response MockupResponder::getPdr(const pldm_msg* request, size_t payloadLength,
                                 pldm_pdr* pdrRepoRef)
{
    lg2::info("GetPDR");

    ::pldm_pdr* pdrRepo = reinterpret_cast<::pldm_pdr*>(pdrRepoRef);

    Response response(sizeof(pldm_msg_hdr) + PLDM_GET_PDR_MIN_RESP_BYTES, 0);

    uint32_t recordHandle{};
    uint32_t dataTransferHandle{};
    uint8_t transferOpFlag{};
    uint16_t reqSizeBytes{};
    uint16_t recordChangeNum{};

    auto rc = decode_get_pdr_req(request, payloadLength, &recordHandle,
                                 &dataTransferHandle, &transferOpFlag,
                                 &reqSizeBytes, &recordChangeNum);

    // Log the recordHandle information
    lg2::info(
        "Decoded getPDR request with instance_id: {ID}, recordHandle: {RECORDHANDLE}",
        "ID", request->hdr.instance_id, "RECORDHANDLE", recordHandle);

    if (rc != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to decode getPDR request, instance_id: {ID}, RC: {RC}",
            "ID", request->hdr.instance_id, "RC", rc);
        return CmdHandler::ccOnlyResponse(request, rc);
    }

    uint16_t respSizeBytes{};
    uint8_t* recordData = nullptr;
    try
    {

        uint8_t* pdrData{};
        uint32_t pdrSize{};
        uint32_t pdrNextRecordHandle{};

        auto record = pldm_pdr_find_record(pdrRepo, recordHandle, &pdrData,
                                           &pdrSize, &pdrNextRecordHandle);

        lg2::info(
            "pdrData={PDRDATA}, pdrSize={PDRSIZE}, pdrNextHandle={PDRNEXTHANDLE}",
            "PDRDATA", *pdrData, "PDRSIZE", pdrSize, "PDRNEXTHANDLE",
            pdrNextRecordHandle);

        if (record == NULL)
        {
            return CmdHandler::ccOnlyResponse(
                request, PLDM_PLATFORM_INVALID_RECORD_HANDLE);
        }

        if (reqSizeBytes)
        {
            respSizeBytes = pdrSize;
            if (respSizeBytes > reqSizeBytes)
            {
                respSizeBytes = reqSizeBytes;
            }
            recordData = pdrData;
        }
        response.resize(sizeof(pldm_msg_hdr) + PLDM_GET_PDR_MIN_RESP_BYTES +
                            respSizeBytes,
                        0);
        auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());
        rc = encode_get_pdr_resp(request->hdr.instance_id, PLDM_SUCCESS,
                                 pdrNextRecordHandle, 0, PLDM_START_AND_END,
                                 respSizeBytes, recordData, 0, responsePtr);

        if (rc != PLDM_SUCCESS)
        {
            lg2::error(
                "Failed to encode getPDR response, instance_id: {ID}, RC: {RC}",
                "ID", request->hdr.instance_id, "RC", rc);
            return CmdHandler::ccOnlyResponse(request, rc);
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Error accessing PDR, HANDLE={RECORDHANDLE}, {ERROR}",
                   "RECORDHANDLE", recordHandle, "ERROR", e);
        return CmdHandler::ccOnlyResponse(request, PLDM_ERROR);
    }
    return response;
}

Response MockupResponder::getStateSensorReadings(const pldm_msg* request,
                                                 size_t payloadLength)
{
    lg2::info("GetStateSensorReadings");

    uint16_t sensorId{};
    bitfield8_t rearm{};
    uint8_t reserved{};
    bool flag = false;
    uint8_t completion_code = PLDM_SUCCESS;

    auto rc = decode_get_state_sensor_readings_req(
        request, payloadLength, &sensorId, &rearm, &reserved);
    if (rc != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to decode getStateSensorReadings request, instance_id: {ID}, RC: {RC}",
            "ID", request->hdr.instance_id, "RC", rc);
        return CmdHandler::ccOnlyResponse(request, rc);
    }

    uint8_t compSensorCnt = 0;
    std::vector<get_sensor_state_field> stateFields;

    for (auto& s : sensors)
    {
        if (s->sensorId == sensorId)
        {
            flag = true;
            compSensorCnt = s->composite_count;
            stateFields.resize(compSensorCnt);

            for (uint8_t i = 0; i < compSensorCnt; i++)
            {
                stateFields[i].sensor_op_state = PLDM_SENSOR_ENABLED;
                stateFields[i].previous_state = s->value;
                stateFields[i].present_state = s->value;
                stateFields[i].event_state = s->value;
            }
        }
    }

    Response response(sizeof(pldm_msg_hdr) +
                          PLDM_GET_STATE_SENSOR_READINGS_MIN_RESP_BYTES +
                          sizeof(get_sensor_state_field) * compSensorCnt,
                      0);

    if (flag)
    {
        completion_code = PLDM_SUCCESS;
    }
    else
    {
        return CmdHandler::ccOnlyResponse(request,
                                          PLDM_PLATFORM_INVALID_SENSOR_ID);
    }
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    rc = encode_get_state_sensor_readings_resp(request->hdr.instance_id,
                                               completion_code, compSensorCnt,
                                               stateFields.data(), responsePtr);
    if (rc != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to encode getStateSensorReadings response, instance_id:{ID}, RC: {RC}",
            "ID", request->hdr.instance_id, "RC", rc);
        return CmdHandler::ccOnlyResponse(request, rc);
    }

    return response;
}

Response MockupResponder::getNumericEffecterValue(const pldm_msg* request,
                                                  size_t payloadLength)
{
    lg2::info("GetNumericEffecterValue");

    bool flag = false;
    size_t hdrSize = sizeof(pldm_msg_hdr);
    Response response(
        hdrSize + PLDM_GET_NUMERIC_EFFECTER_VALUE_MIN_RESP_BYTES + 6, 0);

    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());

    uint16_t effecter_id{};

    auto rc = decode_get_numeric_effecter_value_req(request, payloadLength,
                                                    &effecter_id);

    if (rc != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to decode getNumericEffecterValue request, instance_id:{ID}, RC: {RC}",
            "ID", request->hdr.instance_id, "RC", rc);
        return CmdHandler::ccOnlyResponse(request, rc);
    }

    uint8_t completion_code = 0;
    uint8_t effecter_data_size = PLDM_EFFECTER_DATA_SIZE_UINT32;
    uint8_t effecter_operational_state =
        EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING;
    uint32_t pending_value = 0;
    uint32_t present_value = 0;

    for (auto& e : effecters)
    {
        if (e->effecterId == effecter_id)
        {
            flag = true;
            pending_value = e->value;
            present_value = e->value;
        }
    }

    if (flag)
    {
        completion_code = PLDM_SUCCESS;
    }
    else
    {
        return CmdHandler::ccOnlyResponse(request,
                                          PLDM_PLATFORM_INVALID_EFFECTER_ID);
    }
    rc = encode_get_numeric_effecter_value_resp(
        request->hdr.instance_id, completion_code, effecter_data_size,
        effecter_operational_state, reinterpret_cast<uint8_t*>(&pending_value),
        reinterpret_cast<uint8_t*>(&present_value), responsePtr,
        response.size() - hdrSize);

    if (rc != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to encode getNumericEffecterValue response, instance_id:{ID}, RC: {RC}",
            "ID", request->hdr.instance_id, "RC", rc);
        return CmdHandler::ccOnlyResponse(request, rc);
    }
    return response;
}

Response MockupResponder::getStateEffecterStates(const pldm_msg* request,
                                                 size_t payloadLength)
{
    lg2::info("GetStateEffecterStates");

    uint8_t completion_code = PLDM_SUCCESS;
    bool flag = false;

    uint16_t effecterId{};
    auto rc = decode_get_state_effecter_states_req(request, payloadLength,
                                                   &effecterId);

    if (rc != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to decode getStateEffecterStates request, instance_id: {ID}, RC: {RC}",
            "ID", request->hdr.instance_id, "RC", rc);
        return CmdHandler::ccOnlyResponse(request, rc);
    }

    uint8_t compEffecterCnt = 0;
    std::vector<get_effecter_state_field> stateFields;

    for (auto& e : effecters)
    {
        if (e->effecterId == effecterId)
        {
            flag = true;
            compEffecterCnt = e->composite_count;
            stateFields.resize(compEffecterCnt);

            for (uint8_t i = 0; i < compEffecterCnt; i++)
            {
                stateFields[i].effecter_op_state =
                    EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING;
                stateFields[i].pending_state = e->value;
                stateFields[i].present_state = e->value;
            }
        }
    }

    Response response(sizeof(pldm_msg_hdr) +
                          PLDM_GET_STATE_EFFECTER_STATES_MIN_RESP_BYTES +
                          sizeof(get_effecter_state_field) * compEffecterCnt,
                      0);

    if (flag)
    {
        completion_code = PLDM_SUCCESS;
    }
    else
    {
        return CmdHandler::ccOnlyResponse(request,
                                          PLDM_PLATFORM_INVALID_EFFECTER_ID);
    }

    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    rc = encode_get_state_effecter_states_resp(request->hdr.instance_id,
                                               completion_code, compEffecterCnt,
                                               stateFields.data(), responsePtr);

    if (rc != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to encode getStateEffecterStates response, instance_id: {ID}, RC: {RC}",
            "ID", request->hdr.instance_id, "RC", rc);
        return CmdHandler::ccOnlyResponse(request, rc);
    }

    return response;
}

Response MockupResponder::getSensorReading(const pldm_msg* request,
                                           size_t payloadLength,
                                           pldm_pdr* pdrRepoRef)
{
    lg2::info("GetSensorReading");

    (void)pdrRepoRef;
    bool flag = false;

    size_t hdrSize = sizeof(pldm_msg_hdr);
    Response response(hdrSize + PLDM_GET_SENSOR_READING_MIN_RESP_BYTES + 3, 0);

    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());

    uint16_t sensor_id{};
    bool8_t rearm{};

    auto rc = decode_get_sensor_reading_req(request, payloadLength, &sensor_id,
                                            &rearm);

    if (rc != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to decode getSensorReading request, instance_id: {ID}, RC: {RC}",
            "ID", request->hdr.instance_id, "RC", rc);
        return CmdHandler::ccOnlyResponse(request, rc);
    }

    // TODO move values to pdrRepo
    uint8_t completion_code = 0;
    uint8_t sensor_dataSize = PLDM_EFFECTER_DATA_SIZE_UINT32;
    uint8_t sensor_operationalState = PLDM_SENSOR_ENABLED;
    uint8_t sensor_event_messageEnable = PLDM_NO_EVENT_GENERATION;
    uint8_t presentState = PLDM_SENSOR_NORMAL;
    uint8_t previousState = PLDM_SENSOR_NORMAL;
    uint8_t eventState = PLDM_SENSOR_NORMAL;

    uint32_t presentReading = 0;
    for (auto& s : sensors)
    {
        if (s->sensorId == sensor_id)
        {
            flag = true;
            presentReading = s->value;
        }
    }

    if (flag)
    {
        completion_code = PLDM_SUCCESS;
    }
    else
    {
        return CmdHandler::ccOnlyResponse(request,
                                          PLDM_PLATFORM_INVALID_SENSOR_ID);
    }

    rc = encode_get_sensor_reading_resp(
        request->hdr.instance_id, completion_code, sensor_dataSize,
        sensor_operationalState, sensor_event_messageEnable, presentState,
        previousState, eventState, reinterpret_cast<uint8_t*>(&presentReading),
        responsePtr, response.size() - hdrSize);

    if (rc != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to encode getSensorReading response, instance_id: {ID}, RC: {RC}",
            "ID", request->hdr.instance_id, "RC", rc);
        return CmdHandler::ccOnlyResponse(request, rc);
    }

    return response;
}

Response MockupResponder::getEventMessageBufferSize(const pldm_msg* request,
                                                    size_t payloadLength,
                                                    MockupResponder& responder)
{
    lg2::info("GetEventMessageBufferSize");

    uint16_t event_receiver_max_buffer_size = 256;
    auto rc = decode_event_message_buffer_size_req(
        request, payloadLength, &event_receiver_max_buffer_size);

    if (rc != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to decode EventMessageBufferSize request, instance_id: {ID}, RC: {RC}",
            "ID", request->hdr.instance_id, "RC", rc);
        return CmdHandler::ccOnlyResponse(request, rc);
    }

    uint16_t terminus_max_buffer_size = responder.getTerminusMaxBufferSize();

    lg2::info("terminus_max_buffer_size = {TB}", "TB",
              terminus_max_buffer_size);

    Response response(sizeof(pldm_msg_hdr) + sizeof(uint8_t) + sizeof(uint16_t),
                      0);
    auto* msg = reinterpret_cast<pldm_msg*>(response.data());

    rc = encode_event_message_buffer_size_resp(
        request->hdr.instance_id, PLDM_SUCCESS, terminus_max_buffer_size, msg);

    if (rc != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to encode EventMessageBufferSize response, instance_id: {ID}, RC: {RC}",
            "ID", request->hdr.instance_id, "RC", rc);
        return CmdHandler::ccOnlyResponse(request, rc);
    }

    return response;
}

Response MockupResponder::getEventMessageSupported(const pldm_msg* request,
                                                   size_t payloadLength)
{
    lg2::info("GetEventMessageSupported");
    uint8_t formatVersion = 0;

    auto rc = decode_event_message_supported_req(request, payloadLength,
                                                 &formatVersion);
    if (rc != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to decode EventMessageSupported response, instance_id: {ID}, RC: {RC}",
            "ID", request->hdr.instance_id, "RC", rc);
        return CmdHandler::ccOnlyResponse(request, rc);
    }

    uint8_t event_classes[1] = {0};
    uint8_t number_event_class_returned = 1;

    Response response(
        sizeof(pldm_msg_hdr) + sizeof(pldm_event_message_supported_resp), 0);

    auto* msg = reinterpret_cast<pldm_msg*>(response.data());
    uint8_t synchrony_configuration = 0x00;
    uint8_t synchrony_configuration_supported = 0x0B;
    uint8_t completion_code = PLDM_SUCCESS;

    rc = encode_event_message_supported_resp(
        request->hdr.instance_id, completion_code, synchrony_configuration,
        synchrony_configuration_supported, number_event_class_returned,
        event_classes, msg);

    if (rc != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to encode EventMessageSupported response, instance_id: {ID}, RC: {RC}",
            "ID", request->hdr.instance_id, "RC", rc);
        return CmdHandler::ccOnlyResponse(request, rc);
    }

    return response;
}

Response
    MockupResponder::getPdrRepositoryInfo(const pldm_msg* request,
                                          [[maybe_unused]] size_t payloadLength)
{
    lg2::info("GetPdrRepositoryInfo");

    uint8_t update_time[PLDM_TIMESTAMP104_SIZE] = {0};
    uint8_t oem_update_time[PLDM_TIMESTAMP104_SIZE] = {0};
    uint8_t repository_state = PLDM_AVAILABLE;

    uint32_t record_count = 8;
    uint32_t repository_size = 1024;
    uint32_t largest_record_size = 128;
    uint8_t data_transfer_handle_timeout = PLDM_NO_TIMEOUT;

    Response response(
        sizeof(pldm_msg_hdr) + PLDM_GET_PDR_REPOSITORY_INFO_RESP_BYTES, 0);
    auto* msg = reinterpret_cast<pldm_msg*>(response.data());

    auto rc = encode_get_pdr_repository_info_resp(
        request->hdr.instance_id, PLDM_SUCCESS, repository_state, update_time,
        oem_update_time, record_count, repository_size, largest_record_size,
        data_transfer_handle_timeout, msg);

    if (rc != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to encode GetPdrRepositoryInfo response, instance_id: {ID}, RC: {RC}",
            "ID", request->hdr.instance_id, "RC", rc);
        return CmdHandler::ccOnlyResponse(request, rc);
    }

    return response;
}

Response MockupResponder::setEventReceiver(const pldm_msg* request,
                                           size_t payloadLength,
                                           MockupResponder& responder)
{
    lg2::info("SetEventReceiver");

    uint8_t eventMessageGlobalEnable{};
    uint8_t transportProtocolType{};
    uint8_t eventReceiverAddressInfo{};
    uint16_t heartbeatTimer{0};

    auto rc = decode_set_event_receiver_req(
        request, payloadLength, &eventMessageGlobalEnable,
        &transportProtocolType, &eventReceiverAddressInfo, &heartbeatTimer);

    if (rc != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to decode SetEventReceiver request, instance_id: {ID}, RC: {RC}",
            "ID", request->hdr.instance_id, "RC", rc);
        return CmdHandler::ccOnlyResponse(request, rc);
    }

    size_t hdrSize = sizeof(pldm_msg_hdr);
    Response response(hdrSize + PLDM_SET_EVENT_RECEIVER_RESP_BYTES, 0);

    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());

    uint8_t completion_code = PLDM_SUCCESS;

    // TODO: further implementation should be done according to
    // PlatformEventMessage command
    switch (eventMessageGlobalEnable)
    {
        case 0x00:
            lg2::info("Event generation disabled.");
            break;
        case 0x01:
            lg2::info("Asynchronous event generation enabled.");
            break;
        case 0x02:
            lg2::info("Polling-based event generation enabled.");
            break;
        case 0x03:
            if (heartbeatTimer == 0)
            {
                completion_code = PLDM_PLATFORM_HEARTBEAT_FREQUENCY_TOO_HIGH;
                return CmdHandler::ccOnlyResponse(request, completion_code);
            }
            lg2::info(
                "Asynchronous Keep-Alive enabled with heartbeat timer: {TIMER}",
                "TIMER", heartbeatTimer);
            break;

        default:
            lg2::error("Invalid eventMessageGlobalEnable value: {VALUE}",
                       "VALUE", eventMessageGlobalEnable);
            completion_code = PLDM_PLATFORM_ENABLE_METHOD_NOT_SUPPORTED;
            return CmdHandler::ccOnlyResponse(request, completion_code);
    }

    if (transportProtocolType != 0x00)
    {
        completion_code = PLDM_PLATFORM_INVALID_PROTOCOL_TYPE;
        return CmdHandler::ccOnlyResponse(request, completion_code);
    }

    responder.setEventReceiverEid(eventReceiverAddressInfo);

    rc = encode_set_event_receiver_resp(request->hdr.instance_id,
                                        completion_code, responsePtr);

    if (rc != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to encode SetEventReceiver response, instance_id: {ID}, RC: {RC}",
            "ID", request->hdr.instance_id, "RC", rc);
        return CmdHandler::ccOnlyResponse(request, rc);
    }
    lg2::info("New EventReceiver mctp_eid : {EVREID}", "EVREID",
              responder.getEventReceiverEid());

    return response;
}

std::optional<std::vector<uint8_t>>
    MockupResponder::processRxMsg(const std::vector<uint8_t>& rxMsg)
{
    using MsgTag = uint8_t;
    using type = uint8_t;

    pldm_header_info hdrFields{};
    uint8_t eid = rxMsg[1];
    uint8_t msgType = 0;

    auto hdr = reinterpret_cast<const pldm_msg_hdr*>(
        rxMsg.data() + sizeof(MsgTag) + sizeof(eid) + sizeof(type));

    if (PLDM_SUCCESS != unpack_pldm_header(hdr, &hdrFields))
    {
        lg2::error("Empty PLDM request header");
        return std::nullopt;
    }

    if (PLDM_RESPONSE != hdrFields.msg_type)
    {
        auto request = reinterpret_cast<const pldm_msg*>(hdr);
        size_t requestLen = rxMsg.size() - sizeof(struct pldm_msg_hdr) -
                            sizeof(MsgTag) - sizeof(eid) - sizeof(type);

        auto command = request->hdr.command;
        msgType = request->hdr.type;

        if (verbose)
        {
            lg2::info("pldm msg type={TYPE} command code={COMMAND}", "TYPE",
                      msgType, "COMMAND", command);
        }

        if (msgType == PLDM_BASE)
        {
            switch (command)
            {
                case PLDM_GET_PLDM_COMMANDS:
                    return getPLDMCommands(request, requestLen);
                case PLDM_GET_PLDM_TYPES:
                    return getPLDMTypes(request, requestLen);
                case PLDM_SET_TID:
                    return setTID(request, requestLen, *this);
                case PLDM_GET_TID:
                    return getTID(request, requestLen, *this);
                case PLDM_GET_PLDM_VERSION:
                    return getPLDMVersion(request, requestLen);
                default:
                    lg2::error(
                        "unsupported Message:{TYPE} request length={LEN}",
                        "TYPE", msgType, "LEN", requestLen);
                    return unsupportedCommandHandler(requestLen, hdrFields);
            }
        }
        else if (msgType == PLDM_PLATFORM)
        {
            switch (command)
            {
                case PLDM_GET_TERMINUS_UID:
                    return getTerminusUID(request, *this);
                case PLDM_EVENT_MESSAGE_BUFFER_SIZE:
                    return getEventMessageBufferSize(request, requestLen,
                                                     *this);
                case PLDM_EVENT_MESSAGE_SUPPORTED:
                    return getEventMessageSupported(request, requestLen);
                case PLDM_GET_PDR_REPOSITORY_INFO:
                    return getPdrRepositoryInfo(request, requestLen);
                case PLDM_GET_PDR:
                    return getPdr(request, requestLen, pdrRepo);
                case PLDM_GET_STATE_SENSOR_READINGS:
                    return getStateSensorReadings(request, requestLen);
                case PLDM_GET_NUMERIC_EFFECTER_VALUE:
                    return getNumericEffecterValue(request, requestLen);
                case PLDM_GET_STATE_EFFECTER_STATES:
                    return getStateEffecterStates(request, requestLen);
                case PLDM_GET_SENSOR_READING:
                    return getSensorReading(request, requestLen, pdrRepo);
                case PLDM_SET_EVENT_RECEIVER:
                    return setEventReceiver(request, requestLen, *this);
                default:
                    lg2::error(
                        "unsupported Message:{TYPE} request length={LEN}",
                        "TYPE", msgType, "LEN", requestLen);
                    return unsupportedCommandHandler(requestLen, hdrFields);
                    // unsupportedCommandHandler case
            }
        }
    }
    else
    {
        size_t requestLen = rxMsg.size() - sizeof(struct pldm_msg_hdr) -
                            sizeof(MsgTag) - sizeof(eid) - sizeof(type);
        lg2::error("unsupported Message:{TYPE} request length={LEN}", "TYPE",
                   msgType, "LEN", requestLen);
        return unsupportedCommandHandler(requestLen, hdrFields);
    }
    // HandlingResponse

    lg2::info("Message processing completed.");
    return std::nullopt;
}

std::optional<std::vector<uint8_t>>
    MockupResponder::unsupportedCommandHandler(size_t requestLen,
                                               pldm_header_info& hdrFields)
{
    lg2::info("Handling unsupported command...");

    if (verbose)
    {
        lg2::info("unsupportedCommand: request length={LEN}", "LEN",
                  requestLen);
    }

    uint8_t completion_code = PLDM_ERROR_UNSUPPORTED_PLDM_CMD;
    Response response(sizeof(pldm_msg_hdr));
    auto responseHdr = reinterpret_cast<pldm_msg_hdr*>(response.data());

    pldm_header_info header{};
    header.msg_type = PLDM_RESPONSE;
    header.instance = hdrFields.instance;
    header.pldm_type = hdrFields.pldm_type;
    header.command = hdrFields.command;

    if (PLDM_SUCCESS != pack_pldm_header(&header, responseHdr))
    {
        lg2::error("Failed to pack PLDM response header.");
        return std::nullopt;
    }

    response.insert(response.end(), completion_code);
    lg2::info("Returning response for unsupported command.");

    return response;
}
} // namespace MockupResponder