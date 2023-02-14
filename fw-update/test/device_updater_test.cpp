#include "libpldm/firmware_update.h"

#include "common/utils.hpp"
#define private public
#include "fw-update/device_updater.hpp"
#include "fw-update/package_parser.hpp"
#include "fw-update/update_manager.hpp"
#include "mocked_firmware_update_function.hpp"
#include "pldmd/dbus_impl_requester.hpp"
#include "requester/handler.hpp"

#include <sdbusplus/bus.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>
#include <sdeventplus/test/sdevent.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::_;
using namespace pldm;
using namespace pldm::fw_update;

class DeviceUpdaterTest : public testing::Test
{
  protected:
    DeviceUpdaterTest() :
        package("./test_pkg", std::ios::binary | std::ios::in | std::ios::ate),
        event(sdeventplus::Event::get_default()),
        dbusImplRequester(pldm::utils::DBusHandler::getBus(),
                          "/xyz/openbmc_project/pldm"),
        reqHandler(event, dbusImplRequester, sockManager, false,
                   std::chrono::seconds(1), 2, std::chrono::milliseconds(100)),
        updateManager(event, reqHandler, dbusImplRequester, descriptorMap,
                      componentInfoMap, componentNameMap, true)
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
            {std::make_pair(10, 100), std::make_tuple(1, "comp1Version")}};
        compIdNameInfo = {{11, "ComponentName1"},
                          {55555, "ComponentName2"},
                          {12, "ComponentName3"},
                          {66666, "ComponentName4"}};
    }

    std::ifstream package;
    FirmwareDeviceIDRecord fwDeviceIDRecord;
    ComponentImageInfos compImageInfos;
    ComponentInfo compInfo;
    ComponentIdNameMap compIdNameInfo;
    sdeventplus::Event event;
    pldm::dbus_api::Requester dbusImplRequester;
    pldm::mctp_socket::Manager sockManager;
    requester::Handler<requester::Request> reqHandler;
    DescriptorMap descriptorMap;
    ComponentInfoMap componentInfoMap;
    ComponentNameMap componentNameMap;
    UpdateManager updateManager;
};

TEST_F(DeviceUpdaterTest, validatePackage)
{
    constexpr uintmax_t testPkgSize = 1163;
    uintmax_t packageSize = package.tellg();
    EXPECT_EQ(packageSize, testPkgSize);

    package.seekg(0);
    std::vector<uint8_t> packageHeader(sizeof(pldm_package_header_information));
    package.read(reinterpret_cast<char*>(packageHeader.data()),
                 sizeof(pldm_package_header_information));

    auto pkgHeaderInfo =
        reinterpret_cast<const pldm_package_header_information*>(
            packageHeader.data());
    auto pkgHeaderInfoSize = sizeof(pldm_package_header_information) +
                             pkgHeaderInfo->package_version_string_length;
    packageHeader.clear();
    packageHeader.resize(pkgHeaderInfoSize);
    package.seekg(0);
    package.read(reinterpret_cast<char*>(packageHeader.data()),
                 pkgHeaderInfoSize);

    auto parser = parsePkgHeader(packageHeader);
    EXPECT_NE(parser, nullptr);

    package.seekg(0);
    packageHeader.resize(parser->pkgHeaderSize);
    package.read(reinterpret_cast<char*>(packageHeader.data()),
                 parser->pkgHeaderSize);

    parser->parse(packageHeader, packageSize);
    const auto& fwDeviceIDRecords = parser->getFwDeviceIDRecords();
    const auto& testPkgCompImageInfos = parser->getComponentImageInfos();

    EXPECT_EQ(fwDeviceIDRecords.size(), 1);
    EXPECT_EQ(compImageInfos.size(), 1);
    EXPECT_EQ(fwDeviceIDRecords[0], fwDeviceIDRecord);
    EXPECT_EQ(testPkgCompImageInfos, compImageInfos);
}

TEST_F(DeviceUpdaterTest, requestUpdate)
{
    mctp_eid_t eid = 0;

    DeviceUpdater deviceUpdater(eid, package, fwDeviceIDRecord, compImageInfos,
                                compInfo, compIdNameInfo, 512, &updateManager,
                                false);

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_request_firmware_data_req)>
        reqFwDataReq{0x8A, 0x05, 0x15, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x02, 0x00, 0x00};

    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    deviceUpdater.processRequestUpdateResponse(
        eid, requestMsg, sizeof(struct pldm_request_update_resp));
}

TEST_F(DeviceUpdaterTest,
       private_method_sendPassCompTableRequest_PLDM_START_AND_END)
{
    mctp_eid_t eid = 0;
    size_t offset = 0;

    DeviceUpdater deviceUpdater(eid, package, fwDeviceIDRecord, compImageInfos,
                                compInfo, compIdNameInfo, 512, &updateManager,
                                false);

    EXPECT_NO_THROW({ deviceUpdater.sendPassCompTableRequest(offset); });
}

TEST_F(DeviceUpdaterTest, private_method_sendPassCompTableRequest_PLDM_START)
{
    mctp_eid_t eid = 0;
    size_t offset = 0;

    FirmwareDeviceIDRecord fwDeviceIDRecord2;
    fwDeviceIDRecord2 = {
        1,
        {0x00, 0x01, 0x02},
        "VersionString2",
        {{PLDM_FWUP_UUID,
          std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                               0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                               0x75}}},
        {}};

    DeviceUpdater deviceUpdater(eid, package, fwDeviceIDRecord2, compImageInfos,
                                compInfo, compIdNameInfo, 512, &updateManager,
                                false);

    EXPECT_NO_THROW({ deviceUpdater.sendPassCompTableRequest(offset); });
}

TEST_F(DeviceUpdaterTest, passCompTable)
{
    mctp_eid_t eid = 0;

    DeviceUpdater deviceUpdater(eid, package, fwDeviceIDRecord, compImageInfos,
                                compInfo, compIdNameInfo, 512, &updateManager,
                                false);

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_request_firmware_data_req)>
        reqFwDataReq{0x8A, 0x05, 0x15, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x02, 0x00, 0x00};

    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    EXPECT_NO_THROW({
        deviceUpdater.processPassCompTableResponse(
            eid, requestMsg, sizeof(struct pldm_pass_component_table_resp));
    });
}

TEST_F(DeviceUpdaterTest, sendActivateFirmwareRequest)
{
    mctp_eid_t eid = 0;

    DeviceUpdater deviceUpdater(eid, package, fwDeviceIDRecord, compImageInfos,
                                compInfo, compIdNameInfo, 512, &updateManager,
                                false);

    EXPECT_NO_THROW({ deviceUpdater.sendActivateFirmwareRequest(); });
}

TEST_F(DeviceUpdaterTest, activateFirmware)
{
    mctp_eid_t eid = 0;

    DeviceUpdater deviceUpdater(eid, package, fwDeviceIDRecord, compImageInfos,
                                compInfo, compIdNameInfo, 512, &updateManager,
                                false);

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_activate_firmware_resp)>
        activateFirmwareReq{0x8A, 0x05, 0x15, 0x00, 0x00, 0x00};

    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(activateFirmwareReq.data());

    EXPECT_NO_THROW({
        deviceUpdater.processActivateFirmwareResponse(
            eid, requestMsg, sizeof(struct pldm_activate_firmware_resp));
    });
}

TEST_F(DeviceUpdaterTest, sendCommandNotExpectedResponse)
{
    mctp_eid_t eid = 0;

    DeviceUpdater deviceUpdater(eid, package, fwDeviceIDRecord, compImageInfos,
                                compInfo, compIdNameInfo, 512, &updateManager,
                                false);

    const pldm_msg pldmmsg{};

    EXPECT_NO_THROW({ sendCommandNotExpectedResponse(&pldmmsg, 0); });
}

TEST(DeviceUpdaterSequence, command_RequestUpdate)
{
    struct DeviceUpdaterState deviceUpdaterState;

    DeviceUpdaterSequence sequence = deviceUpdaterState.nextState(
        DeviceUpdaterSequence::RequestUpdate, 0, 0);

    EXPECT_EQ(sequence, DeviceUpdaterSequence::PassComponentTable);
}

TEST(DeviceUpdaterSequence, command_PassComponentTable)
{
    struct DeviceUpdaterState deviceUpdaterState;

    DeviceUpdaterSequence sequence = deviceUpdaterState.nextState(
        DeviceUpdaterSequence::PassComponentTable, 0, 0);

    EXPECT_EQ(sequence, DeviceUpdaterSequence::ActivateFirmware);
}

TEST(DeviceUpdaterSequence,
     command_PassComponentTable_compIndex_less_then_numComps)
{
    struct DeviceUpdaterState deviceUpdaterState;

    DeviceUpdaterSequence sequence = deviceUpdaterState.nextState(
        DeviceUpdaterSequence::PassComponentTable, 0, 1);

    EXPECT_EQ(sequence, DeviceUpdaterSequence::PassComponentTable);
}

TEST(DeviceUpdaterSequence, command_Invalid)
{
    struct DeviceUpdaterState deviceUpdaterState;

    DeviceUpdaterSequence sequence =
        deviceUpdaterState.nextState(DeviceUpdaterSequence::Invalid, 0, 0);

    EXPECT_EQ(sequence, DeviceUpdaterSequence::Invalid);
}

TEST(DeviceUpdaterSequence, command_Invalid_fwDebug)
{
    struct DeviceUpdaterState deviceUpdaterState(true);

    DeviceUpdaterSequence sequence =
        deviceUpdaterState.nextState(DeviceUpdaterSequence::Invalid, 0, 0);

    EXPECT_EQ(sequence, DeviceUpdaterSequence::Invalid);
}

TEST(DeviceUpdaterSequence, command_ActivateFirmware)
{
    struct DeviceUpdaterState deviceUpdaterState(true);

    DeviceUpdaterSequence sequence = deviceUpdaterState.nextState(
        DeviceUpdaterSequence::ActivateFirmware, 0, 0);

    EXPECT_EQ(sequence, DeviceUpdaterSequence::Invalid);
}

TEST(DeviceUpdaterSequence, command_RetryRequest)
{
    struct DeviceUpdaterState deviceUpdaterState(true);

    DeviceUpdaterSequence sequence =
        deviceUpdaterState.nextState(DeviceUpdaterSequence::Valid, 0, 0);

    EXPECT_EQ(sequence, DeviceUpdaterSequence::RetryRequest);
}

TEST_F(DeviceUpdaterTest, SendRecvPldmMsgOverMctp)
{
    mctp_eid_t eid = 0;

    auto instanceId = updateManager.requester.getInstanceId(eid);
    Request request(sizeof(pldm_msg_hdr));
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    const pldm_msg* response = NULL;
    size_t respMsgLen = 0;

    auto rc = encode_cancel_update_req(instanceId, requestMsg,
                                       PLDM_CANCEL_UPDATE_REQ_BYTES);
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_NO_THROW({
        SendRecvPldmMsgOverMctp(updateManager.handler, eid, request, &response,
                                &respMsgLen);
    });
}

TEST_F(DeviceUpdaterTest, sendcancelUpdateRequest)
{
    mctp_eid_t eid = 0;
    DeviceUpdater deviceUpdater(eid, package, fwDeviceIDRecord, compImageInfos,
                                compInfo, compIdNameInfo, 512, &updateManager,
                                false);

    EXPECT_NO_THROW({ deviceUpdater.sendCancelUpdateRequest(); });
}

TEST_F(DeviceUpdaterTest, cancelUpdate_empty_response)
{
    mctp_eid_t eid = 0;
    DeviceUpdater deviceUpdater(eid, package, fwDeviceIDRecord, compImageInfos,
                                compInfo, compIdNameInfo, 512, &updateManager,
                                false);

    EXPECT_NO_THROW(
        { deviceUpdater.processCancelUpdateResponse(eid, nullptr, 0); });
}

TEST_F(DeviceUpdaterTest, cancelUpdate)
{
    mctp_eid_t eid = 0;
    DeviceUpdater deviceUpdater(eid, package, fwDeviceIDRecord, compImageInfos,
                                compInfo, compIdNameInfo, 512, &updateManager,
                                false);

    const pldm_msg pldmmsg{};

    EXPECT_NO_THROW(
        { deviceUpdater.processCancelUpdateResponse(eid, &pldmmsg, 0); });
}

TEST_F(DeviceUpdaterTest, sendRequestUpdate)
{
    mctp_eid_t eid = 0;
    DeviceUpdater deviceUpdater(eid, package, fwDeviceIDRecord, compImageInfos,
                                compInfo, compIdNameInfo, 512, &updateManager,
                                false);
    EXPECT_NO_THROW({ deviceUpdater.sendRequestUpdate(); });
}

TEST_F(DeviceUpdaterTest, updateComponentCompletion)
{
    mctp_eid_t eid = 0;
    size_t componentOffset = 0;
    DeviceUpdater deviceUpdater(eid, package, fwDeviceIDRecord, compImageInfos,
                                compInfo, compIdNameInfo, 512, &updateManager,
                                false);
    std::unique_ptr<ComponentUpdater> compUpdater =
        std::make_unique<ComponentUpdater>(
            eid, package, fwDeviceIDRecord, compImageInfos, compInfo,
            compIdNameInfo, 512, &updateManager, &deviceUpdater,
            componentOffset, false);
    deviceUpdater.componentUpdaterMap.emplace(
        componentOffset, std::make_pair(std::move(compUpdater), false));
    EXPECT_NO_THROW({ deviceUpdater.updateComponentCompletion(0, false); });
}