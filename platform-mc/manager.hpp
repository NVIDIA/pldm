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

#include "libpldm/requester/pldm.h"

#include "common/types.hpp"
#include "event_manager.hpp"
#include "platform_manager.hpp"
#include "pldmd/dbus_impl_requester.hpp"
#include "requester/handler.hpp"
#include "requester/mctp_endpoint_discovery.hpp"
#include "sensor_manager.hpp"
#include "terminus_manager.hpp"

namespace pldm
{
namespace platform_mc
{
using namespace pldm::dbus_api;
using namespace pldm::pdr;

/**
 * @brief Manager
 *
 * This class handles all the aspect of the PLDM Platform Monitoring and Control
 * specification for the MCTP devices
 */
class Manager : public pldm::MctpDiscoveryHandlerIntf
{
  public:
    Manager() = delete;
    Manager(const Manager&) = delete;
    Manager(Manager&&) = delete;
    Manager& operator=(const Manager&) = delete;
    Manager& operator=(Manager&&) = delete;
    ~Manager() = default;

    explicit Manager(sdeventplus::Event& event,
                     requester::Handler<requester::Request>& handler,
                     Requester& requester, fw_update::Manager& fwUpdateManager,
                     bool verbose = false) :
        fwUpdateManager(fwUpdateManager),
        terminusManager(event, handler, requester, termini, LOCAL_EID_OVER_I2C,
                        this),
        platformManager(terminusManager, termini),
        sensorManager(event, terminusManager, termini, this, verbose),
        eventManager(terminusManager, termini, fwUpdateManager, verbose),
        verbose(verbose)
    {}

    requester::Coroutine beforeDiscoverTerminus()
    {
        co_return PLDM_SUCCESS;
    }

    requester::Coroutine afterDiscoverTerminus()
    {
        auto rc = co_await platformManager.initTerminus();
        co_return rc;
    }

    void handleMctpEndpoints(const MctpInfos& mctpInfos,
                             dbus::MctpInterfaces& mctpInterfaces)
    {
        (void)mctpInterfaces;
        terminusManager.discoverMctpTerminus(mctpInfos);
    }

    void onlineMctpEndpoint(const UUID& uuid) override
    {
        auto terminus = terminusManager.getTerminus(uuid);
        if (terminus)
        {
            sensorManager.setOnline(terminus->getTid());
        }
    }

    void offlineMctpEndpoint(const UUID& uuid) override
    {
        auto terminus = terminusManager.getTerminus(uuid);
        if (terminus)
        {
            sensorManager.setOffline(terminus->getTid());
        }
    }

    void startSensorPolling()
    {
        sensorManager.startPolling();
    }

    void stopSensorPolling()
    {
        sensorManager.stopPolling();
    }

    int handleCperEvent(const pldm_msg* request, size_t payloadLength,
                        uint8_t /* formatVersion */, uint8_t tid,
                        size_t eventDataOffset, uint8_t& platformEventStatus)
    {
        auto eventData = reinterpret_cast<const uint8_t*>(request->payload) +
                         eventDataOffset;
        auto eventDataSize = payloadLength - eventDataOffset;
        eventManager.handlePlatformEvent(tid, PLDM_OEM_EVENT_CLASS_0xFA,
                                         eventData, eventDataSize,
                                         platformEventStatus);
        return PLDM_SUCCESS;
    }

    /**
     * @brief Handles the Active Firmware Version Change Event.
     *
     * @param request Pointer to the PLDM message.
     * @param payloadLength Length of the payload in the PLDM message.
     * @param formatVersion Format version of the PLDM message (unused in this
     * function).
     * @param tid PLDM Terminus ID.
     * @param eventDataOffset Offset in the message where event data begins.
     * @param[out] platformEventStatus Status of platform event, to
     * be set by this function.
     * @return int Returns 0 on success, or an error code on failure.
     */
    int handleActiveFWVersionChangeEvent(const pldm_msg* request,
                                         size_t payloadLength,
                                         uint8_t /* formatVersion */,
                                         uint8_t tid, size_t eventDataOffset,
                                         uint8_t& platformEventStatus)
    {
        auto eventData = reinterpret_cast<const uint8_t*>(request->payload) +
                         eventDataOffset;
        auto eventDataSize = payloadLength - eventDataOffset;
        eventManager.handlePlatformEvent(tid, PLDM_OEM_EVENT_CLASS_0xFB,
                                         eventData, eventDataSize,
                                         platformEventStatus);
        return PLDM_SUCCESS;
    }

    int handlePldmMessagePollEvent(const pldm_msg* request,
                                   size_t payloadLength,
                                   uint8_t /* formatVersion */, uint8_t tid,
                                   size_t eventDataOffset,
                                   uint8_t& platformEventStatus)
    {
        auto eventData = reinterpret_cast<const uint8_t*>(request->payload) +
                         eventDataOffset;
        auto eventDataSize = payloadLength - eventDataOffset;
        uint8_t eventDataFormatVersion;
        uint16_t eventId;
        uint32_t dataTransferHandle;
        auto rc = decode_pldm_message_poll_event_data(
            eventData, eventDataSize, &eventDataFormatVersion, &eventId,
            &dataTransferHandle);
        if (rc != PLDM_SUCCESS)
        {
            return PLDM_ERROR;
        }

        if (eventDataFormatVersion != 0x01)
        {
            return PLDM_ERROR_INVALID_DATA;
        }

        eventManager.handlePlatformEvent(tid, PLDM_MESSAGE_POLL_EVENT,
                                         eventData, eventDataSize,
                                         platformEventStatus);
        return PLDM_SUCCESS;
    }

    int handleSensorEvent(const pldm_msg* request, size_t payloadLength,
                          uint8_t /* formatVersion */, uint8_t tid,
                          size_t eventDataOffset, uint8_t& platformEventStatus)
    {
        auto eventData = reinterpret_cast<const uint8_t*>(request->payload) +
                         eventDataOffset;
        auto eventDataSize = payloadLength - eventDataOffset;
        eventManager.handlePlatformEvent(tid, PLDM_SENSOR_EVENT, eventData,
                                         eventDataSize, platformEventStatus);
        return PLDM_SUCCESS;
    }

    requester::Coroutine pollForPlatformEvent(tid_t tid)
    {
        auto it = termini.find(tid);
        if (it != termini.end())
        {
            auto& terminus = it->second;
            co_await eventManager.pollForPlatformEventTask(
                tid, terminus->maxBufferSize);
            terminus->pollEvent = false;
        }
        co_return PLDM_SUCCESS;
    }

  private:
    /** @brief List of discovered termini */
    std::map<tid_t, std::shared_ptr<Terminus>> termini{};

    fw_update::Manager& fwUpdateManager;
    TerminusManager terminusManager;
    PlatformManager platformManager;
    SensorManager sensorManager;
    EventManager eventManager;
    bool verbose;
};
} // namespace platform_mc
} // namespace pldm
