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

#include "common/instance_id.hpp"
#include "mock_terminus_manager.hpp"
#include "platform-mc/numeric_effecter.hpp"
#include "platform-mc/numeric_effecter_base_unit.hpp"
#include "platform-mc/numeric_effecter_power_cap.hpp"
#include "platform-mc/terminus.hpp"
#include "test/test_instance_id.hpp"

#include <array>
#include <cmath>
#include <thread>

#include <gtest/gtest.h>

using namespace pldm::platform_mc;

class TestNumericEffecter : public ::testing::Test
{
  public:
    TestNumericEffecter() :
        bus(pldm::utils::DBusHandler::getBus()),
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        terminusManager(event, reqHandler, instanceIdDb, termini, 0x8, nullptr)
    {}

    sdbusplus::bus::bus& bus;
    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler;
    pldm::platform_mc::MockTerminusManager terminusManager;
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
};

static std::shared_ptr<pldm_numeric_effecter_value_pdr>
    makeNumericEffecterValuePdr(
        uint16_t effecterId, uint8_t effecterDataSize,
        uint8_t baseUnit = PLDM_SENSOR_UNIT_WATTS,
        uint16_t entityType = PLDM_ENTITY_PROC_IO_MODULE,
        int8_t unitModifier = 0)
{
    auto pdr = std::make_shared<pldm_numeric_effecter_value_pdr>();
    pdr->effecter_id = effecterId;
    pdr->entity_type = entityType;
    pdr->entity_instance = 1;
    pdr->container_id = 1;
    pdr->base_unit = baseUnit;
    pdr->unit_modifier = unitModifier;
    pdr->effecter_data_size = effecterDataSize;
    pdr->resolution = 2.0f;
    pdr->offset = 1.0f;
    pdr->range_field_format = PLDM_RANGE_FIELD_FORMAT_UINT8;
    pdr->range_field_support.byte = 0x1F;

    switch (effecterDataSize)
    {
        case PLDM_EFFECTER_DATA_SIZE_UINT8:
            pdr->max_settable.value_u8 = 100;
            pdr->min_settable.value_u8 = 1;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT8:
            pdr->max_settable.value_s8 = 100;
            pdr->min_settable.value_s8 = -100;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT16:
            pdr->max_settable.value_u16 = 1000;
            pdr->min_settable.value_u16 = 10;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT16:
            pdr->max_settable.value_s16 = 1000;
            pdr->min_settable.value_s16 = -1000;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT32:
            pdr->max_settable.value_u32 = 100000;
            pdr->min_settable.value_u32 = 100;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT32:
            pdr->max_settable.value_s32 = 100000;
            pdr->min_settable.value_s32 = -100000;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT64:
            pdr->max_settable.value_u64 = 1000000;
            pdr->min_settable.value_u64 = 1000;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT64:
        default:
            pdr->max_settable.value_s64 = 1000000;
            pdr->min_settable.value_s64 = -1000000;
            break;
    }

    pdr->nominal_value.value_u8 = 50;
    pdr->normal_max.value_u8 = 60;
    pdr->normal_min.value_u8 = 40;
    pdr->rated_max.value_u8 = 70;
    pdr->rated_min.value_u8 = 30;
    return pdr;
}

static std::vector<uint8_t> makeGetNumericEffecterValueResp(
    uint8_t effecterDataSize, pldm_effecter_oper_state operState,
    uint8_t completionCode = PLDM_SUCCESS)
{
    union_effecter_data_size pending{};
    union_effecter_data_size present{};
    size_t payloadLen = PLDM_GET_NUMERIC_EFFECTER_VALUE_MIN_RESP_BYTES;

    switch (effecterDataSize)
    {
        case PLDM_EFFECTER_DATA_SIZE_UINT8:
            pending.value_u8 = 10;
            present.value_u8 = 20;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT8:
            pending.value_s8 = -10;
            present.value_s8 = 20;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT16:
            pending.value_u16 = 1000;
            present.value_u16 = 2000;
            payloadLen += 2;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT16:
            pending.value_s16 = -1000;
            present.value_s16 = 2000;
            payloadLen += 2;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT32:
            pending.value_u32 = 100000;
            present.value_u32 = 200000;
            payloadLen += 6;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT32:
            pending.value_s32 = -100000;
            present.value_s32 = 200000;
            payloadLen += 6;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT64:
            pending.value_u64 = 1000000;
            present.value_u64 = 2000000;
            payloadLen += 14;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT64:
        default:
            pending.value_s64 = -1000000;
            present.value_s64 = 2000000;
            payloadLen += 14;
            break;
    }

    std::vector<uint8_t> response(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* responseMsg = reinterpret_cast<pldm_msg*>(response.data());
    auto rc = encode_get_numeric_effecter_value_resp(
        0, completionCode, effecterDataSize, operState,
        reinterpret_cast<uint8_t*>(&pending),
        reinterpret_cast<uint8_t*>(&present), responseMsg, payloadLen);
    EXPECT_EQ(rc, PLDM_SUCCESS);
    return response;
}

static std::vector<uint8_t> makeSetNumericEffecterValueResp(
    uint8_t completionCode = PLDM_SUCCESS)
{
    std::vector<uint8_t> response(
        sizeof(pldm_msg_hdr) + PLDM_SET_NUMERIC_EFFECTER_VALUE_RESP_BYTES, 0);
    auto* responseMsg = reinterpret_cast<pldm_msg*>(response.data());
    auto rc = encode_set_numeric_effecter_value_resp(
        0, completionCode, responseMsg,
        PLDM_SET_NUMERIC_EFFECTER_VALUE_RESP_BYTES);
    EXPECT_EQ(rc, PLDM_SUCCESS);
    return response;
}

static std::vector<uint8_t> makeCcOnlyResp(uint8_t command,
                                           uint8_t completionCode)
{
    return {0x0, PLDM_PLATFORM, command, completionCode};
}

class NumericEffecterBaseUnitCoverage : public NumericEffecterBaseUnit
{
  public:
    using NumericEffecterBaseUnit::NumericEffecterBaseUnit;

    void handleGetNumericEffecterValue(
        pldm_effecter_oper_state effecterOperState, double pendingValue,
        double presentValue) override
    {
        called = true;
        opState = effecterOperState;
        pending = pendingValue;
        present = presentValue;
    }

    bool called = false;
    pldm_effecter_oper_state opState = EFFECTER_OPER_STATE_STATUSUNKNOWN;
    double pending = 0.0;
    double present = 0.0;
};

TEST_F(TestNumericEffecter, verifyNumericEffecterInventoryPath)
{
    uint16_t sensorId = 0x0801;
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                                          // record handle
        0x1,                                          // PDRHeaderVersion
        PLDM_NUMERIC_EFFECTER_PDR,                    // PDRType
        0x0,
        0x0,                                          // recordChangeNumber
        53,
        0,                                            // dataLength
        0,
        0,                                            // PLDMTerminusHandle
        static_cast<uint8_t>(sensorId & 0xFF),
        static_cast<uint8_t>((sensorId >> 8) & 0xFF), // effecterID=0x0801
        PLDM_ENTITY_PROC_IO_MODULE,
        0,                                            // entityType
        1,
        0,                                            // entityInstanceNumber
        0x1,
        0x0,                                          // containerID=1
        0x0,
        0x0,                                          // effecterSematicID
        PLDM_NO_INIT,                                 // effecterInit
        false,                       // effecterAuxiliaryNames PDR
        PLDM_SENSOR_UNIT_WATTS,      // baseUnit
        0,                           // unitModifier
        0,                           // rateUnit
        0,                           // baseOEMUnitHandle
        0,                           // auxUnit
        0,                           // auxUnitModifier
        0,                           // auxrateUnit
        0,                           // auxOEMUnitHandle
        true,                        // isLinear
        PLDM_SENSOR_DATA_SIZE_UINT8, // effecterDataSize
        1,
        0,
        0,
        0, // resolution
        0,
        0,
        0,
        0, // offset
        0,
        0, // accuracy
        0, // plusTolerance
        0, // minusTolerance
        0,
        0,
        0,
        0, // stateTransitionInterval
        0,
        0,
        1,
        0,                             // TransitionInterval
        1,                             // maxSettable
        0,                             // minSettable
        PLDM_RANGE_FIELD_FORMAT_UINT8, // rangeFieldFormat
        0x3F,                          // rangeFieldSupport
        0,                             // nominalValue
        0,                             // normalMax
        0,                             // normalMin
        0,                             // ratedMax
        0                              // ratedMin
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericEffecterPdrs.size());
    EXPECT_EQ(1, t1.numericEffecters.size());

    auto numericEffecterPdr = t1.numericEffecterPdrs[0];

    std::string sensorName{"test1"};
    std::string inventoryPath{
        "/xyz/openbmc_project/inventroy/Item/Board/PLDM_device_1"};
    NumericEffecter sensor(0x01, true, numericEffecterPdr, sensorName,
                           inventoryPath, terminusManager);

    std::vector<std::string> paths{
        "/xyz/openbmc_project/inventory/system/board/cpu0"};

    sensor.setInventoryPaths(paths);

    auto assocs = sensor.getAssociation();
    EXPECT_EQ(1, assocs.size());
    for (auto& assoc : assocs)
    {
        auto& [forward, reverse, objectPath] = assoc;
        EXPECT_EQ("power_controls", reverse);
        EXPECT_EQ(paths[0], objectPath);
    }

    paths.emplace_back("/xyz/openbmc_project/inventory/system/board/hgx_cpu0");
    sensor.setInventoryPaths(paths);

    assocs = sensor.getAssociation();
    EXPECT_EQ(2, assocs.size());
    uint8_t counter = 0;
    for (auto& assoc : assocs)
    {
        auto& [forward, reverse, objectPath] = assoc;
        EXPECT_EQ("power_controls", reverse);
        for (auto& p : paths)
        {
            if (p == objectPath)
            {
                counter++;
            }
        }
    }
    EXPECT_EQ(counter, assocs.size());
}

TEST_F(TestNumericEffecter, baseUnitAndConversionCoverage)
{
    uint16_t effecterId = 0x0802;
    std::string uuid1("00000000-0000-0000-0000-000000000002");
    auto terminus = Terminus(2, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                             terminusManager);

    std::vector<uint8_t> pdr{
        0x0,
        0x0,
        0x0,
        0x1,
        0x1,
        PLDM_NUMERIC_EFFECTER_PDR,
        0x0,
        0x0,
        53,
        0,
        0,
        0,
        static_cast<uint8_t>(effecterId & 0xFF),
        static_cast<uint8_t>((effecterId >> 8) & 0xFF),
        PLDM_ENTITY_PROC_IO_MODULE,
        0,
        1,
        0,
        0x1,
        0x0,
        0x0,
        0x0,
        PLDM_NO_INIT,
        false,
        PLDM_SENSOR_UNIT_WATTS,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        true,
        PLDM_SENSOR_DATA_SIZE_UINT8,
        1,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        1,
        0,
        1,
        1,
        0,
        PLDM_RANGE_FIELD_FORMAT_UINT8,
        0x3F,
        0,
        0,
        0,
        0,
        0};

    terminus.pdrs.emplace_back(pdr);
    ASSERT_TRUE(terminus.parsePDRs());
    ASSERT_EQ(1u, terminus.numericEffecterPdrs.size());

    std::string effecterName{"test_numeric_effecter"};
    std::string inventoryPath{
        "/xyz/openbmc_project/inventroy/Item/Board/PLDM_device_2"};
    NumericEffecter effecter(0x02, true, terminus.numericEffecterPdrs[0],
                             effecterName, inventoryPath, terminusManager);

    NumericEffecterBaseUnitCoverage baseUnit(effecter);
    baseUnit.pdrMaxSettable(55.0);
    baseUnit.pdrMinSettable(5.0);
    EXPECT_DOUBLE_EQ(55.0, baseUnit.pdrMaxSettable());
    EXPECT_DOUBLE_EQ(5.0, baseUnit.pdrMinSettable());

    baseUnit.handleGetNumericEffecterValue(
        EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING, 22.0, 11.0);
    EXPECT_TRUE(baseUnit.called);
    EXPECT_EQ(EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING, baseUnit.opState);
    EXPECT_DOUBLE_EQ(22.0, baseUnit.pending);
    EXPECT_DOUBLE_EQ(11.0, baseUnit.present);

    baseUnit.called = false;
    baseUnit.handleErrGetNumericEffecterValue();
    EXPECT_TRUE(baseUnit.called);
    EXPECT_EQ(EFFECTER_OPER_STATE_FAILED, baseUnit.opState);

    EXPECT_TRUE(std::isfinite(effecter.rawToBase(100.0)));
    EXPECT_TRUE(std::isfinite(effecter.baseToRaw(10.0)));
    effecter.setPhysicalContext(PhysicalContextType::CPU);
}

TEST_F(TestNumericEffecter, numericEffecterDataSizeAndStateCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis0"};
    std::string effecterName{"effecter_matrix"};

    const std::array<uint8_t, 8> dataSizes{
        PLDM_EFFECTER_DATA_SIZE_UINT8,  PLDM_EFFECTER_DATA_SIZE_SINT8,
        PLDM_EFFECTER_DATA_SIZE_UINT16, PLDM_EFFECTER_DATA_SIZE_SINT16,
        PLDM_EFFECTER_DATA_SIZE_UINT32, PLDM_EFFECTER_DATA_SIZE_SINT32,
        PLDM_EFFECTER_DATA_SIZE_UINT64, PLDM_EFFECTER_DATA_SIZE_SINT64};

    for (size_t i = 0; i < dataSizes.size(); ++i)
    {
        auto pdr = makeNumericEffecterValuePdr(
            static_cast<uint16_t>(0x0900 + i), dataSizes[i],
            PLDM_SENSOR_UNIT_NONE, PLDM_ENTITY_SYS_BOARD, -1);
        NumericEffecter effecter(0x20 + i, false, pdr, effecterName,
                                 inventoryPath, terminusManager);

        effecter.updateValue(EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING, 10.0,
                             20.0);
        EXPECT_EQ(StateType::Deferring, effecter.state());
        effecter.updateValue(EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING, 11.0,
                             21.0);
        EXPECT_EQ(StateType::Enabled, effecter.state());
        effecter.updateValue(EFFECTER_OPER_STATE_DISABLED, 0.0, 0.0);
        EXPECT_EQ(StateType::Disabled, effecter.state());
        effecter.updateValue(EFFECTER_OPER_STATE_INITIALIZING, 0.0, 0.0);
        EXPECT_EQ(StateType::Starting, effecter.state());
        effecter.updateValue(EFFECTER_OPER_STATE_UNAVAILABLE, 0.0, 0.0);
        EXPECT_EQ(StateType::UnavailableOffline, effecter.state());
        effecter.updateValue(EFFECTER_OPER_STATE_STATUSUNKNOWN, 0.0, 0.0);
        effecter.updateValue(EFFECTER_OPER_STATE_FAILED, 0.0, 0.0);
        effecter.updateValue(EFFECTER_OPER_STATE_SHUTTINGDOWN, 0.0, 0.0);
        effecter.updateValue(EFFECTER_OPER_STATE_INTEST, 0.0, 0.0);

        effecter.handleErrGetNumericEffecterValue();
        effecter.setAvailable(true);
        effecter.setAvailable(false);

        EXPECT_TRUE(std::isfinite(effecter.rawToUnit(10.0)));
        EXPECT_TRUE(std::isfinite(effecter.unitToRaw(10.0)));
        EXPECT_TRUE(std::isfinite(effecter.unitToBase(10.0)));
        EXPECT_TRUE(std::isfinite(effecter.baseToUnit(10.0)));
        EXPECT_TRUE(std::isfinite(effecter.rawToBase(10.0)));
        EXPECT_TRUE(std::isfinite(effecter.baseToRaw(10.0)));
    }

    auto badPdr = makeNumericEffecterValuePdr(
        0x0999, PLDM_EFFECTER_DATA_SIZE_UINT8, 0xFF, PLDM_ENTITY_SYS_BOARD);
    EXPECT_THROW((void)NumericEffecter(0x42, false, badPdr, effecterName,
                                       inventoryPath, terminusManager),
                 std::runtime_error);
}

TEST_F(TestNumericEffecter, requestAndPowerCapCoverage)
{
    constexpr pldm::tid_t tid = 0x21;
    const pldm::MctpInfo mctpInfo(
        12, "00000000-0000-0000-0000-000000000021",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Endpoint.BindingTypes.PCIe", std::nullopt);
    ASSERT_TRUE(terminusManager.mapTid(mctpInfo, tid).has_value());

    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis0"};
    std::string effecterName{"power_cap_effecter"};

    auto pdr =
        makeNumericEffecterValuePdr(0x0A01, PLDM_EFFECTER_DATA_SIZE_UINT8,
                                    PLDM_SENSOR_UNIT_WATTS, PLDM_ENTITY_PROC);
    NumericEffecter effecter(tid, false, pdr, effecterName, inventoryPath,
                             terminusManager);

    auto* wattIntf =
        dynamic_cast<NumericEffecterWattInft*>(effecter.unitIntf.get());
    ASSERT_NE(wattIntf, nullptr);

    wattIntf->pdrMinSettable(5);
    wattIntf->pdrMaxSettable(150);
    wattIntf->handleGetNumericEffecterValue(
        EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING, 25, 20);
    EXPECT_EQ(25u, wattIntf->powerCap());
    wattIntf->handleGetNumericEffecterValue(EFFECTER_OPER_STATE_DISABLED, 25,
                                            20);
    EXPECT_THROW((void)wattIntf->powerCap(200), ::errors::InvalidArgument);

    std::vector<uint8_t> response;

    response = makeCcOnlyResp(PLDM_SET_NUMERIC_EFFECTER_ENABLE, PLDM_SUCCESS);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    response = makeGetNumericEffecterValueResp(
        PLDM_EFFECTER_DATA_SIZE_UINT8,
        EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    auto enableRc = stdexec::sync_wait(effecter.setNumericEffecterEnable(
        EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING));
    ASSERT_TRUE(enableRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*enableRc));

    const std::array<uint8_t, 8> dataSizes{
        PLDM_EFFECTER_DATA_SIZE_UINT8,  PLDM_EFFECTER_DATA_SIZE_SINT8,
        PLDM_EFFECTER_DATA_SIZE_UINT16, PLDM_EFFECTER_DATA_SIZE_SINT16,
        PLDM_EFFECTER_DATA_SIZE_UINT32, PLDM_EFFECTER_DATA_SIZE_SINT32,
        PLDM_EFFECTER_DATA_SIZE_UINT64, PLDM_EFFECTER_DATA_SIZE_SINT64};

    for (auto dataSize : dataSizes)
    {
        std::string effecterNameForSize =
            effecterName + "_" + std::to_string(dataSize);
        auto pdrForSize = makeNumericEffecterValuePdr(
            static_cast<uint16_t>(0x0B00 + dataSize), dataSize,
            PLDM_SENSOR_UNIT_NONE, PLDM_ENTITY_SYS_BOARD);
        NumericEffecter effecterForSize(tid, false, pdrForSize,
                                        effecterNameForSize, inventoryPath,
                                        terminusManager);

        response = makeSetNumericEffecterValueResp(PLDM_SUCCESS);
        ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
        response = makeGetNumericEffecterValueResp(
            dataSize, EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING);
        ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));

        auto setRc =
            stdexec::sync_wait(effecterForSize.setNumericEffecterValue(12));
        ASSERT_TRUE(setRc.has_value());
        EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*setRc));
    }

    response = makeGetNumericEffecterValueResp(
        PLDM_EFFECTER_DATA_SIZE_UINT8,
        EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING, PLDM_ERROR);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    auto getRc = stdexec::sync_wait(effecter.getNumericEffecterValue());
    ASSERT_TRUE(getRc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*getRc));
}

TEST_F(TestNumericEffecter, powerCapSetAndEnablePathCoverage)
{
    constexpr pldm::tid_t tid = 0x31;
    const pldm::MctpInfo mctpInfo(
        13, "00000000-0000-0000-0000-000000000031",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Endpoint.BindingTypes.PCIe", std::nullopt);
    ASSERT_TRUE(terminusManager.mapTid(mctpInfo, tid).has_value());

    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis31"};
    std::string effecterName{"power_cap_effecter_coverage"};
    auto pdr =
        makeNumericEffecterValuePdr(0x0C01, PLDM_EFFECTER_DATA_SIZE_UINT8,
                                    PLDM_SENSOR_UNIT_WATTS, PLDM_ENTITY_PROC);
    NumericEffecter effecter(tid, false, pdr, effecterName, inventoryPath,
                             terminusManager);

    auto* wattIntf =
        dynamic_cast<NumericEffecterWattInft*>(effecter.unitIntf.get());
    ASSERT_NE(wattIntf, nullptr);
    wattIntf->pdrMinSettable(5);
    wattIntf->pdrMaxSettable(150);
    wattIntf->handleGetNumericEffecterValue(
        EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING, 25, 25);

    std::vector<uint8_t> response;

    response = makeSetNumericEffecterValueResp(PLDM_SUCCESS);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    response = makeGetNumericEffecterValueResp(
        PLDM_EFFECTER_DATA_SIZE_UINT8,
        EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    EXPECT_NO_THROW((void)wattIntf->powerCap(50));

    response = makeCcOnlyResp(PLDM_SET_NUMERIC_EFFECTER_ENABLE, PLDM_SUCCESS);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    response = makeGetNumericEffecterValueResp(
        PLDM_EFFECTER_DATA_SIZE_UINT8,
        EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    (void)wattIntf->powerCapEnable(true);

    response = makeCcOnlyResp(PLDM_SET_NUMERIC_EFFECTER_ENABLE, PLDM_SUCCESS);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    response = makeGetNumericEffecterValueResp(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                               EFFECTER_OPER_STATE_DISABLED);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    (void)wattIntf->powerCapEnable(false);

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
}

TEST(OemBaseCoverage, ctorDtorCoverage)
{
    OemIntf oem;
    (void)oem;
}

TEST_F(TestNumericEffecter, pcieLinkMaskStateTrackingLifecycle)
{
    constexpr pldm::tid_t tid = 0x51;
    const pldm::MctpInfo mctpInfo(
        15, "00000000-0000-0000-0000-000000000051",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Endpoint.BindingTypes.PCIe", std::nullopt);
    ASSERT_TRUE(terminusManager.mapTid(mctpInfo, tid).has_value());

    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis51"};

    std::string maskEffecterName{"ProcessorModule_0_CPU_0_PCIeRPLinkCtrl_0"};
    auto maskPdr = makeNumericEffecterValuePdr(
        0x0F31, PLDM_EFFECTER_DATA_SIZE_UINT64, PLDM_SENSOR_UNIT_BITS,
        PLDM_ENTITY_PCI_EXPRESS_BUS);
    NumericEffecter maskEffecter(tid, false, maskPdr, maskEffecterName,
                                 inventoryPath, terminusManager);
    EXPECT_TRUE(maskEffecter.trackOperationalState);
    EXPECT_TRUE(maskEffecter.needUpdate);

    std::string plainEffecterName{"plain_counts_effecter"};
    auto plainPdr = makeNumericEffecterValuePdr(
        0x0F32, PLDM_EFFECTER_DATA_SIZE_UINT8, PLDM_SENSOR_UNIT_NONE,
        PLDM_ENTITY_SYS_BOARD);
    NumericEffecter plainEffecter(tid, false, plainPdr, plainEffecterName,
                                  inventoryPath, terminusManager);
    EXPECT_FALSE(plainEffecter.trackOperationalState);

    // Non-terminal operational states keep the tracking armed.
    for (auto operState : {EFFECTER_OPER_STATE_INITIALIZING,
                           EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING,
                           EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING,
                           EFFECTER_OPER_STATE_STATUSUNKNOWN})
    {
        auto response = makeGetNumericEffecterValueResp(
            PLDM_EFFECTER_DATA_SIZE_UINT64, operState);
        ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
        auto rc = stdexec::sync_wait(maskEffecter.getNumericEffecterValue());
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
        EXPECT_TRUE(maskEffecter.needUpdate) << "operState " << operState;
    }

    // Completion-code and decode errors bypass updateValue and keep the
    // tracking armed.
    auto response = makeGetNumericEffecterValueResp(
        PLDM_EFFECTER_DATA_SIZE_UINT64,
        EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING, PLDM_ERROR);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    auto ccRc = stdexec::sync_wait(maskEffecter.getNumericEffecterValue());
    ASSERT_TRUE(ccRc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*ccRc));
    EXPECT_TRUE(maskEffecter.needUpdate);

    response = makeCcOnlyResp(PLDM_GET_NUMERIC_EFFECTER_VALUE, PLDM_SUCCESS);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    auto decodeRc = stdexec::sync_wait(maskEffecter.getNumericEffecterValue());
    ASSERT_TRUE(decodeRc.has_value());
    EXPECT_NE(PLDM_SUCCESS, std::get<0>(*decodeRc));
    EXPECT_TRUE(maskEffecter.needUpdate);

    // unavailable is terminal: the device closed its write window.
    response = makeGetNumericEffecterValueResp(PLDM_EFFECTER_DATA_SIZE_UINT64,
                                               EFFECTER_OPER_STATE_UNAVAILABLE);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    auto unavailRc = stdexec::sync_wait(maskEffecter.getNumericEffecterValue());
    ASSERT_TRUE(unavailRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*unavailRc));
    EXPECT_FALSE(maskEffecter.needUpdate);
    EXPECT_EQ(StateType::UnavailableOffline, maskEffecter.state());

    // disabled is terminal too (feature knob off).
    maskEffecter.needUpdate = true;
    response = makeGetNumericEffecterValueResp(PLDM_EFFECTER_DATA_SIZE_UINT64,
                                               EFFECTER_OPER_STATE_DISABLED);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    auto disabledRc =
        stdexec::sync_wait(maskEffecter.getNumericEffecterValue());
    ASSERT_TRUE(disabledRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*disabledRc));
    EXPECT_FALSE(maskEffecter.needUpdate);
    EXPECT_EQ(StateType::Disabled, maskEffecter.state());

    // A write re-arms the tracking until the state settles...
    response = makeSetNumericEffecterValueResp(PLDM_SUCCESS);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    response = makeGetNumericEffecterValueResp(
        PLDM_EFFECTER_DATA_SIZE_UINT64,
        EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    auto setRc = stdexec::sync_wait(maskEffecter.setNumericEffecterValue(1));
    ASSERT_TRUE(setRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*setRc));
    EXPECT_TRUE(maskEffecter.needUpdate);

    // ...and self-limits when the trailing read already sees the window
    // closed.
    response = makeSetNumericEffecterValueResp(PLDM_SUCCESS);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    response = makeGetNumericEffecterValueResp(PLDM_EFFECTER_DATA_SIZE_UINT64,
                                               EFFECTER_OPER_STATE_UNAVAILABLE);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    setRc = stdexec::sync_wait(maskEffecter.setNumericEffecterValue(2));
    ASSERT_TRUE(setRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*setRc));
    EXPECT_FALSE(maskEffecter.needUpdate);

    // Untracked effecters keep their one-shot needUpdate semantics: neither
    // a terminal reading nor a write may touch the flag.
    plainEffecter.needUpdate = true;
    response = makeGetNumericEffecterValueResp(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                               EFFECTER_OPER_STATE_DISABLED);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    auto plainRc = stdexec::sync_wait(plainEffecter.getNumericEffecterValue());
    ASSERT_TRUE(plainRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*plainRc));
    EXPECT_TRUE(plainEffecter.needUpdate);
    EXPECT_EQ(StateType::Disabled, plainEffecter.state());

    plainEffecter.needUpdate = false;
    response = makeSetNumericEffecterValueResp(PLDM_SUCCESS);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    response = makeGetNumericEffecterValueResp(
        PLDM_EFFECTER_DATA_SIZE_UINT8,
        EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    setRc = stdexec::sync_wait(plainEffecter.setNumericEffecterValue(3));
    ASSERT_TRUE(setRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*setRc));
    EXPECT_FALSE(plainEffecter.needUpdate);
}

TEST_F(TestNumericEffecter, pcieLinkMaskTrackingDispatchNegativeCases)
{
    constexpr pldm::tid_t tid = 0x52;
    const pldm::MctpInfo mctpInfo(
        16, "00000000-0000-0000-0000-000000000052",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Endpoint.BindingTypes.PCIe", std::nullopt);
    ASSERT_TRUE(terminusManager.mapTid(mctpInfo, tid).has_value());

    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis52"};

    // Tracking needs BOTH halves of the ctor condition: the PCI Express Bus
    // entity type AND a "PCIeRPLinkCtrl" aux name. Neither alone qualifies.
    std::string wrongName{"ProcessorModule_0_PCIeBus_0_SomeOtherCtrl_0"};
    auto wrongNamePdr = makeNumericEffecterValuePdr(
        0x0F41, PLDM_EFFECTER_DATA_SIZE_UINT64, PLDM_SENSOR_UNIT_BITS,
        PLDM_ENTITY_PCI_EXPRESS_BUS);
    NumericEffecter wrongNameEffecter(tid, false, wrongNamePdr, wrongName,
                                      inventoryPath, terminusManager);
    EXPECT_FALSE(wrongNameEffecter.trackOperationalState);

    // The D-Bus object path is derived from the effecter name alone, so each
    // effecter below needs a distinct name to avoid colliding on the bus.
    std::string wrongEntityName{"ProcessorModule_1_PCIeRPLinkCtrl_0"};
    auto wrongEntityPdr = makeNumericEffecterValuePdr(
        0x0F42, PLDM_EFFECTER_DATA_SIZE_UINT64, PLDM_SENSOR_UNIT_BITS,
        PLDM_ENTITY_SYS_BOARD);
    NumericEffecter wrongEntityEffecter(tid, false, wrongEntityPdr,
                                        wrongEntityName, inventoryPath,
                                        terminusManager);
    EXPECT_FALSE(wrongEntityEffecter.trackOperationalState);

    // A base unit other than Bits falls through to the plain unit class, which
    // publishes no mask interface at all.
    std::string wrongUnitName{"ProcessorModule_2_PCIeRPLinkCtrl_0"};
    auto wrongUnitPdr = makeNumericEffecterValuePdr(
        0x0F43, PLDM_EFFECTER_DATA_SIZE_UINT64, PLDM_SENSOR_UNIT_COUNTS,
        PLDM_ENTITY_PCI_EXPRESS_BUS);
    NumericEffecter wrongUnitEffecter(tid, false, wrongUnitPdr, wrongUnitName,
                                      inventoryPath, terminusManager);
    EXPECT_FALSE(wrongUnitEffecter.trackOperationalState);

    // The entity type is matched with the 0x8000 "logical" bit masked off, so a
    // logical PCI Express Bus entity still tracks.
    std::string logicalName{"ProcessorModule_3_PCIeRPLinkCtrl_0"};
    auto logicalPdr = makeNumericEffecterValuePdr(
        0x0F44, PLDM_EFFECTER_DATA_SIZE_UINT64, PLDM_SENSOR_UNIT_BITS,
        static_cast<uint16_t>(PLDM_ENTITY_PCI_EXPRESS_BUS | 0x8000));
    NumericEffecter logicalEffecter(tid, false, logicalPdr, logicalName,
                                    inventoryPath, terminusManager);
    EXPECT_TRUE(logicalEffecter.trackOperationalState);
}
