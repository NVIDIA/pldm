#include "config.h"

#include "mock_sensor_manager.hpp"

#include <sdeventplus/event.hpp>

#include <gtest/gtest.h>

using namespace std::chrono;

using ::testing::_;
using ::testing::Between;
using ::testing::Return;

class SensorManagerTest : public testing::Test
{
  protected:
    SensorManagerTest() :
        bus(pldm::utils::DBusHandler::getBus()),
        event(sdeventplus::Event::get_default()),
        dbusImplRequester(bus, "/xyz/openbmc_project/pldm"),
        reqHandler(event, dbusImplRequester, sockManager, false),
        terminusManager(event, reqHandler, dbusImplRequester, termini, 0x8,
                        nullptr),
        sensorManager(event, terminusManager, termini, nullptr)
    {}

    void runEventLoopForSeconds(uint64_t sec)
    {
        uint64_t t0 = 0;
        uint64_t t1 = 0;
        uint64_t usec = sec * 1000000;
        uint64_t elapsed = 0;
        sd_event_now(event.get(), CLOCK_MONOTONIC, &t0);
        do
        {
            sd_event_run(event.get(), usec - elapsed);
            sd_event_now(event.get(), CLOCK_MONOTONIC, &t1);
            elapsed = t1 - t0;
        } while (elapsed < usec);
    }

    sdbusplus::bus::bus& bus;
    sdeventplus::Event event;
    pldm::dbus_api::Requester dbusImplRequester;
    pldm::mctp_socket::Manager sockManager;
    pldm::requester::Handler<pldm::requester::Request> reqHandler;
    pldm::platform_mc::TerminusManager terminusManager;
    pldm::platform_mc::MockSensorManager sensorManager;
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
};

TEST_F(SensorManagerTest, sensorPollingTest)
{
    uint64_t seconds = 10;
    uint64_t expectedTimes = (seconds * 1000) / SENSOR_POLLING_TIME;

    pldm::tid_t tid = 1;
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    termini[tid] = std::make_shared<pldm::platform_mc::Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1, terminusManager);

    EXPECT_CALL(sensorManager, doSensorPolling(tid))
        .Times(Between(expectedTimes - 5, expectedTimes + 5))
        .WillRepeatedly(Return());
    sensorManager.startPolling();
    runEventLoopForSeconds(seconds);
}