#include <stddef.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include <sdbusplus/exception.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/sdbus.hpp>

#include "fw-update/activation.hpp"
#include "fw-update/other_device_update_manager.hpp"

#include <sdbusplus/bus.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>

#include <gtest/gtest.h>

using namespace pldm;
using namespace pldm::fw_update;

class OtherDeviceUpdateManagerTest : public testing::Test
{
  protected:
    OtherDeviceUpdateManagerTest() :
        busMock(sdbusplus::get_mocked_new(&sdbusMock)),
        updatePolicy(busMock, "/xyz/openbmc_project/software"),
        event(sdeventplus::Event::get_default()),
        dbusImplRequester(busMock,
                          "/xyz/openbmc_project/pldm"),
        reqHandler(event, dbusImplRequester, sockManager, false,
                   std::chrono::seconds(1), 2, std::chrono::milliseconds(100)),
        updateManager(event, reqHandler, dbusImplRequester, descriptorMap,
                      componentInfoMap, componentNameMap, true)
    {
    }

    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    sdbusplus::bus::bus busMock;
    UpdatePolicy updatePolicy;
    sdeventplus::Event event;
    pldm::dbus_api::Requester dbusImplRequester;
    pldm::mctp_socket::Manager sockManager;
    requester::Handler<requester::Request> reqHandler;
    DescriptorMap descriptorMap;
    ComponentInfoMap componentInfoMap;
    ComponentNameMap componentNameMap;
    UpdateManager updateManager;
};

TEST_F(OtherDeviceUpdateManagerTest, activate)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(
        busMock,
        &updateManager,
        updatePolicy.targets());

    bool result = otherDeviceUpdateManager.activate();

    EXPECT_EQ(result, true);
}

TEST_F(OtherDeviceUpdateManagerTest, onActivationChangedMsg)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(
        busMock,
        &updateManager,
        updatePolicy.targets());

    std::variant<bool, uint8_t, int16_t, uint16_t, int32_t, uint32_t, int64_t,
                 uint64_t, double, std::string, std::vector<uint8_t>> value { "test" };

    pldm::dbus::PropertyMap properties;

    properties.insert(
        std::pair<std::string,
        std::variant<bool, uint8_t, int16_t, uint16_t, int32_t, uint32_t, int64_t,
                 uint64_t, double, std::string, std::vector<uint8_t>>>("/xyz/openbmc_project/pldm", value));

    EXPECT_NO_THROW({
        otherDeviceUpdateManager.onActivationChanged("/xyz/openbmc_project/pldm",
                                properties);
    });
}

TEST_F(OtherDeviceUpdateManagerTest, setUpdatePolicy)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(
        busMock,
        &updateManager,
        updatePolicy.targets());

    bool result = otherDeviceUpdateManager.setUpdatePolicy("/xyz/openbmc_project/pldm");

    EXPECT_EQ(result, false);
}

TEST_F(OtherDeviceUpdateManagerTest, getNumberOfProcessedImages)
{
    int expectedResult(0);

    OtherDeviceUpdateManager otherDeviceUpdateManager(
        busMock,
        &updateManager,
        updatePolicy.targets());

    int result = otherDeviceUpdateManager.getNumberOfProcessedImages();

    EXPECT_EQ(result, expectedResult);
}

TEST_F(OtherDeviceUpdateManagerTest, getValidTargets)
{
    int expectedResult(0);

    OtherDeviceUpdateManager otherDeviceUpdateManager(
        busMock,
        &updateManager,
        updatePolicy.targets());

    size_t result = otherDeviceUpdateManager.getValidTargets();

    EXPECT_EQ(result, expectedResult);
}

TEST_F(OtherDeviceUpdateManagerTest, extractOtherDevicePkgs)
{
    int expectedResult(0);

    OtherDeviceUpdateManager otherDeviceUpdateManager(
        busMock,
        &updateManager,
        updatePolicy.targets()
        );

    FirmwareDeviceIDRecords fwDeviceIDRecords{
        {1,
         {0},
         "VersionString2",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}}},
         {}},
    };

    ComponentImageInfos compImageInfos{
        {10, 100, 0xFFFFFFFF, 0, 0, 139, 27, "VersionString2"}};


    std::istringstream dummyStream("10 20 30 40");

    size_t result = otherDeviceUpdateManager.extractOtherDevicePkgs(
        fwDeviceIDRecords,
        compImageInfos,
        dummyStream,
        updatePolicy.forceUpdate()
        );

    EXPECT_EQ(result, expectedResult);
}
