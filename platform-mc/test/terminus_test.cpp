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
#include "oem/nvidia/platform-mc/oem_nvidia.hpp"
#include "oem/nvidia/platform-mc/remoteDebug.hpp"
#include "oem/nvidia/platform-mc/state_set/memoryPerformance.hpp"
#include "oem/nvidia/platform-mc/staticPowerHint.hpp"
#include "platform-mc/entity.hpp"
#include "platform-mc/errors.hpp"
#include "platform-mc/state_set/ethIBPortLinkState.hpp"
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#define protected public
#include "platform-mc/terminus.hpp"
#undef protected
#undef private
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "mock_terminus_manager.hpp"
#include "platform-mc/platform_manager.hpp"
#include "platform-mc/sensor_manager.hpp"
#include "test/test_instance_id.hpp"

#include <sdbusplus/async.hpp>
#include <sdeventplus/event.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

using namespace pldm::platform_mc;
const uint8_t localEid = 0x08;
class TerminusTest : public testing::Test
{
  protected:
    TerminusTest() :
        bus(pldm::utils::DBusHandler::getBus()),
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        terminusManager(event, reqHandler, instanceIdDb, termini, localEid,
                        nullptr),
        sensorManager(event, terminusManager, termini, nullptr),
        platformManager(terminusManager, termini)
    {}

    void runEventLoopForMilliseconds(uint64_t msec)
    {
        uint64_t t0 = 0;
        uint64_t t1 = 0;
        uint64_t usec = msec * 1000;
        uint64_t elapsed = 0;
        sd_event_now(event.get(), CLOCK_MONOTONIC, &t0);
        do
        {
            sd_event_run(event.get(), usec - elapsed);
            sd_event_now(event.get(), CLOCK_MONOTONIC, &t1);
            elapsed = t1 - t0;
        } while (elapsed < usec);
    }

    void setupResponsesForDiscoverTerminus()
    {
        auto rc = terminusManager.clearQueuedResponses();
        EXPECT_EQ(rc, PLDM_SUCCESS);

        std::vector<uint8_t> getTidResp0{0x00, PLDM_BASE, PLDM_GET_TID,
                                         PLDM_SUCCESS, 0x00};
        rc = terminusManager.enqueueResponse(getTidResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);

        std::vector<uint8_t> setTidResp0{0x00, PLDM_BASE, PLDM_SET_TID,
                                         PLDM_SUCCESS};
        rc = terminusManager.enqueueResponse(setTidResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);

        // support pldm type0 and type2
        std::vector<uint8_t> getPldmTypesResp0{
            0x00, PLDM_BASE, 0x04, 0x00, 0x05, 0x00,
            0x00, 0x00,      0x00, 0x00, 0x00, 0x00};
        rc = terminusManager.enqueueResponse(getPldmTypesResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);

        std::vector<uint8_t> getTerminusUidResp0{
            0x00, PLDM_PLATFORM, PLDM_GET_TERMINUS_UID,
            PLDM_ERROR_UNSUPPORTED_PLDM_CMD};
        rc = terminusManager.enqueueResponse(getTerminusUidResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);
    }

    void setupResponsesForInitTerminus()
    {
        auto rc = terminusManager.clearQueuedResponses();
        EXPECT_EQ(rc, PLDM_SUCCESS);

        std::vector<uint8_t> eventMessageBufferSizeResp0{
            0x00, PLDM_PLATFORM, PLDM_EVENT_MESSAGE_BUFFER_SIZE,
            PLDM_ERROR_UNSUPPORTED_PLDM_CMD};
        rc = terminusManager.enqueueResponse(eventMessageBufferSizeResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);

        std::vector<uint8_t> eventMessageSupportedResp0{
            0x00, PLDM_PLATFORM, PLDM_EVENT_MESSAGE_SUPPORTED,
            PLDM_ERROR_UNSUPPORTED_PLDM_CMD};
        rc = terminusManager.enqueueResponse(eventMessageSupportedResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);

        std::vector<uint8_t> getPDRRepositoryInfoResp0{
            0x00, PLDM_PLATFORM, PLDM_EVENT_MESSAGE_SUPPORTED,
            PLDM_ERROR_UNSUPPORTED_PLDM_CMD};
        rc = terminusManager.enqueueResponse(getPDRRepositoryInfoResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);

        std::vector<uint8_t> getPdrResp0{
            0x00,
            PLDM_PLATFORM,
            PLDM_GET_PDR,
            PLDM_SUCCESS,
            0x00,
            0x00,
            0x00,
            0x00, // nextRecordHandle
            0x00,
            0x00,
            0x00,
            0x00, // nextDataTransferHandle
            0x05, // startAndEnd
            71,
            0,    // responseCount (PDR = 10 header + 61 body)
            0x00,
            0x00,
            0x00,
            0x01,                        // record handle
            0x01,                        // PDRHeaderVersion
            PLDM_NUMERIC_SENSOR_PDR,     // PDRType
            0x00,
            0x00,                        // recordChangeNumber
            61,
            0,                           // dataLength (body = 61 bytes)
            0x00,
            0x00,                        // PLDMTerminusHandle
            0x01,
            0x00,                        // sensorID=1
            PLDM_ENTITY_POWER_SUPPLY,
            0,                           // entityType=Power Supply(120)
            1,
            0,                           // entityInstanceNumber
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
            0,                             // fatalLow
            0,
            0,
            0 // padding (body = 61 bytes)
        };
        rc = terminusManager.enqueueResponse(getPdrResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);
    }

    void setupResponsesForStartPolling()
    {
        auto rc = terminusManager.clearQueuedResponses();
        EXPECT_EQ(rc, PLDM_SUCCESS);

        std::vector<uint8_t> getSensorReadingResp0{
            0x00,
            PLDM_PLATFORM,
            PLDM_GET_SENSOR_READING,
            PLDM_SUCCESS,
            PLDM_SENSOR_DATA_SIZE_UINT8,
            PLDM_SENSOR_ENABLED,
            PLDM_NO_EVENT_GENERATION,
            PLDM_SENSOR_NORMAL,
            PLDM_SENSOR_NORMAL,
            PLDM_SENSOR_NORMAL,
            0x12};
        rc = terminusManager.enqueueResponse(getSensorReadingResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);
        rc = terminusManager.enqueueResponse(getSensorReadingResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);
        rc = terminusManager.enqueueResponse(getSensorReadingResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);
    }

    sdbusplus::bus::bus& bus;
    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler;
    pldm::platform_mc::MockTerminusManager terminusManager;
    pldm::platform_mc::SensorManager sensorManager;
    pldm::platform_mc::PlatformManager platformManager;
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
};

static void sealAndRewind(sdbusplus::message::message& msg)
{
    EXPECT_GE(sd_bus_message_seal(msg.get(), 0, 0), 0);
    EXPECT_GE(sd_bus_message_rewind(msg.get(), true), 0);
}

static std::vector<uint8_t> makeEntityAssociationPdr()
{
    std::vector<uint8_t> pdr(
        sizeof(pldm_pdr_hdr) + sizeof(pldm_pdr_entity_association), 0);
    auto* hdr = reinterpret_cast<pldm_pdr_hdr*>(pdr.data());
    hdr->record_handle = 4;
    hdr->version = 1;
    hdr->type = PLDM_PDR_ENTITY_ASSOCIATION;
    hdr->record_change_num = 0;
    hdr->length = pdr.size() - sizeof(pldm_pdr_hdr);

    auto* assoc = reinterpret_cast<pldm_pdr_entity_association*>(
        pdr.data() + sizeof(pldm_pdr_hdr));
    assoc->container_id = 1;
    assoc->association_type = PLDM_ENTITY_ASSOCIAION_PHYSICAL;
    assoc->container.entity_type = PLDM_ENTITY_SYSTEM_CHASSIS;
    assoc->container.entity_instance_num = 1;
    assoc->container.entity_container_id = 0;
    assoc->num_children = 1;
    assoc->children[0].entity_type = PLDM_ENTITY_POWER_SUPPLY;
    assoc->children[0].entity_instance_num = 1;
    assoc->children[0].entity_container_id = 1;
    return pdr;
}

static std::vector<uint8_t> makeAuxNamePdr(uint16_t effecterId, uint8_t pdrType)
{
    std::vector<uint8_t> names{
        1,                     // nameStringCount
        'e',  'n',  0x00,      // name language tag: "en"
        0x00, 0x41, 0x00, 0x00 // UTF16-BE "A"
    };

    std::vector<uint8_t> pdr(
        sizeof(pldm_effecter_aux_name_pdr) + names.size() - 1, 0);
    auto* aux = reinterpret_cast<pldm_effecter_aux_name_pdr*>(pdr.data());
    aux->hdr.record_handle = 6;
    aux->hdr.version = 1;
    aux->hdr.type = pdrType;
    aux->hdr.record_change_num = 0;
    aux->hdr.length = pdr.size() - sizeof(pldm_pdr_hdr);
    aux->terminus_handle = 1;
    aux->effecter_id = effecterId;
    aux->effecter_count = 1;
    memcpy(aux->effecter_names, names.data(), names.size());
    return pdr;
}

/* Build a compact (wire-format) numeric effecter PDR byte vector.
 * decode_numeric_effecter_pdr_data() expects each variable-width field
 * (max_settable, min_settable, range fields) to occupy exactly the number
 * of bytes indicated by effecter_data_size / range_field_format, NOT the
 * full sizeof(union_effecter_data_size) = 8 bytes used by the C struct. */
static std::vector<uint8_t> serializeNumericEffecterPdr(
    const pldm_numeric_effecter_value_pdr& pdr)
{
    /* Build payload bytes separately so we can compute hdr.length. */
    std::vector<uint8_t> payload;
    auto pu8 = [&](uint8_t x) { payload.push_back(x); };
    auto ple16 = [&](uint16_t x) {
        payload.push_back(x & 0xFF);
        payload.push_back((x >> 8) & 0xFF);
    };
    auto ple32 = [&](uint32_t x) {
        payload.push_back(x & 0xFF);
        payload.push_back((x >> 8) & 0xFF);
        payload.push_back((x >> 16) & 0xFF);
        payload.push_back((x >> 24) & 0xFF);
    };
    auto ple64 = [&](uint64_t x) {
        ple32(static_cast<uint32_t>(x & 0xFFFFFFFF));
        ple32(static_cast<uint32_t>(x >> 32));
    };
    auto plef32 = [&](float x) {
        uint32_t bits;
        memcpy(&bits, &x, 4);
        ple32(bits);
    };

    ple16(pdr.terminus_handle);
    ple16(pdr.effecter_id);
    ple16(pdr.entity_type);
    ple16(pdr.entity_instance);
    ple16(pdr.container_id);
    ple16(pdr.effecter_semantic_id);
    pu8(pdr.effecter_init);
    pu8(pdr.effecter_auxiliary_names);
    pu8(pdr.base_unit);
    pu8(static_cast<uint8_t>(pdr.unit_modifier));
    pu8(pdr.rate_unit);
    pu8(pdr.base_oem_unit_handle);
    pu8(pdr.aux_unit);
    pu8(static_cast<uint8_t>(pdr.aux_unit_modifier));
    pu8(pdr.aux_rate_unit);
    pu8(pdr.aux_oem_unit_handle);
    pu8(pdr.is_linear);
    pu8(pdr.effecter_data_size);
    plef32(pdr.resolution);
    plef32(pdr.offset);
    ple16(pdr.accuracy);
    pu8(pdr.plus_tolerance);
    pu8(pdr.minus_tolerance);
    plef32(pdr.state_transition_interval);
    plef32(pdr.transition_interval);

    /* Variable-width effecter data (max_settable, min_settable). */
    auto appendEff = [&](const union_effecter_data_size& d) {
        switch (pdr.effecter_data_size)
        {
            case PLDM_EFFECTER_DATA_SIZE_UINT8:
                pu8(d.value_u8);
                break;
            case PLDM_EFFECTER_DATA_SIZE_SINT8:
                pu8(static_cast<uint8_t>(d.value_s8));
                break;
            case PLDM_EFFECTER_DATA_SIZE_UINT16:
                ple16(d.value_u16);
                break;
            case PLDM_EFFECTER_DATA_SIZE_SINT16:
                ple16(static_cast<uint16_t>(d.value_s16));
                break;
            case PLDM_EFFECTER_DATA_SIZE_UINT32:
                ple32(d.value_u32);
                break;
            case PLDM_EFFECTER_DATA_SIZE_SINT32:
                ple32(static_cast<uint32_t>(d.value_s32));
                break;
            case PLDM_EFFECTER_DATA_SIZE_UINT64:
                ple64(d.value_u64);
                break;
            case PLDM_EFFECTER_DATA_SIZE_SINT64:
                ple64(static_cast<uint64_t>(d.value_s64));
                break;
            default:
                break;
        }
    };
    appendEff(pdr.max_settable);
    appendEff(pdr.min_settable);

    pu8(pdr.range_field_format);
    pu8(pdr.range_field_support.byte);

    /* Variable-width range fields. */
    auto appendRng = [&](const union_range_field_format& d) {
        switch (pdr.range_field_format)
        {
            case PLDM_RANGE_FIELD_FORMAT_UINT8:
                pu8(d.value_u8);
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT8:
                pu8(static_cast<uint8_t>(d.value_s8));
                break;
            case PLDM_RANGE_FIELD_FORMAT_UINT16:
                ple16(d.value_u16);
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT16:
                ple16(static_cast<uint16_t>(d.value_s16));
                break;
            case PLDM_RANGE_FIELD_FORMAT_UINT32:
                ple32(d.value_u32);
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT32:
                ple32(static_cast<uint32_t>(d.value_s32));
                break;
            case PLDM_RANGE_FIELD_FORMAT_REAL32:
                plef32(d.value_f32);
                break;
            case PLDM_RANGE_FIELD_FORMAT_UINT64:
                ple64(d.value_u64);
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT64:
                ple64(static_cast<uint64_t>(d.value_s64));
                break;
            default:
                break;
        }
    };
    appendRng(pdr.nominal_value);
    appendRng(pdr.normal_max);
    appendRng(pdr.normal_min);
    appendRng(pdr.rated_max);
    appendRng(pdr.rated_min);

    /* Prepend the 10-byte PDR header (record_handle, version, type,
     * record_change_num, length) where length = payload byte count. */
    std::vector<uint8_t> v;
    auto hu8 = [&](uint8_t x) { v.push_back(x); };
    auto hle16 = [&](uint16_t x) {
        v.push_back(x & 0xFF);
        v.push_back((x >> 8) & 0xFF);
    };
    auto hle32 = [&](uint32_t x) {
        v.push_back(x & 0xFF);
        v.push_back((x >> 8) & 0xFF);
        v.push_back((x >> 16) & 0xFF);
        v.push_back((x >> 24) & 0xFF);
    };
    hle32(pdr.hdr.record_handle);
    hu8(pdr.hdr.version);
    hu8(pdr.hdr.type);
    hle16(pdr.hdr.record_change_num);
    hle16(static_cast<uint16_t>(payload.size()));
    v.insert(v.end(), payload.begin(), payload.end());
    return v;
}

static std::vector<uint8_t> makeNumericEffecterPdr(uint16_t effecterId,
                                                   bool withAuxName)
{
    pldm_numeric_effecter_value_pdr pdr{};
    pdr.hdr.record_handle = 7;
    pdr.hdr.version = 1;
    pdr.hdr.type = PLDM_NUMERIC_EFFECTER_PDR;
    pdr.hdr.record_change_num = 0;
    pdr.terminus_handle = 1;
    pdr.effecter_id = effecterId;
    pdr.entity_type = PLDM_ENTITY_SYS_BOARD;
    pdr.entity_instance = 1;
    pdr.container_id = 1;
    pdr.effecter_semantic_id = 1;
    pdr.effecter_init = PLDM_NO_INIT;
    pdr.effecter_auxiliary_names = withAuxName;
    pdr.base_unit = PLDM_SENSOR_UNIT_NONE;
    pdr.unit_modifier = 0;
    pdr.is_linear = true;
    pdr.effecter_data_size = PLDM_EFFECTER_DATA_SIZE_UINT8;
    pdr.max_settable.value_u8 = 100;
    pdr.min_settable.value_u8 = 0;
    pdr.range_field_format = PLDM_RANGE_FIELD_FORMAT_UINT8;
    pdr.range_field_support.byte = 0x1F;
    pdr.nominal_value.value_u8 = 50;
    pdr.normal_max.value_u8 = 60;
    pdr.normal_min.value_u8 = 40;
    pdr.rated_max.value_u8 = 70;
    pdr.rated_min.value_u8 = 30;
    return serializeNumericEffecterPdr(pdr);
}

static std::vector<uint8_t> makeNumericEffecterPdrVariant(
    uint16_t effecterId, uint8_t effecterDataSize, uint8_t rangeFieldFormat)
{
    pldm_numeric_effecter_value_pdr pdr{};
    pdr.hdr.record_handle = effecterId;
    pdr.hdr.version = 1;
    pdr.hdr.type = PLDM_NUMERIC_EFFECTER_PDR;
    pdr.hdr.record_change_num = 0;
    pdr.terminus_handle = 1;
    pdr.effecter_id = effecterId;
    pdr.entity_type = PLDM_ENTITY_SYS_BOARD;
    pdr.entity_instance = 1;
    pdr.container_id = 1;
    pdr.effecter_semantic_id = 1;
    pdr.effecter_init = PLDM_NO_INIT;
    pdr.effecter_auxiliary_names = false;
    pdr.base_unit = PLDM_SENSOR_UNIT_NONE;
    pdr.unit_modifier = 0;
    pdr.is_linear = true;
    pdr.effecter_data_size = effecterDataSize;

    switch (effecterDataSize)
    {
        case PLDM_EFFECTER_DATA_SIZE_UINT8:
            pdr.max_settable.value_u8 = 200;
            pdr.min_settable.value_u8 = 5;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT8:
            pdr.max_settable.value_s8 = 100;
            pdr.min_settable.value_s8 = -100;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT16:
            pdr.max_settable.value_u16 = 2000;
            pdr.min_settable.value_u16 = 10;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT16:
            pdr.max_settable.value_s16 = 1000;
            pdr.min_settable.value_s16 = -1000;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT32:
            pdr.max_settable.value_u32 = 200000;
            pdr.min_settable.value_u32 = 100;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT32:
            pdr.max_settable.value_s32 = 100000;
            pdr.min_settable.value_s32 = -100000;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT64:
            pdr.max_settable.value_u64 = 2000000;
            pdr.min_settable.value_u64 = 1000;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT64:
            pdr.max_settable.value_s64 = 1000000;
            pdr.min_settable.value_s64 = -1000000;
            break;
        default:
            break;
    }

    pdr.range_field_format = rangeFieldFormat;
    pdr.range_field_support.byte = 0x1F;
    switch (rangeFieldFormat)
    {
        case PLDM_RANGE_FIELD_FORMAT_UINT8:
            pdr.nominal_value.value_u8 = 50;
            pdr.normal_max.value_u8 = 60;
            pdr.normal_min.value_u8 = 40;
            pdr.rated_max.value_u8 = 70;
            pdr.rated_min.value_u8 = 30;
            break;
        case PLDM_RANGE_FIELD_FORMAT_SINT8:
            pdr.nominal_value.value_s8 = 10;
            pdr.normal_max.value_s8 = 20;
            pdr.normal_min.value_s8 = -20;
            pdr.rated_max.value_s8 = 30;
            pdr.rated_min.value_s8 = -30;
            break;
        case PLDM_RANGE_FIELD_FORMAT_UINT16:
            pdr.nominal_value.value_u16 = 500;
            pdr.normal_max.value_u16 = 600;
            pdr.normal_min.value_u16 = 400;
            pdr.rated_max.value_u16 = 700;
            pdr.rated_min.value_u16 = 300;
            break;
        case PLDM_RANGE_FIELD_FORMAT_SINT16:
            pdr.nominal_value.value_s16 = 100;
            pdr.normal_max.value_s16 = 200;
            pdr.normal_min.value_s16 = -200;
            pdr.rated_max.value_s16 = 300;
            pdr.rated_min.value_s16 = -300;
            break;
        case PLDM_RANGE_FIELD_FORMAT_UINT32:
            pdr.nominal_value.value_u32 = 50000;
            pdr.normal_max.value_u32 = 60000;
            pdr.normal_min.value_u32 = 40000;
            pdr.rated_max.value_u32 = 70000;
            pdr.rated_min.value_u32 = 30000;
            break;
        case PLDM_RANGE_FIELD_FORMAT_SINT32:
            pdr.nominal_value.value_s32 = 10000;
            pdr.normal_max.value_s32 = 20000;
            pdr.normal_min.value_s32 = -20000;
            pdr.rated_max.value_s32 = 30000;
            pdr.rated_min.value_s32 = -30000;
            break;
        case PLDM_RANGE_FIELD_FORMAT_REAL32:
            pdr.nominal_value.value_f32 = 12.5f;
            pdr.normal_max.value_f32 = 20.5f;
            pdr.normal_min.value_f32 = -20.5f;
            pdr.rated_max.value_f32 = 30.5f;
            pdr.rated_min.value_f32 = -30.5f;
            break;
        case PLDM_RANGE_FIELD_FORMAT_UINT64:
            pdr.nominal_value.value_u64 = 500000;
            pdr.normal_max.value_u64 = 600000;
            pdr.normal_min.value_u64 = 400000;
            pdr.rated_max.value_u64 = 700000;
            pdr.rated_min.value_u64 = 300000;
            break;
        case PLDM_RANGE_FIELD_FORMAT_SINT64:
            pdr.nominal_value.value_s64 = 100000;
            pdr.normal_max.value_s64 = 200000;
            pdr.normal_min.value_s64 = -200000;
            pdr.rated_max.value_s64 = 300000;
            pdr.rated_min.value_s64 = -300000;
            break;
        default:
            break;
    }

    return serializeNumericEffecterPdr(pdr);
}

static std::vector<uint8_t> makeOemPdr()
{
    std::vector<uint8_t> pdr(sizeof(pldm_oem_pdr) + 3, 0);
    auto* oem = reinterpret_cast<pldm_oem_pdr*>(pdr.data());
    oem->hdr.record_handle = 8;
    oem->hdr.version = 1;
    oem->hdr.type = PLDM_OEM_PDR;
    oem->hdr.record_change_num = 0;
    oem->hdr.length = pdr.size() - sizeof(pldm_pdr_hdr);
    oem->vendor_iana = 0x1234;
    oem->ome_record_id = 1;
    oem->data_length = 3;
    oem->vendor_specific_data[0] = 0xAA;
    oem->vendor_specific_data[1] = 0xBB;
    oem->vendor_specific_data[2] = 0xCC;
    return pdr;
}

static std::vector<uint8_t> makeNvidiaEnergyCountOemPdr(uint16_t sensorId)
{
    pldm_oem_energycount_numeric_sensor_value_pdr energyPdr{};
    energyPdr.terminus_handle = 1;
    energyPdr.nvidia_oem_pdr_type = 3; // NVIDIA_OEM_PDR_TYPE_SENSOR_ENERGYCOUNT
    energyPdr.sensor_id = sensorId;
    energyPdr.entity_type = PLDM_ENTITY_POWER_SUPPLY;
    energyPdr.entity_instance_num = 1;
    energyPdr.container_id = 1;
    energyPdr.sensor_auxiliary_names_pdr = false;
    energyPdr.base_unit = PLDM_SENSOR_UNIT_WATTS;
    energyPdr.unit_modifier = 0;
    energyPdr.sensor_data_size = PLDM_SENSOR_DATA_SIZE_UINT16;
    energyPdr.update_interval = 1.0f;
    energyPdr.max_readable.value_u16 = 1000;
    energyPdr.min_readable.value_u16 = 1;

    std::vector<uint8_t> pdr(sizeof(pldm_oem_pdr) + sizeof(energyPdr) - 1, 0);
    auto* oem = reinterpret_cast<pldm_oem_pdr*>(pdr.data());
    oem->hdr.record_handle = 9;
    oem->hdr.version = 1;
    oem->hdr.type = PLDM_OEM_PDR;
    oem->hdr.record_change_num = 0;
    oem->hdr.length = pdr.size() - sizeof(pldm_pdr_hdr);
    oem->vendor_iana = 0x1647;
    oem->ome_record_id = 1;
    oem->data_length = sizeof(energyPdr) - 1;
    memcpy(oem->vendor_specific_data, &energyPdr, sizeof(energyPdr));
    return pdr;
}

static std::vector<uint8_t> makeSensorAuxNamePdr(uint16_t sensorId)
{
    std::vector<uint8_t> pdr{
        0x0,
        0x0,
        0x0,
        0x1,                             // record handle
        0x1,                             // PDRHeaderVersion
        PLDM_SENSOR_AUXILIARY_NAMES_PDR, // PDRType
        0x0,
        0x0,                             // recordChangeNumber
        0x9,
        0x0,                             // dataLength
        0x0,
        0x0,                             // terminus handle
        static_cast<uint8_t>(sensorId & 0xFF),
        static_cast<uint8_t>((sensorId >> 8) & 0xFF),
        0x1, // sensor count
        0x1, // name string count
        'e',
        'n',
        0x0, // language tag
        0x0,
        'S',
        0x0,
        0x0 // UTF16-BE "S"
    };
    return pdr;
}

static std::vector<uint8_t> makeStateSensorPdr(
    uint16_t sensorId, uint16_t entityType, uint16_t stateSetId,
    bool hasAuxNames)
{
    return {
        0x0,
        0x0,
        0x0,
        0x1, // record handle
        0x1, // PDRHeaderVersion
        PLDM_STATE_SENSOR_PDR,
        0x0,
        0x0,  // recordChangeNumber
        0x0,
        0x11, // dataLength
        0,
        0,    // PLDMTerminusHandle
        static_cast<uint8_t>(sensorId & 0xFF),
        static_cast<uint8_t>((sensorId >> 8) & 0xFF),
        static_cast<uint8_t>(entityType & 0xFF),
        static_cast<uint8_t>((entityType >> 8) & 0xFF),
        1,
        0,                                 // entityInstance
        0x1,
        0x0,                               // containerID
        PLDM_NO_INIT,
        static_cast<uint8_t>(hasAuxNames), // sensorAuxiliaryNamesPDR
        1,                                 // compositeSensorCount
        static_cast<uint8_t>(stateSetId & 0xFF),
        static_cast<uint8_t>((stateSetId >> 8) & 0xFF),
        0x1, // possibleStatesSize
        0x3  // possibleStates
    };
}

static std::vector<uint8_t> makeStateEffecterPdr(
    uint16_t effecterId, uint16_t entityType, uint16_t stateSetId)
{
    return {
        0x0,
        0x0,
        0x0,
        0x1, // record handle
        0x1, // PDRHeaderVersion
        PLDM_STATE_EFFECTER_PDR,
        0x0,
        0x0,  // recordChangeNumber
        0x0,
        0x13, // dataLength
        0,
        0,    // PLDMTerminusHandle
        static_cast<uint8_t>(effecterId & 0xFF),
        static_cast<uint8_t>((effecterId >> 8) & 0xFF),
        static_cast<uint8_t>(entityType & 0xFF),
        static_cast<uint8_t>((entityType >> 8) & 0xFF),
        1,
        0,     // entityInstance
        0x1,
        0x0,   // containerID
        0x0,
        0x0,   // effecterSemanticID
        PLDM_NO_INIT,
        false, // effecterDescriptionPDR
        1,     // compositeEffecterCount
        static_cast<uint8_t>(stateSetId & 0xFF),
        static_cast<uint8_t>((stateSetId >> 8) & 0xFF),
        0x1, // possibleStatesSize
        0x7  // possibleStates
    };
}

static std::shared_ptr<pldm_numeric_sensor_value_pdr>
    makeNumericSensorValuePdrStruct(
        uint16_t sensorId, uint16_t entityType = PLDM_ENTITY_SYS_BOARD,
        uint16_t entityInstance = 1, uint16_t containerId = 1)
{
    auto pdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    pdr->sensor_id = sensorId;
    pdr->entity_type = entityType;
    pdr->entity_instance_num = entityInstance;
    pdr->container_id = containerId;
    pdr->base_unit = PLDM_SENSOR_UNIT_DEGRESS_C;
    pdr->sensor_data_size = PLDM_SENSOR_DATA_SIZE_UINT8;
    pdr->max_readable.value_u8 = 100;
    pdr->min_readable.value_u8 = 0;
    pdr->hysteresis.value_u8 = 1;
    pdr->supported_thresholds.byte = 0;
    pdr->range_field_support.byte = 0;
    pdr->range_field_format = PLDM_RANGE_FIELD_FORMAT_UINT8;
    pdr->resolution = 1.0f;
    pdr->offset = 0.0f;
    pdr->update_interval = 1.0f;
    return pdr;
}

static std::shared_ptr<pldm_numeric_effecter_value_pdr>
    makeNumericEffecterValuePdrStruct(
        uint16_t effecterId, uint16_t entityType = PLDM_ENTITY_SYS_BOARD,
        uint16_t entityInstance = 1, uint16_t containerId = 1)
{
    auto pdr = std::make_shared<pldm_numeric_effecter_value_pdr>();
    pdr->effecter_id = effecterId;
    pdr->entity_type = entityType;
    pdr->entity_instance = entityInstance;
    pdr->container_id = containerId;
    pdr->base_unit = PLDM_SENSOR_UNIT_NONE;
    pdr->effecter_data_size = PLDM_EFFECTER_DATA_SIZE_UINT8;
    pdr->max_settable.value_u8 = 100;
    pdr->min_settable.value_u8 = 0;
    pdr->range_field_format = PLDM_RANGE_FIELD_FORMAT_UINT8;
    pdr->range_field_support.byte = 0x1F;
    pdr->nominal_value.value_u8 = 50;
    pdr->normal_max.value_u8 = 60;
    pdr->normal_min.value_u8 = 40;
    pdr->rated_max.value_u8 = 70;
    pdr->rated_min.value_u8 = 30;
    return pdr;
}

static StateSetInfo makeSimpleStateSetInfo()
{
    StateSetData healthStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_HEALTHSTATE),
                        PossibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                       PLDM_STATESET_HEALTH_STATE_CRITICAL});
    return std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
                           std::vector<StateSetData>{healthStateData});
}

template <typename T>
static std::vector<uint8_t> structToBytes(const T& input)
{
    std::vector<uint8_t> bytes(sizeof(T), 0);
    memcpy(bytes.data(), &input, sizeof(T));
    return bytes;
}

static StateSetInfo makeSingleStateSetInfo(EntityType entityType,
                                           uint16_t stateSetId)
{
    StateSetData stateData =
        std::make_tuple(stateSetId, PossibleStates{0, 1, 2, 3, 4, 5, 6, 7});
    return std::make_tuple(EntityInfo{1, entityType, 1},
                           std::vector<StateSetData>{stateData});
}

static StateSetInfo makeDebugStateSetInfo(EntityType entityType,
                                          size_t compositeCount = 6)
{
    std::vector<StateSetData> stateSets;
    stateSets.reserve(compositeCount);
    for (size_t i = 0; i < compositeCount; ++i)
    {
        stateSets.emplace_back(
            static_cast<uint16_t>(PLDM_NVIDIA_OEM_STATE_SET_DEBUG_STATE),
            PossibleStates{PLDM_STATE_SET_DEBUG_STATE_DISABLED,
                           PLDM_STATE_SET_DEBUG_STATE_ENABLED,
                           PLDM_STATE_SET_DEBUG_STATE_OFFLINE});
    }
    return std::make_tuple(EntityInfo{1, entityType, 1}, stateSets);
}

TEST_F(TerminusTest, supportedTypeTest)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    std::string uuid2("00000000-0000-0000-0000-000000000002");
    auto t1 = Terminus(1, 1 << PLDM_BASE, uuid1, terminusManager);
    auto t2 = Terminus(2, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid2,
                       terminusManager);

    EXPECT_EQ(true, t1.doesSupport(PLDM_BASE));
    EXPECT_EQ(false, t1.doesSupport(PLDM_PLATFORM));
    EXPECT_EQ(true, t2.doesSupport(PLDM_BASE));
    EXPECT_EQ(true, t2.doesSupport(PLDM_PLATFORM));
}

TEST_F(TerminusTest, getTidTest)
{
    const pldm::tid_t tid = 1;
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(tid, 1 << PLDM_BASE, uuid1, terminusManager);

    EXPECT_EQ(tid, t1.getTid());
}

TEST_F(TerminusTest, parseSensorAuxiliaryNamesPDRTest)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                             // record handle
        0x1,                             // PDRHeaderVersion
        PLDM_SENSOR_AUXILIARY_NAMES_PDR, // PDRType
        0x0,
        0x0,                             // recordChangeNumber
        21,
        0,                               // dataLength
        0,
        0x0,                             // PLDMTerminusHandle
        0x1,
        0x0,                             // sensorID
        0x1,                             // sensorCount
        0x1,                             // nameStringCount
        'e',
        'n',
        0x0, // nameLanguageTag
        0x0,
        'T',
        0x0,
        'E',
        0x0,
        'M',
        0x0,
        'P',
        0x0,
        '1',
        0x0,
        0x0 // sensorName
    };

    std::vector<uint8_t> pdr2{
        0x0, 0x0, 0x0,
        0x1,                             // record handle
        0x1,                             // PDRHeaderVersion
        PLDM_SENSOR_AUXILIARY_NAMES_PDR, // PDRType
        0x0,
        0x0,                             // recordChangeNumber
        21,
        0,                               // dataLength
        0,
        0x0,                             // PLDMTerminusHandle
        0x2,
        0x0,                             // sensorID
        0x2,                             // sensorCount
                                         // sensor0
        0x0,                             // nameStringCount
                                         // sensor1
        0x1,                             // nameStringCount
        'e', 'n',
        0x0,                             // nameLanguageTag
        0x0, 'T', 0x0, 'E', 0x0, 'M', 0x0, 'P', 0x0, '2', 0x0,
        0x0                              // sensorName
    };

    t1.pdrs.emplace_back(pdr1);
    t1.pdrs.emplace_back(pdr2);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);

    auto sensorAuxNames = t1.getSensorAuxiliaryNames(0);
    EXPECT_EQ(nullptr, sensorAuxNames);

    sensorAuxNames = t1.getSensorAuxiliaryNames(1);
    EXPECT_NE(nullptr, sensorAuxNames);

    const auto& [sensorId, sensorCnt, names] = *sensorAuxNames;
    EXPECT_EQ(1, sensorId);
    EXPECT_EQ(1, sensorCnt);
    EXPECT_EQ(1, names.size());
    EXPECT_EQ(1, names[0].size());
    EXPECT_EQ("en", names[0][0].first);
    EXPECT_EQ("TEMP1", names[0][0].second);

    sensorAuxNames = t1.getSensorAuxiliaryNames(2);
    EXPECT_NE(nullptr, sensorAuxNames);

    const auto& [sensorId2, sensorCnt2, names2] = *sensorAuxNames;
    EXPECT_EQ(2, sensorId2);
    EXPECT_EQ(2, sensorCnt2);
    EXPECT_EQ(2, names2.size());
    EXPECT_EQ(0, names2[0].size());
    EXPECT_EQ(1, names2[1].size());
    EXPECT_EQ("en", names2[1][0].first);
    EXPECT_EQ("TEMP2", names2[1][0].second);
}

TEST_F(TerminusTest, addNumericSensorTest)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                             // record handle
        0x1,                             // PDRHeaderVersion
        PLDM_SENSOR_AUXILIARY_NAMES_PDR, // PDRType
        0x0,
        0x0,                             // recordChangeNumber
        21,
        0,                               // dataLength
        0,
        0x0,                             // PLDMTerminusHandle
        0x1,
        0x0,                             // sensorID
        0x1,                             // sensorCount
        0x1,                             // nameStringCount
        'e',
        'n',
        0x0, // nameLanguageTag
        0x0,
        'T',
        0x0,
        'E',
        0x0,
        'M',
        0x0,
        'P',
        0x0,
        '1',
        0x0,
        0x0 // sensorName
    };

    std::vector<uint8_t> pdr2{
        0x0,
        0x0,
        0x0,
        0x1,                         // record handle
        0x1,                         // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,     // PDRType
        0x0,
        0x0,                         // recordChangeNumber
        56,
        0,                           // dataLength
        0,
        0,                           // PLDMTerminusHandle
        0x1,
        0x0,                         // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                           // entityType=Power Supply(120)
        1,
        0,                           // entityInstanceNumber
        0x1,
        0x0,                         // containerID=1
        PLDM_NO_INIT,                // sensorInit
        true,                        // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,  // baseUint(2)=degrees C
        0,                           // unitModifier
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
        0, // hysteresis
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
    t1.pdrs.emplace_back(pdr2);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());
    EXPECT_EQ(1, t1.numericSensors.size());
}

TEST_F(TerminusTest, parseNumericSensorPdrTest)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                         // record handle
        0x1,                         // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,     // PDRType
        0x0,
        0x0,                         // recordChangeNumber
        56,
        0,                           // dataLength
        0,
        0,                           // PLDMTerminusHandle
        0x1,
        0x0,                         // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                           // entityType=Power Supply(120)
        1,
        0,                           // entityInstanceNumber
        0x1,
        0x0,                         // containerID=1
        PLDM_NO_INIT,                // sensorInit
        false,                       // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,  // baseUint(2)=degrees C
        0,                           // unitModifier
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
        3, // hysteresis = 3
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
        50,                            // nominalValue = 50
        60,                            // normalMax = 60
        40,                            // normalMin = 40
        70,                            // warningHigh = 70
        30,                            // warningLow = 30
        80,                            // criticalHigh = 80
        20,                            // criticalLow = 20
        90,                            // fatalHigh = 90
        10                             // fatalLow = 10
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());

    auto numericSensorPdrs = t1.numericSensorPdrs[0];
    EXPECT_EQ(1, numericSensorPdrs->sensor_id);
    EXPECT_EQ(PLDM_SENSOR_DATA_SIZE_UINT8, numericSensorPdrs->sensor_data_size);
    EXPECT_EQ(PLDM_ENTITY_POWER_SUPPLY, numericSensorPdrs->entity_type);
    EXPECT_EQ(2, numericSensorPdrs->base_unit);
    EXPECT_EQ(0.0, numericSensorPdrs->offset);
    EXPECT_EQ(3, numericSensorPdrs->hysteresis.value_u8);
    EXPECT_EQ(1.0, numericSensorPdrs->update_interval);
    EXPECT_EQ(255, numericSensorPdrs->max_readable.value_u8);
    EXPECT_EQ(0, numericSensorPdrs->min_readable.value_u8);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_UINT8,
              numericSensorPdrs->range_field_format);
    EXPECT_EQ(0, numericSensorPdrs->range_field_support.byte);
    EXPECT_EQ(50, numericSensorPdrs->nominal_value.value_u8);
    EXPECT_EQ(60, numericSensorPdrs->normal_max.value_u8);
    EXPECT_EQ(40, numericSensorPdrs->normal_min.value_u8);
    EXPECT_EQ(70, numericSensorPdrs->warning_high.value_u8);
    EXPECT_EQ(30, numericSensorPdrs->warning_low.value_u8);
    EXPECT_EQ(80, numericSensorPdrs->critical_high.value_u8);
    EXPECT_EQ(20, numericSensorPdrs->critical_low.value_u8);
    EXPECT_EQ(90, numericSensorPdrs->fatal_high.value_u8);
    EXPECT_EQ(10, numericSensorPdrs->fatal_low.value_u8);
}

TEST_F(TerminusTest, parseNumericSensorPdrSint8Test)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                           // record handle
        0x1,                           // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,       // PDRType
        0x0,
        0x0,                           // recordChangeNumber
        56,
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
        0,                             // unitModifier
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
        3, // hysteresis = 3
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
        0x64,                          // maxReadable = 100
        0x9c,                          // minReadable = -100
        PLDM_RANGE_FIELD_FORMAT_SINT8, // rangeFieldFormat
        0,                             // rangeFieldsupport
        0,                             // nominalValue = 0
        5,                             // normalMax = 5
        0xfb,                          // normalMin = -5
        10,                            // warningHigh = 10
        0xf6,                          // warningLow = -10
        20,                            // criticalHigh = 20
        0xec,                          // criticalLow = -20
        30,                            // fatalHigh = 30
        0xe2                           // fatalLow = -30
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());

    auto numericSensorPdrs = t1.numericSensorPdrs[0];
    EXPECT_EQ(1, numericSensorPdrs->sensor_id);
    EXPECT_EQ(PLDM_SENSOR_DATA_SIZE_SINT8, numericSensorPdrs->sensor_data_size);
    EXPECT_EQ(PLDM_ENTITY_POWER_SUPPLY, numericSensorPdrs->entity_type);
    EXPECT_EQ(2, numericSensorPdrs->base_unit);
    EXPECT_EQ(0.0, numericSensorPdrs->offset);
    EXPECT_EQ(3, numericSensorPdrs->hysteresis.value_s8);
    EXPECT_EQ(1.0, numericSensorPdrs->update_interval);
    EXPECT_EQ(100, numericSensorPdrs->max_readable.value_s8);
    EXPECT_EQ(-100, numericSensorPdrs->min_readable.value_s8);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_SINT8,
              numericSensorPdrs->range_field_format);
    EXPECT_EQ(0, numericSensorPdrs->range_field_support.byte);
    EXPECT_EQ(0, numericSensorPdrs->nominal_value.value_s8);
    EXPECT_EQ(5, numericSensorPdrs->normal_max.value_s8);
    EXPECT_EQ(-5, numericSensorPdrs->normal_min.value_s8);
    EXPECT_EQ(10, numericSensorPdrs->warning_high.value_s8);
    EXPECT_EQ(-10, numericSensorPdrs->warning_low.value_s8);
    EXPECT_EQ(20, numericSensorPdrs->critical_high.value_s8);
    EXPECT_EQ(-20, numericSensorPdrs->critical_low.value_s8);
    EXPECT_EQ(30, numericSensorPdrs->fatal_high.value_s8);
    EXPECT_EQ(-30, numericSensorPdrs->fatal_low.value_s8);
}

TEST_F(TerminusTest, parseNumericSensorPdrUint16Test)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                          // record handle
        0x1,                          // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,      // PDRType
        0x0,
        0x0,                          // recordChangeNumber
        56,
        0,                            // dataLength
        0,
        0,                            // PLDMTerminusHandle
        0x1,
        0x0,                          // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                            // entityType=Power Supply(120)
        1,
        0,                            // entityInstanceNumber
        0x1,
        0x0,                          // containerID=1
        PLDM_NO_INIT,                 // sensorInit
        false,                        // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,   // baseUint(2)=degrees C
        0,                            // unitModifier
        0,                            // rateUnit
        0,                            // baseOEMUnitHandle
        0,                            // auxUnit
        0,                            // auxUnitModifier
        0,                            // auxRateUnit
        0,                            // rel
        0,                            // auxOEMUnitHandle
        true,                         // isLinear
        PLDM_SENSOR_DATA_SIZE_UINT16, // sensorDataSize
        0,
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
        3,
        0, // hysteresis = 3
        0, // supportedThresholds
        0, // thresholdAndHysteresisVolatility
        0,
        0,
        0x80,
        0x3f, // stateTransistionInterval=1.0
        0,
        0,
        0x80,
        0x3f,                           // updateInverval=1.0
        0,
        0x10,                           // maxReadable = 4096
        0,
        0,                              // minReadable = 0
        PLDM_RANGE_FIELD_FORMAT_UINT16, // rangeFieldFormat
        0,                              // rangeFieldsupport
        0x88,
        0x13,                           // nominalValue = 5,000
        0x70,
        0x17,                           // normalMax = 6,000
        0xa0,
        0x0f,                           // normalMin = 4,000
        0x58,
        0x1b,                           // warningHigh = 7,000
        0xb8,
        0x0b,                           // warningLow = 3,000
        0x40,
        0x1f,                           // criticalHigh = 8,000
        0xd0,
        0x07,                           // criticalLow = 2,000
        0x28,
        0x23,                           // fatalHigh = 9,000
        0xe8,
        0x03                            // fatalLow = 1,000
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());

    auto numericSensorPdrs = t1.numericSensorPdrs[0];
    EXPECT_EQ(1, numericSensorPdrs->sensor_id);
    EXPECT_EQ(PLDM_SENSOR_DATA_SIZE_UINT16,
              numericSensorPdrs->sensor_data_size);
    EXPECT_EQ(PLDM_ENTITY_POWER_SUPPLY, numericSensorPdrs->entity_type);
    EXPECT_EQ(2, numericSensorPdrs->base_unit);
    EXPECT_EQ(0.0, numericSensorPdrs->offset);
    EXPECT_EQ(3, numericSensorPdrs->hysteresis.value_u16);
    EXPECT_EQ(1.0, numericSensorPdrs->update_interval);
    EXPECT_EQ(4096, numericSensorPdrs->max_readable.value_u16);
    EXPECT_EQ(0, numericSensorPdrs->min_readable.value_u16);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_UINT16,
              numericSensorPdrs->range_field_format);
    EXPECT_EQ(0, numericSensorPdrs->range_field_support.byte);
    EXPECT_EQ(5000, numericSensorPdrs->nominal_value.value_u16);
    EXPECT_EQ(6000, numericSensorPdrs->normal_max.value_u16);
    EXPECT_EQ(4000, numericSensorPdrs->normal_min.value_u16);
    EXPECT_EQ(7000, numericSensorPdrs->warning_high.value_u16);
    EXPECT_EQ(3000, numericSensorPdrs->warning_low.value_u16);
    EXPECT_EQ(8000, numericSensorPdrs->critical_high.value_u16);
    EXPECT_EQ(2000, numericSensorPdrs->critical_low.value_u16);
    EXPECT_EQ(9000, numericSensorPdrs->fatal_high.value_u16);
    EXPECT_EQ(1000, numericSensorPdrs->fatal_low.value_u16);
}

TEST_F(TerminusTest, parseNumericSensorPdrSint16Test)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                          // record handle
        0x1,                          // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,      // PDRType
        0x0,
        0x0,                          // recordChangeNumber
        56,
        0,                            // dataLength
        0,
        0,                            // PLDMTerminusHandle
        0x1,
        0x0,                          // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                            // entityType=Power Supply(120)
        1,
        0,                            // entityInstanceNumber
        0x1,
        0x0,                          // containerID=1
        PLDM_NO_INIT,                 // sensorInit
        false,                        // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,   // baseUint(2)=degrees C
        0,                            // unitModifier
        0,                            // rateUnit
        0,                            // baseOEMUnitHandle
        0,                            // auxUnit
        0,                            // auxUnitModifier
        0,                            // auxRateUnit
        0,                            // rel
        0,                            // auxOEMUnitHandle
        true,                         // isLinear
        PLDM_SENSOR_DATA_SIZE_SINT16, // sensorDataSize
        0,
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
        3,
        0, // hysteresis
        0, // supportedThresholds
        0, // thresholdAndHysteresisVolatility
        0,
        0,
        0x80,
        0x3f, // stateTransistionInterval=1.0
        0,
        0,
        0x80,
        0x3f,                           // updateInverval=1.0
        0xe8,
        0x03,                           // maxReadable = 1000
        0x18,
        0xfc,                           // minReadable = -1000
        PLDM_RANGE_FIELD_FORMAT_SINT16, // rangeFieldFormat
        0,                              // rangeFieldsupport
        0,
        0,                              // nominalValue = 0
        0xf4,
        0x01,                           // normalMax = 500
        0x0c,
        0xfe,                           // normalMin = -500
        0xe8,
        0x03,                           // warningHigh = 1,000
        0x18,
        0xfc,                           // warningLow = -1,000
        0xd0,
        0x07,                           // criticalHigh = 2,000
        0x30,
        0xf8,                           // criticalLow = -2,000
        0xb8,
        0x0b,                           // fatalHigh = 3,000
        0x48,
        0xf4                            // fatalLow = -3,000
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());

    auto numericSensorPdrs = t1.numericSensorPdrs[0];
    EXPECT_EQ(1, numericSensorPdrs->sensor_id);
    EXPECT_EQ(PLDM_SENSOR_DATA_SIZE_SINT16,
              numericSensorPdrs->sensor_data_size);
    EXPECT_EQ(PLDM_ENTITY_POWER_SUPPLY, numericSensorPdrs->entity_type);
    EXPECT_EQ(2, numericSensorPdrs->base_unit);
    EXPECT_EQ(0.0, numericSensorPdrs->offset);
    EXPECT_EQ(3, numericSensorPdrs->hysteresis.value_s16);
    EXPECT_EQ(1.0, numericSensorPdrs->update_interval);
    EXPECT_EQ(1000, numericSensorPdrs->max_readable.value_s16);
    EXPECT_EQ(-1000, numericSensorPdrs->min_readable.value_s16);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_SINT16,
              numericSensorPdrs->range_field_format);
    EXPECT_EQ(0, numericSensorPdrs->range_field_support.byte);
    EXPECT_EQ(0, numericSensorPdrs->nominal_value.value_s16);
    EXPECT_EQ(500, numericSensorPdrs->normal_max.value_s16);
    EXPECT_EQ(-500, numericSensorPdrs->normal_min.value_s16);
    EXPECT_EQ(1000, numericSensorPdrs->warning_high.value_s16);
    EXPECT_EQ(-1000, numericSensorPdrs->warning_low.value_s16);
    EXPECT_EQ(2000, numericSensorPdrs->critical_high.value_s16);
    EXPECT_EQ(-2000, numericSensorPdrs->critical_low.value_s16);
    EXPECT_EQ(3000, numericSensorPdrs->fatal_high.value_s16);
    EXPECT_EQ(-3000, numericSensorPdrs->fatal_low.value_s16);
}

TEST_F(TerminusTest, parseNumericSensorPdrUint32Test)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                          // record handle
        0x1,                          // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,      // PDRType
        0x0,
        0x0,                          // recordChangeNumber
        56,
        0,                            // dataLength
        0,
        0,                            // PLDMTerminusHandle
        0x1,
        0x0,                          // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                            // entityType=Power Supply(120)
        1,
        0,                            // entityInstanceNumber
        0x1,
        0x0,                          // containerID=1
        PLDM_NO_INIT,                 // sensorInit
        false,                        // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,   // baseUint(2)=degrees C
        0,                            // unitModifier
        0,                            // rateUnit
        0,                            // baseOEMUnitHandle
        0,                            // auxUnit
        0,                            // auxUnitModifier
        0,                            // auxRateUnit
        0,                            // rel
        0,                            // auxOEMUnitHandle
        true,                         // isLinear
        PLDM_SENSOR_DATA_SIZE_UINT32, // sensorDataSize
        0,
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
        3,
        0,
        0,
        0, // hysteresis
        0, // supportedThresholds
        0, // thresholdAndHysteresisVolatility
        0,
        0,
        0x80,
        0x3f, // stateTransistionInterval=1.0
        0,
        0,
        0x80,
        0x3f, // updateInverval=1.0
        0,
        0x10,
        0,
        0, // maxReadable = 4096
        0,
        0,
        0,
        0,                              // minReadable = 0
        PLDM_RANGE_FIELD_FORMAT_UINT32, // rangeFieldFormat
        0,                              // rangeFieldsupport
        0x40,
        0x4b,
        0x4c,
        0x00, // nominalValue = 5,000,000
        0x80,
        0x8d,
        0x5b,
        0x00, // normalMax = 6,000,000
        0x00,
        0x09,
        0x3d,
        0x00, // normalMin = 4,000,000
        0xc0,
        0xcf,
        0x6a,
        0x00, // warningHigh = 7,000,000
        0xc0,
        0xc6,
        0x2d,
        0x00, // warningLow = 3,000,000
        0x00,
        0x12,
        0x7a,
        0x00, // criticalHigh = 8,000,000
        0x80,
        0x84,
        0x1e,
        0x00, // criticalLow = 2,000,000
        0x40,
        0x54,
        0x89,
        0x00, // fatalHigh = 9,000,000
        0x40,
        0x42,
        0x0f,
        0x00 // fatalLow = 1,000,000
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());

    auto numericSensorPdrs = t1.numericSensorPdrs[0];
    EXPECT_EQ(1, numericSensorPdrs->sensor_id);
    EXPECT_EQ(PLDM_SENSOR_DATA_SIZE_UINT32,
              numericSensorPdrs->sensor_data_size);
    EXPECT_EQ(PLDM_ENTITY_POWER_SUPPLY, numericSensorPdrs->entity_type);
    EXPECT_EQ(2, numericSensorPdrs->base_unit);
    EXPECT_EQ(0.0, numericSensorPdrs->offset);
    EXPECT_EQ(3, numericSensorPdrs->hysteresis.value_u32);
    EXPECT_EQ(1.0, numericSensorPdrs->update_interval);
    EXPECT_EQ(4096, numericSensorPdrs->max_readable.value_u32);
    EXPECT_EQ(0, numericSensorPdrs->min_readable.value_u32);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_UINT32,
              numericSensorPdrs->range_field_format);
    EXPECT_EQ(0, numericSensorPdrs->range_field_support.byte);
    EXPECT_EQ(5000000, numericSensorPdrs->nominal_value.value_u32);
    EXPECT_EQ(6000000, numericSensorPdrs->normal_max.value_u32);
    EXPECT_EQ(4000000, numericSensorPdrs->normal_min.value_u32);
    EXPECT_EQ(7000000, numericSensorPdrs->warning_high.value_u32);
    EXPECT_EQ(3000000, numericSensorPdrs->warning_low.value_u32);
    EXPECT_EQ(8000000, numericSensorPdrs->critical_high.value_u32);
    EXPECT_EQ(2000000, numericSensorPdrs->critical_low.value_u32);
    EXPECT_EQ(9000000, numericSensorPdrs->fatal_high.value_u32);
    EXPECT_EQ(1000000, numericSensorPdrs->fatal_low.value_u32);
}

TEST_F(TerminusTest, parseNumericSensorPdrSint32Test)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                          // record handle
        0x1,                          // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,      // PDRType
        0x0,
        0x0,                          // recordChangeNumber
        56,
        0,                            // dataLength
        0,
        0,                            // PLDMTerminusHandle
        0x1,
        0x0,                          // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                            // entityType=Power Supply(120)
        1,
        0,                            // entityInstanceNumber
        0x1,
        0x0,                          // containerID=1
        PLDM_NO_INIT,                 // sensorInit
        false,                        // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,   // baseUint(2)=degrees C
        0,                            // unitModifier
        0,                            // rateUnit
        0,                            // baseOEMUnitHandle
        0,                            // auxUnit
        0,                            // auxUnitModifier
        0,                            // auxRateUnit
        0,                            // rel
        0,                            // auxOEMUnitHandle
        true,                         // isLinear
        PLDM_SENSOR_DATA_SIZE_SINT32, // sensorDataSize
        0,
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
        3,
        0,
        0,
        0, // hysteresis
        0, // supportedThresholds
        0, // thresholdAndHysteresisVolatility
        0,
        0,
        0x80,
        0x3f, // stateTransistionInterval=1.0
        0,
        0,
        0x80,
        0x3f, // updateInverval=1.0
        0xa0,
        0x86,
        0x01,
        0x00, // maxReadable = 100000
        0x60,
        0x79,
        0xfe,
        0xff,                           // minReadable = -10000
        PLDM_RANGE_FIELD_FORMAT_SINT32, // rangeFieldFormat
        0,                              // rangeFieldsupport
        0,
        0,
        0,
        0, // nominalValue = 0
        0x20,
        0xa1,
        0x07,
        0x00, // normalMax = 500,000
        0xe0,
        0x5e,
        0xf8,
        0xff, // normalMin = -500,000
        0x40,
        0x42,
        0x0f,
        0x00, // warningHigh = 1,000,000
        0xc0,
        0xbd,
        0xf0,
        0xff, // warningLow = -1,000,000
        0x80,
        0x84,
        0x1e,
        0x00, // criticalHigh = 2,000,000
        0x80,
        0x7b,
        0xe1,
        0xff, // criticalLow = -2,000,000
        0xc0,
        0xc6,
        0x2d,
        0x00, // fatalHigh = 3,000,000
        0x40,
        0x39,
        0xd2,
        0xff // fatalLow = -3,000,000
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());

    auto numericSensorPdrs = t1.numericSensorPdrs[0];
    EXPECT_EQ(1, numericSensorPdrs->sensor_id);
    EXPECT_EQ(PLDM_SENSOR_DATA_SIZE_SINT32,
              numericSensorPdrs->sensor_data_size);
    EXPECT_EQ(PLDM_ENTITY_POWER_SUPPLY, numericSensorPdrs->entity_type);
    EXPECT_EQ(2, numericSensorPdrs->base_unit);
    EXPECT_EQ(0.0, numericSensorPdrs->offset);
    EXPECT_EQ(3, numericSensorPdrs->hysteresis.value_s32);
    EXPECT_EQ(1.0, numericSensorPdrs->update_interval);
    EXPECT_EQ(100000, numericSensorPdrs->max_readable.value_s32);
    EXPECT_EQ(-100000, numericSensorPdrs->min_readable.value_s32);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_SINT32,
              numericSensorPdrs->range_field_format);
    EXPECT_EQ(0, numericSensorPdrs->range_field_support.byte);
    EXPECT_EQ(0, numericSensorPdrs->nominal_value.value_s32);
    EXPECT_EQ(500000, numericSensorPdrs->normal_max.value_s32);
    EXPECT_EQ(-500000, numericSensorPdrs->normal_min.value_s32);
    EXPECT_EQ(1000000, numericSensorPdrs->warning_high.value_s32);
    EXPECT_EQ(-1000000, numericSensorPdrs->warning_low.value_s32);
    EXPECT_EQ(2000000, numericSensorPdrs->critical_high.value_s32);
    EXPECT_EQ(-2000000, numericSensorPdrs->critical_low.value_s32);
    EXPECT_EQ(3000000, numericSensorPdrs->fatal_high.value_s32);
    EXPECT_EQ(-3000000, numericSensorPdrs->fatal_low.value_s32);
}

TEST_F(TerminusTest, parseNumericSensorPdrReal32Test)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                          // record handle
        0x1,                          // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,      // PDRType
        0x0,
        0x0,                          // recordChangeNumber
        56,
        0,                            // dataLength
        0,
        0,                            // PLDMTerminusHandle
        0x1,
        0x0,                          // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                            // entityType=Power Supply(120)
        1,
        0,                            // entityInstanceNumber
        0x1,
        0x0,                          // containerID=1
        PLDM_NO_INIT,                 // sensorInit
        false,                        // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,   // baseUint(2)=degrees C
        0,                            // unitModifier
        0,                            // rateUnit
        0,                            // baseOEMUnitHandle
        0,                            // auxUnit
        0,                            // auxUnitModifier
        0,                            // auxRateUnit
        0,                            // rel
        0,                            // auxOEMUnitHandle
        true,                         // isLinear
        PLDM_SENSOR_DATA_SIZE_SINT32, // sensorDataSize
        0,
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
        3,
        0,
        0,
        0, // hysteresis
        0, // supportedThresholds
        0, // thresholdAndHysteresisVolatility
        0,
        0,
        0x80,
        0x3f, // stateTransistionInterval=1.0
        0,
        0,
        0x80,
        0x3f, // updateInverval=1.0
        0xa0,
        0x86,
        0x01,
        0x00, // maxReadable = 100000
        0x60,
        0x79,
        0xfe,
        0xff,                           // minReadable = -10000
        PLDM_RANGE_FIELD_FORMAT_REAL32, // rangeFieldFormat
        0,                              // rangeFieldsupport
        0,
        0,
        0,
        0, // nominalValue = 0.0
        0x33,
        0x33,
        0x48,
        0x42, // normalMax = 50.05
        0x33,
        0x33,
        0x48,
        0xc2, // normalMin = -50.05
        0x83,
        0x00,
        0xc8,
        0x42, // warningHigh = 100.001
        0x83,
        0x00,
        0xc8,
        0xc2, // warningLow = -100.001
        0x83,
        0x00,
        0x48,
        0x43, // criticalHigh = 200.002
        0x83,
        0x00,
        0x48,
        0xc3, // criticalLow = -200.002
        0x62,
        0x00,
        0x96,
        0x43, // fatalHigh = 300.003
        0x62,
        0x00,
        0x96,
        0xc3 // fatalLow = -300.003
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());

    auto numericSensorPdrs = t1.numericSensorPdrs[0];
    EXPECT_EQ(1, numericSensorPdrs->sensor_id);
    EXPECT_EQ(PLDM_SENSOR_DATA_SIZE_SINT32,
              numericSensorPdrs->sensor_data_size);
    EXPECT_EQ(PLDM_ENTITY_POWER_SUPPLY, numericSensorPdrs->entity_type);
    EXPECT_EQ(2, numericSensorPdrs->base_unit);
    EXPECT_EQ(0.0, numericSensorPdrs->offset);
    EXPECT_EQ(3, numericSensorPdrs->hysteresis.value_s32);
    EXPECT_EQ(1.0, numericSensorPdrs->update_interval);
    EXPECT_EQ(100000, numericSensorPdrs->max_readable.value_s32);
    EXPECT_EQ(-100000, numericSensorPdrs->min_readable.value_s32);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_REAL32,
              numericSensorPdrs->range_field_format);
    EXPECT_FLOAT_EQ(0, numericSensorPdrs->range_field_support.byte);
    EXPECT_FLOAT_EQ(0, numericSensorPdrs->nominal_value.value_f32);
    EXPECT_FLOAT_EQ(50.05f, numericSensorPdrs->normal_max.value_f32);
    EXPECT_FLOAT_EQ(-50.05f, numericSensorPdrs->normal_min.value_f32);
    EXPECT_FLOAT_EQ(100.001f, numericSensorPdrs->warning_high.value_f32);
    EXPECT_FLOAT_EQ(-100.001f, numericSensorPdrs->warning_low.value_f32);
    EXPECT_FLOAT_EQ(200.002f, numericSensorPdrs->critical_high.value_f32);
    EXPECT_FLOAT_EQ(-200.002f, numericSensorPdrs->critical_low.value_f32);
    EXPECT_FLOAT_EQ(300.003f, numericSensorPdrs->fatal_high.value_f32);
    EXPECT_FLOAT_EQ(-300.003f, numericSensorPdrs->fatal_low.value_f32);
}

TEST_F(TerminusTest, parseNumericSensorPDRInvalidSizeTest)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    // A corrupted PDR. The data after plusTolerance missed.
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                         // record handle
        0x1,                         // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,     // PDRType
        0x0,
        0x0,                         // recordChangeNumber
        34,
        0,                           // dataLength
        0,
        0,                           // PLDMTerminusHandle
        0x1,
        0x0,                         // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                           // entityType=Power Supply(120)
        1,
        0,                           // entityInstanceNumber
        0x1,
        0x0,                         // containerID=1
        PLDM_NO_INIT,                // sensorInit
        false,                       // sensorAuxiliaryNamesPDR
        2,                           // baseUint(2)=degrees C
        0,                           // unitModifier
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
        0,
        0, // resolution
        0,
        0,
        0,
        0, // offset
        0,
        0, // accuracy
        0  // plusTolerance
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(0, t1.numericSensorPdrs.size());
}

TEST_F(TerminusTest, platformManagerInitTerminusCoverage)
{
    pldm::UUID uuid{"f72d6f90-5675-11ed-9b6a-0242ac120002"};
    pldm::MctpInfos mctpInfos{pldm::MctpInfo(
        12, uuid, "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1,
        std::nullopt, "xyz.openbmc_project.MCTP.Endpoint.BindingTypes.PCIe",
        std::nullopt)};

    setupResponsesForDiscoverTerminus();
    terminusManager.discoverMctpTerminus(mctpInfos);
    ASSERT_EQ(termini.size(), 1u);

    auto terminus = terminusManager.getTerminus(uuid);
    ASSERT_NE(terminus, nullptr);

    setupResponsesForInitTerminus();
    auto rc = stdexec::sync_wait(platformManager.initTerminus());
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(std::get<0>(*rc), PLDM_SUCCESS);
    EXPECT_EQ(terminus->numericSensorPdrs.size(), 1u);
}

TEST(Entity, getClosestInventoriesCoverage)
{
    std::vector<std::string> inventories{
        "/xyz/openbmc_project/inventory/system/chassis"};
    std::vector<std::string> containerInventories{
        "/xyz/openbmc_project/inventory/system"};
    Entity entity(inventories, containerInventories);
    EXPECT_EQ(entity.getInventories().size(), 1u);
    EXPECT_EQ(entity.getClosestInventories().size(), 1u);
    EXPECT_EQ(entity.getClosestInventories().front(),
              "/xyz/openbmc_project/inventory/system/chassis");

    std::vector<std::string> emptyInventories{};
    Entity fallbackEntity(emptyInventories, containerInventories);
    EXPECT_EQ(fallbackEntity.getInventories().size(), 0u);
    EXPECT_EQ(fallbackEntity.getClosestInventories().size(), 1u);
    EXPECT_EQ(fallbackEntity.getClosestInventories().front(),
              "/xyz/openbmc_project/inventory/system");
}

TEST(PlatformMcErrors, invalidArgumentCoverage)
{
    ::errors::InvalidArgument ex{"TelemetryEndpoint"};
    EXPECT_STREQ(ex.name(), "xyz.openbmc_project.Common.Error.InvalidArgument");
    EXPECT_STREQ(ex.description(), "Out of range");
    EXPECT_EQ(ex.propertyName, "TelemetryEndpoint");
    EXPECT_FALSE(std::string(ex.what()).empty());
    EXPECT_GT(ex.get_errno(), 0);

    ::errors::InvalidArgument exWithInfo{"TelemetryEndpoint",
                                         "must be non-empty"};
    EXPECT_EQ(exWithInfo.propertyName, "TelemetryEndpoint");
    EXPECT_NE(std::string(exWithInfo.what()).find("must be non-empty"),
              std::string::npos);
}

TEST_F(TerminusTest, parseAdditionalPdrCoverage)
{
    std::string uuid("00000000-0000-0000-0000-0000000000AA");
    Terminus terminus(0x0A, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setTerminusName("TerminusA");

    auto effecterAuxPdr =
        makeAuxNamePdr(0x88, PLDM_EFFECTER_AUXILIARY_NAMES_PDR);
    auto numericEffecterPdr = makeNumericEffecterPdr(0x88, true);
    auto entityAssociationPdr = makeEntityAssociationPdr();
    auto oemPdr = makeOemPdr();
    std::vector<uint8_t> unknownPdr(sizeof(pldm_pdr_hdr), 0);
    auto* unknownHdr = reinterpret_cast<pldm_pdr_hdr*>(unknownPdr.data());
    unknownHdr->version = 1;
    unknownHdr->type = 0xFF;

    terminus.pdrs.emplace_back(effecterAuxPdr);
    terminus.pdrs.emplace_back(numericEffecterPdr);
    terminus.pdrs.emplace_back(entityAssociationPdr);
    terminus.pdrs.emplace_back(oemPdr);
    terminus.pdrs.emplace_back(unknownPdr);

    auto rc = terminus.parsePDRs();
    EXPECT_FALSE(rc);
    ASSERT_EQ(1u, terminus.numericEffecters.size());
    EXPECT_NE(nullptr, terminus.getEffecterAuxiliaryNames(0x88));
    ASSERT_EQ(1u, terminus.oemPdrs.size());
    EXPECT_EQ(0x1234u, std::get<0>(terminus.oemPdrs[0]));
    EXPECT_EQ(1u, std::get<1>(terminus.oemPdrs[0]));
    EXPECT_EQ(4u, std::get<2>(terminus.oemPdrs[0]).size());
    EXPECT_NE(std::string::npos,
              terminus.numericEffecters[0]->path.find("TerminusA_A"));
}

TEST_F(TerminusTest, interfaceAddedAndOnlineOfflineCoverage)
{
    std::string uuid("00000000-0000-0000-0000-0000000000BB");
    Terminus terminus(0x0B, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    // Cover checkNsmDeviceInventory true/false branches.
    EXPECT_TRUE(terminus.checkNsmDeviceInventory(uuid));
    EXPECT_FALSE(terminus.checkNsmDeviceInventory(
        "00000000-0000-0000-0000-0000000000CC"));

    // Cover interfaceAdded fast path and refresh trigger path.
    pldm::dbus::PropertyMap properties;
    pldm::dbus::InterfaceMap interfaces;
    interfaces.emplace(std::string(overallSystemInterface), properties);
    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call("org.test",
                                      "/xyz/openbmc_project/inventory/test",
                                      "org.test.Interface", "Method");
    msg.append(
        sdbusplus::message::object_path("/xyz/openbmc_project/inventory/test"),
        interfaces);
    sealAndRewind(msg);
    EXPECT_NO_THROW(terminus.interfaceAdded(msg));

    terminus.initalized = true;
    EXPECT_NO_THROW(terminus.interfaceAdded(msg));

    auto numericSensorPdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    numericSensorPdr->sensor_id = 0x31;
    numericSensorPdr->entity_type = PLDM_ENTITY_SYS_BOARD;
    numericSensorPdr->entity_instance_num = 1;
    numericSensorPdr->container_id = 1;
    numericSensorPdr->base_unit = PLDM_SENSOR_UNIT_DEGRESS_C;
    numericSensorPdr->sensor_data_size = PLDM_SENSOR_DATA_SIZE_UINT8;
    numericSensorPdr->max_readable.value_u8 = 100;
    numericSensorPdr->min_readable.value_u8 = 0;
    numericSensorPdr->hysteresis.value_u8 = 1;
    numericSensorPdr->supported_thresholds.byte = 0;
    numericSensorPdr->range_field_support.byte = 0;
    numericSensorPdr->range_field_format = PLDM_RANGE_FIELD_FORMAT_UINT8;
    numericSensorPdr->resolution = 1.0f;
    numericSensorPdr->offset = 0.0f;
    numericSensorPdr->update_interval = 1.0f;

    auto numericEffecterPdr =
        std::make_shared<pldm_numeric_effecter_value_pdr>();
    numericEffecterPdr->effecter_id = 0x41;
    numericEffecterPdr->entity_type = PLDM_ENTITY_SYS_BOARD;
    numericEffecterPdr->entity_instance = 1;
    numericEffecterPdr->container_id = 1;
    numericEffecterPdr->base_unit = PLDM_SENSOR_UNIT_NONE;
    numericEffecterPdr->effecter_data_size = PLDM_EFFECTER_DATA_SIZE_UINT8;
    numericEffecterPdr->max_settable.value_u8 = 100;
    numericEffecterPdr->min_settable.value_u8 = 0;
    numericEffecterPdr->range_field_format = PLDM_RANGE_FIELD_FORMAT_UINT8;
    numericEffecterPdr->range_field_support.byte = 0x1F;
    numericEffecterPdr->nominal_value.value_u8 = 50;
    numericEffecterPdr->normal_max.value_u8 = 60;
    numericEffecterPdr->normal_min.value_u8 = 40;
    numericEffecterPdr->rated_max.value_u8 = 70;
    numericEffecterPdr->rated_min.value_u8 = 30;

    std::string sensorName{"offline_sensor"};
    std::string effecterName{"offline_effecter"};
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis0"};

    auto numericSensor = std::make_shared<NumericSensor>(
        terminus.getTid(), false, numericSensorPdr, sensorName, associationPath,
        nullptr);
    auto numericEffecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, numericEffecterPdr, effecterName,
        associationPath, terminusManager);
    numericEffecter->needUpdate = false;

    StateSetData healthStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_HEALTHSTATE),
                        PossibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                       PLDM_STATESET_HEALTH_STATE_CRITICAL});
    StateSetInfo stateSensorInfo =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
                        std::vector<StateSetData>{healthStateData});

    StateSetData bootRequestData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_BOOT_REQUEST),
                        PossibleStates{PLDM_STATESET_BOOT_REQUEST_NORMAL,
                                       PLDM_STATESET_BOOT_REQUEST_REQUESTED});
    StateSetInfo stateEffecterInfo =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
                        std::vector<StateSetData>{bootRequestData});

    auto stateSensor = std::make_shared<StateSensor>(
        terminus.getTid(), false, 0x32, stateSensorInfo, nullptr,
        associationPath, nullptr);
    auto stateEffecter = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x42, stateEffecterInfo, nullptr,
        associationPath, terminusManager);

    terminus.numericSensors.emplace_back(numericSensor);
    terminus.numericEffecters.emplace_back(numericEffecter);
    terminus.stateSensors.emplace_back(stateSensor);
    terminus.stateEffecters.emplace_back(stateEffecter);

    terminus.setOffline();
    EXPECT_FALSE(terminus.resumed);
    EXPECT_TRUE(std::isnan(numericSensor->getReading()));

    terminus.setOnline();
    EXPECT_TRUE(numericEffecter->needUpdate);
}

TEST_F(TerminusTest, asyncDbusMethodCoverageUnderMockUtils)
{
    std::string uuid("00000000-0000-0000-0000-0000000000DD");
    Terminus terminus(0x0D, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    auto checkInvRc = stdexec::sync_wait(terminus.checkDeviceInventory(
        "/xyz/openbmc_project/inventory/system/chassis/chassis0"));
    ASSERT_TRUE(checkInvRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*checkInvRc));

    auto auxNameRc = stdexec::sync_wait(terminus.getSensorAuxNameFromEM(
        0, 0, 0, "/xyz/openbmc_project/inventory"));
    ASSERT_TRUE(auxNameRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*auxNameRc));

    auto inventoryParentRc = stdexec::sync_wait(terminus.getInventoryParent(
        "/xyz/openbmc_project/inventory/system/chassis/chassis0"));
    ASSERT_TRUE(inventoryParentRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*inventoryParentRc));

    auto scanRc = stdexec::sync_wait(terminus.scanInventories());
    ASSERT_TRUE(scanRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*scanRc));

    auto updateAssocRc = stdexec::sync_wait(terminus.updateAssociations());
    ASSERT_TRUE(updateAssocRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateAssocRc));

#ifdef OEM_NVIDIA
    auto portInfoRc = stdexec::sync_wait(
        terminus.getPortInfoFromEM("/xyz/openbmc_project/inventory"));
    ASSERT_TRUE(portInfoRc.has_value());
    EXPECT_EQ(PLDM_FAILED, std::get<0>(*portInfoRc));

    auto switchInfoRc = stdexec::sync_wait(
        terminus.getInfoForNVSwitchFromEM("/xyz/openbmc_project/inventory"));
    ASSERT_TRUE(switchInfoRc.has_value());
    EXPECT_EQ(PLDM_FAILED, std::get<0>(*switchInfoRc));

    auto sensorEventRc = stdexec::sync_wait(
        terminus.getSensorEventInfoFromEM("/xyz/openbmc_project/inventory"));
    ASSERT_TRUE(sensorEventRc.has_value());
    EXPECT_EQ(PLDM_FAILED, std::get<0>(*sensorEventRc));

    EXPECT_EQ(nullptr, terminus.getSensorPortInfo(0xAA));
    EXPECT_EQ(nullptr, terminus.getSensorEventInfo(0xAA));
#endif
}

TEST_F(TerminusTest, nvidiaEnergyCountOemPdrCoverage)
{
    constexpr uint16_t sensorId = 0x91;
    std::string uuid("00000000-0000-0000-0000-0000000000EE");
    Terminus terminus(0x0E, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    auto oemEnergyPdr = makeNvidiaEnergyCountOemPdr(sensorId);
    terminus.pdrs.emplace_back(oemEnergyPdr);

    ASSERT_TRUE(terminus.parsePDRs());
    ASSERT_EQ(1u, terminus.numericSensors.size());

    auto sensor = terminus.numericSensors[0];
    EXPECT_EQ(static_cast<uint8_t>(POLLING_METHOD_INDICATOR_PLDM_TYPE_OEM),
              sensor->getPollingIndicator());
    sensor->updateReading(true, true, 42);
    EXPECT_TRUE(std::isfinite(sensor->getReading()));
}

TEST_F(TerminusTest, parseNumericEffecterPdrCoverageMatrix)
{
    std::string uuid("00000000-0000-0000-0000-0000000000EF");
    Terminus terminus(0x0F, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setTerminusName("TerminusEffecter");

    struct NumericEffecterCase
    {
        uint8_t effecterDataSize;
        uint8_t rangeFieldFormat;
    };
    /* UINT64/SINT64 effecter sizes (enum values 10/11) are rejected by
     * decode_numeric_effecter_pdr_data() in libpldm 0.14.0 because it
     * guards with PLDM_SENSOR_DATA_SIZE_MAX (=7) instead of
     * PLDM_EFFECTER_DATA_SIZE_MAX (=11).  Omit them until that upstream
     * libpldm bug is fixed and the CI Docker image is rebuilt. */
    const std::array<NumericEffecterCase, 7> cases{{
        {PLDM_EFFECTER_DATA_SIZE_UINT8, PLDM_RANGE_FIELD_FORMAT_UINT8},
        {PLDM_EFFECTER_DATA_SIZE_SINT8, PLDM_RANGE_FIELD_FORMAT_SINT8},
        {PLDM_EFFECTER_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16},
        {PLDM_EFFECTER_DATA_SIZE_SINT16, PLDM_RANGE_FIELD_FORMAT_SINT16},
        {PLDM_EFFECTER_DATA_SIZE_UINT32, PLDM_RANGE_FIELD_FORMAT_UINT32},
        {PLDM_EFFECTER_DATA_SIZE_SINT32, PLDM_RANGE_FIELD_FORMAT_SINT32},
        {PLDM_EFFECTER_DATA_SIZE_UINT32, PLDM_RANGE_FIELD_FORMAT_REAL32},
    }};

    uint16_t effecterId = 0xA0;
    for (const auto& item : cases)
    {
        terminus.pdrs.emplace_back(makeNumericEffecterPdrVariant(
            effecterId++, item.effecterDataSize, item.rangeFieldFormat));
    }

    terminus.pdrs.emplace_back(
        makeNumericEffecterPdrVariant(effecterId++, 0xFF, 0xFF));

    EXPECT_TRUE(terminus.parsePDRs());
    /* The last PDR (effecterDataSize=0xFF) is invalid and rejected by
     * decode_numeric_effecter_pdr_data(), so only the 7 valid PDRs
     * are stored. */
    EXPECT_EQ(cases.size(), terminus.numericEffecterPdrs.size());
    EXPECT_EQ(cases.size(), terminus.numericEffecters.size());
    EXPECT_NE(std::string::npos,
              terminus.numericEffecters.front()->path.find("TerminusEffecter"));
}

TEST_F(TerminusTest, addStateSensorAndEffecterCoverage)
{
    constexpr uint16_t sensorId = 0x211;
    constexpr uint16_t effecterId = 0x311;
    std::string uuid("00000000-0000-0000-0000-0000000000F0");
    Terminus terminus(0x10, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setTerminusName("TerminusState");

    auto sensorAuxPdr = makeSensorAuxNamePdr(sensorId);
    auto stateSensorPdr = makeStateSensorPdr(
        sensorId, PLDM_ENTITY_SYS_BOARD, PLDM_STATESET_ID_HEALTHSTATE, true);
    auto effecterAuxPdr =
        makeAuxNamePdr(effecterId, PLDM_EFFECTER_AUXILIARY_NAMES_PDR);
    auto stateEffecterPdr = makeStateEffecterPdr(
        effecterId, PLDM_ENTITY_SYS_BOARD, PLDM_STATESET_ID_BOOT_REQUEST);

    terminus.pdrs.emplace_back(sensorAuxPdr);
    terminus.pdrs.emplace_back(stateSensorPdr);
    terminus.pdrs.emplace_back(effecterAuxPdr);
    terminus.pdrs.emplace_back(stateEffecterPdr);

    ASSERT_TRUE(terminus.parsePDRs());
    ASSERT_EQ(1u, terminus.stateSensors.size());
    ASSERT_EQ(1u, terminus.stateEffecters.size());
}

TEST_F(TerminusTest, privateFindInventoryAndPhysicalContextCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000155");
    Terminus terminus(0x55, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis55";
    terminus.setInstance(7);

    const std::string cpu0{
        "/xyz/openbmc_project/inventory/system/chassis/chassis55/cpu0"};
    const std::string cpu1{
        "/xyz/openbmc_project/inventory/system/chassis/chassis55/cpu1"};
    const std::string dimm0{
        "/xyz/openbmc_project/inventory/system/chassis/chassis55/dimm0"};
    const std::string dimm1{
        "/xyz/openbmc_project/inventory/system/chassis/chassis55/dimm1"};

    terminus.inventories.emplace_back(cpu0, PLDM_ENTITY_PROC, 7);
    terminus.inventories.emplace_back(cpu1, PLDM_ENTITY_PROC, 7);
    terminus.inventories.emplace_back(dimm0, PLDM_ENTITY_MEMORY_CONTROLLER, 2);
    terminus.inventories.emplace_back(dimm1, PLDM_ENTITY_MEMORY_CONTROLLER, 2);
    terminus.inventoryParentMap[dimm0] = cpu0;
    terminus.inventoryParentMap[dimm1] = cpu1;

    EntityInfo containerEntity{overallSystemCotainerId, PLDM_ENTITY_PROC, 1};
    terminus.entityAssociations.emplace(
        1, std::make_pair(containerEntity, std::set<EntityInfo>{}));

    auto overallPaths = terminus.findInventory(overallSystemCotainerId, false);
    ASSERT_EQ(1u, overallPaths.size());
    EXPECT_EQ(terminus.systemInventoryPath, overallPaths.front());

    auto unknownContainerPaths =
        terminus.findInventory(static_cast<ContainerID>(0x9FFF), false);
    ASSERT_EQ(1u, unknownContainerPaths.size());
    EXPECT_EQ(terminus.systemInventoryPath, unknownContainerPaths.front());

    EntityInfo cpuEntity{1, PLDM_ENTITY_PROC, 1};
    auto cpuPaths = terminus.findInventory(cpuEntity, false);
    EXPECT_EQ(2u, cpuPaths.size());

    terminus.entities.clear();
    EntityInfo dimmEntity{1, PLDM_ENTITY_MEMORY_CONTROLLER, 2};
    auto dimmPaths = terminus.findInventory(dimmEntity, false);
    EXPECT_EQ(2u, dimmPaths.size());

    terminus.entities.clear();
    terminus.inventoryParentMap.clear();
    auto dimmFallback = terminus.findInventory(dimmEntity, false);
    EXPECT_EQ(1u, dimmFallback.size());
    EXPECT_EQ(dimm0, dimmFallback.front());

    terminus.entities.clear();
    EntityInfo missingEntity{1, PLDM_ENTITY_NETWORK_CONTROLLER, 99};
    auto closest = terminus.findInventory(missingEntity, true);
    EXPECT_EQ(2u, closest.size());

    terminus.entities.clear();
    auto missingWithoutClosest = terminus.findInventory(missingEntity, false);
    EXPECT_TRUE(missingWithoutClosest.empty());

    EXPECT_EQ(PhysicalContextType::Memory,
              terminus.toPhysicalContextType(PLDM_ENTITY_MEMORY_CONTROLLER));
    EXPECT_EQ(PhysicalContextType::CPU,
              terminus.toPhysicalContextType(PLDM_ENTITY_PROC_MODULE));
    EXPECT_EQ(PhysicalContextType::CPU,
              terminus.toPhysicalContextType(PLDM_ENTITY_PROC_IO_MODULE));
    EXPECT_EQ(PhysicalContextType::VoltageRegulator,
              terminus.toPhysicalContextType(PLDM_ENTITY_DC_DC_CONVERTER));
    EXPECT_EQ(PhysicalContextType::NetworkingDevice,
              terminus.toPhysicalContextType(PLDM_ENTITY_NETWORK_CONTROLLER));
    EXPECT_EQ(PhysicalContextType::SystemBoard,
              terminus.toPhysicalContextType(PLDM_ENTITY_SYS_BOARD));
}

TEST_F(TerminusTest, privateGetterAndUpdateAssociationsCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000156");
    Terminus terminus(0x56, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setTerminusName("TerminusPrivate");
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis56";

    const std::string boardPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis56/board0"};
    terminus.inventories.emplace_back(boardPath, PLDM_ENTITY_SYS_BOARD, 1);
    terminus.inventoryParentMap[boardPath] = terminus.systemInventoryPath;
    EntityInfo containerEntity{overallSystemCotainerId, PLDM_ENTITY_SYS_BOARD,
                               1};
    terminus.entityAssociations.emplace(
        1, std::make_pair(containerEntity, std::set<EntityInfo>{}));

    AuxiliaryNames pdrAuxNames{{{"en", "AuxSensor"}}};
    terminus.sensorAuxiliaryNamesTbl.emplace_back(
        std::make_shared<SensorAuxiliaryNames>(0x52, 1, pdrAuxNames));

    AuxiliaryNames overwriteAuxNames{{{"en", "OverwriteSensor"}}};
    terminus.sensorAuxNameOverwriteTbl[0x53] = std::make_tuple(
        overwriteAuxNames,
        "/xyz/openbmc_project/inventory/system/chassis/chassis53");
    terminus.sensorAuxNameOverwriteTbl[0x54] =
        std::make_tuple(overwriteAuxNames, "/tmp/not_inventory");

#ifdef OEM_NVIDIA
    std::vector<pldm::dbus::PathAssociation> associations{
        {"chassis", "all_states", terminus.systemInventoryPath}};
    terminus.sensorPortInfoOverwriteTbl[0x53] = std::make_tuple(
        PortType::UpstreamPort, std::string("PCIe"), 32000, associations);
    terminus.sensorEventInfoOverwriteTbl[0x53] =
        std::make_shared<pldm::utils::SensorEventInfo>(
            "CPU56", std::unordered_map<std::string, std::string>{
                         {"PLDM_SENSOR_UPPERCRITICAL", "EID56"}});
#endif

    auto overwriteNames = terminus.getSensorAuxiliaryNames(0x53);
    ASSERT_NE(nullptr, overwriteNames);
    EXPECT_TRUE(terminus.getInventoryPath(0x53).has_value());
    EXPECT_FALSE(terminus.getInventoryPath(0x54).has_value());

#ifdef OEM_NVIDIA
    EXPECT_NE(nullptr, terminus.getSensorPortInfo(0x53));
    EXPECT_EQ(nullptr, terminus.getSensorPortInfo(0xFF));
    EXPECT_NE(nullptr, terminus.getSensorEventInfo(0x53));
    EXPECT_EQ(nullptr, terminus.getSensorEventInfo(0xFF));
#endif

    std::string associationPath = terminus.systemInventoryPath;
    auto pdrNoAux = makeNumericSensorValuePdrStruct(0x51);
    auto pdrWithAux = makeNumericSensorValuePdrStruct(0x52);
    auto pdrOverwrite = makeNumericSensorValuePdrStruct(0x53);
    std::string noAuxName{"sensor_no_aux_56"};
    std::string auxName{"sensor_aux_56"};
    std::string overwriteName{"sensor_overwrite_56"};
    auto noAuxSensor =
        std::make_shared<NumericSensor>(terminus.getTid(), false, pdrNoAux,
                                        noAuxName, associationPath, nullptr);
    auto auxSensor =
        std::make_shared<NumericSensor>(terminus.getTid(), false, pdrWithAux,
                                        auxName, associationPath, nullptr);
    auto overwriteSensor = std::make_shared<NumericSensor>(
        terminus.getTid(), false, pdrOverwrite, overwriteName, associationPath,
        nullptr);
    terminus.numericSensors.emplace_back(noAuxSensor);
    terminus.numericSensors.emplace_back(auxSensor);
    terminus.numericSensors.emplace_back(overwriteSensor);

    auto effecterPdr = makeNumericEffecterValuePdrStruct(0x61);
    std::string effecterName{"effecter_56"};
    auto effecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, effecterPdr, effecterName, associationPath,
        terminusManager);
    terminus.numericEffecters.emplace_back(effecter);

    auto stateInfo = makeSimpleStateSetInfo();
    auto stateSensor =
        std::make_shared<StateSensor>(terminus.getTid(), false, 0x62, stateInfo,
                                      nullptr, associationPath, nullptr);
    auto stateEffecter = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x63, stateInfo, nullptr, associationPath,
        terminusManager);
    terminus.stateSensors.emplace_back(stateSensor);
    terminus.stateEffecters.emplace_back(stateEffecter);

    terminusManager.numericSensorsWithoutAuxName = false;
    auto updateRc = stdexec::sync_wait(terminus.updateAssociations());
    ASSERT_TRUE(updateRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateRc));

    EXPECT_EQ(nullptr, noAuxSensor->valueIntf);
    EXPECT_NE(std::string::npos, auxSensor->path.find("TerminusPrivate_"));
    EXPECT_NE(std::string::npos, overwriteSensor->path.find("OverwriteSensor"));
}

#ifdef OEM_NVIDIA
TEST_F(TerminusTest, nvidiaInitTerminusPowerCapAndStorageCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000157");
    Terminus terminus(0x57, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis57"};

    auto numericEffecterPdr = makeNumericEffecterValuePdrStruct(0x710);
    std::string numericEffecterName{"oem_numeric_effecter_710"};
    auto numericEffecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, numericEffecterPdr, numericEffecterName,
        associationPath, terminusManager);
    terminus.numericEffecters.emplace_back(numericEffecter);

    auto stateInfo = makeSingleStateSetInfo(PLDM_ENTITY_SYS_BOARD,
                                            PLDM_STATESET_ID_BOOT_REQUEST);
    auto stateEffecter = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x711, stateInfo, nullptr, associationPath,
        terminusManager);
    terminus.stateEffecters.emplace_back(stateEffecter);

    nvidia::nvidia_oem_effecter_powercap_pdr powerCapPdr{};
    powerCapPdr.terminus_handle = 1;
    powerCapPdr.oem_pdr_type = static_cast<uint8_t>(
        nvidia::NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_EFFECTER_POWERCAP);
    powerCapPdr.oem_effecter_powercap = static_cast<uint8_t>(
        nvidia::OemPowerCapPersistence::OEM_POWERCAP_TDP_NONVOLATILE);
    powerCapPdr.associated_effecterid = 0x710;

    nvidia::nvidia_oem_effecter_storage_pdr storagePdr{};
    storagePdr.terminus_handle = 1;
    storagePdr.oem_pdr_type = static_cast<uint8_t>(
        nvidia::NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_EFFECTER_STORAGE);
    storagePdr.oem_effecter_storage = static_cast<uint8_t>(
        nvidia::OemStorageSecureState::OEM_STORAGE_SECURE_VARIABLE);
    storagePdr.associated_effecterid = 0x711;

    auto truncatedPdrData = structToBytes(powerCapPdr);
    truncatedPdrData.resize(sizeof(nvidia::nvidia_oem_pdr));

    nvidia::nvidia_oem_pdr unknownTypePdr{};
    unknownTypePdr.terminus_handle = 1;
    unknownTypePdr.oem_pdr_type = 0xFF;

    // OemRecordId must match the effecter IDs (0x710/0x711) because
    // processEffecterPowerCapPdr/processEffecterStoragePdr now compare
    // effecter->effecterId against the PLDM OEM PDR header's oemRecordId
    // (not the vendor-specific associated_effecterid).
    terminus.oemPdrs.emplace_back(static_cast<uint32_t>(0xFFFF),
                                  static_cast<OemRecordId>(0x710),
                                  structToBytes(powerCapPdr));
    terminus.oemPdrs.emplace_back(
        nvidia::NvidiaIana, static_cast<OemRecordId>(2), truncatedPdrData);
    terminus.oemPdrs.emplace_back(nvidia::NvidiaIana,
                                  static_cast<OemRecordId>(0x710),
                                  structToBytes(powerCapPdr));
    terminus.oemPdrs.emplace_back(nvidia::NvidiaIana,
                                  static_cast<OemRecordId>(0x711),
                                  structToBytes(storagePdr));
    terminus.oemPdrs.emplace_back(nvidia::NvidiaIana,
                                  static_cast<OemRecordId>(5),
                                  structToBytes(unknownTypePdr));

    nvidia::nvidiaInitTerminus(terminus);

    ASSERT_EQ(1u, numericEffecter->oemIntfs.size());
    auto persistenceIntf =
        std::dynamic_pointer_cast<nvidia::OemPersistenceIntf>(
            numericEffecter->oemIntfs[0]);
    ASSERT_NE(nullptr, persistenceIntf);
    EXPECT_TRUE(persistenceIntf->persistent());

    ASSERT_EQ(1u, stateEffecter->oemIntfs.size());
    auto* storageIntf =
        dynamic_cast<nvidia::OemStorageIntf*>(stateEffecter->oemIntfs[0].get());
    ASSERT_NE(nullptr, storageIntf);
    EXPECT_TRUE(storageIntf->secure());
}

TEST_F(TerminusTest, nvidiaRemoteDebugAndStaticPowerHintCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000158");
    Terminus terminus(0x58, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis58"};

    auto debugEffecterInfo = makeDebugStateSetInfo(PLDM_ENTITY_SYS_BOARD, 6);
    auto debugSensorInfo = makeDebugStateSetInfo(PLDM_ENTITY_SYS_BOARD, 6);
    auto debugStateEffecter = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x720, debugEffecterInfo, nullptr,
        associationPath, terminusManager);
    auto debugStateSensor = std::make_shared<StateSensor>(
        terminus.getTid(), false, 0x721, debugSensorInfo, nullptr,
        associationPath, nullptr);
    terminus.stateEffecters.emplace_back(debugStateEffecter);
    terminus.stateSensors.emplace_back(debugStateSensor);

    auto createNumericEffecter =
        [&](uint16_t effecterId, uint8_t baseUnit, uint8_t minValue,
            uint8_t maxValue, const std::string& effecterName) {
            auto pdr = makeNumericEffecterValuePdrStruct(effecterId);
            pdr->base_unit = baseUnit;
            pdr->min_settable.value_u8 = minValue;
            pdr->max_settable.value_u8 = maxValue;
            std::string name = effecterName;
            auto effecter = std::make_shared<NumericEffecter>(
                terminus.getTid(), false, pdr, name, associationPath,
                terminusManager);
            terminus.numericEffecters.emplace_back(effecter);
            return effecter;
        };

    auto remoteDebugNumericEffecter = createNumericEffecter(
        0x722, PLDM_SENSOR_UNIT_MINUTES, 1, 60, "remote_debug_timeout");
    auto staticPowerTemperatureEffecter = createNumericEffecter(
        0x723, PLDM_SENSOR_UNIT_DEGRESS_C, 10, 80, "static_power_temp");
    auto staticPowerWorkloadEffecter = createNumericEffecter(
        0x724, PLDM_SENSOR_UNIT_NONE, 1, 100, "static_power_workload");
    auto staticPowerClockEffecter = createNumericEffecter(
        0x725, PLDM_SENSOR_UNIT_HERTZ, 20, 200, "static_power_clock");
    auto staticPowerPowerEffecter = createNumericEffecter(
        0x726, PLDM_SENSOR_UNIT_WATTS, 5, 250, "static_power_power");

    nvidia::nvidiaInitTerminus(terminus);

    ASSERT_EQ(1u, debugStateEffecter->oemIntfs.size());
    auto* remoteDebugIntf = dynamic_cast<oem_nvidia::OemRemoteDebugIntf*>(
        debugStateEffecter->oemIntfs[0].get());
    ASSERT_NE(nullptr, remoteDebugIntf);

    debugStateSensor->stateSets[0]->setValue(
        PLDM_STATE_SET_DEBUG_STATE_ENABLED);
    debugStateSensor->stateSets[1]->setValue(
        PLDM_STATE_SET_DEBUG_STATE_DISABLED);
    debugStateSensor->stateSets[2]->setValue(
        PLDM_STATE_SET_DEBUG_STATE_OFFLINE);
    debugStateSensor->stateSets[3]->setValue(0xFE);
    debugStateSensor->stateSets[4]->setValue(
        PLDM_STATE_SET_DEBUG_STATE_ENABLED);
    debugStateSensor->stateSets[5]->setValue(
        PLDM_STATE_SET_DEBUG_STATE_ENABLED);
    for (auto& stateSet : debugStateEffecter->stateSets)
    {
        stateSet->setOpState(EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING);
    }
    debugStateEffecter->stateSets[4]->setOpState(
        EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING);

    EXPECT_EQ(oem_nvidia::DebugState::Enabled, remoteDebugIntf->jtagDebug());
    EXPECT_EQ(oem_nvidia::DebugState::Disabled, remoteDebugIntf->deviceDebug());
    EXPECT_EQ(oem_nvidia::DebugState::Offline,
              remoteDebugIntf->securePrivilegeNonInvasiveDebug());
    EXPECT_EQ(oem_nvidia::DebugState::Unknown,
              remoteDebugIntf->securePrivilegeInvasiveDebug());
    EXPECT_EQ(oem_nvidia::DebugState::Pending,
              remoteDebugIntf->nonInvasiveDebug());
    EXPECT_EQ(oem_nvidia::DebugState::Enabled,
              remoteDebugIntf->invasiveDebug());
    EXPECT_EQ(0, remoteDebugIntf->toCompId(oem_nvidia::DebugPolicy::JtagDebug));
    EXPECT_EQ(255, remoteDebugIntf->toCompId(
                       static_cast<oem_nvidia::DebugPolicy>(0xFF)));
    EXPECT_EQ(oem_nvidia::DebugState::Unknown,
              remoteDebugIntf->toDebugState(0xFF));

    remoteDebugIntf->timeout(17, true);
    (void)remoteDebugIntf->timeout();

    EXPECT_THROW(
        remoteDebugIntf->enable({static_cast<oem_nvidia::DebugPolicy>(0xFF)}),
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument);
    EXPECT_THROW(
        remoteDebugIntf->disable({static_cast<oem_nvidia::DebugPolicy>(0xFF)}),
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument);
    EXPECT_THROW(
        remoteDebugIntf->enable(
            {oem_nvidia::DebugPolicy::SecurePrivilegeNonInvasiveDebug}),
        sdbusplus::xyz::openbmc_project::Common::Error::NotAllowed);

    debugStateSensor->stateSets[2]->setValue(
        PLDM_STATE_SET_DEBUG_STATE_ENABLED);
    EXPECT_NO_THROW(remoteDebugIntf->enable(
        {oem_nvidia::DebugPolicy::JtagDebug,
         oem_nvidia::DebugPolicy::DeviceDebug}));
    EXPECT_NO_THROW(remoteDebugIntf->disable(
        {oem_nvidia::DebugPolicy::JtagDebug,
         oem_nvidia::DebugPolicy::DeviceDebug}));

    ASSERT_EQ(1u, staticPowerPowerEffecter->oemIntfs.size());
    auto staticPowerIntf = std::dynamic_pointer_cast<OemStaticPowerHintInft>(
        staticPowerPowerEffecter->oemIntfs[0]);
    ASSERT_NE(nullptr, staticPowerIntf);

    const auto maxClock = staticPowerIntf->maxCpuClockFrequency();
    const auto minClock = staticPowerIntf->minCpuClockFrequency();
    const auto maxWorkload = staticPowerIntf->maxWorkloadFactor();
    const auto minWorkload = staticPowerIntf->minWorkloadFactor();
    const auto maxTemperature = staticPowerIntf->maxTemperature();
    const auto minTemperature = staticPowerIntf->minTemperature();

    EXPECT_GT(maxClock, minClock);
    EXPECT_GT(maxWorkload, minWorkload);
    EXPECT_GT(maxTemperature, minTemperature);

    EXPECT_THROW(
        staticPowerIntf->estimatePower(maxClock + 1, minWorkload,
                                       minTemperature),
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument);
    EXPECT_THROW(
        staticPowerIntf->estimatePower(minClock, maxWorkload + 1,
                                       minTemperature),
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument);
    EXPECT_THROW(
        staticPowerIntf->estimatePower(minClock, minWorkload,
                                       maxTemperature + 1),
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument);

    const double validClock = (maxClock + minClock) / 2.0;
    const double validWorkload = (maxWorkload + minWorkload) / 2.0;
    const double validTemperature = (maxTemperature + minTemperature) / 2.0;

    EXPECT_NO_THROW(staticPowerIntf->estimatePower(validClock, validWorkload,
                                                   validTemperature));
    runEventLoopForMilliseconds(10);
    try
    {
        staticPowerIntf->estimatePower(validClock, validWorkload,
                                       validTemperature);
    }
    catch (const sdbusplus::xyz::openbmc_project::Common::Error::Unavailable&)
    {}

    EXPECT_NE(nullptr, remoteDebugNumericEffecter);
    EXPECT_NE(nullptr, staticPowerTemperatureEffecter);
    EXPECT_NE(nullptr, staticPowerWorkloadEffecter);
    EXPECT_NE(nullptr, staticPowerClockEffecter);
}

TEST_F(TerminusTest, nvidiaUpdateAssociationsOemCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000159");
    Terminus terminus(0x59, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis59"};

    StateSetData linkStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_LINKSTATE),
                        PossibleStates{PLDM_STATESET_LINK_STATE_DISCONNECTED,
                                       PLDM_STATESET_LINK_STATE_CONNECTED});
    StateSetData performanceStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_PERFORMANCE),
                        PossibleStates{PLDM_STATESET_PERFORMANCE_NORMAL,
                                       PLDM_STATESET_PERFORMANCE_THROTTLED});

    auto createStateSensor = [&](uint16_t sensorId, EntityType entityType,
                                 const StateSetData& stateSetData) {
        StateSetInfo info =
            std::make_tuple(EntityInfo{1, entityType, 1},
                            std::vector<StateSetData>{stateSetData});
        auto sensor = std::make_shared<StateSensor>(
            terminus.getTid(), false, sensorId, info, nullptr, associationPath,
            nullptr);
        terminus.stateSensors.emplace_back(sensor);
        return sensor;
    };

    auto ethSensor =
        createStateSensor(0x801, PLDM_ENTITY_ETHERNET, linkStateData);
    auto ethSensorFallback =
        createStateSensor(0x802, PLDM_ENTITY_ETHERNET, linkStateData);
    auto ibSensor =
        createStateSensor(0x803, PLDM_ENTITY_INFINIBAND, linkStateData);
    auto memSensorCpu0 = createStateSensor(0x804, PLDM_ENTITY_MEMORY_CONTROLLER,
                                           performanceStateData);
    auto memSensorUnknown = createStateSensor(
        0x805, PLDM_ENTITY_MEMORY_CONTROLLER, performanceStateData);

    ethSensor->setInventoryPaths({associationPath}, false);
    ethSensorFallback->setInventoryPaths({associationPath}, false);
    ibSensor->setInventoryPaths({associationPath}, false);
    memSensorCpu0->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis59/CPU_0"},
        false);
    memSensorUnknown->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis59/cpu_unknown"},
        false);

    std::vector<pldm::dbus::PathAssociation> portAssociations{
        {"chassis", "all_states", associationPath},
        {"associated_port", "associated_port",
         "/xyz/openbmc_project/inventory/system/fabrics/fabric0/port0"}};
    const std::string ethernetProtocol{
        "xyz.openbmc_project.Inventory.Decorator.PortInfo.PortProtocol.Ethernet"};
    terminus.sensorPortInfoOverwriteTbl[0x801] = std::make_tuple(
        PortType::UpstreamPort, ethernetProtocol, 25000, portAssociations);
    terminus.sensorPortInfoOverwriteTbl[0x802] = std::make_tuple(
        PortType::BidirectionalPort, std::string(""), 10000, portAssociations);
    terminus.sensorPortInfoOverwriteTbl[0x803] = std::make_tuple(
        PortType::DownstreamPort, std::string(""), 50000, portAssociations);

    auto sensorEventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    sensorEventInfo->impactedComponent = "CoverageComponent";
    sensorEventInfo->eventIdsMap["LinkDown"] = "ResourceEvent.1.0.LinkDown";
    terminus.sensorEventInfoOverwriteTbl[0x801] = sensorEventInfo;
    terminus.sensorEventInfoOverwriteTbl[0x803] = sensorEventInfo;

    auto numericSensorPdr = makeNumericSensorValuePdrStruct(0x901);
    std::string numericSensorName{"oem_numeric_sensor_901"};
    auto numericSensor = std::make_shared<NumericSensor>(
        terminus.getTid(), false, numericSensorPdr, numericSensorName,
        associationPath, nullptr);
    terminus.numericSensors.emplace_back(numericSensor);
    terminus.sensorEventInfoOverwriteTbl[0x901] = sensorEventInfo;

    std::string switchType{
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet"};
    std::vector<std::string> switchProtocols{
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet",
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.PCIe"};
    std::vector<pldm::dbus::PathAssociation> switchAssociations{
        {"chassis", "all_states", associationPath}};
    terminus.switchBandwidthSensor =
        std::make_shared<oem_nvidia::SwitchBandwidthSensor>(
            terminus.getTid(), "switch_bandwidth_cov", switchType,
            switchProtocols, switchAssociations);

    auto updateRc =
        stdexec::sync_wait(nvidia::nvidiaUpdateAssociations(terminus));
    ASSERT_TRUE(updateRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateRc));

    auto ethStateSet = std::dynamic_pointer_cast<StateSetEthIBPortLinkState>(
        ethSensor->stateSets[0]);
    auto ethFallbackStateSet =
        std::dynamic_pointer_cast<StateSetEthIBPortLinkState>(
            ethSensorFallback->stateSets[0]);
    auto ibStateSet = std::dynamic_pointer_cast<StateSetEthIBPortLinkState>(
        ibSensor->stateSets[0]);
    ASSERT_NE(nullptr, ethStateSet);
    ASSERT_NE(nullptr, ethFallbackStateSet);
    ASSERT_NE(nullptr, ibStateSet);
    EXPECT_TRUE(ethStateSet->isDerivedSensorAssociated());
    EXPECT_TRUE(ethFallbackStateSet->isDerivedSensorAssociated());
    EXPECT_TRUE(ibStateSet->isDerivedSensorAssociated());
    EXPECT_EQ("switch_bandwidth_cov",
              terminus.switchBandwidthSensor->getSensorName());
    EXPECT_GT(terminus.switchBandwidthSensor->switchIntf->maxBandwidth(), 0);

    EXPECT_NE(nullptr, ethSensor->getSensorEventInfo());
    EXPECT_NE(nullptr, ibSensor->getSensorEventInfo());
    EXPECT_NE(nullptr, numericSensor->getSensorEventInfo());

    auto updateSecondRc =
        stdexec::sync_wait(nvidia::nvidiaUpdateAssociations(terminus));
    ASSERT_TRUE(updateSecondRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateSecondRc));

    auto memPerformanceStateSet =
        std::dynamic_pointer_cast<StateSetMemoryPerformance>(
            memSensorCpu0->stateSets[0]);
    ASSERT_NE(nullptr, memPerformanceStateSet);
    memPerformanceStateSet->updateShmemReading("Value");
}

TEST_F(TerminusTest, nvidiaEnergyCountPdrParserCoverageMatrix)
{
    auto createVendorData = [](uint8_t sensorDataSize) {
        pldm_oem_energycount_numeric_sensor_value_pdr pdr{};
        pdr.terminus_handle = 1;
        pdr.nvidia_oem_pdr_type = static_cast<uint8_t>(
            nvidia::NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_SENSOR_ENERGYCOUNT);
        pdr.sensor_id = 0x77;
        pdr.entity_type = PLDM_ENTITY_SYS_BOARD;
        pdr.entity_instance_num = 1;
        pdr.container_id = 1;
        pdr.base_unit = PLDM_SENSOR_UNIT_WATTS;
        pdr.sensor_data_size = sensorDataSize;
        pdr.update_interval = 1.0f;
        switch (sensorDataSize)
        {
            case PLDM_SENSOR_DATA_SIZE_UINT8:
            case PLDM_SENSOR_DATA_SIZE_SINT8:
                pdr.max_readable.value_u8 = 0x7F;
                pdr.min_readable.value_u8 = 0x01;
                break;
            case PLDM_SENSOR_DATA_SIZE_UINT16:
            case PLDM_SENSOR_DATA_SIZE_SINT16:
                pdr.max_readable.value_u16 = 0x1234;
                pdr.min_readable.value_u16 = 0x10;
                break;
            case PLDM_SENSOR_DATA_SIZE_UINT32:
            case PLDM_SENSOR_DATA_SIZE_SINT32:
                pdr.max_readable.value_u32 = 0x12345678;
                pdr.min_readable.value_u32 = 0x1000;
                break;
            case PLDM_SENSOR_DATA_SIZE_UINT64:
            case PLDM_SENSOR_DATA_SIZE_SINT64:
                pdr.max_readable.value_u64 = 0x123456789ABCDEF0ull;
                pdr.min_readable.value_u64 = 0x10000ull;
                break;
            default:
                break;
        }
        return structToBytes(pdr);
    };

    const std::array<uint8_t, 9> sensorDataSizes{
        PLDM_SENSOR_DATA_SIZE_UINT8,
        PLDM_SENSOR_DATA_SIZE_SINT8,
        PLDM_SENSOR_DATA_SIZE_UINT16,
        PLDM_SENSOR_DATA_SIZE_SINT16,
        PLDM_SENSOR_DATA_SIZE_UINT32,
        PLDM_SENSOR_DATA_SIZE_SINT32,
        PLDM_SENSOR_DATA_SIZE_UINT64,
        PLDM_SENSOR_DATA_SIZE_SINT64,
        0xFF};

    for (auto sensorDataSize : sensorDataSizes)
    {
        auto vendorData = createVendorData(sensorDataSize);
        auto parsed = nvidia::parseOEMEnergyCountNumericSensorPDR(vendorData);
        ASSERT_NE(nullptr, parsed);
    }

    std::vector<uint8_t> tooSmall(
        PLDM_PDR_OEM_ENERGYCOUNT_NUMERIC_SENSOR_PDR_MIN_LENGTH - 1, 0);
    EXPECT_EQ(nullptr, nvidia::parseOEMEnergyCountNumericSensorPDR(tooSmall));

    auto truncatedUint64VendorData =
        createVendorData(PLDM_SENSOR_DATA_SIZE_UINT64);
    truncatedUint64VendorData.resize(
        PLDM_PDR_OEM_ENERGYCOUNT_NUMERIC_SENSOR_PDR_MIN_LENGTH);
    EXPECT_EQ(nullptr, nvidia::parseOEMEnergyCountNumericSensorPDR(
                           truncatedUint64VendorData));
}

TEST_F(TerminusTest, switchBandwidthSensorCoverage)
{
    std::string switchType{
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet"};
    std::vector<std::string> switchProtocols{
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet",
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.PCIe"};
    std::vector<pldm::dbus::PathAssociation> associations{
        {"chassis", "all_states",
         "/xyz/openbmc_project/inventory/system/chassis/chassis60"}};

    oem_nvidia::SwitchBandwidthSensor sensor(
        0x60, "switch_bandwidth_sensor_cov", switchType, switchProtocols,
        associations);
    EXPECT_EQ("switch_bandwidth_sensor_cov", sensor.getSensorName());

    sensor.updateCurrentBandwidth(std::numeric_limits<double>::quiet_NaN(),
                                  12.5);
    sensor.updateCurrentBandwidth(2.5,
                                  std::numeric_limits<double>::quiet_NaN());
    sensor.updateMaxBandwidth(100.0);
    sensor.addAssociatedSensorID(0x1234);
    sensor.updateOnSharedMemory();

    EXPECT_GT(sensor.switchIntf->maxBandwidth(), 0);
}
#endif

// Currently due to async nature of polling this can't be tested.
// TODO: Test this in a different way.

// TEST_F(TerminusTest, TerminusOnOffLineTest)
// {
//     pldm::UUID uuidBad{"f72d6f90-5675-11ed-9b6a-0242ac120003"};
//     pldm::UUID uuid{"f72d6f90-5675-11ed-9b6a-0242ac120002"};
//     pldm::MctpInfos mctpInfos{pldm::MctpInfo(
//         12, uuid, "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1,
//         std::nullopt,
//         "xyz.openbmc_project.MCTP.Endpoint.BindingTypes.PCIe")};

//     /* 1. test discoverMctpTerminus(): check if terminus is discovered
//      * successfully by mock responses */
//     setupResponsesForDiscoverTerminus();
//     terminusManager.discoverMctpTerminus(mctpInfos);
//     EXPECT_EQ(1, termini.size());

//     /* 2. test getTerminus(): check if terminus can be found by uuid */
//     auto terminus = terminusManager.getTerminus(uuidBad);
//     EXPECT_EQ(nullptr, terminus);

//     terminus = terminusManager.getTerminus(uuid);
//     EXPECT_NE(nullptr, terminus);
//     EXPECT_EQ(uuid, terminus->getUuid());

//     /* 3. test initTerminus(): check if sensor is created successfully by
//     mock
//      * response */
//     setupResponsesForInitTerminus();
//     stdexec::sync_wait(platformManager.initTerminus());
//     EXPECT_EQ(1, terminus->numericSensorPdrs.size());

//     /* 4. test updateReading(): check if sensor PDIs are good */
//     auto numericSensor = terminus->numericSensors[0];
//     numericSensor->updateReading(true, true, 10);
//     EXPECT_EQ(true, numericSensor->availabilityIntf->available());
//     EXPECT_EQ(true, numericSensor->operationalStatusIntf->functional());
//     // raw = 10, converted value= 10*1.5 + 1 = 16
//     EXPECT_EQ(16, numericSensor->valueIntf->value());

//     /* 5. test setOffline(): check if sensor PDIs are in offline state*/
//     sensorManager.setOffline(terminus->getTid());
//     EXPECT_EQ(false, numericSensor->operationalStatusIntf->functional());
//     EXPECT_THAT(numericSensor->valueIntf->value(), testing::IsNan());

//     /* 6. test setOnline(): check if sensor PDIs are in online state */
//     setupResponsesForStartPolling();
//     sensorManager.setOnline(terminus->getTid());
//     runEventLoopForMilliseconds(2000);
//     EXPECT_EQ(true, numericSensor->operationalStatusIntf->functional());
//     // raw = 18, converted value= 18*1.5 + 1 = 28
//     EXPECT_EQ(28, numericSensor->valueIntf->value());
// }
