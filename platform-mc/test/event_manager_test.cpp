#include "libpldm/base.h"
#include "libpldm/entity.h"
#include "libpldm/platform.h"

#include "common/types.hpp"
#include "mock_event_manager.hpp"
#include "platform-mc/terminus_manager.hpp"

#include <gtest/gtest.h>

using ::testing::_;
using ::testing::Return;

using namespace pldm::platform_mc;

constexpr uint8_t mockTerminusManagerLocalEid = 0x08;

class EventManagerTest : public testing::Test
{
  protected:
    EventManagerTest() :
        bus(pldm::utils::DBusHandler::getBus()),
        event(sdeventplus::Event::get_default()),
        dbusImplRequester(bus, "/xyz/openbmc_project/pldm"),
        reqHandler(event, dbusImplRequester, sockManager, false, seconds(1), 2,
                   milliseconds(100)),
        terminusManager(event, reqHandler, dbusImplRequester, termini,
                        mockTerminusManagerLocalEid, nullptr),
        eventManager(*(static_cast<sdeventplus::Event*>(nullptr)),
                     *(static_cast<TerminusManager*>(nullptr)), termini)
    {}

    sdbusplus::bus::bus& bus;
    sdeventplus::Event event;
    pldm::dbus_api::Requester dbusImplRequester;
    pldm::mctp_socket::Manager sockManager;
    pldm::requester::Handler<pldm::requester::Request> reqHandler;
    pldm::platform_mc::TerminusManager terminusManager;
    MockEventManager eventManager;
    std::map<pldm::tid_t, std::shared_ptr<Terminus>> termini{};
};

TEST_F(EventManagerTest, processNumericSensorEventTest)
{
    pldm::tid_t tid = 1;
    termini[tid] = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                     // record handle
        0x1,                     // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR, // PDRType
        0x0,
        0x0, // recordChangeNumber
        0x0,
        56, // dataLength
        0,
        0, // PLDMTerminusHandle
        0x1,
        0x0, // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0, // entityType=Power Supply(120)
        1,
        0, // entityInstanceNumber
        0x1,
        0x0,                         // containerID=1
        PLDM_NO_INIT,                // sensorInit
        false,                       // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,  // baseUint(2)=degrees C
        0,                           // unitModifier = 0
        0,                           // rateUnit
        0,                           // baseOEMUnitHandle
        0,                           // auxUnit
        0,                           // auxUnitModifier
        0,                           // auxRateUnit
        0,                           // rel
        0,                           // auxOEMUnitHandle
        true,                        // isLinear
        PLDM_SENSOR_DATA_SIZE_UINT8, // sensorDataSize
        0,
        0,
        0xc0,
        0x3f, // resolution=1.5
        0,
        0,
        0x80,
        0x3f, // offset=1.0
        0,
        0, // accuracy
        0, // plusTolerance
        0, // minusTolerance
        2, // hysteresis
        0, // supportedThresholds
        0, // thresholdAndHysteresisVolatility
        0,
        0,
        0x80,
        0x3f, // stateTransistionInterval=1.0
        0,
        0,
        0x80,
        0x3f,                          // updateInverval=1.0
        255,                           // maxReadable
        0,                             // minReadable
        PLDM_RANGE_FIELD_FORMAT_UINT8, // rangeFieldFormat
        0x18,                          // rangeFieldsupport
        0,                             // nominalValue
        0,                             // normalMax
        0,                             // normalMin
        45,                            // warningHigh
        20,                            // warningLow
        60,                            // criticalHigh
        10,                            // criticalLow
        0,                             // fatalHigh
        0                              // fatalLow
    };

    // add dummy numeric sensor
    termini[tid]->pdrs.emplace_back(pdr1);
    auto rc = termini[1]->parsePDRs();
    uint8_t platformEventStatus = 0;
    EXPECT_EQ(true, rc);

    EXPECT_CALL(eventManager,
                createSensorThresholdLogEntry(
                    SensorThresholdWarningHighGoingHigh, _, 50, 45))
        .Times(1)
        .WillRepeatedly(Return());
    std::vector<uint8_t> eventData{0x1,
                                   0x0, // sensor id
                                   PLDM_NUMERIC_SENSOR_STATE,
                                   PLDM_SENSOR_UPPERWARNING,
                                   PLDM_SENSOR_NORMAL,
                                   PLDM_SENSOR_DATA_SIZE_UINT8,
                                   50};
    rc = eventManager.handlePlatformEvent(tid, PLDM_SENSOR_EVENT,
                                          eventData.data(), eventData.size(),
                                          platformEventStatus);
    EXPECT_EQ(PLDM_SUCCESS, rc);
    EXPECT_EQ(PLDM_EVENT_NO_LOGGING, platformEventStatus);
}

TEST_F(EventManagerTest, getSensorThresholdMessageIdTest)
{
    std::string messageId{};
    messageId = eventManager.getSensorThresholdMessageId(
        PLDM_SENSOR_NORMAL, PLDM_SENSOR_LOWERWARNING);
    EXPECT_EQ(messageId, SensorThresholdWarningLowGoingLow);

    messageId = eventManager.getSensorThresholdMessageId(
        PLDM_SENSOR_LOWERWARNING, PLDM_SENSOR_LOWERCRITICAL);
    EXPECT_EQ(messageId, SensorThresholdCriticalLowGoingLow);

    messageId = eventManager.getSensorThresholdMessageId(
        PLDM_SENSOR_LOWERCRITICAL, PLDM_SENSOR_LOWERWARNING);
    EXPECT_EQ(messageId, SensorThresholdCriticalLowGoingHigh);

    messageId = eventManager.getSensorThresholdMessageId(
        PLDM_SENSOR_LOWERWARNING, PLDM_SENSOR_NORMAL);
    EXPECT_EQ(messageId, SensorThresholdWarningLowGoingHigh);

    messageId = eventManager.getSensorThresholdMessageId(
        PLDM_SENSOR_NORMAL, PLDM_SENSOR_UPPERWARNING);
    EXPECT_EQ(messageId, SensorThresholdWarningHighGoingHigh);

    messageId = eventManager.getSensorThresholdMessageId(
        PLDM_SENSOR_UPPERWARNING, PLDM_SENSOR_UPPERCRITICAL);
    EXPECT_EQ(messageId, SensorThresholdCriticalHighGoingHigh);

    messageId = eventManager.getSensorThresholdMessageId(
        PLDM_SENSOR_UPPERCRITICAL, PLDM_SENSOR_UPPERWARNING);
    EXPECT_EQ(messageId, SensorThresholdCriticalHighGoingLow);

    messageId = eventManager.getSensorThresholdMessageId(
        PLDM_SENSOR_UPPERWARNING, PLDM_SENSOR_NORMAL);
    EXPECT_EQ(messageId, SensorThresholdWarningHighGoingLow);
}
