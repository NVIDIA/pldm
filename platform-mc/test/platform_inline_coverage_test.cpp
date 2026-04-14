#include "common/instance_id.hpp"
#include "mock_terminus_manager.hpp"
#include "platform-mc/numeric_sensor.hpp"
#include "platform-mc/pldmServiceReadyInterface.hpp"
#include "platform-mc/state_sensor.hpp"
#include "platform-mc/terminus.hpp"
#include "test/test_instance_id.hpp"

#include <gtest/gtest.h>
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "platform-mc/platform_manager.hpp"
#undef private
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <fcntl.h>
#include <libpldm/platform.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace std::chrono;
using namespace pldm::platform_mc;

namespace
{

class PlatformInlineCoverageTest : public testing::Test
{
  protected:
    PlatformInlineCoverageTest() :
        bus(pldm::utils::DBusHandler::getBus()),
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        terminusManager(event, reqHandler, instanceIdDb, termini, 0x8, nullptr),
        platformManager(terminusManager, termini)
    {}

    sdbusplus::bus::bus& bus;
    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler;
    MockTerminusManager terminusManager;
    PlatformManager platformManager;
    std::map<pldm::tid_t, std::shared_ptr<Terminus>> termini;
};

std::shared_ptr<pldm_numeric_sensor_value_pdr> makeNumericSensorValuePdr(
    uint16_t sensorId, uint8_t baseUnit, bool enableThresholds = true)
{
    auto pdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    pdr->sensor_id = sensorId;
    pdr->entity_type = PLDM_ENTITY_POWER_SUPPLY;
    pdr->entity_instance_num = 1;
    pdr->container_id = 1;
    pdr->base_unit = baseUnit;
    pdr->unit_modifier = 0;
    pdr->sensor_data_size = PLDM_SENSOR_DATA_SIZE_UINT8;
    pdr->resolution = 1.0f;
    pdr->offset = 0.0f;
    pdr->update_interval = 1.0f;
    pdr->max_readable.value_u8 = 100;
    pdr->min_readable.value_u8 = 1;
    pdr->hysteresis.value_u8 = 2;
    pdr->range_field_format = PLDM_RANGE_FIELD_FORMAT_UINT8;

    if (enableThresholds)
    {
        pdr->supported_thresholds.byte = 0x3F;
        pdr->range_field_support.byte = 0x78;
        pdr->warning_high.value_u8 = 80;
        pdr->warning_low.value_u8 = 20;
        pdr->critical_high.value_u8 = 90;
        pdr->critical_low.value_u8 = 10;
        pdr->fatal_high.value_u8 = 95;
        pdr->fatal_low.value_u8 = 5;
    }

    return pdr;
}

StateSetInfo makeBootRequestStateSetInfo()
{
    return std::make_tuple(
        EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
        std::vector<StateSetData>{
            {PLDM_STATESET_ID_BOOT_REQUEST,
             PossibleStates{PLDM_STATESET_BOOT_REQUEST_NORMAL,
                            PLDM_STATESET_BOOT_REQUEST_REQUESTED}}});
}

std::vector<uint8_t> makeSetEventReceiverResp(uint8_t instanceId,
                                              uint8_t completionCode)
{
    std::vector<uint8_t> response(
        sizeof(pldm_msg_hdr) + PLDM_SET_EVENT_RECEIVER_RESP_BYTES, 0);
    auto* msg = reinterpret_cast<pldm_msg*>(response.data());
    EXPECT_EQ(encode_set_event_receiver_resp(instanceId, completionCode, msg),
              PLDM_SUCCESS);
    return response;
}

std::vector<uint8_t> makeEventMessageSupportedResp(
    uint8_t instanceId, uint8_t completionCode,
    uint8_t synchronyConfigurationSupported,
    const std::vector<uint8_t>& eventClasses)
{
    std::vector<uint8_t> response{
        instanceId,
        PLDM_PLATFORM,
        PLDM_EVENT_MESSAGE_SUPPORTED,
        completionCode,
        0,
        synchronyConfigurationSupported,
        static_cast<uint8_t>(eventClasses.size())};
    response.insert(response.end(), eventClasses.begin(), eventClasses.end());
    return response;
}

std::vector<uint8_t> makeEventMessageBufferSizeResp(
    uint8_t instanceId, uint8_t completionCode, uint16_t size)
{
    return {instanceId,
            PLDM_PLATFORM,
            PLDM_EVENT_MESSAGE_BUFFER_SIZE,
            completionCode,
            static_cast<uint8_t>(size & 0xFF),
            static_cast<uint8_t>((size >> 8) & 0xFF)};
}

std::vector<uint8_t> makeGetPdrRepositoryInfoResp(
    uint8_t instanceId, uint8_t completionCode, uint8_t repositoryState,
    uint32_t recordCount, uint32_t largestRecordSize)
{
    uint8_t updateTime[PLDM_TIMESTAMP104_SIZE] = {0};
    uint8_t oemUpdateTime[PLDM_TIMESTAMP104_SIZE] = {0};
    std::vector<uint8_t> response(
        sizeof(pldm_msg_hdr) + PLDM_GET_PDR_REPOSITORY_INFO_RESP_BYTES, 0);
    auto* msg = reinterpret_cast<pldm_msg*>(response.data());
    EXPECT_EQ(encode_get_pdr_repository_info_resp(
                  instanceId, completionCode, repositoryState, updateTime,
                  oemUpdateTime, recordCount, 256, largestRecordSize,
                  PLDM_NO_TIMEOUT, msg),
              PLDM_SUCCESS);
    return response;
}

std::vector<uint8_t> makeGetPdrResp(
    uint8_t instanceId, uint8_t completionCode, uint32_t nextRecordHandle,
    uint32_t nextDataTransferHandle, uint8_t transferFlag,
    const std::vector<uint8_t>& recordData)
{
    std::vector<uint8_t> response(
        sizeof(pldm_msg_hdr) + PLDM_GET_PDR_MIN_RESP_BYTES + recordData.size() +
            ((transferFlag == PLDM_START_AND_END) ? 0 : 1),
        0);
    auto* msg = reinterpret_cast<pldm_msg*>(response.data());
    EXPECT_EQ(encode_get_pdr_resp(instanceId, completionCode, nextRecordHandle,
                                  nextDataTransferHandle, transferFlag,
                                  static_cast<uint16_t>(recordData.size()),
                                  recordData.data(), 0, msg),
              PLDM_SUCCESS);
    return response;
}

TEST_F(PlatformInlineCoverageTest, CommonUtilsInlineCoverageFromPlatformTu)
{
    EXPECT_EQ(pldm::utils::decimalToBcd<uint8_t>(0), uint8_t{0});
    EXPECT_EQ(pldm::utils::decimalToBcd<uint16_t>(45), uint16_t{0x45});
    EXPECT_EQ(pldm::utils::decimalToBcd<uint32_t>(123), uint32_t{0x123});

    const std::vector<uint8_t> data{0x34, 0x12, 0x78, 0x56, 0x34,
                                    0x12, 0xF0, 0xDE, 0xBC, 0x9A,
                                    0x78, 0x56, 0x34, 0x12, 0x5A};
    const auto* ptr = data.data();
    EXPECT_EQ(pldm::utils::readLEValue<uint16_t>(ptr), 0x1234u);
    EXPECT_EQ(pldm::utils::readLEValue<uint32_t>(ptr), 0x12345678u);
    EXPECT_EQ(pldm::utils::readLEValue<uint64_t>(ptr), 0x123456789ABCDEF0ull);
    EXPECT_EQ(pldm::utils::readLEValue<uint8_t>(ptr), 0x5Au);

    int pipeFds[2] = {-1, -1};
    ASSERT_EQ(pipe(pipeFds), 0);
    const int trackedFd = pipeFds[0];
    {
        pldm::utils::CustomFD fd(trackedFd);
        EXPECT_EQ(fd(), trackedFd);
    }
    EXPECT_EQ(fcntl(trackedFd, F_GETFD), -1);
    close(pipeFds[1]);

    {
        pldm::utils::CustomFD invalidFd(-1);
        EXPECT_EQ(invalidFd(), -1);
    }
}

TEST_F(PlatformInlineCoverageTest, NumericSensorInlineBranchMatrixInFreshTu)
{
    const pldm::tid_t tid = 0x41;
    std::string associationPath =
        "/xyz/openbmc_project/inventory/system/chassis/platform_inline";
    auto valuePdr =
        makeNumericSensorValuePdr(0x4101, PLDM_SENSOR_UNIT_WATTS, true);
    std::string valueName{"platform inline power"};
    auto valueInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    valueInfo->impactedComponent = "GPU0";
    NumericSensor valueSensor(tid, false, valuePdr, valueName, associationPath,
                              valueInfo);

    EXPECT_EQ(valueSensor.getBaseUnit(), PLDM_SENSOR_UNIT_WATTS);
    EXPECT_FALSE(valueSensor.getSensorName().empty());
    EXPECT_FALSE(valueSensor.getSensorNameSpace().empty());
    EXPECT_EQ(valueSensor.getSensorEventInfo(), valueInfo);

    valueSensor.updateReading(true, true, 55);
    EXPECT_DOUBLE_EQ(valueSensor.getReading(), 55.0);
    EXPECT_TRUE(std::isfinite(valueSensor.getThresholdUpperCritical()));
    EXPECT_TRUE(std::isfinite(valueSensor.getThresholdLowerCritical()));
    EXPECT_TRUE(std::isfinite(valueSensor.getThresholdUpperWarning()));
    EXPECT_TRUE(std::isfinite(valueSensor.getThresholdLowerWarning()));

    valueSensor.setInventoryPaths(
        {associationPath + "/chassis0", associationPath + "/chassis1"}, false);
    ASSERT_NE(valueSensor.associationDefinitionsIntf, nullptr);
    EXPECT_EQ(valueSensor.associationDefinitionsIntf->associations().size(),
              2u);

    ASSERT_NE(valueSensor.inventoryDecoratorAreaIntf, nullptr);
    valueSensor.setPhysicalContext(PhysicalContextType::CPU);
    valueSensor.inventoryDecoratorAreaIntf.reset();
    valueSensor.setPhysicalContext(PhysicalContextType::CPU);

    valueSensor.setRefreshed(false);
    EXPECT_FALSE(valueSensor.isRefreshed());
    valueSensor.setRefreshed(true);
    EXPECT_TRUE(valueSensor.isRefreshed());

    valueSensor.setLastUpdatedTimeStamp(100);
    valueSensor.updateReading(true, true, 55);
    valueSensor.removeValueIntf();
    EXPECT_FALSE(valueSensor.needsUpdate(200));
    EXPECT_DOUBLE_EQ(valueSensor.getReading(), 55.0);
    valueSensor.setInventoryPaths({associationPath + "/ignored"}, true);
    std::string renamedValueName{"platform inline power renamed"};
    valueSensor.updateSensorName(renamedValueName);
    valueSensor.updateTime = 150;
    EXPECT_FALSE(valueSensor.needsUpdate(200));
    valueSensor.updateTime = 20;
    valueSensor.isPriority = false;
    valueSensor.refreshLimitInUsec = 120;
    EXPECT_FALSE(valueSensor.needsUpdate(200));
    valueSensor.isPriority = true;
    EXPECT_TRUE(valueSensor.needsUpdate(200));
    valueSensor.isPriority = false;
    EXPECT_TRUE(valueSensor.needsUpdate(260));

    valueSensor.thresholdCriticalIntf.reset();
    EXPECT_TRUE(std::isnan(valueSensor.getThresholdUpperCritical()));
    EXPECT_TRUE(std::isnan(valueSensor.getThresholdLowerCritical()));
    valueSensor.updateReading(true, true, 21);
    valueSensor.valueIntf.reset();
    valueSensor.metricIntf.reset();
    EXPECT_DOUBLE_EQ(valueSensor.getReading(), 21.0);

    auto replacementInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    replacementInfo->impactedComponent = "GPU1";
    valueSensor.updateSensorEventInfo(replacementInfo);
    EXPECT_EQ(valueSensor.getSensorEventInfo(), replacementInfo);
    valueSensor.updateSensorEventInfo(nullptr);
    EXPECT_EQ(valueSensor.getSensorEventInfo(), nullptr);

    auto metricPdr =
        makeNumericSensorValuePdr(0x4102, PLDM_SENSOR_UNIT_SECONDS, false);
    std::string metricName{"platform inline time"};
    NumericSensor metricSensor(tid, false, metricPdr, metricName,
                               associationPath, nullptr);
    metricSensor.updateReading(true, true, 9);
    EXPECT_DOUBLE_EQ(metricSensor.getReading(), 9.0);
    metricSensor.setInventoryPaths({associationPath + "/metric0"}, false);
    ASSERT_NE(metricSensor.associationDefinitionsIntf, nullptr);
    const auto metricAssocs =
        metricSensor.associationDefinitionsIntf->associations();
    ASSERT_EQ(metricAssocs.size(), 1u);
    EXPECT_EQ(std::get<0>(metricAssocs.front()), "measuring");
    EXPECT_EQ(std::get<1>(metricAssocs.front()), "measured_by");

    auto unsupportedPdr = makeNumericSensorValuePdr(0x4103, 0xFF, false);
    std::string unsupportedName{"platform inline unsupported"};
    NumericSensor unsupportedSensor(tid, false, unsupportedPdr, unsupportedName,
                                    associationPath, nullptr);
    unsupportedSensor.updateReading(true, true, 33);
    unsupportedSensor.removeValueIntf();
    EXPECT_DOUBLE_EQ(unsupportedSensor.getReading(), 33.0);
}

TEST_F(PlatformInlineCoverageTest, StateSensorInlineBranchMatrixInFreshTu)
{
    const pldm::tid_t tid = 0x42;
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/platform_inline_state"};
    auto stateInfo = makeBootRequestStateSetInfo();
    auto eventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    eventInfo->impactedComponent = "CPU0";
    StateSensor sensor(tid, false, 0x4201, stateInfo, nullptr, associationPath,
                       eventInfo);

    EXPECT_EQ(sensor.getAssociationEntityId(), "");
    EXPECT_EQ(sensor.getSensorEventInfo(), eventInfo);
    EXPECT_TRUE(sensor.isDefaultInventoryAssociated());

    sensor.stateSets.push_back(nullptr);
    sensor.setInventoryPaths(
        {associationPath + "/board0", associationPath + "/module1"}, false);
    EXPECT_FALSE(sensor.isDefaultInventoryAssociated());
    EXPECT_EQ(sensor.getAssociationEntityId(), "module1");

    auto numericPdr =
        makeNumericSensorValuePdr(0x4202, PLDM_SENSOR_UNIT_WATTS, false);
    std::string numericName{"associated numeric"};
    std::vector<std::shared_ptr<NumericSensor>> numericSensors{
        std::make_shared<NumericSensor>(tid, false, numericPdr, numericName,
                                        associationPath, nullptr)};
    EXPECT_NO_THROW(sensor.associateNumericSensor(numericSensors));

    sensor.setRefreshed(false);
    EXPECT_FALSE(sensor.isRefreshed());
    sensor.setRefreshed(true);
    EXPECT_TRUE(sensor.isRefreshed());

    sensor.setLastUpdatedTimeStamp(100);
    sensor.refreshLimitInUsec = 25;
    EXPECT_FALSE(sensor.needsUpdate(120));
    EXPECT_TRUE(sensor.needsUpdate(126));

    auto replacementInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    replacementInfo->impactedComponent = "CPU1";
    sensor.updateSensorEventInfo(replacementInfo);
    EXPECT_EQ(sensor.getSensorEventInfo(), replacementInfo);
    sensor.updateSensorEventInfo(nullptr);
    EXPECT_EQ(sensor.getSensorEventInfo(), nullptr);

    sensor.setInventoryPaths({}, true);
    EXPECT_TRUE(sensor.isDefaultInventoryAssociated());
}

TEST_F(PlatformInlineCoverageTest, PlatformManagerPrivateBranchCoverage)
{
    const pldm::tid_t directTid = 0x44;
    const std::string directUuid("00000000-0000-0000-0000-000000000044");
    const pldm::MctpInfo directInfo(0x54, directUuid, "smbus", 1, std::nullopt,
                                    "mctp-over-smbus", std::nullopt);
    ASSERT_EQ(terminusManager.mapTid(directInfo, directTid), directTid);

    uint16_t terminusBufferSize = 0;
    auto bufferSuccess = makeEventMessageBufferSizeResp(0, PLDM_SUCCESS, 72);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(bufferSuccess));
    auto bufferRc = stdexec::sync_wait(platformManager.eventMessageBufferSize(
        directTid, 128, terminusBufferSize));
    ASSERT_TRUE(bufferRc.has_value());
    EXPECT_EQ(std::get<0>(*bufferRc), PLDM_SUCCESS);
    EXPECT_EQ(terminusBufferSize, 72);

    auto shortBufferResp = std::vector<uint8_t>{
        0x00, PLDM_PLATFORM, PLDM_EVENT_MESSAGE_BUFFER_SIZE, PLDM_SUCCESS,
        0x40};
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(shortBufferResp));
    auto bufferDecodeRc = stdexec::sync_wait(
        platformManager.eventMessageBufferSize(directTid, 128,
                                               terminusBufferSize));
    ASSERT_TRUE(bufferDecodeRc.has_value());
    EXPECT_NE(std::get<0>(*bufferDecodeRc), PLDM_SUCCESS);

    uint8_t synchronyConfiguration = 0;
    bitfield8_t synchronyConfigurationSupported{};
    uint8_t numberEventClassReturned = 0;
    std::vector<uint8_t> eventClasses;
    auto supportedResp = makeEventMessageSupportedResp(
        0, PLDM_SUCCESS, 1 << PLDM_EVENT_MESSAGE_GLOBAL_ENABLE_ASYNC, {0xFA});
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(supportedResp));
    auto supportedRc = stdexec::sync_wait(platformManager.eventMessageSupported(
        directTid, 1, synchronyConfiguration, synchronyConfigurationSupported,
        numberEventClassReturned, eventClasses));
    ASSERT_TRUE(supportedRc.has_value());
    EXPECT_EQ(std::get<0>(*supportedRc), PLDM_SUCCESS);
    EXPECT_EQ(numberEventClassReturned, 1);
    EXPECT_EQ(eventClasses, std::vector<uint8_t>({0xFA}));

    auto ccResp = makeEventMessageSupportedResp(0, PLDM_ERROR, 0, {});
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(ccResp));
    auto ccRc = stdexec::sync_wait(platformManager.eventMessageSupported(
        directTid, 1, synchronyConfiguration, synchronyConfigurationSupported,
        numberEventClassReturned, eventClasses));
    ASSERT_TRUE(ccRc.has_value());
    EXPECT_EQ(std::get<0>(*ccRc), PLDM_ERROR);

    uint8_t repositoryState = 0;
    uint32_t recordCount = 0;
    uint32_t repositorySize = 0;
    uint32_t largestRecordSize = 0;
    auto repoResp =
        makeGetPdrRepositoryInfoResp(0, PLDM_SUCCESS, PLDM_AVAILABLE, 3, 64);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(repoResp));
    auto repoRc = stdexec::sync_wait(platformManager.getPDRRepositoryInfo(
        directTid, repositoryState, recordCount, repositorySize,
        largestRecordSize));
    ASSERT_TRUE(repoRc.has_value());
    EXPECT_EQ(std::get<0>(*repoRc), PLDM_SUCCESS);
    EXPECT_EQ(repositoryState, PLDM_AVAILABLE);
    EXPECT_EQ(recordCount, 3u);
    EXPECT_EQ(largestRecordSize, 64u);

    uint32_t nextRecordHandle = 0;
    uint32_t nextDataTransferHandle = 0;
    uint8_t transferFlag = 0;
    uint16_t responseCount = 0;
    uint8_t transferCrc = 0;
    std::vector<uint8_t> recordData(64);
    const std::vector<uint8_t> pdrChunk{0x01, 0x02, 0x03, 0x04};
    auto pdrResp =
        makeGetPdrResp(0, PLDM_SUCCESS, 2, 0, PLDM_START_AND_END, pdrChunk);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(pdrResp));
    auto pdrRc = stdexec::sync_wait(platformManager.getPDR(
        directTid, 1, 0, PLDM_GET_FIRSTPART,
        static_cast<uint16_t>(recordData.size()), 0, nextRecordHandle,
        nextDataTransferHandle, transferFlag, responseCount, recordData,
        transferCrc));
    ASSERT_TRUE(pdrRc.has_value());
    EXPECT_EQ(std::get<0>(*pdrRc), PLDM_SUCCESS);
    EXPECT_EQ(nextRecordHandle, 2u);
    EXPECT_EQ(transferFlag, PLDM_START_AND_END);
    EXPECT_EQ(responseCount, pdrChunk.size());

    const pldm::tid_t tid = 0x45;
    std::string uuid("00000000-0000-0000-0000-000000000045");
    auto terminus = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    terminus->initalized = true;
    terminus->maxBufferSize = 160;
    termini[tid] = terminus;
    const pldm::MctpInfo terminusInfo(0x55, uuid, "smbus", 1, std::nullopt,
                                      "mctp-over-smbus", std::nullopt);
    ASSERT_EQ(terminusManager.mapTid(terminusInfo, tid), tid);

    auto initBufferResp = makeEventMessageBufferSizeResp(0, PLDM_SUCCESS, 80);
    auto initSupportedResp =
        makeEventMessageSupportedResp(0, PLDM_SUCCESS, 0, {PLDM_SENSOR_EVENT});
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(initBufferResp));
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(initSupportedResp));
    auto initRc = stdexec::sync_wait(platformManager.initTerminus());
    ASSERT_TRUE(initRc.has_value());
    EXPECT_EQ(std::get<0>(*initRc), PLDM_SUCCESS);
    EXPECT_EQ(terminus->maxBufferSize, 80);
    EXPECT_EQ(terminus->synchronyConfigurationSupported.byte, 0);

    EXPECT_TRUE(std::get<0>(*stdexec::sync_wait(
                    platformManager.initEventReceiver(0x99))) == PLDM_SUCCESS);

    terminus->synchronyConfigurationSupported.byte =
        1 << PLDM_EVENT_MESSAGE_GLOBAL_ENABLE_ASYNC;
    auto setRcResp = makeSetEventReceiverResp(0, PLDM_SUCCESS);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(setRcResp));
    auto receiverRc =
        stdexec::sync_wait(platformManager.initEventReceiver(tid));
    ASSERT_TRUE(receiverRc.has_value());
    EXPECT_EQ(std::get<0>(*receiverRc), PLDM_SUCCESS);
    EXPECT_EQ(terminus->resumptionStatus.eventReciever, PLDM_SUCCESS);

    auto errorResp = makeSetEventReceiverResp(0, PLDM_ERROR);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(errorResp));
    auto receiverErrRc =
        stdexec::sync_wait(platformManager.initEventReceiver(tid));
    ASSERT_TRUE(receiverErrRc.has_value());
    EXPECT_EQ(std::get<0>(*receiverErrRc), PLDM_ERROR);

    const pldm::tid_t mappedTid = 0x46;
    std::string mappedUuid("00000000-0000-0000-0000-000000000046");
    auto mappedTerminus = std::make_shared<Terminus>(
        mappedTid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, mappedUuid,
        terminusManager);
    mappedTerminus->synchronyConfigurationSupported.byte =
        1 << PLDM_EVENT_MESSAGE_GLOBAL_ENABLE_ASYNC;
    termini[mappedTid] = mappedTerminus;
    const pldm::MctpInfo mappedInfo(0x52, mappedUuid, "smbus", 1, std::nullopt,
                                    "mctp-over-smbus", std::nullopt);
    ASSERT_EQ(terminusManager.mapTid(mappedInfo, mappedTid), mappedTid);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(errorResp));
    auto mappedErrRc =
        stdexec::sync_wait(platformManager.initEventReceiver(mappedTid));
    ASSERT_TRUE(mappedErrRc.has_value());
    EXPECT_EQ(std::get<0>(*mappedErrRc), PLDM_ERROR);
}

TEST_F(PlatformInlineCoverageTest, ServiceReadyInterfaceCoverageInFreshTu)
{
    constexpr auto objectPath =
        "/xyz/openbmc_project/state/service_ready/pldm_platform_inline";
    EXPECT_THROW((void)PldmServiceReadyIntf::getInstance(), std::runtime_error);
    EXPECT_NO_THROW(PldmServiceReadyIntf::initialize(bus, objectPath));
    auto& intf = PldmServiceReadyIntf::getInstance();
    EXPECT_NO_THROW(intf.setStateStarting());
    EXPECT_NO_THROW(intf.setStateEnabled());
    EXPECT_THROW(PldmServiceReadyIntf::initialize(bus, objectPath),
                 std::logic_error);
}

} // namespace
