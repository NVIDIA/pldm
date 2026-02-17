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
#include "libpldm/firmware_update.h"

#include "common/utils.hpp"
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "fw-update/component_updater.hpp"
#include "fw-update/dbusutil.hpp"
#include "fw-update/device_updater.hpp"
#include "fw-update/error_handling.hpp"
#include "fw-update/package_parser.hpp"
#include "fw-update/update_manager.hpp"
#include "mocked_firmware_update_function.hpp"
#include "requester/handler.hpp"
#include "test/test_instance_id.hpp"

#include <endian.h>
#include <fcntl.h>
#include <systemd/sd-event.h>
#include <unistd.h>

#include <sdbusplus/bus.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>
#include <sdeventplus/test/sdevent.hpp>

#include <filesystem>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::_;
using namespace pldm;
using namespace pldm::fw_update;
using namespace std::chrono;

namespace
{
void resetEncodeMockControls();

int processPackageStream(UpdateManager& updateManager,
                         const std::filesystem::path& packagePath)
{
    if (!updateManager.updater)
    {
        return -1;
    }

    updateManager.updater->clearImageStream();
    int imageFd = open(packagePath.c_str(), O_RDONLY);
    if (imageFd < 0)
    {
        return -1;
    }

    if (!updateManager.updater->mmapFile.map(imageFd, true))
    {
        return -1;
    }

    updateManager.updater->mmapStream = std::make_unique<pldm::MmapStream>(
        updateManager.updater->mmapFile.data(),
        updateManager.updater->mmapFile.size());
    if (!updateManager.updater->mmapStream->good())
    {
        return -1;
    }

    try
    {
        if (!updateManager.otherDeviceUpdateManager)
        {
            updateManager.otherDeviceUpdateManager =
                std::make_unique<OtherDeviceUpdateManager>(
                    pldm::utils::DBusHandler::getBus(), &updateManager,
                    std::vector<sdbusplus::message::object_path>{});
        }

        auto task =
            updateManager.processStream(*updateManager.updater->mmapStream,
                                        updateManager.updater->mmapFile.size());
        auto rc = stdexec::sync_wait(std::move(task));
        if (!rc.has_value() || !updateManager.parser)
        {
            return -1;
        }
        return 0;
    }
    catch (const std::exception&)
    {
        return -1;
    }
}
} // namespace

class ComponentUpdaterTest : public testing::Test
{
  protected:
    ComponentUpdaterTest() :
        package("./test_pkg", std::ios::binary | std::ios::in | std::ios::ate),
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                      componentInfoMap, componentNameMap, true, nullptr),
        deviceUpdater(0x1, package, fwDeviceIDRecord, compImageInfos, compInfo,
                      compIdNameInfo, 512, &updateManager),
        componentUpdater(0x1, package, fwDeviceIDRecord, compImageInfos,
                         compInfo, compIdNameInfo, 512, &updateManager,
                         &deviceUpdater, 0)
    {
        fwDeviceIDRecord = {
            1,
            {0x00},
            "VersionString2",
            {{PLDM_FWUP_UUID,
              std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41,
                                   0x15, 0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49,
                                   0xD6, 0x75}}},
            {}};
        compImageInfos = {
            {10, 100, 0xFFFFFFFF, 0, 0, 139, 1024, "VersionString3"}};
        compInfo = {
            {std::make_pair(10, 100),
             std::make_tuple(1, "comp1Version", static_cast<uint16_t>(0))}};
        compIdNameInfo = {{11, "ComponentName1"},
                          {55555, "ComponentName2"},
                          {12, "ComponentName3"},
                          {66666, "ComponentName4"}};
    }

    void SetUp() override
    {
        resetEncodeMockControls();
    }

    void TearDown() override
    {
        drainPendingAsyncWork();
        finalizeAsyncHandle(componentUpdater.getStatusTaskHandle);
        finalizeAsyncHandle(componentUpdater.discoverMctpTerminusTaskHandle);
        finalizeAsyncHandle(componentUpdater.updateCompletionCoHandle);
        finalizeAsyncHandle(deviceUpdater.deviceUpdaterHandle);
    }

    void initializeFromParsedPackage()
    {
        ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
        ASSERT_NE(updateManager.parser, nullptr);
        const auto& records = updateManager.parser->getFwDeviceIDRecords();
        ASSERT_FALSE(records.empty());
        fwDeviceIDRecord = records.front();

        compImageInfos = updateManager.parser->getComponentImageInfos();
        const auto& applicableComponents =
            std::get<ApplicableComponents>(fwDeviceIDRecord);
        ASSERT_FALSE(applicableComponents.empty());

        const auto& comp = compImageInfos[applicableComponents.front()];
        auto compKey = std::make_pair(std::get<0>(comp), std::get<1>(comp));
        compInfo[compKey] =
            std::make_tuple(static_cast<uint8_t>(1), std::get<7>(comp),
                            static_cast<uint16_t>(0));
    }

    void runEvent(uint64_t timeoutUsec = 200000)
    {
        EXPECT_GE(sd_event_run(event.get(), timeoutUsec), 0);
    }

    bool requesterPending() const
    {
        if (!reqHandler.handlers.empty() ||
            !reqHandler.removeRequestContainer.empty())
        {
            return true;
        }

        return std::ranges::any_of(
            reqHandler.endpointMessageQueues, [](const auto& queueEntry) {
                const auto& queue = queueEntry.second;
                return queue->activeRequest || !queue->requestQueue.empty();
            });
    }

    void expireOutstandingRequests()
    {
        std::vector<requester::RequestKey> keys;
        keys.reserve(reqHandler.handlers.size());
        for (const auto& [key, value] : reqHandler.handlers)
        {
            (void)value;
            if (!reqHandler.removeRequestContainer.contains(key))
            {
                keys.push_back(key);
            }
        }

        for (const auto& key : keys)
        {
            if (reqHandler.handlers.contains(key) &&
                !reqHandler.removeRequestContainer.contains(key))
            {
                reqHandler.instanceIdExpiryCallBack(key);
            }
        }
    }

    void flushReadyEvents(int iterations = 8)
    {
        for (int i = 0; i < iterations; ++i)
        {
            runEvent(0);
        }
    }

    void drainPendingAsyncWork()
    {
        auto settleAsyncHandle = [](auto& handle) {
            if (!handle.has_value())
            {
                return false;
            }

            auto& [scope, rcOpt] = *handle;
            if (!rcOpt.has_value())
            {
                return true;
            }

            stdexec::sync_wait(scope.on_empty());
            handle.reset();
            return false;
        };

        for (int i = 0; i < 64; ++i)
        {
            flushReadyEvents();

            if (requesterPending())
            {
                expireOutstandingRequests();
            }

            const bool handlePending =
                settleAsyncHandle(componentUpdater.getStatusTaskHandle) ||
                settleAsyncHandle(
                    componentUpdater.discoverMctpTerminusTaskHandle) ||
                settleAsyncHandle(componentUpdater.updateCompletionCoHandle) ||
                settleAsyncHandle(deviceUpdater.deviceUpdaterHandle);

            flushReadyEvents();

            if (!requesterPending() && !handlePending &&
                !componentUpdater.getStatusTaskHandle.has_value() &&
                !componentUpdater.discoverMctpTerminusTaskHandle.has_value() &&
                !componentUpdater.updateCompletionCoHandle.has_value() &&
                !deviceUpdater.deviceUpdaterHandle.has_value())
            {
                flushReadyEvents();
                if (!requesterPending() &&
                    !componentUpdater.getStatusTaskHandle.has_value() &&
                    !componentUpdater.discoverMctpTerminusTaskHandle
                         .has_value() &&
                    !componentUpdater.updateCompletionCoHandle.has_value() &&
                    !deviceUpdater.deviceUpdaterHandle.has_value())
                {
                    return;
                }
            }
        }
    }

    template <typename Handle>
    void finalizeAsyncHandle(Handle& handle)
    {
        if (!handle.has_value())
        {
            return;
        }

        auto& [scope, rcOpt] = *handle;
        if (rcOpt.has_value())
        {
            stdexec::sync_wait(scope.on_empty());
        }

        handle.reset();
    }

    template <typename Handle>
    void waitForAsyncHandle(Handle& handle)
    {
        if (!handle.has_value())
        {
            return;
        }

        for (int i = 0; i < 64; ++i)
        {
            auto& [scope, rcOpt] = *handle;
            if (rcOpt.has_value())
            {
                stdexec::sync_wait(scope.on_empty());
                handle.reset();
                return;
            }

            if (!reqHandler.handlers.empty())
            {
                expireOutstandingRequests();
            }

            flushReadyEvents();
        }
    }

    template <typename Handle>
    void waitForAsyncResult(Handle& handle)
    {
        if (!handle.has_value())
        {
            return;
        }

        for (int i = 0; i < 64; ++i)
        {
            auto& [scope, rcOpt] = *handle;
            (void)scope;
            if (rcOpt.has_value())
            {
                return;
            }

            if (!reqHandler.handlers.empty())
            {
                expireOutstandingRequests();
            }

            flushReadyEvents();
        }
    }

    void waitForDiscoverTask()
    {
        waitForAsyncHandle(componentUpdater.discoverMctpTerminusTaskHandle);
    }

    void waitForGetStatusTask()
    {
        waitForAsyncHandle(componentUpdater.getStatusTaskHandle);
    }

    void waitForUpdateCompletionTask()
    {
        waitForAsyncHandle(componentUpdater.updateCompletionCoHandle);
    }

    std::ifstream package;
    FirmwareDeviceIDRecord fwDeviceIDRecord;
    ComponentImageInfos compImageInfos;
    ComponentInfo compInfo;
    ComponentIdNameMap compIdNameInfo;
    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    requester::Handler<requester::Request> reqHandler;
    DescriptorMap descriptorMap;
    ComponentInfoMap componentInfoMap;
    ComponentNameMap componentNameMap;
    UpdateManager updateManager;
    DeviceUpdater deviceUpdater;
    ComponentUpdater componentUpdater;
};

namespace
{

bool gFailEncodeRequestFwDataResp = false;
int gEncodeRequestFwDataRespRc = PLDM_ERROR;
bool gFailEncodeTransferCompleteResp = false;
int gEncodeTransferCompleteRespRc = PLDM_ERROR;
bool gFailEncodeVerifyCompleteResp = false;
int gEncodeVerifyCompleteRespRc = PLDM_ERROR;
bool gFailEncodeApplyCompleteResp = false;
int gEncodeApplyCompleteRespRc = PLDM_ERROR;

void resetEncodeMockControls()
{
    gFailEncodeRequestFwDataResp = false;
    gEncodeRequestFwDataRespRc = PLDM_ERROR;
    gFailEncodeTransferCompleteResp = false;
    gEncodeTransferCompleteRespRc = PLDM_ERROR;
    gFailEncodeVerifyCompleteResp = false;
    gEncodeVerifyCompleteRespRc = PLDM_ERROR;
    gFailEncodeApplyCompleteResp = false;
    gEncodeApplyCompleteRespRc = PLDM_ERROR;
}

} // namespace

extern "C" int encode_request_firmware_data_resp(
    uint8_t instance_id, uint8_t completion_code, struct pldm_msg* msg,
    size_t payload_length)
{
    if (gFailEncodeRequestFwDataResp)
    {
        return gEncodeRequestFwDataRespRc;
    }
    if (msg == nullptr || !payload_length)
    {
        return PLDM_ERROR_INVALID_DATA;
    }

    pldm_header_info header{};
    header.instance = instance_id;
    header.msg_type = PLDM_RESPONSE;
    header.pldm_type = PLDM_FWUP;
    header.command = PLDM_REQUEST_FIRMWARE_DATA;
    const auto rc = pack_pldm_header(&header, &(msg->hdr));
    if (rc)
    {
        return rc;
    }

    msg->payload[0] = completion_code;
    return PLDM_SUCCESS;
}

extern "C" int encode_transfer_complete_resp(
    uint8_t instance_id, uint8_t completion_code, struct pldm_msg* msg,
    size_t payload_length)
{
    if (gFailEncodeTransferCompleteResp)
    {
        return gEncodeTransferCompleteRespRc;
    }
    if (msg == nullptr)
    {
        return PLDM_ERROR_INVALID_DATA;
    }
    if (payload_length != sizeof(completion_code))
    {
        return PLDM_ERROR_INVALID_LENGTH;
    }

    pldm_header_info header{};
    header.instance = instance_id;
    header.msg_type = PLDM_RESPONSE;
    header.pldm_type = PLDM_FWUP;
    header.command = PLDM_TRANSFER_COMPLETE;
    const auto rc = pack_pldm_header(&header, &(msg->hdr));
    if (rc)
    {
        return rc;
    }
    msg->payload[0] = completion_code;
    return PLDM_SUCCESS;
}

extern "C" int encode_verify_complete_resp(
    uint8_t instance_id, uint8_t completion_code, struct pldm_msg* msg,
    size_t payload_length)
{
    if (gFailEncodeVerifyCompleteResp)
    {
        return gEncodeVerifyCompleteRespRc;
    }
    if (msg == nullptr)
    {
        return PLDM_ERROR_INVALID_DATA;
    }
    if (payload_length != sizeof(completion_code))
    {
        return PLDM_ERROR_INVALID_LENGTH;
    }

    pldm_header_info header{};
    header.instance = instance_id;
    header.msg_type = PLDM_RESPONSE;
    header.pldm_type = PLDM_FWUP;
    header.command = PLDM_VERIFY_COMPLETE;
    const auto rc = pack_pldm_header(&header, &(msg->hdr));
    if (rc)
    {
        return rc;
    }
    msg->payload[0] = completion_code;
    return PLDM_SUCCESS;
}

extern "C" int encode_apply_complete_resp(
    uint8_t instance_id, uint8_t completion_code, struct pldm_msg* msg,
    size_t payload_length)
{
    if (gFailEncodeApplyCompleteResp)
    {
        return gEncodeApplyCompleteRespRc;
    }
    if (msg == nullptr)
    {
        return PLDM_ERROR_INVALID_DATA;
    }
    if (payload_length != sizeof(completion_code))
    {
        return PLDM_ERROR_INVALID_LENGTH;
    }

    pldm_header_info header{};
    header.instance = instance_id;
    header.msg_type = PLDM_RESPONSE;
    header.pldm_type = PLDM_FWUP;
    header.command = PLDM_APPLY_COMPLETE;
    const auto rc = pack_pldm_header(&header, &(msg->hdr));
    if (rc)
    {
        return rc;
    }
    msg->payload[0] = completion_code;
    return PLDM_SUCCESS;
}

static std::array<uint8_t,
                  sizeof(pldm_msg_hdr) + sizeof(pldm_request_firmware_data_req)>
    makeRequestFwDataReq(uint32_t offset, uint32_t length)
{
    std::array<uint8_t,
               sizeof(pldm_msg_hdr) + sizeof(pldm_request_firmware_data_req)>
        reqFwDataReq{0x8A, 0x05, 0x15, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x02, 0x00, 0x00};
    auto* payload = reinterpret_cast<pldm_request_firmware_data_req*>(
        reqFwDataReq.data() + sizeof(pldm_msg_hdr));
    payload->offset = htole32(offset);
    payload->length = htole32(length);
    return reqFwDataReq;
}

static std::pair<std::vector<uint8_t>, size_t> makeUpdateComponentResp(
    uint8_t completionCode, uint8_t compCompatibilityResp,
    uint8_t compCompatibilityRespCode)
{
    size_t payloadLen = sizeof(pldm_update_component_resp);
    std::vector<uint8_t> response(sizeof(pldm_msg_hdr) + payloadLen);
    auto* responseMsg = reinterpret_cast<pldm_msg*>(response.data());

    pldm_header_info header{};
    header.instance = 0x0A;
    header.msg_type = PLDM_RESPONSE;
    header.pldm_type = PLDM_FWUP;
    header.command = PLDM_UPDATE_COMPONENT;
    auto rc = pack_pldm_header(&header, &(responseMsg->hdr));
    EXPECT_EQ(rc, PLDM_SUCCESS);

    auto* responseData =
        reinterpret_cast<pldm_update_component_resp*>(responseMsg->payload);
    responseData->completion_code = completionCode;
    responseData->comp_compatibility_resp = compCompatibilityResp;
    responseData->comp_compatibility_resp_code = compCompatibilityRespCode;
    responseData->update_option_flags_enabled.value = htole32(0);
    responseData->time_before_req_fw_data = htole16(0);

    response.resize(sizeof(pldm_msg_hdr) + payloadLen);
    return {response, payloadLen};
}

// TEST_F(ComponentUpdaterTest, ReadPackage512B)
// {
//     // mctp_eid_t eid = 0x1;
//     // size_t componentOffset = 0;
//
//     constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
//                                       sizeof(pldm_request_firmware_data_req)>
//         reqFwDataReq{0x8A, 0x05, 0x15, 0x00, 0x00, 0x00,
//                      0x00, 0x00, 0x02, 0x00, 0x00};
//     constexpr uint8_t instanceId = 0x0A;
//     constexpr uint8_t completionCode = PLDM_SUCCESS;
//     constexpr uint32_t length = 512;
//     auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());
//     componentUpdater.componentUpdaterState.set(
//         ComponentUpdaterSequence::RequestFirmwareData);
//     std::cerr << "Flag1" << std::endl;
//     auto response = componentUpdater.requestFwData(
//         requestMsg, sizeof(pldm_request_firmware_data_req));
//     std::cerr << "Flag2" << std::endl;
//
//     try
//     {
//     EXPECT_EQ(response.size(),
//               sizeof(pldm_msg_hdr) + sizeof(completionCode) + length);
//     std::cerr << "Flag3" << std::endl;
//     auto responeMsg = reinterpret_cast<const pldm_msg*>(response.data());
//     std::cerr << "Flag4" << std::endl;
//     EXPECT_EQ(responeMsg->hdr.request, PLDM_RESPONSE);
//     std::cerr << "Flag5" << std::endl;
//     EXPECT_EQ(responeMsg->hdr.instance_id, instanceId);
//     std::cerr << "Flag6" << std::endl;
//     EXPECT_EQ(responeMsg->hdr.type, PLDM_FWUP);
//     std::cerr << "Flag7" << std::endl;
//     EXPECT_EQ(responeMsg->hdr.command, PLDM_REQUEST_FIRMWARE_DATA);
//     std::cerr << "Flag8" << std::endl;
//     EXPECT_EQ(response[sizeof(pldm_msg_hdr)], completionCode);
//     std::cerr << "Flag9" << std::endl;
//
//     const std::vector<uint8_t> compFirst512B{
//         0x0A, 0x05, 0x15, 0x00, 0x48, 0xD2, 0x1E, 0x80, 0x2E, 0x77, 0x71,
//         0x2C, 0x8E, 0xE3, 0x1F, 0x6F, 0x30, 0x76, 0x65, 0x08, 0xB8, 0x1B,
//         0x4B, 0x03, 0x7E, 0x96, 0xD9, 0x2A, 0x36, 0x3A, 0xA2, 0xEE, 0x8A,
//         0x30, 0x21, 0x33, 0xFC, 0x27, 0xE7, 0x3E, 0x56, 0x79, 0x0E, 0xBD,
//         0xED, 0x44, 0x96, 0x2F, 0x84, 0xB5, 0xED, 0x19, 0x3A, 0x5E, 0x62,
//         0x2A, 0x6E, 0x41, 0x7E, 0xDC, 0x2E, 0xBB, 0x87, 0x41, 0x7F, 0xCE,
//         0xF0, 0xD7, 0xE4, 0x0F, 0x95, 0x33, 0x3B, 0xF9, 0x04, 0xF8, 0x1A,
//         0x92, 0x54, 0xFD, 0x33, 0xBA, 0xCD, 0xA6, 0x08, 0x0D, 0x32, 0x2C,
//         0xEB, 0x75, 0xDC, 0xEA, 0xBA, 0x30, 0x94, 0x78, 0x8C, 0x61, 0x58,
//         0xD0, 0x59, 0xF3, 0x29, 0x6D, 0x67, 0xD3, 0x26, 0x08, 0x25, 0x1E,
//         0x69, 0xBB, 0x28, 0xB0, 0x61, 0xFB, 0x96, 0xA3, 0x8C, 0xBF, 0x01,
//         0x94, 0xEB, 0x3A, 0x63, 0x6F, 0xC8, 0x0F, 0x42, 0x7F, 0xEB, 0x3D,
//         0xA7, 0x8B, 0xE5, 0xD2, 0xFB, 0xB8, 0xD3, 0x15, 0xAA, 0xDF, 0x86,
//         0xAB, 0x6E, 0x29, 0xB3, 0x12, 0x96, 0xB7, 0x86, 0xDA, 0xF9, 0xD7,
//         0x70, 0xAD, 0xB6, 0x1A, 0x29, 0xB1, 0xA4, 0x2B, 0x6F, 0x63, 0xEE,
//         0x05, 0x9F, 0x35, 0x49, 0xA1, 0xAB, 0xA2, 0x6F, 0x7C, 0xFC, 0x23,
//         0x09, 0x55, 0xED, 0xF7, 0x35, 0xD8, 0x2F, 0x8F, 0xD2, 0xBD, 0x77,
//         0xED, 0x0C, 0x7A, 0xE9, 0xD3, 0xF7, 0x90, 0xA7, 0x45, 0x97, 0xAA,
//         0x3A, 0x79, 0xC4, 0xF8, 0xD2, 0xFE, 0xFB, 0xB3, 0x25, 0x86, 0x98,
//         0x6B, 0x98, 0x10, 0x15, 0xB3, 0xDD, 0x43, 0x0B, 0x20, 0x5F, 0xE4,
//         0x62, 0xC8, 0xA1, 0x3E, 0x9C, 0xF3, 0xD8, 0xEA, 0x15, 0xA1, 0x24,
//         0x94, 0x1C, 0xF5, 0xB4, 0x86, 0x04, 0x30, 0x2C, 0x84, 0xB6, 0x29,
//         0xF6, 0x9D, 0x76, 0x6E, 0xD4, 0x0C, 0x1C, 0xBD, 0xF9, 0x95, 0x7E,
//         0xAF, 0x62, 0x80, 0x14, 0xE6, 0x1C, 0x43, 0x51, 0x5C, 0xCA, 0x50,
//         0xE1, 0x73, 0x3D, 0x75, 0x66, 0x52, 0x9E, 0xB6, 0x15, 0x7E, 0xF7,
//         0xE5, 0xE2, 0xAF, 0x54, 0x75, 0x82, 0x3D, 0x55, 0xC7, 0x59, 0xD7,
//         0xBD, 0x8C, 0x4B, 0x74, 0xD1, 0x3F, 0xA8, 0x1B, 0x0A, 0xF0, 0x5A,
//         0x32, 0x2B, 0xA7, 0xA4, 0xBE, 0x38, 0x18, 0xAE, 0x69, 0xDC, 0x54,
//         0x7C, 0x60, 0xEF, 0x4F, 0x0F, 0x7F, 0x5A, 0xA6, 0xC8, 0x3E, 0x59,
//         0xFD, 0xF5, 0x98, 0x26, 0x71, 0xD0, 0xEF, 0x54, 0x47, 0x38, 0x1F,
//         0x18, 0x9D, 0x37, 0x9D, 0xF0, 0xCD, 0x00, 0x73, 0x30, 0xD4, 0xB7,
//         0xDA, 0x2D, 0x36, 0xA1, 0xA9, 0xAD, 0x4F, 0x9F, 0x17, 0xA5, 0xA1,
//         0x62, 0x18, 0x21, 0xDD, 0x0E, 0xB6, 0x72, 0xDE, 0x17, 0xF0, 0x71,
//         0x94, 0xA9, 0x67, 0xB4, 0x75, 0xDB, 0x64, 0xF0, 0x6E, 0x3D, 0x4E,
//         0x29, 0x45, 0x42, 0xC3, 0xDA, 0x1F, 0x9E, 0x31, 0x4D, 0x1B, 0xA7,
//         0x9D, 0x07, 0xD9, 0x10, 0x75, 0x27, 0x92, 0x16, 0x35, 0xF5, 0x51,
//         0x3E, 0x14, 0x00, 0xB4, 0xBD, 0x21, 0xAF, 0x90, 0xC5, 0xE5, 0xEE,
//         0xD0, 0xB3, 0x7F, 0x61, 0xA5, 0x1B, 0x91, 0xD5, 0x66, 0x08, 0xB5,
//         0x16, 0x25, 0xC2, 0x16, 0x53, 0xDC, 0xB5, 0xF1, 0xDD, 0xCF, 0x28,
//         0xDD, 0x57, 0x90, 0x66, 0x33, 0x7B, 0x75, 0xF4, 0x8A, 0x19, 0xAC,
//         0x1F, 0x44, 0xC2, 0xF6, 0x21, 0x07, 0xE9, 0xCC, 0xDD, 0xCF, 0x4A,
//         0x34, 0xA1, 0x24, 0x82, 0xF8, 0xA1, 0x1D, 0x06, 0x90, 0x4B, 0x97,
//         0xB8, 0x10, 0xF2, 0x6A, 0x55, 0x30, 0xD9, 0x4F, 0x94, 0xE7, 0x7C,
//         0xBB, 0x73, 0xA3, 0x5F, 0xC6, 0xF1, 0xDB, 0x84, 0x3D, 0x29, 0x72,
//         0xD1, 0xAD, 0x2D, 0x77, 0x3F, 0x36, 0x24, 0x0F, 0xC4, 0x12, 0xD7,
//         0x3C, 0x65, 0x6C, 0xE1, 0x5A, 0x32, 0xAA, 0x0B, 0xA3, 0xA2, 0x72,
//         0x33, 0x00, 0x3C, 0x7E, 0x28, 0x36, 0x10, 0x90, 0x38, 0xFB};
//     EXPECT_EQ(response, compFirst512B);
//     std::cerr << "Flag10" << std::endl;
//     }
//     catch (std::exception& e)
//     {
//         std::cerr << "Flag exception" << std::endl;
//     }
// }
//
// TEST_F(ComponentUpdaterTest, sendUpdateComponentRequest)
// {
//     mctp_eid_t eid = 0x1;
//     size_t componentOffset = 0;
//     ComponentUpdater componentUpdater(eid, package, fwDeviceIDRecord,
//                                       compImageInfos, compInfo,
//                                       compIdNameInfo, 512, &updateManager,
//                                       &deviceUpdater, componentOffset,
//                                       false);
//
//     EXPECT_NO_THROW({
//         [[maybe_unused]] auto co =
//             componentUpdater.sendUpdateComponentRequest(componentOffset);
//     });
// }

TEST_F(ComponentUpdaterTest, transferComplete)
{
    mctp_eid_t eid = 0x1;
    size_t componentOffset = 0;
    ComponentUpdater componentUpdater(
        eid, package, fwDeviceIDRecord, compImageInfos, compInfo,
        compIdNameInfo, 512, &updateManager, &deviceUpdater, componentOffset);

    // Timer must exist before requestFwData (created in
    // processUpdateComponentResponse in real flow)
    componentUpdater.createRequestFwDataTimer();

    {
        constexpr std::array<uint8_t,
                             sizeof(pldm_msg_hdr) +
                                 sizeof(pldm_request_firmware_data_req)>
            reqFwDataReq{0x8A, 0x05, 0x15, 0x00, 0x00, 0x00,
                         0x00, 0x00, 0x02, 0x00, 0x00};
        auto requestMsg =
            reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());
        componentUpdater.componentUpdaterState.set(
            ComponentUpdaterSequence::RequestFirmwareData);
        componentUpdater.requestFwData(requestMsg,
                                       sizeof(pldm_request_firmware_data_req));
    }

    constexpr uint8_t transferResult = PLDM_FWUP_TRANSFER_SUCCESS;
    constexpr uint64_t pldm_request_transfer_complete =
        sizeof(pldm_msg_hdr) + sizeof(transferResult);
    constexpr std::array<uint8_t,
                         sizeof(pldm_msg_hdr) + pldm_request_transfer_complete>
        transferCompleteReq{0x8A, 0x05, 0x15, 0x00, 0x00, 0x00, 0x00};
    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(transferCompleteReq.data());

    constexpr uint8_t instanceId = 0x0A;
    constexpr uint8_t completionCode = PLDM_SUCCESS;
    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::RequestFirmwareData);

    auto response =
        componentUpdater.transferComplete(requestMsg, sizeof(transferResult));

    EXPECT_EQ(response.size(), sizeof(pldm_msg_hdr) + sizeof(completionCode));
    auto responeMsg = reinterpret_cast<const pldm_msg*>(response.data());
    EXPECT_EQ(responeMsg->hdr.request, PLDM_RESPONSE);
    EXPECT_EQ(responeMsg->hdr.instance_id, instanceId);
    EXPECT_EQ(responeMsg->hdr.type, PLDM_FWUP);
    EXPECT_EQ(responeMsg->hdr.command, PLDM_TRANSFER_COMPLETE);
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], completionCode);

    const std::vector<uint8_t> compTransferData{0x0A, 0x05, 0x16, 0x00};
    EXPECT_EQ(response, compTransferData);

    constexpr std::array<uint8_t,
                         sizeof(pldm_msg_hdr) + pldm_request_transfer_complete>
        transferCompleteReqError{0x98, 0x05, 0x16, 0x02};
    auto requestMsgError =
        reinterpret_cast<const pldm_msg*>(transferCompleteReqError.data());
    auto responseError = componentUpdater.transferComplete(
        requestMsgError, sizeof(transferResult));
    EXPECT_EQ(responseError[sizeof(pldm_msg_hdr)], completionCode);
}

TEST_F(ComponentUpdaterTest, transferCompleteDecodeFailureSchedulesDeferred)
{
    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::TransferComplete);
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr)> invalidReq{
        0x8A, 0x05, 0x16};
    auto requestMsg = reinterpret_cast<const pldm_msg*>(invalidReq.data());

    auto response = componentUpdater.transferComplete(requestMsg, 0);
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_ERROR_INVALID_DATA);
    ASSERT_NE(componentUpdater.pldmRequest, nullptr);
    runEvent();
    runEvent();
    waitForDiscoverTask();
    waitForUpdateCompletionTask();
}

TEST_F(ComponentUpdaterTest, verifyComplete)
{
    mctp_eid_t eid = 0x1;
    size_t componentOffset = 0;
    ComponentUpdater componentUpdater(
        eid, package, fwDeviceIDRecord, compImageInfos, compInfo,
        compIdNameInfo, 512, &updateManager, &deviceUpdater, componentOffset);

    // Timer must exist before requestFwData (created in
    // processUpdateComponentResponse in real flow)
    componentUpdater.createRequestFwDataTimer();

    {
        constexpr std::array<uint8_t,
                             sizeof(pldm_msg_hdr) +
                                 sizeof(pldm_request_firmware_data_req)>
            reqFwDataReq{0x8A, 0x05, 0x15, 0x00, 0x00, 0x00,
                         0x00, 0x00, 0x02, 0x00, 0x00};
        auto requestMsg =
            reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());
        componentUpdater.componentUpdaterState.set(
            ComponentUpdaterSequence::RequestFirmwareData);
        componentUpdater.requestFwData(requestMsg,
                                       sizeof(pldm_request_firmware_data_req));
    }

    constexpr uint8_t verifyResult = PLDM_FWUP_VERIFY_SUCCESS;
    constexpr uint64_t pldm_request_verify_complete =
        sizeof(pldm_msg_hdr) + sizeof(verifyResult);
    constexpr std::array<uint8_t,
                         sizeof(pldm_msg_hdr) + pldm_request_verify_complete>
        verifyCompleteReq{0x8A, 0x05, 0x16, 0x00, 0x00, 0x00, 0x00};
    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(verifyCompleteReq.data());

    constexpr uint8_t instanceId = 0x0A;
    constexpr uint8_t completionCode = PLDM_SUCCESS;
    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::VerifyComplete);

    auto response =
        componentUpdater.verifyComplete(requestMsg, sizeof(verifyResult));

    EXPECT_EQ(response.size(), sizeof(pldm_msg_hdr) + sizeof(completionCode));
    auto responeMsg = reinterpret_cast<const pldm_msg*>(response.data());
    EXPECT_EQ(responeMsg->hdr.request, PLDM_RESPONSE);
    EXPECT_EQ(responeMsg->hdr.instance_id, instanceId);
    EXPECT_EQ(responeMsg->hdr.type, PLDM_FWUP);
    EXPECT_EQ(responeMsg->hdr.command, PLDM_VERIFY_COMPLETE);
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], completionCode);

    const std::vector<uint8_t> compTransferData{0x0A, 0x05, 0x17, 0x00};
    EXPECT_EQ(response, compTransferData);

    constexpr std::array<uint8_t,
                         sizeof(pldm_msg_hdr) + pldm_request_verify_complete>
        verifyCompleteReqError{0x86, 0x05, 0x17, 0x97};

    auto requestMsgError =
        reinterpret_cast<const pldm_msg*>(verifyCompleteReqError.data());
    auto responseError =
        componentUpdater.verifyComplete(requestMsgError, sizeof(verifyResult));
    EXPECT_EQ(responseError[sizeof(pldm_msg_hdr)], completionCode);
}

TEST_F(ComponentUpdaterTest, verifyCompleteDecodeFailureSchedulesDeferred)
{
    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::VerifyComplete);
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr)> invalidReq{
        0x8A, 0x05, 0x17};
    auto requestMsg = reinterpret_cast<const pldm_msg*>(invalidReq.data());

    auto response = componentUpdater.verifyComplete(requestMsg, 0);
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_ERROR_INVALID_DATA);
    ASSERT_NE(componentUpdater.pldmRequest, nullptr);
    runEvent();
    runEvent();
    waitForDiscoverTask();
    waitForUpdateCompletionTask();
}

TEST_F(ComponentUpdaterTest, verifyCompleteFailureSchedulesDeferred)
{
    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::VerifyComplete);
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + sizeof(uint8_t)>
        verifyCompleteReq{0x8A, 0x05, 0x17, 0x01};
    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(verifyCompleteReq.data());

    auto response =
        componentUpdater.verifyComplete(requestMsg, sizeof(uint8_t));
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_SUCCESS);
    ASSERT_NE(componentUpdater.pldmRequest, nullptr);
    runEvent();
    runEvent();
    waitForDiscoverTask();
    waitForUpdateCompletionTask();
}

TEST_F(ComponentUpdaterTest, sendcancelUpdateComponentRequest)
{
    mctp_eid_t eid = 0x1;
    size_t componentOffset = 0;
    ComponentUpdater componentUpdater(
        eid, package, fwDeviceIDRecord, compImageInfos, compInfo,
        compIdNameInfo, 512, &updateManager, &deviceUpdater, componentOffset);

    EXPECT_NO_THROW({
        [[maybe_unused]] auto co =
            componentUpdater.sendcancelUpdateComponentRequest();
    });
}

TEST_F(ComponentUpdaterTest, cancelUpdateComponent_empty_response)
{
    mctp_eid_t eid = 0x1;
    size_t componentOffset = 0;
    ComponentUpdater componentUpdater(
        eid, package, fwDeviceIDRecord, compImageInfos, compInfo,
        compIdNameInfo, 512, &updateManager, &deviceUpdater, componentOffset);

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr)> invalidResponse{
        0x80, 0x05, 0x1c};
    auto responseMsg =
        reinterpret_cast<const pldm_msg*>(invalidResponse.data());

    auto co = componentUpdater.processCancelUpdateComponentResponse(
        eid, responseMsg, 0);
    auto rc = stdexec::sync_wait(std::move(co));
    ASSERT_TRUE(rc.has_value());
    EXPECT_NE(std::get<0>(*rc), PLDM_SUCCESS);
}

TEST_F(ComponentUpdaterTest, cancelUpdateComponent)
{
    mctp_eid_t eid = 0x1;
    size_t componentOffset = 0;
    ComponentUpdater componentUpdater(
        eid, package, fwDeviceIDRecord, compImageInfos, compInfo,
        compIdNameInfo, 512, &updateManager, &deviceUpdater, componentOffset);
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + sizeof(uint8_t)>
        cancelCompUpdateResponse{0x80, 0x05, 0x1c};

    auto cancelCompUpdateResponseMsg =
        reinterpret_cast<const pldm_msg*>(cancelCompUpdateResponse.data());

    EXPECT_NO_THROW({
        auto co = componentUpdater.processCancelUpdateComponentResponse(
            eid, cancelCompUpdateResponseMsg, sizeof(uint8_t));
        auto rc = stdexec::sync_wait(std::move(co));
        ASSERT_TRUE(rc.has_value());
    });
}

TEST_F(ComponentUpdaterTest, cancelUpdateComponentCompletionCodeFailure)
{
    mctp_eid_t eid = 0x1;
    size_t componentOffset = 0;
    ComponentUpdater componentUpdater(
        eid, package, fwDeviceIDRecord, compImageInfos, compInfo,
        compIdNameInfo, 512, &updateManager, &deviceUpdater, componentOffset);
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + sizeof(uint8_t)>
        cancelCompUpdateResponse{0x80, 0x05, 0x1c, 0x01};

    auto cancelCompUpdateResponseMsg =
        reinterpret_cast<const pldm_msg*>(cancelCompUpdateResponse.data());

    auto co = componentUpdater.processCancelUpdateComponentResponse(
        eid, cancelCompUpdateResponseMsg, sizeof(uint8_t));
    auto rc = stdexec::sync_wait(std::move(co));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(std::get<0>(*rc), PLDM_ERROR);
}

TEST_F(ComponentUpdaterTest, command_UpdateComponent)
{
    struct ComponentUpdaterState componentUpdaterState;

    ComponentUpdaterSequence sequence = componentUpdaterState.nextState(
        ComponentUpdaterSequence::UpdateComponent);

    EXPECT_EQ(sequence, ComponentUpdaterSequence::RequestFirmwareData);
}

TEST_F(ComponentUpdaterTest, command_RequestFirmwareData)
{
    struct ComponentUpdaterState componentUpdaterState;

    ComponentUpdaterSequence sequence = componentUpdaterState.nextState(
        ComponentUpdaterSequence::RequestFirmwareData);

    EXPECT_EQ(sequence, ComponentUpdaterSequence::TransferComplete);
}

TEST_F(ComponentUpdaterTest, command_TransferComplete)
{
    struct ComponentUpdaterState componentUpdaterState;

    ComponentUpdaterSequence sequence = componentUpdaterState.nextState(
        ComponentUpdaterSequence::TransferComplete);

    EXPECT_EQ(sequence, ComponentUpdaterSequence::VerifyComplete);
}

TEST_F(ComponentUpdaterTest, command_VerifyComplete)
{
    struct ComponentUpdaterState componentUpdaterState;

    ComponentUpdaterSequence sequence = componentUpdaterState.nextState(
        ComponentUpdaterSequence::VerifyComplete);

    EXPECT_EQ(sequence, ComponentUpdaterSequence::ApplyComplete);
}

TEST_F(ComponentUpdaterTest, command_ApplyComplete)
{
    struct ComponentUpdaterState componentUpdaterState;
    componentUpdaterState.set(ComponentUpdaterSequence::ApplyComplete);
    ComponentUpdaterSequence sequence = componentUpdaterState.nextState(
        ComponentUpdaterSequence::ApplyComplete);

    EXPECT_EQ(sequence, ComponentUpdaterSequence::ApplyComplete);
}

TEST_F(ComponentUpdaterTest, command_DefaultState)
{
    struct ComponentUpdaterState componentUpdaterState;
    componentUpdaterState.set(ComponentUpdaterSequence::ApplyComplete);
    ComponentUpdaterSequence sequence =
        componentUpdaterState.nextState(ComponentUpdaterSequence::Invalid);

    EXPECT_EQ(sequence, ComponentUpdaterSequence::Invalid);
}

TEST_F(ComponentUpdaterTest, expectedState_RetryRequest)
{
    struct ComponentUpdaterState componentUpdaterState;
    ComponentUpdaterSequence sequence = componentUpdaterState.expectedState(
        ComponentUpdaterSequence::UpdateComponent);

    EXPECT_EQ(sequence, ComponentUpdaterSequence::RetryRequest);
}

TEST_F(ComponentUpdaterTest, expectedState_InvalidState)
{
    struct ComponentUpdaterState componentUpdaterState;
    ComponentUpdaterSequence sequence = componentUpdaterState.expectedState(
        ComponentUpdaterSequence::ApplyComplete);

    EXPECT_EQ(sequence, ComponentUpdaterSequence::Invalid);
}

TEST_F(ComponentUpdaterTest, GetStatusResponse)
{
    mctp_eid_t eid = 0x1;
    size_t componentOffset = 0;
    uint8_t currentFDState = 0;
    uint8_t progressPercent = 0x65;
    uint8_t retryCount = 0;
    ComponentUpdater componentUpdater(
        eid, package, fwDeviceIDRecord, compImageInfos, compInfo,
        compIdNameInfo, 512, &updateManager, &deviceUpdater, componentOffset);
    constexpr std::array<uint8_t,
                         sizeof(pldm_msg_hdr) + sizeof(pldm_get_status_resp)>
        getStatusResponse{0x01, 0x00, 0x00, 0x00, 0x00, 0x03, 0x03,
                          0x09, 0x65, 0x05, 0x00, 0x00, 0x00, 0x00};
    auto pldmmsg = reinterpret_cast<const pldm_msg*>(getStatusResponse.data());

    EXPECT_NO_THROW({
        auto co = componentUpdater.processGetStatusResponse(
            eid, pldmmsg, sizeof(pldm_get_status_resp), currentFDState,
            progressPercent, retryCount);
        auto rc = stdexec::sync_wait(std::move(co));
        ASSERT_TRUE(rc.has_value());
    });
}

TEST_F(ComponentUpdaterTest, startComponentUpdater)
{
    mctp_eid_t eid = 0x1;
    size_t componentOffset = 0;
    ComponentUpdater componentUpdater(
        eid, package, fwDeviceIDRecord, compImageInfos, compInfo,
        compIdNameInfo, 512, &updateManager, &deviceUpdater, componentOffset);

    EXPECT_NO_THROW({
        [[maybe_unused]] auto co = componentUpdater.startComponentUpdater();
    });
}

TEST_F(ComponentUpdaterTest, requestFwData_commandNotExpectedForInvalidState)
{
    auto reqFwDataReq =
        makeRequestFwDataReq(0, PLDM_FWUP_BASELINE_TRANSFER_SIZE);
    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::UpdateComponent);
    auto response = componentUpdater.requestFwData(
        requestMsg, sizeof(pldm_request_firmware_data_req));

    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_FWUP_COMMAND_NOT_EXPECTED);
    for (int i = 0; i < 8; ++i)
    {
        runEvent();
    }
}

TEST_F(ComponentUpdaterTest, requestFwData_invalidTransferLength)
{
    auto reqFwDataReq = makeRequestFwDataReq(0, 513);
    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::RequestFirmwareData);
    auto response = componentUpdater.requestFwData(
        requestMsg, sizeof(pldm_request_firmware_data_req));

    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)],
              PLDM_FWUP_INVALID_TRANSFER_LENGTH);
}

TEST_F(ComponentUpdaterTest, requestFwDataBelowBaselineRejectedByDecode)
{
    auto reqFwDataReq =
        makeRequestFwDataReq(0, PLDM_FWUP_BASELINE_TRANSFER_SIZE - 4);
    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::RequestFirmwareData);
    auto response = componentUpdater.requestFwData(
        requestMsg, sizeof(pldm_request_firmware_data_req));

    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_ERROR_INVALID_DATA);
}

TEST_F(ComponentUpdaterTest,
       requestFwData_invalidTransferLengthAboveMaxTransfer)
{
    auto reqFwDataReq =
        makeRequestFwDataReq(0, componentUpdater.maxTransferSize + 1);
    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::RequestFirmwareData);
    auto response = componentUpdater.requestFwData(
        requestMsg, sizeof(pldm_request_firmware_data_req));

    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)],
              PLDM_FWUP_INVALID_TRANSFER_LENGTH);
}

TEST_F(ComponentUpdaterTest, requestFwDataInvalidStateWhenNotInUpdateComponent)
{
    auto reqFwDataReq =
        makeRequestFwDataReq(0, PLDM_FWUP_BASELINE_TRANSFER_SIZE);
    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    componentUpdater.componentUpdaterState.current =
        ComponentUpdaterSequence::Invalid;
    componentUpdater.componentUpdaterState.prev =
        ComponentUpdaterSequence::Invalid;

    auto response = componentUpdater.requestFwData(
        requestMsg, sizeof(pldm_request_firmware_data_req));

    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_FWUP_COMMAND_NOT_EXPECTED);
}

TEST_F(ComponentUpdaterTest, requestFwDataSuccessWhenDebugDisabled)
{
    auto reqFwDataReq =
        makeRequestFwDataReq(0, PLDM_FWUP_BASELINE_TRANSFER_SIZE);
    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    componentUpdater.updateManager->fwDebug = false;
    componentUpdater.createRequestFwDataTimer();
    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::RequestFirmwareData);

    auto response = componentUpdater.requestFwData(
        requestMsg, sizeof(pldm_request_firmware_data_req));
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_SUCCESS);
}

TEST_F(ComponentUpdaterTest, transferCompleteWithoutPreCreatedCompleteTimer)
{
    auto reqFwDataReq =
        makeRequestFwDataReq(0, PLDM_FWUP_BASELINE_TRANSFER_SIZE);
    auto requestFwDataMsg =
        reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    componentUpdater.createRequestFwDataTimer();
    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::RequestFirmwareData);
    auto reqFwResp = componentUpdater.requestFwData(
        requestFwDataMsg, sizeof(pldm_request_firmware_data_req));
    ASSERT_EQ(reqFwResp[sizeof(pldm_msg_hdr)], PLDM_SUCCESS);

    componentUpdater.interRequestSamplesGlobal = 0;
    componentUpdater.totalInterRequestTimeGlobal = std::chrono::milliseconds{0};

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + sizeof(uint8_t)>
        transferCompleteReq{0x8A, 0x05, 0x16, 0x00};
    auto transferMsg =
        reinterpret_cast<const pldm_msg*>(transferCompleteReq.data());

    auto response =
        componentUpdater.transferComplete(transferMsg, sizeof(uint8_t));

    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_SUCCESS);
    EXPECT_EQ(componentUpdater.reqFwDataTimer, nullptr);
    EXPECT_NE(componentUpdater.completeCommandsTimeoutTimer, nullptr);
}

TEST_F(ComponentUpdaterTest, requestFwData_outOfRange)
{
    auto reqFwDataReq =
        makeRequestFwDataReq(3000, PLDM_FWUP_BASELINE_TRANSFER_SIZE);
    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::RequestFirmwareData);
    auto response = componentUpdater.requestFwData(
        requestMsg, sizeof(pldm_request_firmware_data_req));

    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_FWUP_DATA_OUT_OF_RANGE);
}

TEST_F(ComponentUpdaterTest,
       requestFwData_missingTimerReturnsCommandNotExpected)
{
    auto reqFwDataReq =
        makeRequestFwDataReq(0, PLDM_FWUP_BASELINE_TRANSFER_SIZE);
    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::RequestFirmwareData);
    auto response = componentUpdater.requestFwData(
        requestMsg, sizeof(pldm_request_firmware_data_req));

    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_FWUP_COMMAND_NOT_EXPECTED);
}

TEST_F(ComponentUpdaterTest, requestFwData_successWithTimer)
{
    auto reqFwDataReq =
        makeRequestFwDataReq(0, PLDM_FWUP_BASELINE_TRANSFER_SIZE);
    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    componentUpdater.createRequestFwDataTimer();
    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::RequestFirmwareData);
    auto response = componentUpdater.requestFwData(
        requestMsg, sizeof(pldm_request_firmware_data_req));

    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_SUCCESS);
    EXPECT_EQ(response.size(), sizeof(pldm_msg_hdr) + sizeof(uint8_t) +
                                   PLDM_FWUP_BASELINE_TRANSFER_SIZE);
}

TEST_F(ComponentUpdaterTest, requestFwDataExercisesLoggingBranches)
{
    componentUpdater.createRequestFwDataTimer();
    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::RequestFirmwareData);

    auto reqStart = makeRequestFwDataReq(0, PLDM_FWUP_BASELINE_TRANSFER_SIZE);
    auto reqRetry = makeRequestFwDataReq(0, PLDM_FWUP_BASELINE_TRANSFER_SIZE);
    auto reqForward = makeRequestFwDataReq(2 * PLDM_FWUP_BASELINE_TRANSFER_SIZE,
                                           PLDM_FWUP_BASELINE_TRANSFER_SIZE);
    auto reqBackward = makeRequestFwDataReq(
        PLDM_FWUP_BASELINE_TRANSFER_SIZE / 2, PLDM_FWUP_BASELINE_TRANSFER_SIZE);

    auto responseStart = componentUpdater.requestFwData(
        reinterpret_cast<const pldm_msg*>(reqStart.data()),
        sizeof(pldm_request_firmware_data_req));
    auto responseRetry = componentUpdater.requestFwData(
        reinterpret_cast<const pldm_msg*>(reqRetry.data()),
        sizeof(pldm_request_firmware_data_req));
    auto responseForward = componentUpdater.requestFwData(
        reinterpret_cast<const pldm_msg*>(reqForward.data()),
        sizeof(pldm_request_firmware_data_req));
    auto responseBackward = componentUpdater.requestFwData(
        reinterpret_cast<const pldm_msg*>(reqBackward.data()),
        sizeof(pldm_request_firmware_data_req));

    EXPECT_EQ(responseStart[sizeof(pldm_msg_hdr)], PLDM_SUCCESS);
    EXPECT_EQ(responseRetry[sizeof(pldm_msg_hdr)], PLDM_SUCCESS);
    EXPECT_EQ(responseForward[sizeof(pldm_msg_hdr)], PLDM_SUCCESS);
    EXPECT_EQ(responseBackward[sizeof(pldm_msg_hdr)], PLDM_SUCCESS);
}

TEST_F(ComponentUpdaterTest, requestFwDataRetryStateBranch)
{
    auto reqFwDataReq =
        makeRequestFwDataReq(0, PLDM_FWUP_BASELINE_TRANSFER_SIZE);
    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    componentUpdater.createRequestFwDataTimer();
    componentUpdater.componentUpdaterState.prev =
        ComponentUpdaterSequence::RequestFirmwareData;
    componentUpdater.componentUpdaterState.current =
        ComponentUpdaterSequence::TransferComplete;

    auto response = componentUpdater.requestFwData(
        requestMsg, sizeof(pldm_request_firmware_data_req));

    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_SUCCESS);
}

TEST_F(ComponentUpdaterTest,
       requestFwDataLastChunkWithoutReqTimerWithCompleteTimerPresent)
{
    auto reqFwDataReq =
        makeRequestFwDataReq(1024 - PLDM_FWUP_BASELINE_TRANSFER_SIZE,
                             PLDM_FWUP_BASELINE_TRANSFER_SIZE);
    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::RequestFirmwareData);
    componentUpdater.createCompleteCommandsTimeoutTimer();
    componentUpdater.reqFwDataTimer.reset();

    auto response = componentUpdater.requestFwData(
        requestMsg, sizeof(pldm_request_firmware_data_req));

    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_SUCCESS);
    EXPECT_EQ(componentUpdater.reqFwDataTimer, nullptr);
    EXPECT_NE(componentUpdater.completeCommandsTimeoutTimer, nullptr);
}

TEST_F(ComponentUpdaterTest,
       transferCompleteStopsReqTimerWhenCompleteTimerMissing)
{
    componentUpdater.createRequestFwDataTimer();
    componentUpdater.completeCommandsTimeoutTimer.reset();
    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::TransferComplete);

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + sizeof(uint8_t)>
        transferCompleteReq{0x8A, 0x05, 0x16, 0x00};
    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(transferCompleteReq.data());

    auto response =
        componentUpdater.transferComplete(requestMsg, sizeof(uint8_t));

    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_SUCCESS);
    EXPECT_EQ(componentUpdater.reqFwDataTimer, nullptr);
    EXPECT_NE(componentUpdater.completeCommandsTimeoutTimer, nullptr);
}

TEST_F(ComponentUpdaterTest, transferCompleteLogsAverageWhenSamplesPresent)
{
    componentUpdater.createRequestFwDataTimer();
    componentUpdater.completeCommandsTimeoutTimer.reset();
    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::TransferComplete);
    componentUpdater.interRequestSamplesGlobal = 3;
    componentUpdater.totalInterRequestTimeGlobal = std::chrono::milliseconds{9};

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + sizeof(uint8_t)>
        transferCompleteReq{0x8A, 0x05, 0x16, 0x00};
    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(transferCompleteReq.data());

    auto response =
        componentUpdater.transferComplete(requestMsg, sizeof(uint8_t));

    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_SUCCESS);
}

TEST_F(ComponentUpdaterTest, transferComplete_retryStateBranch)
{
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + sizeof(uint8_t)>
        transferCompleteReq{0x8A, 0x05, 0x16, 0x00};
    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(transferCompleteReq.data());

    componentUpdater.componentUpdaterState.prev =
        ComponentUpdaterSequence::TransferComplete;
    componentUpdater.componentUpdaterState.current =
        ComponentUpdaterSequence::VerifyComplete;

    auto response =
        componentUpdater.transferComplete(requestMsg, sizeof(uint8_t));
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_SUCCESS);
}

TEST_F(ComponentUpdaterTest,
       transferCompleteInvalidStateReturnsCommandNotExpected)
{
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + sizeof(uint8_t)>
        transferCompleteReq{0x8A, 0x05, 0x16, 0x00};
    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(transferCompleteReq.data());

    componentUpdater.componentUpdaterState.prev =
        ComponentUpdaterSequence::RequestFirmwareData;
    componentUpdater.componentUpdaterState.current =
        ComponentUpdaterSequence::ApplyComplete;

    auto response =
        componentUpdater.transferComplete(requestMsg, sizeof(uint8_t));
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_FWUP_COMMAND_NOT_EXPECTED);
}

TEST_F(ComponentUpdaterTest, verifyComplete_retryStateBranch)
{
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + sizeof(uint8_t)>
        verifyCompleteReq{0x8A, 0x05, 0x17, 0x00};
    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(verifyCompleteReq.data());

    componentUpdater.componentUpdaterState.prev =
        ComponentUpdaterSequence::VerifyComplete;
    componentUpdater.componentUpdaterState.current =
        ComponentUpdaterSequence::ApplyComplete;

    auto response =
        componentUpdater.verifyComplete(requestMsg, sizeof(uint8_t));
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_SUCCESS);
}

TEST_F(ComponentUpdaterTest,
       verifyCompleteInvalidStateReturnsCommandNotExpected)
{
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + sizeof(uint8_t)>
        verifyCompleteReq{0x8A, 0x05, 0x17, 0x00};
    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(verifyCompleteReq.data());

    componentUpdater.componentUpdaterState.prev =
        ComponentUpdaterSequence::TransferComplete;
    componentUpdater.componentUpdaterState.current =
        ComponentUpdaterSequence::RequestFirmwareData;

    auto response =
        componentUpdater.verifyComplete(requestMsg, sizeof(uint8_t));
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_FWUP_COMMAND_NOT_EXPECTED);
}

TEST_F(ComponentUpdaterTest, applyComplete_retryStateBranch)
{
    constexpr std::array<uint8_t,
                         sizeof(pldm_msg_hdr) + sizeof(pldm_apply_complete_req)>
        applyCompleteReq{0x00, 0x00, 0x18, 0x00, 0x00, 0x00};
    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(applyCompleteReq.data());

    componentUpdater.componentUpdaterState.prev =
        ComponentUpdaterSequence::ApplyComplete;
    componentUpdater.componentUpdaterState.current =
        ComponentUpdaterSequence::Invalid;

    auto response = componentUpdater.applyComplete(
        requestMsg, sizeof(pldm_apply_complete_req));
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_SUCCESS);
}

TEST_F(ComponentUpdaterTest, applyCompleteInvalidStateReturnsCommandNotExpected)
{
    constexpr std::array<uint8_t,
                         sizeof(pldm_msg_hdr) + sizeof(pldm_apply_complete_req)>
        applyCompleteReq{0x00, 0x00, 0x18, 0x00, 0x00, 0x00};
    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(applyCompleteReq.data());

    componentUpdater.componentUpdaterState.prev =
        ComponentUpdaterSequence::TransferComplete;
    componentUpdater.componentUpdaterState.current =
        ComponentUpdaterSequence::RequestFirmwareData;

    auto response = componentUpdater.applyComplete(
        requestMsg, sizeof(pldm_apply_complete_req));
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_FWUP_COMMAND_NOT_EXPECTED);
}

TEST_F(ComponentUpdaterTest, processGetStatusResponseDecodeFailureAtRetryLimit)
{
    uint8_t currentFDState = 0;
    uint8_t progressPercent = 0;
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr)> invalidGetStatusResp{
        0x01, 0x00, 0x1A};
    auto pldmmsg =
        reinterpret_cast<const pldm_msg*>(invalidGetStatusResp.data());
    auto co = componentUpdater.processGetStatusResponse(
        0x1, pldmmsg, 0, currentFDState, progressPercent,
        maxDecodeFailureRetries);
    auto rc = stdexec::sync_wait(std::move(co));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(std::get<0>(*rc), PLDM_ERROR_INVALID_DATA);
}

TEST_F(ComponentUpdaterTest, processGetStatusResponseCompletionCodeFailure)
{
    uint8_t currentFDState = 0;
    uint8_t progressPercent = 0;
    constexpr std::array<uint8_t,
                         sizeof(pldm_msg_hdr) + sizeof(pldm_get_status_resp)>
        getStatusResponseWithError{0x01, 0x00, 0x1a, 0x01, 0x00, 0x03, 0x03,
                                   0x09, 0x65, 0x05, 0x00, 0x00, 0x00, 0x00};
    auto pldmmsg =
        reinterpret_cast<const pldm_msg*>(getStatusResponseWithError.data());

    auto co = componentUpdater.processGetStatusResponse(
        0x1, pldmmsg, sizeof(pldm_get_status_resp), currentFDState,
        progressPercent, 0);
    auto rc = stdexec::sync_wait(std::move(co));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(std::get<0>(*rc), PLDM_ERROR);
}

TEST_F(ComponentUpdaterTest, updateComponentCompleteReturnsWhenHandlePending)
{
    componentUpdater.updateCompletionCoHandle.emplace();
    EXPECT_NO_THROW({
        componentUpdater.updateComponentComplete(
            ComponentUpdateStatus::UpdateFailed);
    });
}

TEST_F(ComponentUpdaterTest, getStatusReturnsWhenHandlePending)
{
    componentUpdater.getStatusTaskHandle.emplace();
    EXPECT_NO_THROW({ componentUpdater.GetStatus([](uint8_t) {}); });
}

TEST_F(ComponentUpdaterTest,
       processUpdateComponentResponseCompletionCodeFailure)
{
    initializeFromParsedPackage();
    auto [responseBytes,
          payloadLen] = makeUpdateComponentResp(PLDM_SUCCESS, 0, 0);
    responseBytes[sizeof(pldm_msg_hdr)] = PLDM_ERROR;
    auto* responseMsg = reinterpret_cast<const pldm_msg*>(responseBytes.data());

    auto co = componentUpdater.processUpdateComponentResponse(
        0x1, responseMsg, payloadLen, 0);
    auto rc = stdexec::sync_wait(std::move(co));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(std::get<0>(*rc), PLDM_ERROR);
    EXPECT_EQ(componentUpdater.componentUpdaterState.current,
              ComponentUpdaterSequence::Invalid);
    EXPECT_NE(componentUpdater.pldmRequest, nullptr);
}

TEST_F(ComponentUpdaterTest,
       processUpdateComponentResponseDecodeFailureBelowRetryLimit)
{
    initializeFromParsedPackage();
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr)> invalidResp{
        0x01, 0x00, 0x14};
    auto* responseMsg = reinterpret_cast<const pldm_msg*>(invalidResp.data());

    auto co =
        componentUpdater.processUpdateComponentResponse(0x1, responseMsg, 0, 0);
    auto rc = stdexec::sync_wait(std::move(co));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(std::get<0>(*rc), PLDM_ERROR_INVALID_DATA);
    EXPECT_EQ(componentUpdater.componentUpdaterState.current,
              ComponentUpdaterSequence::UpdateComponent);
    EXPECT_EQ(componentUpdater.pldmRequest, nullptr);
}

TEST_F(ComponentUpdaterTest,
       processUpdateComponentResponseDecodeFailureAtRetryLimit)
{
    initializeFromParsedPackage();
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr)> invalidResp{
        0x01, 0x00, 0x14};
    auto* responseMsg = reinterpret_cast<const pldm_msg*>(invalidResp.data());

    auto co = componentUpdater.processUpdateComponentResponse(
        0x1, responseMsg, 0, maxDecodeFailureRetries);
    auto rc = stdexec::sync_wait(std::move(co));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(std::get<0>(*rc), PLDM_ERROR_INVALID_DATA);
    EXPECT_EQ(componentUpdater.componentUpdaterState.current,
              ComponentUpdaterSequence::Invalid);
    ASSERT_NE(componentUpdater.pldmRequest, nullptr);
    runEvent();
    runEvent();
    waitForUpdateCompletionTask();
}

TEST_F(ComponentUpdaterTest,
       processUpdateComponentResponseCompatibilitySkipSchedulesCompletion)
{
    initializeFromParsedPackage();
    auto [responseBytes, payloadLen] = makeUpdateComponentResp(
        PLDM_SUCCESS, 1, PLDM_CCRC_COMP_COMPARISON_STAMP_IDENTICAL);
    auto* responseMsg = reinterpret_cast<const pldm_msg*>(responseBytes.data());

    auto co = componentUpdater.processUpdateComponentResponse(
        0x1, responseMsg, payloadLen, 0);
    auto rc = stdexec::sync_wait(std::move(co));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(std::get<0>(*rc), PLDM_ERROR);
    EXPECT_NE(componentUpdater.pldmRequest, nullptr);
    runEvent();
    runEvent();
    waitForUpdateCompletionTask();
}

TEST_F(ComponentUpdaterTest,
       processUpdateComponentResponseCompatibilityFailureSchedulesCompletion)
{
    initializeFromParsedPackage();
    auto [responseBytes, payloadLen] = makeUpdateComponentResp(
        PLDM_SUCCESS, 1, PLDM_CCRC_COMP_COMPARISON_STAMP_LOWER);
    auto* responseMsg = reinterpret_cast<const pldm_msg*>(responseBytes.data());

    auto co = componentUpdater.processUpdateComponentResponse(
        0x1, responseMsg, payloadLen, 0);
    auto rc = stdexec::sync_wait(std::move(co));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(std::get<0>(*rc), PLDM_ERROR);
    EXPECT_EQ(componentUpdater.componentUpdaterState.current,
              ComponentUpdaterSequence::Invalid);
    EXPECT_NE(componentUpdater.pldmRequest, nullptr);
    runEvent();
    runEvent();
    waitForUpdateCompletionTask();
}

TEST_F(ComponentUpdaterTest, requestFwDataTimerCallbackTriggersCancelPath)
{
    initializeFromParsedPackage();
    componentUpdater.createRequestFwDataTimer();
    ASSERT_NE(componentUpdater.reqFwDataTimer, nullptr);

    EXPECT_NO_THROW({
        componentUpdater.reqFwDataTimer->start(std::chrono::seconds(0), false);
        runEvent();
        runEvent();
    });
    waitForDiscoverTask();
    waitForUpdateCompletionTask();
}

TEST_F(ComponentUpdaterTest,
       completeCommandsTimeoutTimerCallbackTriggersCancelPath)
{
    initializeFromParsedPackage();
    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::TransferComplete);
    componentUpdater.createCompleteCommandsTimeoutTimer();
    ASSERT_NE(componentUpdater.completeCommandsTimeoutTimer, nullptr);

    EXPECT_NO_THROW({
        componentUpdater.completeCommandsTimeoutTimer->start(
            std::chrono::seconds(0), false);
        runEvent();
        runEvent();
    });
    waitForDiscoverTask();
    waitForUpdateCompletionTask();
}

TEST_F(ComponentUpdaterTest, applyCompleteSuccessSchedulesGetStatusPath)
{
    initializeFromParsedPackage();
    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::ApplyComplete);
    constexpr std::array<uint8_t,
                         sizeof(pldm_msg_hdr) + sizeof(pldm_apply_complete_req)>
        applyCompleteReq{0x00, 0x00, 0x18, 0x00, 0x00, 0x00};
    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(applyCompleteReq.data());

    auto response = componentUpdater.applyComplete(
        requestMsg, sizeof(pldm_apply_complete_req));
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_SUCCESS);
    ASSERT_TRUE(static_cast<bool>(componentUpdater.pendingPostResponseAction));
    EXPECT_EQ(componentUpdater.pldmRequest, nullptr);
    componentUpdater.onResponseSendComplete(true);
    ASSERT_NE(componentUpdater.pldmRequest, nullptr);
    EXPECT_NO_THROW({
        runEvent();
        runEvent();
    });
    waitForGetStatusTask();
    runEvent();
    waitForDiscoverTask();
    waitForUpdateCompletionTask();
}

TEST_F(ComponentUpdaterTest, applyCompleteDecodeFailureSchedulesDeferred)
{
    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::ApplyComplete);
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr)> invalidReq{
        0x8A, 0x05, 0x18};
    auto requestMsg = reinterpret_cast<const pldm_msg*>(invalidReq.data());

    auto response = componentUpdater.applyComplete(requestMsg, 0);
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_ERROR_INVALID_DATA);
    ASSERT_NE(componentUpdater.pldmRequest, nullptr);
    runEvent();
    runEvent();
    waitForDiscoverTask();
    waitForUpdateCompletionTask();
}

TEST_F(ComponentUpdaterTest, applyCompleteFailureSchedulesDeferred)
{
    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::ApplyComplete);
    constexpr std::array<uint8_t,
                         sizeof(pldm_msg_hdr) + sizeof(pldm_apply_complete_req)>
        applyCompleteReq{0x00, 0x00, 0x18, 0x02, 0x00, 0x00};
    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(applyCompleteReq.data());

    auto response = componentUpdater.applyComplete(
        requestMsg, sizeof(pldm_apply_complete_req));
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_SUCCESS);
    ASSERT_NE(componentUpdater.pldmRequest, nullptr);
    runEvent();
    runEvent();
    waitForDiscoverTask();
    waitForUpdateCompletionTask();
}

TEST_F(ComponentUpdaterTest,
       DISABLED_sendGetStatusRequestInvokesCallbackOnFailure)
{
    bool callbackCalled = false;
    uint8_t callbackValue = 0xff;
    auto co = componentUpdater.sendGetStatusRequest([&](uint8_t state) {
        callbackCalled = true;
        callbackValue = state;
    });
    auto rc = stdexec::sync_wait(std::move(co));
    ASSERT_TRUE(rc.has_value());
    EXPECT_TRUE(callbackCalled);
    EXPECT_EQ(callbackValue, 0);
    EXPECT_NE(std::get<0>(*rc), PLDM_SUCCESS);
}

TEST_F(ComponentUpdaterTest, logComponentUpdateDurationNoStartTimeNoop)
{
    componentUpdater.componentUpdateStartTime =
        std::chrono::steady_clock::time_point{};
    EXPECT_NO_THROW({ componentUpdater.logComponentUpdateDuration(); });
}

TEST_F(ComponentUpdaterTest, applyCompleteSucceededStatusHandlerStopsTimer)
{
    initializeFromParsedPackage();
    bitfield16_t compActivationModification{0x1};
    componentUpdater.componentUpdateStartTime =
        std::chrono::steady_clock::now() - std::chrono::seconds(1);
    componentUpdater.createCompleteCommandsTimeoutTimer();
    ASSERT_TRUE(componentUpdater.completeCommandsTimeoutTimer != nullptr);

    EXPECT_NO_THROW({
        componentUpdater.applyCompleteSucceededStatusHandler(
            "v1", compActivationModification);
    });
    EXPECT_EQ(componentUpdater.completeCommandsTimeoutTimer, nullptr);
    ASSERT_NE(componentUpdater.pldmRequest, nullptr);
    runEvent();
    waitForUpdateCompletionTask();
}

TEST_F(ComponentUpdaterTest,
       applyCompleteSucceededStatusHandlerWithoutTimerStillSchedulesCompletion)
{
    initializeFromParsedPackage();
    bitfield16_t compActivationModification{0x1};
    componentUpdater.completeCommandsTimeoutTimer.reset();
    componentUpdater.componentUpdateStartTime =
        std::chrono::steady_clock::now() - std::chrono::seconds(1);

    EXPECT_NO_THROW({
        componentUpdater.applyCompleteSucceededStatusHandler(
            "v1", compActivationModification);
    });
    EXPECT_EQ(componentUpdater.completeCommandsTimeoutTimer, nullptr);
    ASSERT_NE(componentUpdater.pldmRequest, nullptr);
    runEvent();
    waitForUpdateCompletionTask();
}

TEST_F(ComponentUpdaterTest,
       completeFailedStatusHandlerReturnsWhenDiscoverTaskPending)
{
    componentUpdater.discoverMctpTerminusTaskHandle.emplace();
    EXPECT_NO_THROW({
        componentUpdater.completeFailedStatusHandler(
            transferFailed, PLDM_TRANSFER_COMPLETE, PLDM_ERROR);
    });
    EXPECT_TRUE(componentUpdater.discoverMctpTerminusTaskHandle.has_value());
}

TEST_F(ComponentUpdaterTest,
       completeFailedStatusHandlerStopsExistingCompleteCommandsTimer)
{
    initializeFromParsedPackage();
    componentUpdater.createCompleteCommandsTimeoutTimer();
    ASSERT_NE(componentUpdater.completeCommandsTimeoutTimer, nullptr);

    componentUpdater.completeFailedStatusHandler(
        transferFailed, PLDM_TRANSFER_COMPLETE, PLDM_ERROR);

    EXPECT_EQ(componentUpdater.completeCommandsTimeoutTimer, nullptr);
    for (int i = 0; i < 6; ++i)
    {
        runEvent();
    }
    EXPECT_TRUE(componentUpdater.discoverMctpTerminusTaskHandle.has_value());
    waitForDiscoverTask();
    waitForUpdateCompletionTask();
}

TEST_F(ComponentUpdaterTest, requestFwDataDecodeFailureReturnsInvalidData)
{
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr)> invalidReq{
        0x8A, 0x05, 0x15};
    auto requestMsg = reinterpret_cast<const pldm_msg*>(invalidReq.data());

    auto response = componentUpdater.requestFwData(requestMsg, 0);
    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_ERROR_INVALID_DATA);
}

TEST_F(ComponentUpdaterTest, requestFwDataLastChunkStartsCompleteTimer)
{
    auto reqFwDataReq =
        makeRequestFwDataReq(1024 - PLDM_FWUP_BASELINE_TRANSFER_SIZE,
                             PLDM_FWUP_BASELINE_TRANSFER_SIZE);
    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    componentUpdater.createRequestFwDataTimer();
    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::RequestFirmwareData);
    auto response = componentUpdater.requestFwData(
        requestMsg, sizeof(pldm_request_firmware_data_req));

    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_SUCCESS);
    EXPECT_EQ(componentUpdater.reqFwDataTimer, nullptr);
    EXPECT_NE(componentUpdater.completeCommandsTimeoutTimer, nullptr);
}

TEST_F(ComponentUpdaterTest, getStatusStoresCompletionResult)
{
    componentUpdater.getStatusTaskHandle.reset();
    componentUpdater.GetStatus([](uint8_t) {});
    for (int i = 0; i < 6; ++i)
    {
        runEvent();
    }

    ASSERT_TRUE(componentUpdater.getStatusTaskHandle.has_value());
    auto& [scope, rcOpt] = *componentUpdater.getStatusTaskHandle;
    (void)scope;
    EXPECT_TRUE(rcOpt.has_value());
}

TEST_F(ComponentUpdaterTest, getStatusReplacesCompletedHandle)
{
    auto& [scope, rcOpt] = componentUpdater.getStatusTaskHandle.emplace();
    (void)scope;
    rcOpt.emplace(PLDM_SUCCESS);

    componentUpdater.GetStatus([](uint8_t) {});
    for (int i = 0; i < 6; ++i)
    {
        runEvent();
    }

    ASSERT_TRUE(componentUpdater.getStatusTaskHandle.has_value());
    auto& [newScope, newRcOpt] = *componentUpdater.getStatusTaskHandle;
    (void)newScope;
    EXPECT_TRUE(newRcOpt.has_value());
}

TEST_F(ComponentUpdaterTest, updateComponentCompleteStoresCompletionResult)
{
    initializeFromParsedPackage();
    componentUpdater.updateCompletionCoHandle.reset();
    componentUpdater.updateComponentComplete(
        ComponentUpdateStatus::UpdateFailed);
    waitForAsyncResult(componentUpdater.updateCompletionCoHandle);

    ASSERT_TRUE(componentUpdater.updateCompletionCoHandle.has_value());
    auto& [scope, rcOpt] = *componentUpdater.updateCompletionCoHandle;
    (void)scope;
    EXPECT_TRUE(rcOpt.has_value());
    waitForUpdateCompletionTask();
}

TEST_F(ComponentUpdaterTest, updateComponentCompleteReplacesCompletedHandle)
{
    initializeFromParsedPackage();
    auto& [scope, rcOpt] = componentUpdater.updateCompletionCoHandle.emplace();
    (void)scope;
    rcOpt.emplace(PLDM_SUCCESS);

    componentUpdater.updateComponentComplete(
        ComponentUpdateStatus::UpdateFailed);
    waitForAsyncResult(componentUpdater.updateCompletionCoHandle);

    ASSERT_TRUE(componentUpdater.updateCompletionCoHandle.has_value());
    auto& [newScope, newRcOpt] = *componentUpdater.updateCompletionCoHandle;
    (void)newScope;
    EXPECT_TRUE(newRcOpt.has_value());
    waitForUpdateCompletionTask();
}

TEST_F(ComponentUpdaterTest, requestFwDataTimerCallbackCompletesCancelTask)
{
    initializeFromParsedPackage();
    componentUpdater.createRequestFwDataTimer();
    ASSERT_NE(componentUpdater.reqFwDataTimer, nullptr);

    componentUpdater.reqFwDataTimer->start(std::chrono::seconds(0), false);
    for (int i = 0; i < 8; ++i)
    {
        runEvent();
    }

    ASSERT_TRUE(componentUpdater.discoverMctpTerminusTaskHandle.has_value());
    auto& [scope, rcOpt] = *componentUpdater.discoverMctpTerminusTaskHandle;
    (void)scope;
    EXPECT_TRUE(rcOpt.has_value());
    waitForUpdateCompletionTask();
}

TEST_F(ComponentUpdaterTest,
       completeFailedStatusHandlerResetsCompletedCancelTaskHandle)
{
    auto& [scope,
           rcOpt] = componentUpdater.discoverMctpTerminusTaskHandle.emplace();
    (void)scope;
    rcOpt.emplace(PLDM_SUCCESS);

    componentUpdater.completeFailedStatusHandler(
        transferFailed, PLDM_TRANSFER_COMPLETE, PLDM_ERROR);
    for (int i = 0; i < 4; ++i)
    {
        runEvent();
    }

    ASSERT_TRUE(componentUpdater.discoverMctpTerminusTaskHandle.has_value());
    waitForDiscoverTask();
    waitForUpdateCompletionTask();
}

TEST_F(ComponentUpdaterTest, completeCommandsTimerApplyStateCompletesCancelTask)
{
    initializeFromParsedPackage();
    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::ApplyComplete);
    componentUpdater.createCompleteCommandsTimeoutTimer();
    ASSERT_NE(componentUpdater.completeCommandsTimeoutTimer, nullptr);

    componentUpdater.completeCommandsTimeoutTimer->start(
        std::chrono::seconds(0), false);
    for (int i = 0; i < 8; ++i)
    {
        runEvent();
    }

    ASSERT_TRUE(componentUpdater.discoverMctpTerminusTaskHandle.has_value());
    auto& [scope, rcOpt] = *componentUpdater.discoverMctpTerminusTaskHandle;
    (void)scope;
    EXPECT_TRUE(rcOpt.has_value());
    waitForUpdateCompletionTask();
}

TEST_F(ComponentUpdaterTest,
       completeCommandsTimerVerifyStateCompletesCancelTask)
{
    initializeFromParsedPackage();
    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::VerifyComplete);
    componentUpdater.createCompleteCommandsTimeoutTimer();
    ASSERT_NE(componentUpdater.completeCommandsTimeoutTimer, nullptr);

    componentUpdater.completeCommandsTimeoutTimer->start(
        std::chrono::seconds(0), false);
    for (int i = 0; i < 8; ++i)
    {
        runEvent();
    }

    ASSERT_TRUE(componentUpdater.discoverMctpTerminusTaskHandle.has_value());
    auto& [scope, rcOpt] = *componentUpdater.discoverMctpTerminusTaskHandle;
    (void)scope;
    EXPECT_TRUE(rcOpt.has_value());
    waitForUpdateCompletionTask();
}

TEST_F(ComponentUpdaterTest,
       completeCommandsTimerInvalidStateCompletesCancelTask)
{
    initializeFromParsedPackage();
    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::Invalid);
    componentUpdater.createCompleteCommandsTimeoutTimer();
    ASSERT_NE(componentUpdater.completeCommandsTimeoutTimer, nullptr);

    componentUpdater.completeCommandsTimeoutTimer->start(
        std::chrono::seconds(0), false);
    for (int i = 0; i < 8; ++i)
    {
        runEvent();
    }

    ASSERT_TRUE(componentUpdater.discoverMctpTerminusTaskHandle.has_value());
    auto& [scope, rcOpt] = *componentUpdater.discoverMctpTerminusTaskHandle;
    (void)scope;
    EXPECT_TRUE(rcOpt.has_value());
    waitForUpdateCompletionTask();
}

TEST_F(ComponentUpdaterTest, handleLoggingPrintsAverageForMegabyteBoundary)
{
    componentUpdater.lastRequestTime =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(2);
    componentUpdater.interRequestSamples = 1;
    componentUpdater.totalInterRequestTime = std::chrono::milliseconds(2);
    componentUpdater.lastAvgPrintedMBIndex = 0;

    EXPECT_NO_THROW({
        componentUpdater.handleLogging(1024 * 1024,
                                       PLDM_FWUP_BASELINE_TRANSFER_SIZE);
    });
    EXPECT_EQ(componentUpdater.lastAvgPrintedMBIndex, 1);
}

TEST_F(ComponentUpdaterTest, requestFwDataSuccessPathEncodeFailureBranch)
{
    auto reqFwDataReq =
        makeRequestFwDataReq(0, PLDM_FWUP_BASELINE_TRANSFER_SIZE);
    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    componentUpdater.createRequestFwDataTimer();
    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::RequestFirmwareData);

    gFailEncodeRequestFwDataResp = true;
    gEncodeRequestFwDataRespRc = PLDM_ERROR;

    auto response = componentUpdater.requestFwData(
        requestMsg, sizeof(pldm_request_firmware_data_req));
    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);

    gFailEncodeRequestFwDataResp = false;
}

TEST_F(ComponentUpdaterTest, transferCompleteDecodeFailureEncodeFailureBranch)
{
    initializeFromParsedPackage();
    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::TransferComplete);
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr)> invalidReq{
        0x8A, 0x05, 0x16};
    auto requestMsg = reinterpret_cast<const pldm_msg*>(invalidReq.data());

    gFailEncodeTransferCompleteResp = true;
    gEncodeTransferCompleteRespRc = PLDM_ERROR;

    auto response = componentUpdater.transferComplete(requestMsg, 0);
    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);
    ASSERT_NE(componentUpdater.pldmRequest, nullptr);
    runEvent();
    runEvent();
    waitForDiscoverTask();
    waitForUpdateCompletionTask();

    gFailEncodeTransferCompleteResp = false;
}

TEST_F(ComponentUpdaterTest, transferCompleteRetryPathEncodeFailureBranch)
{
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + sizeof(uint8_t)>
        transferCompleteReq{0x8A, 0x05, 0x16, 0x00};
    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(transferCompleteReq.data());

    componentUpdater.componentUpdaterState.prev =
        ComponentUpdaterSequence::TransferComplete;
    componentUpdater.componentUpdaterState.current =
        ComponentUpdaterSequence::VerifyComplete;

    gFailEncodeTransferCompleteResp = true;
    gEncodeTransferCompleteRespRc = PLDM_ERROR;

    auto response =
        componentUpdater.transferComplete(requestMsg, sizeof(uint8_t));
    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);

    gFailEncodeTransferCompleteResp = false;
}

TEST_F(ComponentUpdaterTest, transferCompleteSuccessPathEncodeFailureBranch)
{
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + sizeof(uint8_t)>
        transferCompleteReq{0x8A, 0x05, 0x16, 0x00};
    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(transferCompleteReq.data());

    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::TransferComplete);
    componentUpdater.interRequestSamplesGlobal = 0;
    componentUpdater.totalInterRequestTimeGlobal = std::chrono::milliseconds{0};

    gFailEncodeTransferCompleteResp = true;
    gEncodeTransferCompleteRespRc = PLDM_ERROR;

    auto response =
        componentUpdater.transferComplete(requestMsg, sizeof(uint8_t));
    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);

    gFailEncodeTransferCompleteResp = false;
}

TEST_F(ComponentUpdaterTest, verifyCompleteDecodeFailureEncodeFailureBranch)
{
    initializeFromParsedPackage();
    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::VerifyComplete);
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr)> invalidReq{
        0x8A, 0x05, 0x17};
    auto requestMsg = reinterpret_cast<const pldm_msg*>(invalidReq.data());

    gFailEncodeVerifyCompleteResp = true;
    gEncodeVerifyCompleteRespRc = PLDM_ERROR;

    auto response = componentUpdater.verifyComplete(requestMsg, 0);
    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);
    ASSERT_NE(componentUpdater.pldmRequest, nullptr);
    runEvent();
    runEvent();
    waitForDiscoverTask();
    waitForUpdateCompletionTask();

    gFailEncodeVerifyCompleteResp = false;
}

TEST_F(ComponentUpdaterTest, verifyCompleteRetryPathEncodeFailureBranch)
{
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + sizeof(uint8_t)>
        verifyCompleteReq{0x8A, 0x05, 0x17, 0x00};
    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(verifyCompleteReq.data());

    componentUpdater.componentUpdaterState.prev =
        ComponentUpdaterSequence::VerifyComplete;
    componentUpdater.componentUpdaterState.current =
        ComponentUpdaterSequence::ApplyComplete;

    gFailEncodeVerifyCompleteResp = true;
    gEncodeVerifyCompleteRespRc = PLDM_ERROR;

    auto response =
        componentUpdater.verifyComplete(requestMsg, sizeof(uint8_t));
    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);

    gFailEncodeVerifyCompleteResp = false;
}

TEST_F(ComponentUpdaterTest, verifyCompleteSuccessPathEncodeFailureBranch)
{
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + sizeof(uint8_t)>
        verifyCompleteReq{0x8A, 0x05, 0x17, 0x00};
    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(verifyCompleteReq.data());

    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::VerifyComplete);

    gFailEncodeVerifyCompleteResp = true;
    gEncodeVerifyCompleteRespRc = PLDM_ERROR;

    auto response =
        componentUpdater.verifyComplete(requestMsg, sizeof(uint8_t));
    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);

    gFailEncodeVerifyCompleteResp = false;
}

TEST_F(ComponentUpdaterTest, applyCompleteRetryPathEncodeFailureBranch)
{
    constexpr std::array<uint8_t,
                         sizeof(pldm_msg_hdr) + sizeof(pldm_apply_complete_req)>
        applyCompleteReq{0x00, 0x00, 0x18, 0x00, 0x00, 0x00};
    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(applyCompleteReq.data());

    componentUpdater.componentUpdaterState.prev =
        ComponentUpdaterSequence::ApplyComplete;
    componentUpdater.componentUpdaterState.current =
        ComponentUpdaterSequence::Invalid;

    gFailEncodeApplyCompleteResp = true;
    gEncodeApplyCompleteRespRc = PLDM_ERROR;

    auto response = componentUpdater.applyComplete(
        requestMsg, sizeof(pldm_apply_complete_req));
    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);

    gFailEncodeApplyCompleteResp = false;
}
