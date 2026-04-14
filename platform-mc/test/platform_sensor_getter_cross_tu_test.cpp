#include "platform-mc/numeric_sensor.hpp"
#include "platform-mc/state_sensor.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace pldm::platform_mc;

namespace
{

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

TEST(PlatformSensorGetterCrossTuCoverage, NumericSensorInlineGettersAndTimers)
{
    const pldm::tid_t tid = 0x61;
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/platform_sensor_getter"};

    auto eventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    eventInfo->impactedComponent = "GPU2";
    auto pdr = makeNumericSensorValuePdr(0x6101, PLDM_SENSOR_UNIT_WATTS, true);
    std::string sensorName{"getter fresh power"};
    NumericSensor sensor(tid, false, pdr, sensorName, associationPath,
                         eventInfo);

    EXPECT_FALSE(sensor.getSensorName().empty());
    EXPECT_FALSE(sensor.getSensorNameSpace().empty());
    EXPECT_EQ(sensor.getSensorEventInfo(), eventInfo);

    sensor.updateReading(true, true, 44);
    EXPECT_DOUBLE_EQ(sensor.getReading(), 44.0);

    sensor.setRefreshed(false);
    EXPECT_FALSE(sensor.isRefreshed());
    sensor.setRefreshed(true);
    EXPECT_TRUE(sensor.isRefreshed());

    sensor.setLastUpdatedTimeStamp(100);
    sensor.updateTime = 80;
    sensor.refreshLimitInUsec = 200;
    sensor.isPriority = false;
    EXPECT_FALSE(sensor.needsUpdate(150));
    EXPECT_FALSE(sensor.needsUpdate(250));
    sensor.isPriority = true;
    EXPECT_TRUE(sensor.needsUpdate(250));
    sensor.isPriority = false;
    EXPECT_TRUE(sensor.needsUpdate(350));

    sensor.setInventoryPaths({associationPath + "/chassis0"}, false);
    ASSERT_NE(sensor.associationDefinitionsIntf, nullptr);
    EXPECT_EQ(sensor.associationDefinitionsIntf->associations().size(), 1u);

    sensor.associationDefinitionsIntf.reset();
    sensor.setInventoryPaths({associationPath + "/ignored"}, true);

    ASSERT_NE(sensor.inventoryDecoratorAreaIntf, nullptr);
    sensor.setPhysicalContext(PhysicalContextType::CPU);
    sensor.inventoryDecoratorAreaIntf.reset();
    sensor.setPhysicalContext(PhysicalContextType::CPU);

    sensor.thresholdCriticalIntf.reset();
    EXPECT_TRUE(std::isnan(sensor.getThresholdUpperCritical()));
    EXPECT_TRUE(std::isnan(sensor.getThresholdLowerCritical()));

    sensor.updateSensorName("getter fresh power renamed");
    EXPECT_NE(sensor.getSensorName().find("renamed"), std::string::npos);

    auto replacementInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    replacementInfo->impactedComponent = "GPU3";
    sensor.updateSensorEventInfo(replacementInfo);
    EXPECT_EQ(sensor.getSensorEventInfo(), replacementInfo);
    sensor.updateSensorEventInfo(nullptr);
    EXPECT_EQ(sensor.getSensorEventInfo(), nullptr);

    sensor.removeValueIntf();
    sensor.metricIntf.reset();
    EXPECT_DOUBLE_EQ(sensor.getReading(), 44.0);

    auto metricPdr =
        makeNumericSensorValuePdr(0x6102, PLDM_SENSOR_UNIT_SECONDS, false);
    std::string metricName{"getter fresh metric"};
    NumericSensor metricSensor(tid, false, metricPdr, metricName,
                               associationPath, nullptr);
    metricSensor.updateReading(true, true, 9);
    EXPECT_DOUBLE_EQ(metricSensor.getReading(), 9.0);
    metricSensor.setInventoryPaths({associationPath + "/metric0"}, false);
    ASSERT_NE(metricSensor.associationDefinitionsIntf, nullptr);
    const auto associations =
        metricSensor.associationDefinitionsIntf->associations();
    ASSERT_EQ(associations.size(), 1u);
    EXPECT_EQ(std::get<0>(associations.front()), "measuring");
    EXPECT_EQ(std::get<1>(associations.front()), "measured_by");
}

TEST(PlatformSensorGetterCrossTuCoverage,
     StateSensorInlineGettersAndAssociations)
{
    const pldm::tid_t tid = 0x62;
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/platform_state_getter"};
    auto eventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    eventInfo->impactedComponent = "CPU2";
    auto stateInfo = makeBootRequestStateSetInfo();
    StateSensor sensor(tid, false, 0x6201, stateInfo, nullptr, associationPath,
                       eventInfo);

    EXPECT_EQ(sensor.getAssociationEntityId(), "");
    EXPECT_TRUE(sensor.isDefaultInventoryAssociated());
    EXPECT_EQ(sensor.getSensorEventInfo(), eventInfo);

    sensor.setInventoryPaths({}, true);
    EXPECT_TRUE(sensor.isDefaultInventoryAssociated());

    sensor.stateSets.push_back(nullptr);
    sensor.setInventoryPaths(
        {associationPath + "/board0", associationPath + "/module1"}, false);
    EXPECT_FALSE(sensor.isDefaultInventoryAssociated());
    EXPECT_EQ(sensor.getAssociationEntityId(), "module1");

    auto numericPdr =
        makeNumericSensorValuePdr(0x6202, PLDM_SENSOR_UNIT_WATTS, false);
    std::string numericName{"getter associated numeric"};
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
    replacementInfo->impactedComponent = "CPU3";
    sensor.updateSensorEventInfo(replacementInfo);
    EXPECT_EQ(sensor.getSensorEventInfo(), replacementInfo);
    sensor.updateSensorEventInfo(nullptr);
    EXPECT_EQ(sensor.getSensorEventInfo(), nullptr);
}

} // namespace
