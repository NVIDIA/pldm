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
#include "common/utils.hpp"
#include "fw-update/package_parser.hpp"
#include "fw-update/update_manager.hpp"

#include <systemd/sd-event.h>

#include <sdbusplus/bus.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>

#include <gtest/gtest.h>

using namespace pldm;
using namespace pldm::fw_update;

class UpdateManagerTest : public testing::Test
{
  protected:
    UpdateManagerTest() :
        busMock(sdbusplus::get_mocked_new(&sdbusMock)),
        event(sdeventplus::Event::get_default()),
        dbusImplRequester(busMock, "/xyz/openbmc_project/pldm"),
        reqHandler(event, dbusImplRequester, sockManager, false,
                   std::chrono::seconds(1), 2, std::chrono::milliseconds(100))
    {}

    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    sdbusplus::bus::bus busMock;
    sdeventplus::Event event;
    pldm::dbus_api::Requester dbusImplRequester;
    pldm::mctp_socket::Manager sockManager;
    requester::Handler<requester::Request> reqHandler;
    DescriptorMap descriptorMap;
    ComponentInfoMap componentInfoMap;
    ComponentNameMap componentNameMap;
};

TEST_F(UpdateManagerTest, getActivationMethod_Automatic)
{
    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap, componentInfoMap,
                                componentNameMap, true);

    const std::string activationMethodResult = "Automatic";

    bitfield16_t compActivationModification{0x1};

    std::string result =
        updateManager.getActivationMethod(compActivationModification);

    EXPECT_EQ(result, activationMethodResult);
}

TEST_F(UpdateManagerTest, getActivationMethod_SelfContained)
{
    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap, componentInfoMap,
                                componentNameMap, true);

    const std::string activationMethodResult = "Self-Contained";

    bitfield16_t compActivationModification{0x2};

    std::string result =
        updateManager.getActivationMethod(compActivationModification);

    EXPECT_EQ(result, activationMethodResult);
}

TEST_F(UpdateManagerTest, getActivationMethod_AutomaticOrSelfContained)
{
    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap, componentInfoMap,
                                componentNameMap, true);

    const std::string activationMethodResult = "Automatic or Self-Contained";

    bitfield16_t compActivationModification{0x3};

    std::string result =
        updateManager.getActivationMethod(compActivationModification);

    EXPECT_EQ(result, activationMethodResult);
}

TEST_F(UpdateManagerTest, getActivationMethod_MediumSpecificReset)
{
    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap, componentInfoMap,
                                componentNameMap, true);

    const std::string activationMethodResult = "Medium-specific reset";

    bitfield16_t compActivationModification{0x4};

    std::string result =
        updateManager.getActivationMethod(compActivationModification);

    EXPECT_EQ(result, activationMethodResult);
}

TEST_F(UpdateManagerTest, getActivationMethod_SystemReboot)
{
    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap, componentInfoMap,
                                componentNameMap, true);

    const std::string activationMethodResult = "System reboot";

    bitfield16_t compActivationModification{0x8};

    std::string result =
        updateManager.getActivationMethod(compActivationModification);

    EXPECT_EQ(result, activationMethodResult);
}

TEST_F(UpdateManagerTest, getActivationMethod_AcPowerCycle)
{
    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap, componentInfoMap,
                                componentNameMap, true);

    const std::string activationMethodResult = "AC power cycle";

    bitfield16_t compActivationModification{0x20};

    std::string result =
        updateManager.getActivationMethod(compActivationModification);

    EXPECT_EQ(result, activationMethodResult);
}

TEST_F(UpdateManagerTest, getActivationMethod_DcOrAcPowerCycle)
{
    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap, componentInfoMap,
                                componentNameMap, true);

    const std::string activationMethodResult =
        "DC power cycle or AC power cycle";

    bitfield16_t compActivationModification{0x30};

    std::string result =
        updateManager.getActivationMethod(compActivationModification);

    EXPECT_EQ(result, activationMethodResult);
}

TEST_F(UpdateManagerTest, clearFirmwareUpdatePackage)
{
    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap, componentInfoMap,
                                componentNameMap, true);

    EXPECT_NO_THROW({ updateManager.clearFirmwareUpdatePackage(); });
}

TEST_F(UpdateManagerTest, updateDeviceCompletion)
{
    mctp_eid_t eid = 0;
    bool status = true;

    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap, componentInfoMap,
                                componentNameMap, true);

    std::vector<ComponentName> successCompNames = {
        "TestComponentName1", "TestComponentName2", "TestComponentName3"};

    EXPECT_NO_THROW({
        updateManager.updateDeviceCompletion(eid, status, successCompNames);
    });
}

TEST_F(UpdateManagerTest, updateDeviceCompletion_withStatusEqualsFalse)
{
    mctp_eid_t eid = 0;
    bool status = false;

    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap, componentInfoMap,
                                componentNameMap, true);

    EXPECT_NO_THROW({ updateManager.updateDeviceCompletion(eid, status); });
}

TEST_F(UpdateManagerTest, updateDeviceCompletion_withoutSuccessCompNames)
{
    mctp_eid_t eid = 0;
    bool status = true;

    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap, componentInfoMap,
                                componentNameMap, true);

    EXPECT_NO_THROW({ updateManager.updateDeviceCompletion(eid, status); });
}

TEST_F(UpdateManagerTest, updateActivationProgress)
{
    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap, componentInfoMap,
                                componentNameMap, true);

    EXPECT_NO_THROW({ updateManager.updateActivationProgress(); });
}

TEST_F(UpdateManagerTest, clearActivationInfo)
{
    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap, componentInfoMap,
                                componentNameMap, true);

    EXPECT_NO_THROW({ updateManager.clearActivationInfo(); });
}

TEST_F(UpdateManagerTest, activatePackage_throw_exception)
{
    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap, componentInfoMap,
                                componentNameMap, true);

    EXPECT_THROW(updateManager.activatePackage(),
                 sdbusplus::exception::SdBusError);
}

TEST_F(UpdateManagerTest, processPackage_empty_descriptorMap)
{
    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap, componentInfoMap,
                                componentNameMap, true);

    updateManager.processPackage("./test_pkg");
}

TEST_F(UpdateManagerTest, processPackage_no_matching_devices_found)
{
    mctp_eid_t eid = 0;

    const DescriptorMap descriptorMap2{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("ECSKU",
                           std::vector<uint8_t>{0x49, 0x35, 0x36, 0x81})}}}};

    ComponentInfoMap componentInfoMap;
    ComponentNameMap componentNameMap;
    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap2, componentInfoMap,
                                componentNameMap, true);

    updateManager.processPackage("./test_pkg");
}

TEST_F(UpdateManagerTest, processPackage_new)
{

    int expectedResult(0);

    requester::Handler<requester::Request> reqHandler2(
        event, dbusImplRequester, sockManager, false, std::chrono::seconds(1),
        2, std::chrono::milliseconds(100));

    mctp_eid_t eid = 0x01;

    const DescriptorMap descriptorMap2{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xc9, 0x3e, 0xc5, 0x41, 0x15,
                                0x95, 0xf4, 0x48, 0x70, 0x1d, 0x49, 0xd6,
                                0x75}}}}};

    UpdateManager updateManager(event, reqHandler2, dbusImplRequester,
                                descriptorMap2, componentInfoMap,
                                componentNameMap, true);

    int result = updateManager.processPackage("./test_pkg");

    EXPECT_EQ(result, expectedResult);
}

TEST_F(UpdateManagerTest, handleRequest_empty_descriptorMap)
{
    uint8_t expectedResult = 0x15;

    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap, componentInfoMap,
                                componentNameMap, true);

    mctp_eid_t eid = 0;

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_request_firmware_data_req)>
        reqFwDataReq{0x8A, 0x05, 0x15, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x02, 0x00, 0x00};

    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    auto result =
        updateManager.handleRequest(eid, PLDM_REQUEST_FIRMWARE_DATA, requestMsg,
                                    sizeof(pldm_request_firmware_data_req));

    EXPECT_EQ(result[2], expectedResult);
}

TEST_F(UpdateManagerTest, handleRequest_request_fw_data)
{
    uint8_t expectedResult = 0x15;
    mctp_eid_t eid = 0;

    const DescriptorMap descriptorMap2{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xc9, 0x3e, 0xc5, 0x41, 0x15,
                                0x95, 0xf4, 0x48, 0x70, 0x1d, 0x49, 0xd6,
                                0x75}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("OpenBMC", std::vector<uint8_t>{0x01, 0x02})}}}};

    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap2, componentInfoMap,
                                componentNameMap, true);

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_request_firmware_data_req)>
        reqFwDataReq{0x8A, 0x05, 0x15, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x02, 0x00, 0x00};

    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());
    updateManager.processPackage("./test_pkg");

    auto result =
        updateManager.handleRequest(eid, PLDM_REQUEST_FIRMWARE_DATA, requestMsg,
                                    sizeof(pldm_request_firmware_data_req));

    EXPECT_EQ(result[2], expectedResult);
}

TEST_F(UpdateManagerTest, handleRequest_transfer_complete)
{
    uint8_t expectedResult = 0x16;
    mctp_eid_t eid = 0;

    const DescriptorMap descriptorMap2{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xc9, 0x3e, 0xc5, 0x41, 0x15,
                                0x95, 0xf4, 0x48, 0x70, 0x1d, 0x49, 0xd6,
                                0x75}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("OpenBMC", std::vector<uint8_t>{0x01, 0x02})}}}};

    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap2, componentInfoMap,
                                componentNameMap, true);

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_request_firmware_data_req)>
        reqFwDataReq{0x8A, 0x05, 0x16, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x02, 0x00, 0x00};

    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    updateManager.processPackage("./test_pkg");

    auto result =
        updateManager.handleRequest(eid, PLDM_TRANSFER_COMPLETE, requestMsg,
                                    sizeof(pldm_request_firmware_data_req));

    EXPECT_EQ(result[2], expectedResult);
}

TEST_F(UpdateManagerTest, handleRequest_verify_complete)
{
    uint8_t expectedResult = 0x17;
    mctp_eid_t eid = 0;

    const DescriptorMap descriptorMap2{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xc9, 0x3e, 0xc5, 0x41, 0x15,
                                0x95, 0xf4, 0x48, 0x70, 0x1d, 0x49, 0xd6,
                                0x75}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("OpenBMC", std::vector<uint8_t>{0x01, 0x02})}}}};

    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap2, componentInfoMap,
                                componentNameMap, true);

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_request_firmware_data_req)>
        reqFwDataReq{0x8A, 0x05, 0x17, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x02, 0x00, 0x00};

    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    updateManager.processPackage("./test_pkg");

    auto result =
        updateManager.handleRequest(eid, PLDM_VERIFY_COMPLETE, requestMsg,
                                    sizeof(pldm_request_firmware_data_req));

    EXPECT_EQ(result[2], expectedResult);
}

TEST_F(UpdateManagerTest, handleRequest_apply_complete)
{
    uint8_t expectedResult = 0x18;
    mctp_eid_t eid = 0;

    const DescriptorMap descriptorMap2{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xc9, 0x3e, 0xc5, 0x41, 0x15,
                                0x95, 0xf4, 0x48, 0x70, 0x1d, 0x49, 0xd6,
                                0x75}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("OpenBMC", std::vector<uint8_t>{0x01, 0x02})}}}};

    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap2, componentInfoMap,
                                componentNameMap, true);

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_request_firmware_data_req)>
        reqFwDataReq{0x8A, 0x05, 0x18, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x02, 0x00, 0x00};

    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    updateManager.processPackage("./test_pkg");

    auto result =
        updateManager.handleRequest(eid, PLDM_APPLY_COMPLETE, requestMsg,
                                    sizeof(pldm_request_firmware_data_req));

    EXPECT_EQ(result[2], expectedResult);
}

TEST_F(UpdateManagerTest, handleRequest_not_supported_command)
{
    uint8_t expectedResult = 0x15;
    mctp_eid_t eid = 0;

    const DescriptorMap descriptorMap2{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xc9, 0x3e, 0xc5, 0x41, 0x15,
                                0x95, 0xf4, 0x48, 0x70, 0x1d, 0x49, 0xd6,
                                0x75}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("OpenBMC", std::vector<uint8_t>{0x01, 0x02})}}}};

    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap2, componentInfoMap,
                                componentNameMap, true);

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_request_firmware_data_req)>
        reqFwDataReq{0x8A, 0x05, 0x15, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x02, 0x00, 0x00};

    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    updateManager.processPackage("./test_pkg");

    auto result = updateManager.handleRequest(
        eid, PLDM_QUERY_DEVICE_IDENTIFIERS, requestMsg,
        sizeof(pldm_request_firmware_data_req));

    EXPECT_EQ(result[2], expectedResult);
}

TEST_F(UpdateManagerTest, setActivationStatus)
{

    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap, componentInfoMap,
                                componentNameMap, true);

    const Server::Activation::Activations activationState =
        Server::Activation::Activations::Active;

    updateManager.processPackage("./test_pkg");

    EXPECT_NO_THROW({ updateManager.setActivationStatus(activationState); });
}

TEST_F(UpdateManagerTest, updateOtherDeviceComponents)
{
    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap, componentInfoMap,
                                componentNameMap, true);

    std::unordered_map<std::string, bool> otherDeviceMap = {
        {"device1", true}, {"device2", false}, {"device3", true}};

    updateManager.processPackage("./test_pkg");
    EXPECT_NO_THROW(
        { updateManager.updateOtherDeviceComponents(otherDeviceMap); });
}

TEST_F(UpdateManagerTest, resetActivationBlocksTransition)
{
    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap, componentInfoMap,
                                componentNameMap, true);

    EXPECT_NO_THROW({ updateManager.resetActivationBlocksTransition(); });
}

TEST_F(UpdateManagerTest, getComponentName)
{
    EID eid = 0;
    const std::string componentName{"Component1"};

    const std::string activeCompVersion1{"Comp1v2.0"};
    const std::string activeCompVersion2{"Comp2v3.0"};
    constexpr uint16_t compClassification1 = 10;
    constexpr uint16_t compIdentifier1 = 100;
    constexpr uint8_t compClassificationIndex1 = 20;
    constexpr uint16_t compClassification2 = 16;
    constexpr uint16_t compIdentifier2 = 301;
    constexpr uint8_t compClassificationIndex2 = 30;
    ComponentInfoMap componentInfoMap2{
        {eid,
         {{std::make_pair(compClassification1, compIdentifier1),
           std::make_tuple(compClassificationIndex1, activeCompVersion1)},
          {std::make_pair(compClassification2, compIdentifier2),
           std::make_tuple(compClassificationIndex2, activeCompVersion2)}}}};

    const DescriptorMap descriptorMap2{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xc9, 0x3e, 0xc5, 0x41, 0x15,
                                0x95, 0xf4, 0x48, 0x70, 0x1d, 0x49, 0xd6,
                                0x75}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("OpenBMC", std::vector<uint8_t>{0x01, 0x02})}}}};

    ComponentNameMap componentNameMap2{
        {eid, {{compIdentifier1, componentName}}}};

    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap2, componentInfoMap2,
                                componentNameMap2, true);

    FirmwareDeviceIDRecord fwDeviceIDRecord = {
        1,
        {0x00},
        "VersionString2",
        {{PLDM_FWUP_UUID,
          std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                               0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                               0x75}}},
        {}};

    size_t componentIndex = 0;
    updateManager.processPackage("./test_pkg");

    std::string componentNameResult =
        updateManager.getComponentName(eid, fwDeviceIDRecord, componentIndex);
    EXPECT_EQ(componentNameResult, componentName);
}

TEST_F(UpdateManagerTest, getComponentName_DoesNotFindComponent)
{
    EID eid = 0;
    const std::string componentName{"Component1"};

    const std::string activeCompVersion1{"Comp1v2.0"};
    const std::string activeCompVersion2{"Comp2v3.0"};
    constexpr uint16_t compClassification1 = 10;
    constexpr uint16_t compIdentifier1 = 200;
    constexpr uint8_t compClassificationIndex1 = 20;
    constexpr uint16_t compClassification2 = 16;
    constexpr uint16_t compIdentifier2 = 301;
    constexpr uint8_t compClassificationIndex2 = 30;
    ComponentInfoMap componentInfoMap2{
        {eid,
         {{std::make_pair(compClassification1, compIdentifier1),
           std::make_tuple(compClassificationIndex1, activeCompVersion1)},
          {std::make_pair(compClassification2, compIdentifier2),
           std::make_tuple(compClassificationIndex2, activeCompVersion2)}}}};

    const DescriptorMap descriptorMap2{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xc9, 0x3e, 0xc5, 0x41, 0x15,
                                0x95, 0xf4, 0x48, 0x70, 0x1d, 0x49, 0xd6,
                                0x75}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("OpenBMC", std::vector<uint8_t>{0x01, 0x02})}}}};

    ComponentNameMap componentNameMap2{
        {eid, {{compIdentifier1, componentName}}}};

    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap2, componentInfoMap2,
                                componentNameMap2, true);

    FirmwareDeviceIDRecord fwDeviceIDRecord = {
        1,
        {0x00},
        "VersionString2",
        {{PLDM_FWUP_UUID,
          std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                               0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                               0x75}}},
        {}};

    size_t componentIndex = 0;
    updateManager.processPackage("./test_pkg");

    std::string componentNameResult =
        updateManager.getComponentName(eid, fwDeviceIDRecord, componentIndex);
    EXPECT_EQ(componentNameResult, "");
}

TEST_F(UpdateManagerTest, getComponentName_ForEmptyComponentNameMap)
{
    EID eid = 0;
    const std::string componentName{"Component1"};

    const std::string activeCompVersion1{"Comp1v2.0"};
    const std::string activeCompVersion2{"Comp2v3.0"};
    constexpr uint16_t compClassification1 = 10;
    constexpr uint16_t compIdentifier1 = 200;
    constexpr uint8_t compClassificationIndex1 = 20;
    constexpr uint16_t compClassification2 = 16;
    constexpr uint16_t compIdentifier2 = 301;
    constexpr uint8_t compClassificationIndex2 = 30;
    ComponentInfoMap componentInfoMap2{
        {eid,
         {{std::make_pair(compClassification1, compIdentifier1),
           std::make_tuple(compClassificationIndex1, activeCompVersion1)},
          {std::make_pair(compClassification2, compIdentifier2),
           std::make_tuple(compClassificationIndex2, activeCompVersion2)}}}};

    const DescriptorMap descriptorMap2{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xc9, 0x3e, 0xc5, 0x41, 0x15,
                                0x95, 0xf4, 0x48, 0x70, 0x1d, 0x49, 0xd6,
                                0x75}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("OpenBMC", std::vector<uint8_t>{0x01, 0x02})}}}};

    UpdateManager updateManager(event, reqHandler, dbusImplRequester,
                                descriptorMap2, componentInfoMap2,
                                componentNameMap, true);

    FirmwareDeviceIDRecord fwDeviceIDRecord = {
        1,
        {0x00},
        "VersionString2",
        {{PLDM_FWUP_UUID,
          std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                               0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                               0x75}}},
        {}};

    size_t componentIndex = 0;
    updateManager.processPackage("./test_pkg");

    EXPECT_NO_THROW({
        updateManager.getComponentName(eid, fwDeviceIDRecord, componentIndex);
    });
}

TEST_F(UpdateManagerTest, processPackage_Package_v3_truncated)
{

    int expectedResult = -1;

    requester::Handler<requester::Request> reqHandler2(
        event, dbusImplRequester, sockManager, false, std::chrono::seconds(1),
        2, std::chrono::milliseconds(100));

    mctp_eid_t eid = 0x01;

    const DescriptorMap descriptorMap2{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xc9, 0x3e, 0xc5, 0x41, 0x15,
                                0x95, 0xf4, 0x48, 0x70, 0x1d, 0x49, 0xd6,
                                0x75}}}}};

    UpdateManager updateManager(event, reqHandler2, dbusImplRequester,
                                descriptorMap2, componentInfoMap,
                                componentNameMap, true);

    int result = updateManager.processPackage("./test_pkg_v3_signed_truncated");

    EXPECT_EQ(result, expectedResult);
}