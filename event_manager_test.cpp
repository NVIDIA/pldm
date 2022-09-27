#include "platform-mc/event_manager.hpp"

#include <gtest/gtest.h>

using namespace pldm::platform_mc;

TEST(EventManagerTest, processNumericSensorEvent)
{
    std::map<pldm::tid_t, std::shared_ptr<Terminus>> termini{};
    auto eventManager =
        EventManager(*(static_cast<sdeventplus::Event*>(nullptr)),
                     *(static_cast<TerminusManager*>(nullptr)), termini);

    auto messageId = eventManager.getSensorThresholdMessageId(
        PLDM_SENSOR_UPPERCRITICAL, PLDM_SENSOR_NORMAL);
    EXPECT_EQ(messageId, SensorThresholdCriticalHighGoingHigh);
}