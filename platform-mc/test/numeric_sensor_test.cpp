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

#include "libpldm/entity.h"

#include "../../test/test_valgrind_utils.hpp"
#include "common/instance_id.hpp"
#include "platform-mc/numeric_sensor.hpp"
#include "platform-mc/terminus.hpp"
#include "platform-mc/terminus_manager.hpp"
#include "test/test_instance_id.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>

#include <gtest/gtest.h>

using namespace pldm::platform_mc;

namespace numeric_sensor_test_alloc
{

thread_local bool failAllocations = false;
thread_local std::size_t failAtAllocation = 0;
thread_local std::size_t allocationCount = 0;

bool shouldFailAllocation()
{
    return failAllocations && failAtAllocation != 0 &&
           ++allocationCount == failAtAllocation;
}

void* allocate(std::size_t size,
               std::size_t alignment = alignof(std::max_align_t))
{
    if (shouldFailAllocation())
    {
        throw std::bad_alloc();
    }

    if (size == 0)
    {
        size = 1;
    }

    void* ptr = nullptr;
    if (alignment <= alignof(std::max_align_t))
    {
        ptr = std::malloc(size);
    }
    else if (posix_memalign(&ptr, alignment, size) != 0)
    {
        ptr = nullptr;
    }

    if (ptr == nullptr)
    {
        throw std::bad_alloc();
    }

    return ptr;
}

struct ScopedAllocationFailure
{
    explicit ScopedAllocationFailure(std::size_t failIndex) :
        previousFailAllocations(failAllocations),
        previousFailAtAllocation(failAtAllocation),
        previousAllocationCount(allocationCount)
    {
        failAllocations = true;
        failAtAllocation = failIndex;
        allocationCount = 0;
    }

    ~ScopedAllocationFailure()
    {
        failAllocations = previousFailAllocations;
        failAtAllocation = previousFailAtAllocation;
        allocationCount = previousAllocationCount;
    }

  private:
    bool previousFailAllocations;
    std::size_t previousFailAtAllocation;
    std::size_t previousAllocationCount;
};

template <typename Operation>
bool exerciseAllBadAlloc(Operation&& operation, std::size_t maxFailAt = 256)
{
    if (pldm::test::runningOnValgrind())
    {
        return true;
    }

    bool sawBadAlloc = false;

    for (std::size_t failIndex = 1; failIndex <= maxFailAt; ++failIndex)
    {
        try
        {
            ScopedAllocationFailure failure(failIndex);
            operation();
        }
        catch (const std::bad_alloc&)
        {
            sawBadAlloc = true;
        }
        catch (...)
        {}
    }

    return sawBadAlloc;
}

} // namespace numeric_sensor_test_alloc

void* operator new(std::size_t size)
{
    return numeric_sensor_test_alloc::allocate(size);
}

void* operator new[](std::size_t size)
{
    return numeric_sensor_test_alloc::allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    return numeric_sensor_test_alloc::allocate(
        size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return numeric_sensor_test_alloc::allocate(
        size, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return numeric_sensor_test_alloc::allocate(size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return numeric_sensor_test_alloc::allocate(size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void operator delete(void* ptr) noexcept
{
    std::free(ptr);
}

void operator delete[](void* ptr) noexcept
{
    std::free(ptr);
}

void operator delete(void* ptr, std::align_val_t) noexcept
{
    std::free(ptr);
}

void operator delete[](void* ptr, std::align_val_t) noexcept
{
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept
{
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept
{
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept
{
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept
{
    std::free(ptr);
}

class NumericSensorTest : public testing::Test
{
  protected:
    NumericSensorTest() :
        bus(pldm::utils::DBusHandler::getBus()),
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        terminusManager(event, reqHandler, instanceIdDb, termini, 0x8, nullptr)
    {}

    sdbusplus::bus_t& bus;
    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler;
    pldm::platform_mc::TerminusManager terminusManager;
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
};

static std::shared_ptr<pldm_numeric_sensor_value_pdr> makeNumericSensorValuePdr(
    uint8_t sensorDataSize, uint8_t rangeFieldFormat,
    uint8_t baseUnit = PLDM_SENSOR_UNIT_DEGRESS_C, int8_t unitModifier = 0,
    bool enableThresholds = true)
{
    auto pdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    pdr->sensor_id = 0x21;
    pdr->entity_type = PLDM_ENTITY_POWER_SUPPLY;
    pdr->entity_instance_num = 1;
    pdr->container_id = 1;
    pdr->base_unit = baseUnit;
    pdr->unit_modifier = unitModifier;
    pdr->sensor_data_size = sensorDataSize;
    pdr->resolution = 1.25f;
    pdr->offset = 2.0f;
    pdr->update_interval = 1.0f;

    switch (sensorDataSize)
    {
        case PLDM_SENSOR_DATA_SIZE_UINT8:
            pdr->max_readable.value_u8 = 100;
            pdr->min_readable.value_u8 = 1;
            pdr->hysteresis.value_u8 = 3;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT8:
            pdr->max_readable.value_s8 = 100;
            pdr->min_readable.value_s8 = -100;
            pdr->hysteresis.value_s8 = 3;
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT16:
            pdr->max_readable.value_u16 = 1000;
            pdr->min_readable.value_u16 = 10;
            pdr->hysteresis.value_u16 = 3;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT16:
            pdr->max_readable.value_s16 = 1000;
            pdr->min_readable.value_s16 = -1000;
            pdr->hysteresis.value_s16 = 3;
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT32:
            pdr->max_readable.value_u32 = 100000;
            pdr->min_readable.value_u32 = 100;
            pdr->hysteresis.value_u32 = 3;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT32:
            pdr->max_readable.value_s32 = 100000;
            pdr->min_readable.value_s32 = -100000;
            pdr->hysteresis.value_s32 = 3;
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT64:
            pdr->max_readable.value_u64 = 1000000;
            pdr->min_readable.value_u64 = 1000;
            pdr->hysteresis.value_u64 = 3;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT64:
        default:
            pdr->max_readable.value_s64 = 1000000;
            pdr->min_readable.value_s64 = -1000000;
            pdr->hysteresis.value_s64 = 3;
            break;
    }

    pdr->range_field_format = rangeFieldFormat;
    if (enableThresholds)
    {
        pdr->supported_thresholds.byte = 0x3F;
        pdr->range_field_support.byte = 0x78;
    }
    else
    {
        pdr->supported_thresholds.byte = 0;
        pdr->range_field_support.byte = 0;
    }

    switch (rangeFieldFormat)
    {
        case PLDM_RANGE_FIELD_FORMAT_UINT8:
            pdr->warning_high.value_u8 = 80;
            pdr->warning_low.value_u8 = 20;
            pdr->critical_high.value_u8 = 90;
            pdr->critical_low.value_u8 = 10;
            pdr->fatal_high.value_u8 = 100;
            pdr->fatal_low.value_u8 = 5;
            break;
        case PLDM_RANGE_FIELD_FORMAT_SINT8:
            pdr->warning_high.value_s8 = 40;
            pdr->warning_low.value_s8 = -40;
            pdr->critical_high.value_s8 = 50;
            pdr->critical_low.value_s8 = -50;
            pdr->fatal_high.value_s8 = 60;
            pdr->fatal_low.value_s8 = -60;
            break;
        case PLDM_RANGE_FIELD_FORMAT_UINT16:
            pdr->warning_high.value_u16 = 800;
            pdr->warning_low.value_u16 = 200;
            pdr->critical_high.value_u16 = 900;
            pdr->critical_low.value_u16 = 100;
            pdr->fatal_high.value_u16 = 1000;
            pdr->fatal_low.value_u16 = 50;
            break;
        case PLDM_RANGE_FIELD_FORMAT_SINT16:
            pdr->warning_high.value_s16 = 400;
            pdr->warning_low.value_s16 = -400;
            pdr->critical_high.value_s16 = 500;
            pdr->critical_low.value_s16 = -500;
            pdr->fatal_high.value_s16 = 600;
            pdr->fatal_low.value_s16 = -600;
            break;
        case PLDM_RANGE_FIELD_FORMAT_UINT32:
            pdr->warning_high.value_u32 = 80000;
            pdr->warning_low.value_u32 = 20000;
            pdr->critical_high.value_u32 = 90000;
            pdr->critical_low.value_u32 = 10000;
            pdr->fatal_high.value_u32 = 100000;
            pdr->fatal_low.value_u32 = 5000;
            break;
        case PLDM_RANGE_FIELD_FORMAT_SINT32:
            pdr->warning_high.value_s32 = 40000;
            pdr->warning_low.value_s32 = -40000;
            pdr->critical_high.value_s32 = 50000;
            pdr->critical_low.value_s32 = -50000;
            pdr->fatal_high.value_s32 = 60000;
            pdr->fatal_low.value_s32 = -60000;
            break;
        case PLDM_RANGE_FIELD_FORMAT_REAL32:
            pdr->warning_high.value_f32 = 40.5f;
            pdr->warning_low.value_f32 = -40.5f;
            pdr->critical_high.value_f32 = 50.5f;
            pdr->critical_low.value_f32 = -50.5f;
            pdr->fatal_high.value_f32 = 60.5f;
            pdr->fatal_low.value_f32 = -60.5f;
            break;
        case PLDM_RANGE_FIELD_FORMAT_UINT64:
            pdr->warning_high.value_u64 = 800000;
            pdr->warning_low.value_u64 = 200000;
            pdr->critical_high.value_u64 = 900000;
            pdr->critical_low.value_u64 = 100000;
            pdr->fatal_high.value_u64 = 1000000;
            pdr->fatal_low.value_u64 = 50000;
            break;
        case PLDM_RANGE_FIELD_FORMAT_SINT64:
        default:
            pdr->warning_high.value_s64 = 400000;
            pdr->warning_low.value_s64 = -400000;
            pdr->critical_high.value_s64 = 500000;
            pdr->critical_low.value_s64 = -500000;
            pdr->fatal_high.value_s64 = 600000;
            pdr->fatal_low.value_s64 = -600000;
            break;
    }

    return pdr;
}

#ifdef OEM_NVIDIA
static std::shared_ptr<pldm_oem_energycount_numeric_sensor_value_pdr>
    makeOemEnergyCountSensorPdr(uint16_t sensorId, uint8_t sensorDataSize,
                                uint8_t baseUnit, int8_t unitModifier = 0,
                                float updateInterval = 1.0f)
{
    auto pdr =
        std::make_shared<pldm_oem_energycount_numeric_sensor_value_pdr>();
    pdr->terminus_handle = 1;
    pdr->nvidia_oem_pdr_type = 3;
    pdr->sensor_id = sensorId;
    pdr->entity_type = PLDM_ENTITY_SYS_BOARD;
    pdr->entity_instance_num = 1;
    pdr->container_id = 1;
    pdr->sensor_auxiliary_names_pdr = false;
    pdr->base_unit = baseUnit;
    pdr->unit_modifier = unitModifier;
    pdr->sensor_data_size = sensorDataSize;
    pdr->update_interval = updateInterval;

    switch (sensorDataSize)
    {
        case PLDM_SENSOR_DATA_SIZE_UINT8:
            pdr->max_readable.value_u8 = 50;
            pdr->min_readable.value_u8 = 1;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT8:
            pdr->max_readable.value_s8 = 50;
            pdr->min_readable.value_s8 = -1;
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT16:
            pdr->max_readable.value_u16 = 500;
            pdr->min_readable.value_u16 = 10;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT16:
            pdr->max_readable.value_s16 = 500;
            pdr->min_readable.value_s16 = -10;
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT32:
            pdr->max_readable.value_u32 = 5000;
            pdr->min_readable.value_u32 = 100;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT32:
            pdr->max_readable.value_s32 = 5000;
            pdr->min_readable.value_s32 = -100;
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT64:
            pdr->max_readable.value_u64 = 50000;
            pdr->min_readable.value_u64 = 1000;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT64:
            pdr->max_readable.value_s64 = 50000;
            pdr->min_readable.value_s64 = -1000;
            break;
        default:
            break;
    }

    return pdr;
}
#endif

TEST_F(NumericSensorTest, conversionFormula)
{
    std::vector<uint8_t> pdr1{
        0x1,
        0x0,
        0x0,
        0x0,                     // record handle
        0x1,                     // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR, // PDRType
        0x0,
        0x0,                     // recordChangeNumber
        PLDM_PDR_NUMERIC_SENSOR_PDR_FIXED_LENGTH +
            PLDM_PDR_NUMERIC_SENSOR_PDR_VARIED_SENSOR_DATA_SIZE_MIN_LENGTH +
            PLDM_PDR_NUMERIC_SENSOR_PDR_VARIED_RANGE_FIELD_MIN_LENGTH,
        0,                             // dataLength
        0,
        0,                             // PLDMTerminusHandle
        0x1,
        0x0,                           // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                             // entityType=Power Supply(120)
        1,
        0,                             // entityInstanceNumber
        0x1,
        0x0,                           // containerID=1
        PLDM_NO_INIT,                  // sensorInit
        false,                         // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,    // baseUint(2)=degrees C
        1,                             // unitModifier = 1
        0,                             // rateUnit
        0,                             // baseOEMUnitHandle
        0,                             // auxUnit
        0,                             // auxUnitModifier
        0,                             // auxRateUnit
        0,                             // rel
        0,                             // auxOEMUnitHandle
        true,                          // isLinear
        PLDM_RANGE_FIELD_FORMAT_SINT8, // sensorDataSize
        0,
        0,
        0xc0,
        0x3f, // resolution=1.5
        0,
        0,
        0x80,
        0x3f, // offset=1.0
        0,
        0,    // accuracy
        0,    // plusTolerance
        0,    // minusTolerance
        2,    // hysteresis
        0,    // supportedThresholds
        0,    // thresholdAndHysteresisVolatility
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
        0,                             // rangeFieldsupport
        0,                             // nominalValue
        0,                             // normalMax
        0,                             // normalMin
        0,                             // warningHigh
        0,                             // warningLow
        0,                             // criticalHigh
        0,                             // criticalLow
        0,                             // fatalHigh
        0                              // fatalLow
    };

    auto numericSensorPdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    std::printf("pdr size=%ld\n", pdr1.size());
    auto rc = decode_numeric_sensor_pdr_data(pdr1.data(), pdr1.size(),
                                             numericSensorPdr.get());
    EXPECT_EQ(rc, PLDM_SUCCESS);

    std::string sensorName{"test1"};
    std::string inventoryPath{
        "/xyz/openbmc_project/inventroy/Item/Board/PLDM_device_1"};
    NumericSensor sensor(0x01, true, numericSensorPdr, sensorName,
                         inventoryPath, nullptr);
    double reading = 40.0;
    double convertedValue = 0;
    convertedValue = sensor.conversionFormula(reading);
    convertedValue = sensor.unitModifier(convertedValue);

    // (40*1.5 + 1.0 ) * 10^1 = 610
    EXPECT_EQ(610, convertedValue);
}

TEST_F(NumericSensorTest, checkThreshold)
{
    std::vector<uint8_t> pdr1{
        0x1,
        0x0,
        0x0,
        0x0,                     // record handle
        0x1,                     // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR, // PDRType
        0x0,
        0x0,                     // recordChangeNumber
        PLDM_PDR_NUMERIC_SENSOR_PDR_FIXED_LENGTH +
            PLDM_PDR_NUMERIC_SENSOR_PDR_VARIED_SENSOR_DATA_SIZE_MIN_LENGTH +
            PLDM_PDR_NUMERIC_SENSOR_PDR_VARIED_RANGE_FIELD_MIN_LENGTH,
        0,                             // dataLength
        0,
        0,                             // PLDMTerminusHandle
        0x1,
        0x0,                           // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                             // entityType=Power Supply(120)
        1,
        0,                             // entityInstanceNumber
        0x1,
        0x0,                           // containerID=1
        PLDM_NO_INIT,                  // sensorInit
        false,                         // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,    // baseUint(2)=degrees C
        1,                             // unitModifier = 1
        0,                             // rateUnit
        0,                             // baseOEMUnitHandle
        0,                             // auxUnit
        0,                             // auxUnitModifier
        0,                             // auxRateUnit
        0,                             // rel
        0,                             // auxOEMUnitHandle
        true,                          // isLinear
        PLDM_RANGE_FIELD_FORMAT_SINT8, // sensorDataSize
        0,
        0,
        0xc0,
        0x3f, // resolution=1.5
        0,
        0,
        0x80,
        0x3f, // offset=1.0
        0,
        0,    // accuracy
        0,    // plusTolerance
        0,    // minusTolerance
        2,    // hysteresis
        0,    // supportedThresholds
        0,    // thresholdAndHysteresisVolatility
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
        0,                             // rangeFieldsupport
        0,                             // nominalValue
        0,                             // normalMax
        0,                             // normalMin
        0,                             // warningHigh
        0,                             // warningLow
        0,                             // criticalHigh
        0,                             // criticalLow
        0,                             // fatalHigh
        0                              // fatalLow
    };

    auto numericSensorPdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    auto rc = decode_numeric_sensor_pdr_data(pdr1.data(), pdr1.size(),
                                             numericSensorPdr.get());
    EXPECT_EQ(rc, PLDM_SUCCESS);
    std::string sensorName{"test1"};
    std::string inventoryPath{
        "/xyz/openbmc_project/inventroy/Item/Board/PLDM_device_1"};
    pldm::platform_mc::NumericSensor sensor(0x01, true, numericSensorPdr,
                                            sensorName, inventoryPath, nullptr);

    bool highAlarm = false;
    bool lowAlarm = false;
    double highThreshold = 40;
    double lowThreshold = 30;
    double hysteresis = 2;

    // reading     35->40->45->38->35->30->25->32->35
    // highAlarm    F->T ->T ->T ->F ->F ->F -> F-> F
    // lowAlarm     F->F ->F ->F ->F ->T ->T -> T ->F
    double reading = 35;
    highAlarm = sensor.checkThreshold(highAlarm, true, reading, highThreshold,
                                      hysteresis);
    EXPECT_EQ(false, highAlarm);
    lowAlarm = sensor.checkThreshold(lowAlarm, false, reading, lowThreshold,
                                     hysteresis);
    EXPECT_EQ(false, lowAlarm);

    reading = 40;
    highAlarm = sensor.checkThreshold(highAlarm, true, reading, highThreshold,
                                      hysteresis);
    EXPECT_EQ(true, highAlarm);
    lowAlarm = sensor.checkThreshold(lowAlarm, false, reading, lowThreshold,
                                     hysteresis);
    EXPECT_EQ(false, lowAlarm);

    reading = 45;
    highAlarm = sensor.checkThreshold(highAlarm, true, reading, highThreshold,
                                      hysteresis);
    EXPECT_EQ(true, highAlarm);
    lowAlarm = sensor.checkThreshold(lowAlarm, false, reading, lowThreshold,
                                     hysteresis);
    EXPECT_EQ(false, lowAlarm);

    reading = 38;
    highAlarm = sensor.checkThreshold(highAlarm, true, reading, highThreshold,
                                      hysteresis);
    EXPECT_EQ(true, highAlarm);
    lowAlarm = sensor.checkThreshold(lowAlarm, false, reading, lowThreshold,
                                     hysteresis);
    EXPECT_EQ(false, lowAlarm);

    reading = 35;
    highAlarm = sensor.checkThreshold(highAlarm, true, reading, highThreshold,
                                      hysteresis);
    EXPECT_EQ(false, highAlarm);
    lowAlarm = sensor.checkThreshold(lowAlarm, false, reading, lowThreshold,
                                     hysteresis);
    EXPECT_EQ(false, lowAlarm);

    reading = 30;
    highAlarm = sensor.checkThreshold(highAlarm, true, reading, highThreshold,
                                      hysteresis);
    EXPECT_EQ(false, highAlarm);
    lowAlarm = sensor.checkThreshold(lowAlarm, false, reading, lowThreshold,
                                     hysteresis);
    EXPECT_EQ(true, lowAlarm);

    reading = 25;
    highAlarm = sensor.checkThreshold(highAlarm, true, reading, highThreshold,
                                      hysteresis);
    EXPECT_EQ(false, highAlarm);
    lowAlarm = sensor.checkThreshold(lowAlarm, false, reading, lowThreshold,
                                     hysteresis);
    EXPECT_EQ(true, lowAlarm);

    reading = 32;
    highAlarm = sensor.checkThreshold(highAlarm, true, reading, highThreshold,
                                      hysteresis);
    EXPECT_EQ(false, highAlarm);
    lowAlarm = sensor.checkThreshold(lowAlarm, false, reading, lowThreshold,
                                     hysteresis);
    EXPECT_EQ(true, lowAlarm);

    reading = 35;
    highAlarm = sensor.checkThreshold(highAlarm, true, reading, highThreshold,
                                      hysteresis);
    EXPECT_EQ(false, highAlarm);
    lowAlarm = sensor.checkThreshold(lowAlarm, false, reading, lowThreshold,
                                     hysteresis);
    EXPECT_EQ(false, lowAlarm);
}

TEST_F(NumericSensorTest, MemoryPageRetirementSensor)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    // Same PDR layout as checkThreshold/conversionFormula: LE record handle,
    // dataLength = MIN_LENGTH (69), body 61 bytes so decode accepts it.
    std::vector<uint8_t> pdr1{
        0x1,
        0x0,
        0x0,
        0x0,                     // record handle (little-endian)
        0x1,                     // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR, // PDRType
        0x0,
        0x0,                     // recordChangeNumber
        PLDM_PDR_NUMERIC_SENSOR_PDR_FIXED_LENGTH +
            PLDM_PDR_NUMERIC_SENSOR_PDR_VARIED_SENSOR_DATA_SIZE_MIN_LENGTH +
            PLDM_PDR_NUMERIC_SENSOR_PDR_VARIED_RANGE_FIELD_MIN_LENGTH,
        0,                           // dataLength
        0,
        0,                           // PLDMTerminusHandle
        0x1,
        0x0,                         // sensorID=1
        PLDM_ENTITY_MEMORY_CONTROLLER,
        0,                           // entityType=Memory Controller(143)
        1,
        0,                           // entityInstanceNumber
        0x1,
        0x0,                         // containerID=1
        PLDM_NO_INIT,                // sensorInit
        false,                       // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_COUNTS,     // baseUint(67)=counts
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
        0x80,
        0x3f, // resolution=1.0
        0,
        0,
        0,
        0, // offset=0
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
        0,                             // rangeFieldsupport
        0,                             // nominalValue
        0,                             // normalMax
        0,                             // normalMin
        0,                             // warningHigh
        0,                             // warningLow
        0,                             // criticalHigh
        0,                             // criticalLow
        0,                             // fatalHigh
        0                              // fatalLow
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_TRUE(rc);
    EXPECT_EQ(1u, t1.numericSensorPdrs.size());
    EXPECT_EQ(1u, t1.numericSensors.size());

    auto numericSensor = t1.numericSensors[0];
    EXPECT_TRUE(numericSensor->oemIntfs.empty());

    // Should be the same as value in updateReading()
    numericSensor->updateReading(true, true, 10);
    EXPECT_EQ(10, numericSensor->getReading());

    // Count sensors now expose their metric value directly.
    numericSensor->updateReading(true, true,
                                 std::numeric_limits<double>::quiet_NaN());
    EXPECT_TRUE(std::isnan(numericSensor->getReading()));
}

TEST_F(NumericSensorTest, constructorCoverageAcrossDataSizesAndRanges)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis0"};

    const std::array<uint8_t, 8> sensorDataSizes{
        PLDM_SENSOR_DATA_SIZE_UINT8,  PLDM_SENSOR_DATA_SIZE_SINT8,
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_SENSOR_DATA_SIZE_SINT16,
        PLDM_SENSOR_DATA_SIZE_UINT32, PLDM_SENSOR_DATA_SIZE_SINT32,
        PLDM_SENSOR_DATA_SIZE_UINT64, PLDM_SENSOR_DATA_SIZE_SINT64};

    for (size_t i = 0; i < sensorDataSizes.size(); ++i)
    {
        auto pdr = makeNumericSensorValuePdr(sensorDataSizes[i],
                                             PLDM_RANGE_FIELD_FORMAT_UINT16);
        std::string sensorName = "sensor_data_size_" + std::to_string(i);
        NumericSensor sensor(0x01, false, pdr, sensorName, inventoryPath,
                             nullptr);

        sensor.updateReading(true, true, 25.0 + i);
        EXPECT_TRUE(std::isfinite(sensor.getReading()));
        sensor.handleErrGetSensorReading();
        EXPECT_TRUE(std::isnan(sensor.getReading()));
    }

    const std::array<uint8_t, 9> rangeFieldFormats{
        PLDM_RANGE_FIELD_FORMAT_UINT8,  PLDM_RANGE_FIELD_FORMAT_SINT8,
        PLDM_RANGE_FIELD_FORMAT_UINT16, PLDM_RANGE_FIELD_FORMAT_SINT16,
        PLDM_RANGE_FIELD_FORMAT_UINT32, PLDM_RANGE_FIELD_FORMAT_SINT32,
        PLDM_RANGE_FIELD_FORMAT_REAL32, PLDM_RANGE_FIELD_FORMAT_UINT64,
        PLDM_RANGE_FIELD_FORMAT_SINT64};

    for (size_t i = 0; i < rangeFieldFormats.size(); ++i)
    {
        auto pdr = makeNumericSensorValuePdr(
            PLDM_SENSOR_DATA_SIZE_UINT16, rangeFieldFormats[i],
            PLDM_SENSOR_UNIT_VOLTS);
        std::string sensorName = "sensor_range_format_" + std::to_string(i);
        NumericSensor sensor(0x02, false, pdr, sensorName, inventoryPath,
                             nullptr);

        sensor.updateReading(true, true, 200.0);
        sensor.updateReading(true, true, -200.0);
        sensor.updateReading(true, true, 0.0);

        EXPECT_TRUE(std::isfinite(sensor.getThresholdUpperWarning()));
        EXPECT_TRUE(std::isfinite(sensor.getThresholdLowerWarning()));
        EXPECT_TRUE(std::isfinite(sensor.getThresholdUpperCritical()));
        EXPECT_TRUE(std::isfinite(sensor.getThresholdLowerCritical()));
    }
}

TEST_F(NumericSensorTest, baseUnitNameUpdateAndPollingCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis0"};

    const std::array<uint8_t, 11> baseUnits{
        PLDM_SENSOR_UNIT_DEGRESS_C,
        PLDM_SENSOR_UNIT_VOLTS,
        PLDM_SENSOR_UNIT_AMPS,
        PLDM_SENSOR_UNIT_RPM,
        PLDM_SENSOR_UNIT_WATTS,
        PLDM_SENSOR_UNIT_JOULES,
        PLDM_SENSOR_UNIT_HERTZ,
        PLDM_SENSOR_UNIT_PERCENTAGE,
        PLDM_SENSOR_UNIT_COUNTS,
        PLDM_SENSOR_UNIT_SECONDS,
        0xFF};

    for (size_t i = 0; i < baseUnits.size(); ++i)
    {
        auto pdr = makeNumericSensorValuePdr(
            PLDM_SENSOR_DATA_SIZE_UINT8, PLDM_RANGE_FIELD_FORMAT_UINT8,
            baseUnits[i], -1, false);
        std::string sensorName = "sensor_base_unit_" + std::to_string(i);
        NumericSensor sensor(0x03, false, pdr, sensorName, inventoryPath,
                             nullptr);

        sensor.isPriority = true;
        sensor.setLastUpdatedTimeStamp(0);
        EXPECT_TRUE(sensor.needsUpdate(2 * 1000 * 1000));

        sensor.removeValueIntf();
        if (baseUnits[i] != 0xFF)
        {
            EXPECT_FALSE(sensor.needsUpdate(3 * 1000 * 1000));
        }

        std::vector<std::string> assoc{
            "/xyz/openbmc_project/inventory/system/chassis/chassis0",
            "/xyz/openbmc_project/inventory/system/chassis/chassis1"};
        sensor.setInventoryPaths(assoc, false);
        sensor.setPhysicalContext(PhysicalContextType::CPU);
        sensor.updateReading(true, true, 10.0);
    }

    auto pdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_WATTS, 0, true);
    std::string sensorName{"name.with symbols"};
    NumericSensor sensor(0x04, false, pdr, sensorName, inventoryPath, nullptr);

    auto oldPath = sensor.path;
    sensor.updateSensorName(sensorName);
    EXPECT_EQ(oldPath, sensor.path);

    sensor.updateSensorName("new sensor/name+#1");
    EXPECT_NE(oldPath, sensor.path);
    EXPECT_EQ(std::string::npos, sensor.path.find('#'));
    EXPECT_EQ(std::string::npos, sensor.path.find('+'));

    sensor.handleErrGetSensorReading();
    EXPECT_TRUE(std::isnan(sensor.getReading()));
}

TEST_F(NumericSensorTest, metricInterfaceThresholdSuppressionCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis0"};
    auto pdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_SECONDS, 0, true);
    std::string sensorName{"metric_threshold_sensor"};
    NumericSensor sensor(0x05, false, pdr, sensorName, inventoryPath, nullptr);

    ASSERT_EQ(nullptr, sensor.valueIntf);
    ASSERT_NE(nullptr, sensor.metricIntf);
    EXPECT_EQ(nullptr, sensor.thresholdWarningIntf);
    EXPECT_EQ(nullptr, sensor.thresholdCriticalIntf);
    EXPECT_EQ(nullptr, sensor.thresholdFatalIntf);
    EXPECT_EQ("/xyz/openbmc_project/metric/time/", sensor.getSensorNameSpace());

    sensor.updateReading(true, true, 15.0);
    EXPECT_DOUBLE_EQ(sensor.unitModifier(sensor.conversionFormula(15.0)),
                     sensor.getReading());
    sensor.updateReading(false, false, 15.0);
    EXPECT_TRUE(std::isnan(sensor.getReading()));
}

TEST_F(NumericSensorTest, thresholdAlarmTransitionCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis0"};
    auto pdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_WATTS, 0, true);
    pdr->resolution = 1.0f;
    pdr->offset = 0.0f;

    std::string sensorName{"threshold_alarm_sensor"};
    NumericSensor sensor(0x06, false, pdr, sensorName, inventoryPath, nullptr);
    ASSERT_NE(nullptr, sensor.thresholdWarningIntf);
    ASSERT_NE(nullptr, sensor.thresholdCriticalIntf);
    ASSERT_NE(nullptr, sensor.thresholdFatalIntf);

    sensor.updateReading(true, true, 850.0);
    EXPECT_TRUE(sensor.thresholdWarningIntf->warningAlarmHigh());
    sensor.updateReading(true, true, 950.0);
    EXPECT_TRUE(sensor.thresholdCriticalIntf->criticalAlarmHigh());
    sensor.updateReading(true, true, 1050.0);
    EXPECT_TRUE(sensor.thresholdFatalIntf->hardShutdownAlarmHigh());

    sensor.updateReading(true, true, 990.0);
    EXPECT_FALSE(sensor.thresholdFatalIntf->hardShutdownAlarmHigh());
    sensor.updateReading(true, true, 890.0);
    EXPECT_FALSE(sensor.thresholdCriticalIntf->criticalAlarmHigh());
    sensor.updateReading(true, true, 790.0);
    EXPECT_FALSE(sensor.thresholdWarningIntf->warningAlarmHigh());

    sensor.updateReading(true, true, 150.0);
    EXPECT_TRUE(sensor.thresholdWarningIntf->warningAlarmLow());
    sensor.updateReading(true, true, 80.0);
    EXPECT_TRUE(sensor.thresholdCriticalIntf->criticalAlarmLow());
    sensor.updateReading(true, true, 40.0);
    EXPECT_TRUE(sensor.thresholdFatalIntf->hardShutdownAlarmLow());

    sensor.updateReading(true, true, 60.0);
    EXPECT_FALSE(sensor.thresholdFatalIntf->hardShutdownAlarmLow());
    sensor.updateReading(true, true, 120.0);
    EXPECT_FALSE(sensor.thresholdCriticalIntf->criticalAlarmLow());
    sensor.updateReading(true, true, 220.0);
    EXPECT_FALSE(sensor.thresholdWarningIntf->warningAlarmLow());
}

TEST_F(NumericSensorTest, thresholdPresenceBitCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis0"};

    auto warningOnlyPdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_VOLTS, 0, true);
    warningOnlyPdr->supported_thresholds.byte = 0x3F;
    warningOnlyPdr->range_field_support.byte = 0;
    std::string warningOnlyName{"warning_only_sensor"};
    NumericSensor warningOnlySensor(0x07, false, warningOnlyPdr,
                                    warningOnlyName, inventoryPath, nullptr);
    EXPECT_NE(nullptr, warningOnlySensor.thresholdWarningIntf);
    EXPECT_EQ(nullptr, warningOnlySensor.thresholdCriticalIntf);
    EXPECT_EQ(nullptr, warningOnlySensor.thresholdFatalIntf);

    auto noThresholdPdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_VOLTS, 0, false);
    noThresholdPdr->range_field_support.byte = 0x78;
    std::string noThresholdName{"no_threshold_sensor"};
    NumericSensor noThresholdSensor(0x08, false, noThresholdPdr,
                                    noThresholdName, inventoryPath, nullptr);
    EXPECT_EQ(nullptr, noThresholdSensor.thresholdWarningIntf);
    EXPECT_EQ(nullptr, noThresholdSensor.thresholdCriticalIntf);
    EXPECT_EQ(nullptr, noThresholdSensor.thresholdFatalIntf);
}

TEST_F(NumericSensorTest, numericSensorGuardBranchCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis50"};
    auto pdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_WATTS, 0, true);
    pdr->resolution = 1.0f;
    pdr->offset = 0.0f;

    std::string sensorName{"guard_branch_sensor"};
    NumericSensor sensor(0x50, false, pdr, sensorName, inventoryPath, nullptr);

    sensor.associationDefinitionsIntf->associations({});
    sensor.updateReading(true, true, 44.0);

    sensor.valueIntf.reset();
    sensor.metricIntf.reset();
    sensor.updateReading(false, true, 55.0);
    EXPECT_DOUBLE_EQ(55.0, sensor.getReading());

    sensor.associationDefinitionsIntf.reset();
    sensor.setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis50/cpu0"},
        false);

    sensor.inventoryDecoratorAreaIntf.reset();
    sensor.setPhysicalContext(PhysicalContextType::CPU);

    sensor.removeValueIntf();
    EXPECT_FALSE(sensor.needsUpdate(10 * 1000 * 1000));

    sensor.thresholdCriticalIntf.reset();
    EXPECT_TRUE(std::isnan(sensor.getThresholdUpperCritical()));
    EXPECT_TRUE(std::isnan(sensor.getThresholdLowerCritical()));
}

TEST_F(NumericSensorTest, numericSensorUnsupportedRenameCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis51"};
    auto unsupportedPdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT8, PLDM_RANGE_FIELD_FORMAT_UINT8, 0xFF, 0,
        false);
    std::string sensorName{"unsupported_sensor"};
    NumericSensor sensor(0x51, false, unsupportedPdr, sensorName, inventoryPath,
                         nullptr);

    ASSERT_EQ(nullptr, sensor.valueIntf);
    ASSERT_EQ(nullptr, sensor.metricIntf);

    sensor.availabilityIntf.reset();
    sensor.operationalStatusIntf.reset();
    sensor.inventoryDecoratorAreaIntf.reset();

    sensor.updateSensorName("unsupported/sensor+#51");
    EXPECT_EQ(std::string::npos, sensor.path.find('#'));
    EXPECT_EQ(std::string::npos, sensor.path.find('+'));

    sensor.associationDefinitionsIntf.reset();
    sensor.setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis51/cpu0"},
        false);
    sensor.setPhysicalContext(PhysicalContextType::CPU);
}

TEST_F(NumericSensorTest, numericSensorMetricRenameGuardCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis52"};
    auto metricPdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_SECONDS, 0, false);
    std::string sensorName{"metric_sensor_guard"};
    NumericSensor sensor(0x52, false, metricPdr, sensorName, inventoryPath,
                         nullptr);

    ASSERT_EQ(nullptr, sensor.valueIntf);
    ASSERT_NE(nullptr, sensor.metricIntf);

    sensor.removeValueIntf();
    EXPECT_FALSE(sensor.needsUpdate(2 * 1000 * 1000));

    sensor.updateReading(true, true, 77.0);
    EXPECT_DOUBLE_EQ(sensor.unitModifier(sensor.conversionFormula(77.0)),
                     sensor.getReading());

    sensor.updateSensorName("metric_sensor_guard_renamed");
    EXPECT_NE(std::string::npos, sensor.path.find("metric_sensor_guard"));
}

TEST_F(NumericSensorTest, numericSensorTelemetryAssociationCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis53"};

    auto valuePdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_WATTS, 0, true);
    std::string valueName{"telemetry_value_sensor"};
    NumericSensor valueSensor(0x53, false, valuePdr, valueName, inventoryPath,
                              nullptr);

    valueSensor.setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis53/cpu0"},
        false);
    valueSensor.updateReading(true, true, 64.0);
    EXPECT_FALSE(std::isnan(valueSensor.getReading()));

    valueSensor.setInventoryPaths({}, false);
    valueSensor.updateReading(true, true, 65.0);
    EXPECT_FALSE(std::isnan(valueSensor.getReading()));

    auto metricPdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_SECONDS, 0, false);
    std::string metricName{"telemetry_metric_sensor"};
    NumericSensor metricSensor(0x54, false, metricPdr, metricName,
                               inventoryPath, nullptr);

    metricSensor.setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis53/cpu1"},
        false);
    metricSensor.updateReading(true, true, 11.0);
    EXPECT_FALSE(std::isnan(metricSensor.getReading()));

    metricSensor.setInventoryPaths({}, false);
    metricSensor.updateReading(true, true, 12.0);
    EXPECT_FALSE(std::isnan(metricSensor.getReading()));
}

TEST_F(NumericSensorTest, numericSensorEmptyEndpointCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis59"};
    auto pdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_WATTS, 0, true);
    std::string sensorName{"empty_endpoint_sensor"};
    NumericSensor sensor(0x59, false, pdr, sensorName, inventoryPath, nullptr);

    sensor.setInventoryPaths({""}, false);
    sensor.updateReading(true, true, 66.0);

    auto associations = sensor.associationDefinitionsIntf->associations();
    ASSERT_EQ(1u, associations.size());
    EXPECT_TRUE(std::get<2>(associations.front()).empty());
    EXPECT_FALSE(std::isnan(sensor.getReading()));
}

TEST_F(NumericSensorTest, numericSensorFunctionalUnavailableCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis5a"};
    auto pdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_WATTS, 0, true);
    std::string sensorName{"functional_unavailable_sensor"};
    NumericSensor sensor(0x5A, false, pdr, sensorName, inventoryPath, nullptr);

    sensor.updateReading(true, true, 71.0);
    sensor.updateReading(true, false, 72.0);
    EXPECT_TRUE(std::isnan(sensor.getReading()));
}

TEST_F(NumericSensorTest, numericSensorAvailableFalseFunctionalTrueCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis5b"};
    auto pdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_WATTS, 0, true);
    std::string sensorName{"available_false_sensor"};
    NumericSensor sensor(0x5B, false, pdr, sensorName, inventoryPath, nullptr);

    sensor.updateReading(true, true, 81.0);
    sensor.updateReading(false, true, 82.0);

    EXPECT_FALSE(sensor.availabilityIntf->available());
    EXPECT_TRUE(sensor.operationalStatusIntf->functional());
    ASSERT_NE(nullptr, sensor.valueIntf);
    EXPECT_TRUE(std::isnan(sensor.valueIntf->value()));
    EXPECT_TRUE(std::isnan(sensor.getReading()));
}

TEST_F(NumericSensorTest, numericSensorCriticalThresholdGetterNaNCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis5c"};
    auto pdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_WATTS, 0, false);
    std::string sensorName{"threshold_nan_sensor"};
    NumericSensor sensor(0x5C, false, pdr, sensorName, inventoryPath, nullptr);

    EXPECT_TRUE(std::isnan(sensor.getThresholdUpperCritical()));
    EXPECT_TRUE(std::isnan(sensor.getThresholdLowerCritical()));

    auto metricPdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_SECONDS, 0, false);
    std::string metricName{"metric_unavailable_sensor"};
    NumericSensor metricSensor(0x5D, false, metricPdr, metricName,
                               inventoryPath, nullptr);

    metricSensor.updateReading(true, true, 13.0);
    metricSensor.updateReading(false, true, 14.0);
    ASSERT_NE(nullptr, metricSensor.metricIntf);
    EXPECT_TRUE(std::isnan(metricSensor.metricIntf->value()));
    EXPECT_TRUE(std::isnan(metricSensor.getReading()));
}

TEST_F(NumericSensorTest, unsupportedFormatAndUpdateGuardCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis54"};

    auto unsupportedPdr =
        makeNumericSensorValuePdr(0xFF, 0xFF, PLDM_SENSOR_UNIT_WATTS);
    unsupportedPdr->resolution = std::numeric_limits<float>::quiet_NaN();
    unsupportedPdr->offset = std::numeric_limits<float>::quiet_NaN();
    unsupportedPdr->update_interval = std::numeric_limits<float>::quiet_NaN();
    std::string unsupportedName{"unsupported_format_sensor"};
    NumericSensor unsupportedSensor(0x55, false, unsupportedPdr,
                                    unsupportedName, inventoryPath, nullptr);

    EXPECT_EQ(std::numeric_limits<uint64_t>::max(),
              unsupportedSensor.updateTime);
    ASSERT_NE(nullptr, unsupportedSensor.valueIntf);
    ASSERT_NE(nullptr, unsupportedSensor.thresholdWarningIntf);
    ASSERT_NE(nullptr, unsupportedSensor.thresholdCriticalIntf);
    ASSERT_NE(nullptr, unsupportedSensor.thresholdFatalIntf);
    EXPECT_TRUE(std::isnan(unsupportedSensor.valueIntf->maxValue()));
    EXPECT_TRUE(std::isnan(unsupportedSensor.valueIntf->minValue()));
    EXPECT_TRUE(std::isnan(unsupportedSensor.getThresholdUpperWarning()));
    EXPECT_TRUE(std::isnan(unsupportedSensor.getThresholdLowerWarning()));
    EXPECT_TRUE(std::isnan(unsupportedSensor.getThresholdUpperCritical()));
    EXPECT_TRUE(std::isnan(unsupportedSensor.getThresholdLowerCritical()));

    unsupportedSensor.updateReading(true, true, 23.0);
    EXPECT_DOUBLE_EQ(23.0, unsupportedSensor.getReading());

    auto sensorEventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    sensorEventInfo->impactedComponent = "CPU55";
    unsupportedSensor.updateSensorEventInfo(sensorEventInfo);
    ASSERT_NE(nullptr, unsupportedSensor.getSensorEventInfo());
    EXPECT_EQ("CPU55",
              unsupportedSensor.getSensorEventInfo()->impactedComponent);

    auto guardedPdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_WATTS, 0, false);
    guardedPdr->update_interval = 0.0002f;
    std::string guardedName{"update_guard_sensor"};
    NumericSensor guardedSensor(0x56, false, guardedPdr, guardedName,
                                inventoryPath, nullptr);
    guardedSensor.refreshLimitInUsec = 500;
    guardedSensor.setLastUpdatedTimeStamp(100);

    EXPECT_FALSE(guardedSensor.needsUpdate(150));
    EXPECT_FALSE(guardedSensor.needsUpdate(250));
    guardedSensor.isPriority = true;
    EXPECT_TRUE(guardedSensor.needsUpdate(350));
    guardedSensor.isPriority = false;
    EXPECT_FALSE(guardedSensor.isRefreshed());
    guardedSensor.setRefreshed(true);
    EXPECT_TRUE(guardedSensor.isRefreshed());

    guardedSensor.removeValueIntf();
    EXPECT_FALSE(guardedSensor.needsUpdate(1000));
}

TEST_F(NumericSensorTest,
       numericSensorInlineAssociationAndRefreshWindowCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis57"};

    auto valuePdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_WATTS, 0, false);
    std::string valueName{"inline_value_sensor"};
    NumericSensor valueSensor(0x57, false, valuePdr, valueName, inventoryPath,
                              nullptr);

    std::vector<std::string> valueInventories{inventoryPath + "/cpu0",
                                              inventoryPath + "/cpu1"};
    valueSensor.setInventoryPaths(valueInventories, false);
    auto valueAssocs = valueSensor.associationDefinitionsIntf->associations();
    ASSERT_EQ(2u, valueAssocs.size());
    EXPECT_EQ("chassis", std::get<0>(valueAssocs.front()));
    EXPECT_EQ("all_sensors", std::get<1>(valueAssocs.front()));
    EXPECT_EQ(valueInventories.front(), std::get<2>(valueAssocs.front()));

    auto [containerId, entityType,
          entityInstance] = valueSensor.getEntityInfo();
    EXPECT_EQ(1, containerId);
    EXPECT_EQ(PLDM_ENTITY_POWER_SUPPLY, entityType);
    EXPECT_EQ(1, entityInstance);

    valueSensor.updateTime = 200;
    valueSensor.refreshLimitInUsec = 1000;
    valueSensor.setLastUpdatedTimeStamp(100);
    EXPECT_FALSE(valueSensor.needsUpdate(350));

    EXPECT_EQ(nullptr, valueSensor.getSensorEventInfo());
    auto sensorEventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    sensorEventInfo->impactedComponent = "CPU57";
    valueSensor.updateSensorEventInfo(sensorEventInfo);
    EXPECT_EQ(sensorEventInfo, valueSensor.getSensorEventInfo());
    valueSensor.updateSensorEventInfo(nullptr);
    EXPECT_EQ(nullptr, valueSensor.getSensorEventInfo());

    auto metricPdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_COUNTS, 0, false);
    std::string metricName{"inline_metric_sensor"};
    NumericSensor metricSensor(0x58, false, metricPdr, metricName,
                               inventoryPath, nullptr);
    metricSensor.setInventoryPaths({inventoryPath + "/metrics0"}, true);
    auto metricAssocs = metricSensor.associationDefinitionsIntf->associations();
    ASSERT_EQ(1u, metricAssocs.size());
    EXPECT_EQ("measuring", std::get<0>(metricAssocs.front()));
    EXPECT_EQ("measured_by", std::get<1>(metricAssocs.front()));

    metricSensor.updateReading(true, true, 13.0);
    ASSERT_NE(nullptr, metricSensor.metricIntf);
    EXPECT_DOUBLE_EQ(metricSensor.metricIntf->value(),
                     metricSensor.getReading());
}

TEST_F(NumericSensorTest,
       numericSensorAssociationDefinitionGuardAfterRemovalCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis5e"};
    auto pdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_WATTS, 0, false);
    auto sensorEventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    sensorEventInfo->impactedComponent = "CPU5E";
    std::string sensorName{"association_guard_sensor"};
    NumericSensor sensor(0x5E, false, pdr, sensorName, inventoryPath,
                         sensorEventInfo);

    const std::vector<std::string> inventoryPaths{inventoryPath + "/cpu0",
                                                  inventoryPath + "/cpu1"};
    sensor.setInventoryPaths(inventoryPaths, false);
    auto associations = sensor.associationDefinitionsIntf->associations();
    ASSERT_EQ(2u, associations.size());
    EXPECT_EQ("chassis", std::get<0>(associations.front()));
    EXPECT_EQ("all_sensors", std::get<1>(associations.front()));
    EXPECT_EQ(inventoryPaths.back(), std::get<2>(associations.back()));
    EXPECT_EQ(sensorEventInfo, sensor.getSensorEventInfo());

    sensor.removeValueIntf();
    EXPECT_EQ(nullptr, sensor.associationDefinitionsIntf);
    sensor.setInventoryPaths({inventoryPath + "/cpu2"}, true);

    sensor.inventoryDecoratorAreaIntf.reset();
    EXPECT_NO_THROW(sensor.setPhysicalContext(PhysicalContextType::CPU));
}

TEST_F(NumericSensorTest,
       numericSensorMetricAssociationDefinitionGuardAfterRemovalCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis5f"};
    auto pdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_SECONDS, 0, false);
    std::string sensorName{"metric_guard_sensor"};
    NumericSensor sensor(0x5F, false, pdr, sensorName, inventoryPath, nullptr);

    const std::vector<std::string> inventoryPaths{inventoryPath + "/metric0",
                                                  inventoryPath + "/metric1"};
    sensor.setInventoryPaths(inventoryPaths, false);
    auto associations = sensor.associationDefinitionsIntf->associations();
    ASSERT_EQ(2u, associations.size());
    EXPECT_EQ("measuring", std::get<0>(associations.front()));
    EXPECT_EQ("measured_by", std::get<1>(associations.front()));
    EXPECT_EQ(inventoryPaths.back(), std::get<2>(associations.back()));

    sensor.removeValueIntf();
    EXPECT_EQ(nullptr, sensor.associationDefinitionsIntf);
    sensor.setInventoryPaths({inventoryPath + "/metric2"}, true);

    sensor.inventoryDecoratorAreaIntf.reset();
    EXPECT_NO_THROW(
        sensor.setPhysicalContext(PhysicalContextType::NetworkingDevice));
}

TEST_F(NumericSensorTest, numericSensorUnsupportedNamespaceGetterCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis64"};
    auto pdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT8, PLDM_RANGE_FIELD_FORMAT_UINT8, 0xFF, 0,
        false);
    auto sensorEventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    sensorEventInfo->impactedComponent = "CPU64";
    std::string sensorName{"x"};
    NumericSensor sensor(0x64, false, pdr, sensorName, inventoryPath,
                         sensorEventInfo);

    EXPECT_EQ("x", sensor.getSensorName());
    EXPECT_EQ("/xyz/openbmc_project/sensors/none/",
              sensor.getSensorNameSpace());
    EXPECT_EQ(sensorEventInfo, sensor.getSensorEventInfo());

    sensor.updateSensorEventInfo(nullptr);
    EXPECT_EQ(nullptr, sensor.getSensorEventInfo());
    sensor.setRefreshed(false);
    EXPECT_FALSE(sensor.isRefreshed());
    sensor.setLastUpdatedTimeStamp(0);
    EXPECT_EQ(0u, sensor.lastUpdatedTimeStampInUsec);
}

TEST_F(NumericSensorTest, numericSensorGetterCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis60"};
    auto pdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_WATTS, 0, false);
    std::string sensorName{"getter_sensor"};
    NumericSensor sensor(0x60, false, pdr, sensorName, inventoryPath, nullptr);

    EXPECT_EQ("getter_sensor", sensor.getSensorName());
    EXPECT_EQ("/xyz/openbmc_project/sensors/power/",
              sensor.getSensorNameSpace());
    EXPECT_EQ(nullptr, sensor.getSensorEventInfo());

    sensor.setRefreshed(true);
    EXPECT_TRUE(sensor.isRefreshed());
    sensor.setRefreshed(false);
    EXPECT_FALSE(sensor.isRefreshed());
}

TEST_F(NumericSensorTest, numericSensorLongNameAndEventInfoGetterCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis61"};
    auto pdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_COUNTS, 0, false);
    std::string sensorName(96, 'n');
    NumericSensor sensor(0x61, false, pdr, sensorName, inventoryPath, nullptr);

    EXPECT_EQ(sensorName, sensor.getSensorName());
    EXPECT_EQ("/xyz/openbmc_project/metric/count/",
              sensor.getSensorNameSpace());

    auto sensorEventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    sensorEventInfo->impactedComponent = std::string(96, 'X');
    sensor.updateSensorEventInfo(sensorEventInfo);
    EXPECT_EQ(sensorEventInfo, sensor.getSensorEventInfo());

    sensor.updateSensorEventInfo(nullptr);
    EXPECT_EQ(nullptr, sensor.getSensorEventInfo());
}

TEST_F(NumericSensorTest, numericSensorSetLastUpdatedTimeStampCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis62"};
    auto pdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_WATTS, 0, false);
    std::string sensorName{"timestamp_sensor"};
    NumericSensor sensor(0x62, false, pdr, sensorName, inventoryPath, nullptr);

    sensor.setLastUpdatedTimeStamp(12345);
    EXPECT_EQ(12345u, sensor.lastUpdatedTimeStampInUsec);
    sensor.setLastUpdatedTimeStamp(67890);
    EXPECT_EQ(67890u, sensor.lastUpdatedTimeStampInUsec);
}

TEST_F(NumericSensorTest, numericSensorSkipPollingGuardCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis63"};
    auto pdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_WATTS, 0, false);
    std::string sensorName{"skip_polling_sensor"};
    NumericSensor sensor(0x63, false, pdr, sensorName, inventoryPath, nullptr);

    sensor.updateTime = 200;
    sensor.refreshLimitInUsec = 1000;
    sensor.setLastUpdatedTimeStamp(100);
    EXPECT_FALSE(sensor.needsUpdate(250));
    EXPECT_FALSE(sensor.needsUpdate(900));
    EXPECT_TRUE(sensor.needsUpdate(1201));

    sensor.removeValueIntf();
    EXPECT_FALSE(sensor.needsUpdate(5000));
}

TEST_F(NumericSensorTest, numericSensorGetterSizeAndSharedPtrMatrixCoverage)
{
    const std::string inventoryPathValue{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_getter_matrix"};
    const std::array<std::size_t, 5> nameSizes{1, 15, 16, 63, 127};
    const std::array<uint8_t, 3> baseUnits{PLDM_SENSOR_UNIT_WATTS,
                                           PLDM_SENSOR_UNIT_COUNTS, 0xFF};

    for (std::size_t nameSize : nameSizes)
    {
        for (uint8_t baseUnit : baseUnits)
        {
            auto pdr = makeNumericSensorValuePdr(
                PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
                baseUnit, 0, false);
            std::string sensorName(nameSize, 'n');
            std::string inventoryPath = inventoryPathValue;
            NumericSensor sensor(0x65, false, pdr, sensorName, inventoryPath,
                                 nullptr);

            EXPECT_EQ(sensorName, sensor.getSensorName());
            EXPECT_FALSE(sensor.getSensorNameSpace().empty());

            auto sensorEventInfo =
                std::make_shared<pldm::utils::SensorEventInfo>();
            sensorEventInfo->impactedComponent = std::string(nameSize + 1, 'I');
            sensor.updateSensorEventInfo(sensorEventInfo);

            auto firstInfoCopy = sensor.getSensorEventInfo();
            auto secondInfoCopy = sensor.getSensorEventInfo();
            ASSERT_EQ(sensorEventInfo, firstInfoCopy);
            ASSERT_EQ(firstInfoCopy, secondInfoCopy);

            sensor.updateSensorEventInfo(firstInfoCopy);
            EXPECT_EQ(firstInfoCopy, sensor.getSensorEventInfo());

            sensor.setRefreshed(false);
            EXPECT_FALSE(sensor.isRefreshed());
            sensor.setRefreshed(true);
            EXPECT_TRUE(sensor.isRefreshed());

            sensor.updateTime = nameSize + 2;
            sensor.refreshLimitInUsec = nameSize + 1;
            sensor.setLastUpdatedTimeStamp(nameSize * 10);
            EXPECT_FALSE(sensor.needsUpdate(nameSize * 10 + nameSize));
            EXPECT_TRUE(sensor.needsUpdate(nameSize * 10 + nameSize + 3));

            sensor.updateSensorEventInfo(nullptr);
            EXPECT_EQ(nullptr, sensor.getSensorEventInfo());
        }
    }
}

TEST_F(NumericSensorTest,
       numericSensorInlineStringGetterExhaustiveBadAllocCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_getter_alloc"};
    auto pdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_COUNTS, 0, false);
    std::string sensorName(192, 'n');
    NumericSensor sensor(0x66, false, pdr, sensorName, inventoryPath, nullptr);

    EXPECT_TRUE(numeric_sensor_test_alloc::exerciseAllBadAlloc(
        [&] {
            auto copy = sensor.getSensorName();
            (void)copy;
        },
        2048));

    EXPECT_TRUE(numeric_sensor_test_alloc::exerciseAllBadAlloc(
        [&] {
            auto copy = sensor.getSensorNameSpace();
            (void)copy;
        },
        2048));
}

TEST_F(NumericSensorTest, numericSensorInlineSharedPtrAndRefreshMatrixCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_getter_state"};
    auto pdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_WATTS, 0, false);
    std::string sensorName{"inline_state_sensor"};
    NumericSensor sensor(0x67, false, pdr, sensorName, inventoryPath, nullptr);

    EXPECT_EQ(nullptr, sensor.getSensorEventInfo());

    auto sensorEventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    sensorEventInfo->impactedComponent = std::string(128, 'I');
    sensorEventInfo->eventIdsMap.emplace(
        "LinkDown", "ResourceEvent.1.0." + std::string(48, 'D'));
    sensor.updateSensorEventInfo(sensorEventInfo);

    auto firstCopy = sensor.getSensorEventInfo();
    auto secondCopy = sensor.getSensorEventInfo();
    ASSERT_EQ(sensorEventInfo, firstCopy);
    ASSERT_EQ(firstCopy, secondCopy);
    EXPECT_EQ(std::string(128, 'I'), firstCopy->impactedComponent);
    EXPECT_EQ("ResourceEvent.1.0." + std::string(48, 'D'),
              firstCopy->eventIdsMap.at("LinkDown"));

    sensor.updateSensorEventInfo(firstCopy);
    EXPECT_EQ(firstCopy, sensor.getSensorEventInfo());

    sensor.updateSensorEventInfo(nullptr);
    EXPECT_EQ(nullptr, sensor.getSensorEventInfo());

    sensor.setRefreshed(false);
    EXPECT_FALSE(sensor.isRefreshed());
    sensor.setRefreshed(true);
    EXPECT_TRUE(sensor.isRefreshed());
    sensor.setRefreshed(false);
    EXPECT_FALSE(sensor.isRefreshed());

    sensor.updateTime = 100;
    sensor.refreshLimitInUsec = 1000;
    sensor.setLastUpdatedTimeStamp(1000);

    sensor.isPriority = true;
    EXPECT_TRUE(sensor.needsUpdate(1100));

    sensor.isPriority = false;
    EXPECT_FALSE(sensor.needsUpdate(1100));
    EXPECT_TRUE(sensor.needsUpdate(2001));

    sensor.setLastUpdatedTimeStamp(std::numeric_limits<uint64_t>::max() - 10);
    EXPECT_FALSE(sensor.needsUpdate(std::numeric_limits<uint64_t>::max() - 5));

    sensor.removeValueIntf();
    EXPECT_FALSE(sensor.needsUpdate(std::numeric_limits<uint64_t>::max()));
}

TEST_F(NumericSensorTest, numericSensorInlineTransitionMatrixCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_transition_matrix"};
    auto pdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_WATTS, 0, false);
    std::string sensorName(192, 'n');
    NumericSensor sensor(0x68, false, pdr, sensorName, inventoryPath, nullptr);

    EXPECT_EQ(nullptr, sensor.getSensorEventInfo());
    EXPECT_EQ(sensorName, sensor.getSensorName());
    EXPECT_FALSE(sensor.getSensorNameSpace().empty());

    auto shortInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    shortInfo->impactedComponent = "GPU0";
    auto longInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    longInfo->impactedComponent = std::string(168, 'C');
    longInfo->eventIdsMap.emplace("Power",
                                  "ResourceEvent.1.0." + std::string(72, 'P'));

    sensor.updateSensorEventInfo(shortInfo);
    auto shortCopy1 = sensor.getSensorEventInfo();
    auto shortCopy2 = sensor.getSensorEventInfo();
    ASSERT_EQ(shortInfo, shortCopy1);
    ASSERT_EQ(shortCopy1, shortCopy2);

    sensor.updateSensorEventInfo(longInfo);
    auto longCopy1 = sensor.getSensorEventInfo();
    auto longCopy2 = sensor.getSensorEventInfo();
    ASSERT_EQ(longInfo, longCopy1);
    ASSERT_EQ(longCopy1, longCopy2);
    EXPECT_EQ(std::string(168, 'C'), longCopy1->impactedComponent);
    EXPECT_EQ("ResourceEvent.1.0." + std::string(72, 'P'),
              longCopy1->eventIdsMap.at("Power"));

    sensor.updateSensorEventInfo(longCopy1);
    EXPECT_EQ(longInfo, sensor.getSensorEventInfo());
    sensor.updateSensorEventInfo(nullptr);
    EXPECT_EQ(nullptr, sensor.getSensorEventInfo());

    sensor.setRefreshed(false);
    EXPECT_FALSE(sensor.isRefreshed());
    sensor.setRefreshed(true);
    EXPECT_TRUE(sensor.isRefreshed());

    sensor.updateTime = 200;
    sensor.refreshLimitInUsec = 250;
    sensor.isPriority = false;
    sensor.setLastUpdatedTimeStamp(1000);
    EXPECT_FALSE(sensor.needsUpdate(1100));
    EXPECT_FALSE(sensor.needsUpdate(1250));
    EXPECT_TRUE(sensor.needsUpdate(1251));

    sensor.isPriority = true;
    EXPECT_FALSE(sensor.needsUpdate(1100));
    EXPECT_TRUE(sensor.needsUpdate(1201));
}

#ifdef OEM_NVIDIA
TEST_F(NumericSensorTest, oemEnergyCountConstructorCoverageMatrix)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis0"};

    const std::array<uint8_t, 11> baseUnits{
        PLDM_SENSOR_UNIT_DEGRESS_C,
        PLDM_SENSOR_UNIT_VOLTS,
        PLDM_SENSOR_UNIT_AMPS,
        PLDM_SENSOR_UNIT_RPM,
        PLDM_SENSOR_UNIT_WATTS,
        PLDM_SENSOR_UNIT_JOULES,
        PLDM_SENSOR_UNIT_HERTZ,
        PLDM_SENSOR_UNIT_PERCENTAGE,
        PLDM_SENSOR_UNIT_COUNTS,
        PLDM_SENSOR_UNIT_SECONDS,
        0xFF};

    for (size_t i = 0; i < baseUnits.size(); ++i)
    {
        auto pdr = makeOemEnergyCountSensorPdr(
            static_cast<uint16_t>(0x100 + i), PLDM_SENSOR_DATA_SIZE_UINT16,
            baseUnits[i], -1);
        std::string sensorName = "oem_base_unit_" + std::to_string(i);
        NumericSensor sensor(0x40, false, pdr, sensorName, inventoryPath,
                             POLLING_METHOD_INDICATOR_PLDM_TYPE_OEM);

        EXPECT_EQ(static_cast<uint8_t>(POLLING_METHOD_INDICATOR_PLDM_TYPE_OEM),
                  sensor.getPollingIndicator());
        sensor.updateReading(true, true, 20.0 + i);

        if (baseUnits[i] == PLDM_SENSOR_UNIT_COUNTS ||
            baseUnits[i] == PLDM_SENSOR_UNIT_SECONDS)
        {
            EXPECT_EQ(nullptr, sensor.valueIntf);
            EXPECT_NE(nullptr, sensor.metricIntf);
        }
        else if (baseUnits[i] == 0xFF)
        {
            EXPECT_EQ(nullptr, sensor.valueIntf);
            EXPECT_EQ(nullptr, sensor.metricIntf);
        }
        else
        {
            EXPECT_NE(nullptr, sensor.valueIntf);
            EXPECT_EQ(nullptr, sensor.metricIntf);
        }
    }

    const std::array<uint8_t, 9> dataSizes{
        PLDM_SENSOR_DATA_SIZE_UINT8,
        PLDM_SENSOR_DATA_SIZE_SINT8,
        PLDM_SENSOR_DATA_SIZE_UINT16,
        PLDM_SENSOR_DATA_SIZE_SINT16,
        PLDM_SENSOR_DATA_SIZE_UINT32,
        PLDM_SENSOR_DATA_SIZE_SINT32,
        PLDM_SENSOR_DATA_SIZE_UINT64,
        PLDM_SENSOR_DATA_SIZE_SINT64,
        0xFF};

    for (size_t i = 0; i < dataSizes.size(); ++i)
    {
        auto pdr =
            makeOemEnergyCountSensorPdr(static_cast<uint16_t>(0x200 + i),
                                        dataSizes[i], PLDM_SENSOR_UNIT_WATTS);
        std::string sensorName = "oem_data_size_" + std::to_string(i);
        NumericSensor sensor(0x41, false, pdr, sensorName, inventoryPath,
                             POLLING_METHOD_INDICATOR_PLDM_TYPE_OEM);
        sensor.updateReading(true, true, 30.0 + i);
        EXPECT_TRUE(std::isfinite(sensor.getReading()));
    }

    auto pdr = makeOemEnergyCountSensorPdr(
        0x2FF, PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_SENSOR_UNIT_COUNTS, 0,
        std::numeric_limits<float>::quiet_NaN());
    std::string sensorName{"oem_update_time_nan"};
    NumericSensor sensor(0x42, false, pdr, sensorName, inventoryPath,
                         POLLING_METHOD_INDICATOR_PLDM_TYPE_OEM);
    EXPECT_EQ(std::numeric_limits<uint64_t>::max(), sensor.updateTime);
}

TEST_F(NumericSensorTest, numericSensorInlineRuntimeGetterCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_runtime_metric"};
    auto pdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_WATTS, 0, false);
    std::string sensorName{"runtime_metric_sensor"};
    NumericSensor sensor(0x68, false, pdr, sensorName, inventoryPath, nullptr);

    auto getName = &NumericSensor::getSensorName;
    auto getNamespace = &NumericSensor::getSensorNameSpace;
    auto updateInfo = &NumericSensor::updateSensorEventInfo;
    auto getInfo = &NumericSensor::getSensorEventInfo;
    auto setRefreshed = &NumericSensor::setRefreshed;
    auto isRefreshed = &NumericSensor::isRefreshed;
    auto setTimestamp = &NumericSensor::setLastUpdatedTimeStamp;
    auto needsUpdate = &NumericSensor::needsUpdate;

    EXPECT_EQ(sensorName, (sensor.*getName)());
    EXPECT_FALSE((sensor.*getNamespace)().empty());

    auto shortInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    shortInfo->impactedComponent = "GPU68";
    auto longInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    longInfo->impactedComponent = std::string(192, 'N');
    longInfo->eventIdsMap.emplace("Power",
                                  "ResourceEvent.1.0." + std::string(72, 'P'));

    const std::array<std::shared_ptr<pldm::utils::SensorEventInfo>, 4> infos{
        shortInfo, nullptr, longInfo, shortInfo};
    for (const auto& infoPtr : infos)
    {
        (sensor.*updateInfo)(infoPtr);
        auto copy = (sensor.*getInfo)();
        EXPECT_EQ(infoPtr, copy);
    }

    const std::array<bool, 3> refreshedStates{false, true, false};
    for (const auto refreshed : refreshedStates)
    {
        (sensor.*setRefreshed)(refreshed);
        EXPECT_EQ(refreshed, (sensor.*isRefreshed)());
    }

    sensor.updateTime = 0;
    sensor.refreshLimitInUsec = 250;
    (sensor.*setTimestamp)(1000);
    const std::array<uint64_t, 3> currentTimes{1100, 1250, 1251};
    const std::array<bool, 3> expectedNeedsUpdate{false, false, true};
    for (size_t idx = 0; idx < currentTimes.size(); ++idx)
    {
        EXPECT_EQ(expectedNeedsUpdate[idx],
                  (sensor.*needsUpdate)(currentTimes[idx]));
    }
}

TEST_F(NumericSensorTest, numericSensorInlineSharedPtrOwnershipMatrixCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_numeric_shared_ptr"};
    auto pdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_WATTS, 0, false);
    std::string sensorName{"numeric_shared_ptr_sensor"};
    NumericSensor sensor(0x69, false, pdr, sensorName, inventoryPath, nullptr);

    auto makeInfo = [](char fill, std::size_t size) {
        auto value = std::make_shared<pldm::utils::SensorEventInfo>();
        value->impactedComponent = std::string(size, fill);
        value->eventIdsMap.emplace(
            "Event", "ResourceEvent.1.0." + std::string(size / 2 + 1, fill));
        return value;
    };

    auto shared = makeInfo('S', 40);
    auto copied = shared;
    auto aliasOwner = makeInfo('A', 56);
    std::shared_ptr<pldm::utils::SensorEventInfo> alias(aliasOwner,
                                                        aliasOwner.get());
    auto customDeleter = std::shared_ptr<pldm::utils::SensorEventInfo>(
        new pldm::utils::SensorEventInfo{}, [](auto* value) { delete value; });
    customDeleter->impactedComponent = std::string(72, 'C');
    customDeleter->eventIdsMap.emplace(
        "Custom", "ResourceEvent.1.0." + std::string(22, 'C'));
    auto uniqueOwned = std::make_unique<pldm::utils::SensorEventInfo>();
    uniqueOwned->impactedComponent = std::string(88, 'U');
    uniqueOwned->eventIdsMap.emplace(
        "Unique", "ResourceEvent.1.0." + std::string(18, 'U'));
    std::shared_ptr<pldm::utils::SensorEventInfo> fromUnique(
        std::move(uniqueOwned));

    const std::vector<std::shared_ptr<pldm::utils::SensorEventInfo>> variants{
        nullptr, shared, copied, alias, customDeleter, fromUnique};
    for (const auto& value : variants)
    {
        sensor.updateSensorEventInfo(value);
        auto firstCopy = sensor.getSensorEventInfo();
        auto secondCopy = sensor.getSensorEventInfo();
        EXPECT_EQ(value.get(), firstCopy.get());
        EXPECT_EQ(value.get(), secondCopy.get());
        if (value)
        {
            EXPECT_FALSE(firstCopy->impactedComponent.empty());
            EXPECT_FALSE(secondCopy->eventIdsMap.empty());
        }
    }
}

TEST_F(NumericSensorTest, numericSensorInlineNameBoundaryMatrixCoverage)
{
    const std::vector<std::size_t> sizes{1, 15, 16, 31, 32, 96};
    const std::vector<uint8_t> units{
        PLDM_SENSOR_UNIT_WATTS, PLDM_SENSOR_UNIT_VOLTS, PLDM_SENSOR_UNIT_COUNTS,
        PLDM_SENSOR_UNIT_DEGRESS_C};
    auto getName = &NumericSensor::getSensorName;
    auto getNamespace = &NumericSensor::getSensorNameSpace;

    for (std::size_t idx = 0; idx < sizes.size(); ++idx)
    {
        auto pdr = makeNumericSensorValuePdr(
            PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
            units[idx % units.size()], 0, false);
        std::string inventoryPath =
            "/xyz/openbmc_project/inventory/system/chassis/numeric_boundary_" +
            std::to_string(idx);
        std::string sensorName(sizes[idx], static_cast<char>('a' + idx));
        NumericSensor sensor(static_cast<uint16_t>(0x80 + idx), false, pdr,
                             sensorName, inventoryPath, nullptr);

        EXPECT_EQ((sensor.*getName)(), sensorName);
        EXPECT_FALSE((sensor.*getNamespace)().empty());

        std::string renamed(sizes[(idx + 1) % sizes.size()],
                            static_cast<char>('k' + idx));
        sensor.updateSensorName(renamed);
        EXPECT_EQ((sensor.*getName)(), renamed);
        EXPECT_FALSE((sensor.*getNamespace)().empty());
    }

    auto longNamePdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_WATTS, 0, false);
    std::string longName(160, 'n');
    std::string longInventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/numeric_boundary_long"};
    NumericSensor longNameSensor(0x90, false, longNamePdr, longName,
                                 longInventoryPath, nullptr);
    EXPECT_TRUE(numeric_sensor_test_alloc::exerciseAllBadAlloc(
        [&] {
            auto name = longNameSensor.getSensorName();
            auto sensorNamespace = longNameSensor.getSensorNameSpace();
            (void)name;
            (void)sensorNamespace;
        },
        2048));
}
#endif
