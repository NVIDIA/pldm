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
#include "common/types.hpp"
#include "mock_terminus_manager.hpp"
#include "oem/nvidia/platform-mc/pcoreDump.hpp"
#include "platform-mc/numeric_effecter.hpp"
#include "platform-mc/terminus.hpp"
#include "requester/handler.hpp"
#include "test/test_instance_id.hpp"

#include <xyz/openbmc_project/Common/error.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace pldm::platform_mc;

using InvalidArgument =
    sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument;
using Unavailable = sdbusplus::xyz::openbmc_project::Common::Error::Unavailable;

constexpr pldm::tid_t testTid = 0x31;
constexpr const char* testObjectPath =
    "/xyz/openbmc_project/control/ProcessorModule_0_OOBPcoreDump_0";

/** @brief A PDR shaped like the one the device exposes for the trigger:
 *  UINT32, base unit Counts, selectors 1..6.
 */
static std::shared_ptr<pldm_numeric_effecter_value_pdr> makePCoreDumpPdr(
    uint16_t effecterId = 0x0846, uint32_t minSettable = 1,
    uint32_t maxSettable = 6, float resolution = 1.0f, float offset = 0.0f,
    int8_t unitModifier = 0)
{
    auto pdr = std::make_shared<pldm_numeric_effecter_value_pdr>();
    pdr->effecter_id = effecterId;
    pdr->entity_type = PLDM_ENTITY_SYS_BOARD;
    pdr->entity_instance = 1;
    pdr->container_id = 1;
    pdr->base_unit = PLDM_SENSOR_UNIT_COUNTS;
    pdr->unit_modifier = unitModifier;
    pdr->effecter_data_size = PLDM_EFFECTER_DATA_SIZE_UINT32;
    pdr->resolution = resolution;
    pdr->offset = offset;
    pdr->range_field_format = PLDM_RANGE_FIELD_FORMAT_UINT32;
    pdr->range_field_support.byte = 0x1F;
    pdr->max_settable.value_u32 = maxSettable;
    pdr->min_settable.value_u32 = minSettable;
    return pdr;
}

static std::vector<uint8_t> makeSetResp(uint8_t completionCode = PLDM_SUCCESS)
{
    std::vector<uint8_t> response(
        sizeof(pldm_msg_hdr) + PLDM_SET_NUMERIC_EFFECTER_VALUE_RESP_BYTES, 0);
    auto* responseMsg = reinterpret_cast<pldm_msg*>(response.data());
    EXPECT_EQ(PLDM_SUCCESS, encode_set_numeric_effecter_value_resp(
                                0, completionCode, responseMsg,
                                PLDM_SET_NUMERIC_EFFECTER_VALUE_RESP_BYTES));
    return response;
}

static std::vector<uint8_t> makeGetResp(
    pldm_effecter_oper_state operState =
        EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING,
    uint32_t presentValue = 0)
{
    union_effecter_data_size pending{};
    union_effecter_data_size present{};
    pending.value_u32 = presentValue;
    present.value_u32 = presentValue;
    const size_t payloadLen =
        PLDM_GET_NUMERIC_EFFECTER_VALUE_MIN_RESP_BYTES + 6;

    std::vector<uint8_t> response(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* responseMsg = reinterpret_cast<pldm_msg*>(response.data());
    EXPECT_EQ(PLDM_SUCCESS, encode_get_numeric_effecter_value_resp(
                                0, PLDM_SUCCESS, PLDM_EFFECTER_DATA_SIZE_UINT32,
                                operState, reinterpret_cast<uint8_t*>(&pending),
                                reinterpret_cast<uint8_t*>(&present),
                                responseMsg, payloadLen));
    return response;
}

class TestPCoreDump : public ::testing::Test
{
  public:
    TestPCoreDump() :
        bus(pldm::utils::DBusHandler::getBus()),
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, std::chrono::seconds(1),
                   2, std::chrono::milliseconds(100)),
        terminusManager(event, reqHandler, instanceIdDb, termini, 0x8, nullptr)
    {
        const pldm::MctpInfo mctpInfo(
            14, "00000000-0000-0000-0000-000000000031",
            "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB", 1, std::nullopt,
            "xyz.openbmc_project.MCTP.Endpoint.BindingTypes.USB", std::nullopt);
        EXPECT_TRUE(terminusManager.mapTid(mctpInfo, testTid).has_value());
    }

    /** @brief Build an effecter carrying the given PDR, named so that its
     *  object path is the one the trigger is discovered on.
     */
    std::unique_ptr<NumericEffecter> makeEffecter(
        std::shared_ptr<pldm_numeric_effecter_value_pdr> pdr,
        const std::string& name = "ProcessorModule_0_OOBPcoreDump_0")
    {
        std::string effecterName{name};
        std::string inventoryPath{
            "/xyz/openbmc_project/inventory/system/chassis/chassis0"};
        return std::make_unique<NumericEffecter>(
            testTid, false, pdr, effecterName, inventoryPath, terminusManager);
    }

    /** @brief Queue the pair of responses one successful set consumes:
     *  the SetNumericEffecterValue reply, then the read-back that follows it.
     */
    void queueSuccessfulSet(pldm_effecter_oper_state operState =
                                EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING)
    {
        auto setResp = makeSetResp();
        ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(setResp));
        auto getResp = makeGetResp(operState);
        ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(getResp));
    }

    sdbusplus::bus_t& bus;
    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler;
    MockTerminusManager terminusManager;
    std::map<pldm::tid_t, std::shared_ptr<Terminus>> termini;
};

/** The bounds advertised on D-Bus are the PDR's settable range expressed in
 *  base units, and they are what the range check enforces.
 */
TEST_F(TestPCoreDump, advertisesPdrSelectorRange)
{
    auto effecter = makeEffecter(makePCoreDumpPdr());
    OemPCoreDumpIntf intf(bus, testObjectPath, *effecter);

    EXPECT_EQ(1u, intf.minPCoreId());
    EXPECT_EQ(6u, intf.maxPCoreId());
}

/** A PDR that never populated its limits leaves them NaN. Every comparison
 *  against NaN is false, so a naive range check would accept any selector at
 *  all -- the adapter must refuse to dispatch instead.
 */
TEST_F(TestPCoreDump, refusesCreateDumpWhenPdrBoundsAreNotPopulated)
{
    auto effecter = makeEffecter(makePCoreDumpPdr());
    // Undo what the PDR populated, reproducing the sentinel the base unit
    // starts out with.
    effecter->unitIntf->pdrMinSettable(
        std::numeric_limits<double>::quiet_NaN());
    effecter->unitIntf->pdrMaxSettable(
        std::numeric_limits<double>::quiet_NaN());

    OemPCoreDumpIntf intf(bus, testObjectPath, *effecter);

    // Nothing settable is advertised, and every selector is refused.
    EXPECT_EQ(0u, intf.minPCoreId());
    EXPECT_EQ(0u, intf.maxPCoreId());
    EXPECT_THROW(intf.createDump(3), Unavailable);
    // Nothing was put on the wire.
    EXPECT_TRUE(terminusManager.responseMsgs.empty());
}

/** Selectors outside the advertised range are rejected before any traffic. */
TEST_F(TestPCoreDump, rejectsSelectorOutsideAdvertisedRange)
{
    auto effecter = makeEffecter(makePCoreDumpPdr());
    OemPCoreDumpIntf intf(bus, testObjectPath, *effecter);

    EXPECT_THROW(intf.createDump(0), InvalidArgument);
    EXPECT_THROW(intf.createDump(7), InvalidArgument);

    // A rejected CreateDump must not consume a queued response, i.e. it must
    // not have put anything on the wire.
    queueSuccessfulSet();
    EXPECT_THROW(intf.createDump(99), InvalidArgument);
    EXPECT_EQ(2u, terminusManager.responseMsgs.size());
}

/** The range check runs on the value the caller asked in, before the PDR's
 *  resolution and offset are applied. Checking after the conversion would
 *  compare against scaled numbers, and the value put on the wire would denote
 *  a different PCore than the one that was validated.
 */
TEST_F(TestPCoreDump, rangeCheckPrecedesRawConversion)
{
    // resolution 2, offset 1: raw = (base - 1) / 2, so the whole 1..6 window
    // maps to raw 0..2.5 and a naive check on the raw value would accept
    // selectors far outside it.
    auto effecter = makeEffecter(makePCoreDumpPdr(0x0846, 1, 6, 2.0f, 1.0f));
    OemPCoreDumpIntf intf(bus, testObjectPath, *effecter);

    ASSERT_EQ(1u, intf.minPCoreId());
    ASSERT_EQ(6u, intf.maxPCoreId());

    // 3 is inside the base-unit window and goes out as raw (3 - 1) / 2 = 1.
    EXPECT_DOUBLE_EQ(1.0, effecter->baseToRaw(3.0));
    queueSuccessfulSet();
    EXPECT_NO_THROW(intf.createDump(3));

    // 7 converts to raw 3.0, which is inside the *raw* window but outside the
    // advertised one. It must still be rejected.
    EXPECT_DOUBLE_EQ(3.0, effecter->baseToRaw(7.0));
    EXPECT_THROW(intf.createDump(7), InvalidArgument);
}

/** A unit modifier shifts base units against effecter units; the advertised
 *  bounds and the range check must move with it together.
 */
TEST_F(TestPCoreDump, advertisedBoundsFollowUnitModifier)
{
    // unitModifier 1: base = effecter * 10, so settable 1..6 is 10..60 base.
    auto effecter = makeEffecter(makePCoreDumpPdr(0x0846, 1, 6, 1.0f, 0.0f, 1));
    OemPCoreDumpIntf intf(bus, testObjectPath, *effecter);

    EXPECT_EQ(10u, intf.minPCoreId());
    EXPECT_EQ(60u, intf.maxPCoreId());
    EXPECT_THROW(intf.createDump(6), InvalidArgument);

    queueSuccessfulSet();
    EXPECT_NO_THROW(intf.createDump(10));
    EXPECT_DOUBLE_EQ(1.0, effecter->baseToRaw(10.0));
}

/** Back-to-back requests are all dispatched. pldmd keeps no notion of an
 *  outstanding collection, so it never refuses a write on the grounds that an
 *  earlier one has not been answered; issuing one at a time is the caller's
 *  discipline to keep.
 */
TEST_F(TestPCoreDump, consecutiveCreateDumpsAreAllDispatched)
{
    auto effecter = makeEffecter(makePCoreDumpPdr());
    OemPCoreDumpIntf intf(bus, testObjectPath, *effecter);

    for (uint64_t selector = 1; selector <= 6; ++selector)
    {
        queueSuccessfulSet();
        EXPECT_NO_THROW(intf.createDump(selector)) << "selector " << selector;
    }

    // Every one of them reached the wire.
    EXPECT_TRUE(terminusManager.responseMsgs.empty());
}

/** A set that fails on the wire is logged and otherwise leaves nothing behind:
 *  the next request is dispatched exactly as if it had succeeded.
 */
TEST_F(TestPCoreDump, aFailedDispatchDoesNotAffectTheNext)
{
    auto effecter = makeEffecter(makePCoreDumpPdr());
    OemPCoreDumpIntf intf(bus, testObjectPath, *effecter);

    // No queued response: the mock transport fails the set.
    EXPECT_NO_THROW(intf.createDump(6));

    queueSuccessfulSet();
    EXPECT_NO_THROW(intf.createDump(6));
    EXPECT_TRUE(terminusManager.responseMsgs.empty());
}

/** An effecter the device has reported as unusable is not dispatched to. */
TEST_F(TestPCoreDump, refusesCreateDumpWhileTheEffecterIsUnusable)
{
    auto effecter = makeEffecter(makePCoreDumpPdr());
    OemPCoreDumpIntf intf(bus, testObjectPath, *effecter);

    // Device reports the effecter disabled.
    effecter->updateValue(EFFECTER_OPER_STATE_DISABLED, 0, 0);
    EXPECT_THROW(intf.createDump(1), Unavailable);

    // Device is unreachable.
    effecter->updateValue(EFFECTER_OPER_STATE_UNAVAILABLE, 0, 0);
    EXPECT_THROW(intf.createDump(1), Unavailable);

    // Back to enabled, and it dispatches again.
    effecter->updateValue(EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING, 0, 0);
    queueSuccessfulSet();
    EXPECT_NO_THROW(intf.createDump(1));
    EXPECT_TRUE(terminusManager.responseMsgs.empty());
}

/** OperationalStatus only carries a state once the effecter has been read
 *  back, so an effecter that has never been polled must still accept the
 *  first collection rather than refuse it as "not Enabled".
 */
TEST_F(TestPCoreDump, dispatchesOnAnEffecterThatHasNeverBeenPolled)
{
    auto effecter = makeEffecter(makePCoreDumpPdr());
    OemPCoreDumpIntf intf(bus, testObjectPath, *effecter);

    ASSERT_NE(StateType::Enabled, effecter->state());
    queueSuccessfulSet();
    EXPECT_NO_THROW(intf.createDump(2));
    EXPECT_TRUE(terminusManager.responseMsgs.empty());
}

constexpr const char* testCpuPath =
    "/xyz/openbmc_project/inventory/system/processors/CPU_0";
constexpr const char* testBoardPath =
    "/xyz/openbmc_project/inventory/system/board/HGX_ProcessorModule_0";

static Associations::value_type edge(const std::string& forward,
                                     const std::string& reverse,
                                     const std::string& path)
{
    return std::make_tuple(forward, reverse, path);
}

static size_t countCpuEdges(const Associations& assocs)
{
    return static_cast<size_t>(
        std::count_if(assocs.begin(), assocs.end(), [](const auto& a) {
            return std::get<0>(a) == pcoreDumpCpuForward &&
                   std::get<1>(a) == pcoreDumpCpuReverse;
        }));
}

/** The CPU edge is added without disturbing the generic one the effecter
 *  already carries.
 */
TEST(PCoreDumpAssociation, addsTheCpuEdgeAlongsideTheGenericOne)
{
    Associations assocs{edge("chassis", "all_controls", testBoardPath)};

    auto result = withPCoreDumpCpuAssociation(assocs, testCpuPath);

    ASSERT_EQ(2u, result.size());
    EXPECT_EQ(edge("chassis", "all_controls", testBoardPath), result[0]);
    EXPECT_EQ(edge(pcoreDumpCpuForward, pcoreDumpCpuReverse, testCpuPath),
              result[1]);
}

/** Terminus::updateAssociations re-runs on rediscovery, so this is applied
 *  repeatedly over the life of the daemon. Applying it twice must not leave two
 *  CPU edges behind.
 */
TEST(PCoreDumpAssociation, replacesRatherThanAccumulates)
{
    Associations assocs{edge("chassis", "all_controls", testBoardPath)};

    auto once = withPCoreDumpCpuAssociation(assocs, testCpuPath);
    auto twice = withPCoreDumpCpuAssociation(once, testCpuPath);
    auto thrice = withPCoreDumpCpuAssociation(twice, testCpuPath);

    EXPECT_EQ(once, thrice);
    EXPECT_EQ(1u, countCpuEdges(thrice));
}

/** setInventoryPaths rebuilds the association list keyed on the (forward,
 *  reverse) pair and rewrites every path to the generic entity-resolved one --
 *  including ours. The next pass has to put it back.
 */
TEST(PCoreDumpAssociation, repointsAnEdgeClobberedByInventoryRefresh)
{
    // What the list looks like after setInventoryPaths has rewritten both
    // entries to the generic path.
    Associations clobbered{
        edge("chassis", "all_controls", testBoardPath),
        edge(pcoreDumpCpuForward, pcoreDumpCpuReverse, testBoardPath)};

    auto result = withPCoreDumpCpuAssociation(clobbered, testCpuPath);

    ASSERT_EQ(1u, countCpuEdges(result));
    EXPECT_EQ(edge(pcoreDumpCpuForward, pcoreDumpCpuReverse, testCpuPath),
              result.back());
    // The generic edge is still the effecter's own, untouched.
    EXPECT_EQ(edge("chassis", "all_controls", testBoardPath), result.front());
}

/** An effecter whose entity type already resolved to the CPU carries the same
 *  path on both edges, and that is fine -- they are distinct edges.
 */
TEST(PCoreDumpAssociation, toleratesAGenericEdgeAlreadyOnTheCpu)
{
    Associations assocs{edge("chassis", "all_controls", testCpuPath)};

    auto result = withPCoreDumpCpuAssociation(assocs, testCpuPath);

    ASSERT_EQ(2u, result.size());
    EXPECT_EQ(1u, countCpuEdges(result));
}

TEST(PCoreDumpAssociation, addsTheEdgeToAnEmptyList)
{
    auto result = withPCoreDumpCpuAssociation(Associations{}, testCpuPath);

    ASSERT_EQ(1u, result.size());
    EXPECT_EQ(edge(pcoreDumpCpuForward, pcoreDumpCpuReverse, testCpuPath),
              result.front());
}
