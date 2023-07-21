#include <systemd/sd-event.h>
#include "common/utils.hpp"
#include "fw-update/package_parser.hpp"
#include "fw-update/update_manager.hpp"

#include <gtest/gtest.h>

#include <sdbusplus/bus.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>

using namespace pldm;
using namespace pldm::fw_update;

class UpdateManagerMustBeSignedTest : public testing::Test
{
  protected:
    UpdateManagerMustBeSignedTest() :
        busMock(sdbusplus::get_mocked_new(&sdbusMock)),
        event(sdeventplus::Event::get_default()),
        dbusImplRequester(busMock,
                          "/xyz/openbmc_project/pldm"),
        reqHandler(event, dbusImplRequester, sockManager, false,
                   std::chrono::seconds(1), 2, std::chrono::milliseconds(100))
    {
    }

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

TEST_F(UpdateManagerMustBeSignedTest, processPackage_pkg_v3_signed_enabled_must_be_signed)
{
    int expectedResult = 0;

    requester::Handler<requester::Request> reqHandler2(event, dbusImplRequester, sockManager, false,
                   std::chrono::seconds(1), 2, std::chrono::milliseconds(100));

    mctp_eid_t eid = 0x01;

    const DescriptorMap descriptorMap2{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
                std::vector<uint8_t>{0x16, 0x20, 0x23, 0xc9, 0x3e, 0xc5, 0x41, 0x15,
                                    0x95, 0xf4, 0x48, 0x70, 0x1d, 0x49, 0xd6, 0x75}}}}};

    UpdateManager updateManager(event, reqHandler2, dbusImplRequester, descriptorMap2,
                      componentInfoMap, componentNameMap, true);

    int result = updateManager.processPackage("./test_pkg_v3_signed");

    EXPECT_EQ(result, expectedResult);
}

TEST_F(UpdateManagerMustBeSignedTest, processPackage_pkg_v3_not_signed_enabled_must_be_signed)
{
    int expectedResult = -1;

    requester::Handler<requester::Request> reqHandler2(event, dbusImplRequester, sockManager, false,
                   std::chrono::seconds(1), 2, std::chrono::milliseconds(100));

    mctp_eid_t eid = 0x01;

    const DescriptorMap descriptorMap2{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
                std::vector<uint8_t>{0x16, 0x20, 0x23, 0xc9, 0x3e, 0xc5, 0x41, 0x15,
                                    0x95, 0xf4, 0x48, 0x70, 0x1d, 0x49, 0xd6, 0x75}}}}};

    UpdateManager updateManager(event, reqHandler2, dbusImplRequester, descriptorMap2,
                      componentInfoMap, componentNameMap, true);

    int result = updateManager.processPackage("./test_pkg");

    EXPECT_EQ(result, expectedResult);
}