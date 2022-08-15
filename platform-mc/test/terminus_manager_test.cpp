#include "libpldm/base.h"

#include "platform-mc/terminus_manager.hpp"
#include "pldmd/dbus_impl_requester.hpp"
#include "requester/handler.hpp"
#include "requester/mctp_endpoint_discovery.hpp"
#include "requester/request.hpp"

#include <sdbusplus/timer.hpp>
#include <sdeventplus/event.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace std::chrono;

using ::testing::AtLeast;
using ::testing::Between;
using ::testing::Exactly;
using ::testing::NiceMock;
using ::testing::Return;

class TerminusManagerTest : public testing::Test
{
  protected:
    TerminusManagerTest() :
        bus(pldm::utils::DBusHandler::getBus()),
        event(sdeventplus::Event::get_default()),
        dbusImplRequester(bus, "/xyz/openbmc_project/pldm"),
        reqHandler(event, dbusImplRequester, sockManager, false, seconds(1), 2,
                   milliseconds(100)),
        terminusManager(event, reqHandler, dbusImplRequester, termini, 0x8,
                        nullptr)
    {}

    int fd = -1;
    sdbusplus::bus::bus& bus;
    sdeventplus::Event event;
    pldm::dbus_api::Requester dbusImplRequester;
    pldm::mctp_socket::Manager sockManager;
    pldm::requester::Handler<pldm::requester::Request> reqHandler;
    pldm::platform_mc::TerminusManager terminusManager;
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
};

TEST_F(TerminusManagerTest, mapTidTest)
{
    pldm::MctpInfo mctpInfo1(1, "", "", 0);

    auto tid1 = terminusManager.toTid(mctpInfo1);
    EXPECT_EQ(tid1, std::nullopt);

    tid1 = terminusManager.mapTid(mctpInfo1);
    EXPECT_NE(tid1, std::nullopt);

    auto mctpInfo2 = terminusManager.toMctpInfo(tid1.value());
    EXPECT_EQ(mctpInfo1, mctpInfo2.value());

    terminusManager.unmapTid(tid1.value());

    tid1 = terminusManager.toTid(mctpInfo1);
    EXPECT_EQ(tid1, std::nullopt);
}

TEST_F(TerminusManagerTest, negativeMapTidTest)
{
    // map null EID(0) to TID
    pldm::MctpInfo m0(0, "", "", 0);
    auto mappedTid = terminusManager.mapTid(m0);
    EXPECT_EQ(mappedTid, std::nullopt);

    // map broadcast EID(0xff) to TID
    pldm::MctpInfo m1(0xff, "", "", 0);
    mappedTid = terminusManager.mapTid(m1);
    EXPECT_EQ(mappedTid, std::nullopt);

    // look up an unmapped MctpInfo to TID
    pldm::MctpInfo m2(1, "", "", 0);
    mappedTid = terminusManager.toTid(m2);
    EXPECT_EQ(mappedTid, std::nullopt);

    // look up reserved TID(0)
    auto mappedEid = terminusManager.toMctpInfo(0);
    EXPECT_EQ(mappedEid, std::nullopt);

    // look up reserved TID(0xff)
    mappedEid = terminusManager.toMctpInfo(0xff);
    EXPECT_EQ(mappedEid, std::nullopt);

    // look up an unmapped TID
    mappedEid = terminusManager.toMctpInfo(1);
    EXPECT_EQ(mappedEid, std::nullopt);

    // map two mctpInfo with same EID but different network Id
    pldm::MctpInfo m3(12, "", "", 1);
    pldm::MctpInfo m4(12, "", "", 2);
    auto mappedTid3 = terminusManager.mapTid(m3);
    auto mappedTid4 = terminusManager.mapTid(m4);
    EXPECT_NE(mappedTid3.value(), mappedTid4.value());

    // map same mctpInfo twice
    pldm::MctpInfo m5(12, "", "", 3);
    auto mappedTid5 = terminusManager.mapTid(m5);
    auto mappedTid6 = terminusManager.mapTid(m5);
    EXPECT_EQ(mappedTid5.value(), mappedTid6.value());
}