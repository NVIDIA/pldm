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

#pragma once

#include "libpldm/pldm.h"

#include "common/types.hpp"
#include "common/utils.hpp"
#include "libpldmresponder/base.hpp"
#include "pdr_json_parser.hpp"

#include <sdbusplus/asio/object_server.hpp>
#include <sdeventplus/event.hpp>
#include <sdeventplus/source/io.hpp>

#include <optional>

using namespace pldm::responder::base;

namespace MockupResponder
{

constexpr uint8_t MCTP_MSG_TYPE_PLDM = 1;
constexpr uint8_t MCTP_MSG_EMU_PREFIX = 0xFF;

constexpr size_t mctpMaxMessageSize = 4096;

constexpr char MCTP_SOCKET_PATH[] = "\0mctp-pcie-mux";

struct HeaderType
{
    uint8_t eid;
    uint8_t type;
};

/**
 * @class MockupResponder
 * @brief Class that represents a mockup responder for handling PLDM requests.
 */
class MockupResponder
{
  public:
    /**
     * @brief Constructor for MockupResponder
     *
     * Initializes the mock responder with event handling, object server, and
     * other necessary configurations.
     *
     * @param verbose Enables verbose logging if set to true.
     * @param event Reference to an sdeventplus event loop object.
     * @param server Reference to an sdbusplus object server.
     * @param eid Endpoint ID of the mock responder.
     * @param pdrPath File path to the PDR JSON file.
     * @param terminusMaxBufferSize Maximum buffer size for terminus.
     */
    MockupResponder(bool verbose, sdeventplus::Event& event,
                    sdbusplus::asio::object_server& server, uint8_t eid,
                    std::string pdrPath, uint16_t terminusMaxBufferSize,
                    uint8_t* uuid);
    ~MockupResponder()
    {}

    int initSocket();

    std::optional<std::vector<uint8_t>>
        processRxMsg(const std::vector<uint8_t>& rxMsg);

    std::optional<std::vector<uint8_t>>
        unsupportedCommandHandler(size_t requestLen,
                                  pldm_header_info& hdrFields);

    void readJsonPdrs(std::string& path);

    uint8_t getTid() const
    {
        return tid;
    }

    void setTid(uint8_t newTid)
    {
        tid = newTid;
    }

    uint8_t getEventReceiverEid() const
    {
        return eventReceiverEid;
    }

    void setEventReceiverEid(uint8_t newEventReceiver)
    {
        eventReceiverEid = newEventReceiver;
    }

    uint16_t getTerminusMaxBufferSize() const
    {
        return mockTerminusMaxBufferSize;
    }

    pldm_pdr* getPdrRepo()
    {
        return pdrRepo;
    }

    uint8_t* getUUID()
    {
        return mockUUID;
    }

    Response getPLDMTypes(const pldm_msg* request,
                          [[maybe_unused]] size_t payloadLength);

    Response getPLDMCommands(const pldm_msg* request, size_t payloadLength);

    Response getPLDMVersion(const pldm_msg* request, size_t payloadLength);

    Response getTID(const pldm_msg* request,
                    [[maybe_unused]] size_t payloadLength,
                    MockupResponder& responder);

    Response setTID(const pldm_msg* request, size_t payloadLength,
                    MockupResponder& responder);

    Response getTerminusUID(const pldm_msg* request,
                            MockupResponder& responder);

    Response getPdr(const pldm_msg* request, size_t payloadLength,
                    pldm_pdr* pdrRepoRef);

    Response getStateSensorReadings(const pldm_msg* request,
                                    size_t payloadLength);

    Response getNumericEffecterValue(const pldm_msg* request,
                                     size_t payloadLength);

    Response getStateEffecterStates(const pldm_msg* request,
                                    size_t payloadLength);

    Response getSensorReading(const pldm_msg* request, size_t payloadLength,
                              pldm_pdr* pdrRepoRef);

    Response getEventMessageBufferSize(const pldm_msg* request,
                                       size_t payloadLength,
                                       MockupResponder& responder);

    Response getEventMessageSupported(const pldm_msg* request,
                                      [[maybe_unused]] size_t payloadLength);

    Response getPdrRepositoryInfo(const pldm_msg* request,
                                  [[maybe_unused]] size_t payloadLength);

    Response setEventReceiver(const pldm_msg* request, size_t payloadLength,
                              MockupResponder& responder);

  private:
    sdeventplus::Event& event;
    bool verbose;
    uint8_t mockEid;
    uint8_t mockInstanceId = 0;
    sdbusplus::asio::object_server& server;
    std::shared_ptr<sdbusplus::asio::dbus_interface> iface;
    int sockFd;
    std::unique_ptr<sdeventplus::source::IO> io;
    uint8_t eventReceiverEid;
    PdrJsonParser jsonParser;
    pldm_pdr* pdrRepo{nullptr};
    uint8_t tid = 1;
    uint16_t mockTerminusMaxBufferSize;
    uint8_t mockUUID[16];
};
} // namespace MockupResponder
