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
#include "libpldm/oem/nvidia/state_set_oem_nvidia.h"

#include "common/instance_id.hpp"
#include "mock_terminus_manager.hpp"
#include "oem/nvidia/platform-mc/state_set/cpuDiagnosticsRefresh.hpp"
#include "platform-mc/state_effecter.hpp"
#include "platform-mc/state_set.hpp"
#include "platform-mc/state_set/clearNonVolatileVariables.hpp"
#include "platform-mc/terminus.hpp"
#include "test/test_instance_id.hpp"

#include <gtest/gtest.h>

using namespace pldm::platform_mc;

static std::vector<uint8_t> makeStateEffecterPdr(
    uint16_t effecterId, uint16_t entityType, uint16_t entityInstance,
    uint16_t stateSetId, uint8_t possibleStates = 0x7)
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
        0,
        0x13, // dataLength
        0,
        0,    // PLDMTerminusHandle
        static_cast<uint8_t>(effecterId & 0xFF),
        static_cast<uint8_t>((effecterId >> 8) & 0xFF),
        static_cast<uint8_t>(entityType & 0xFF),
        static_cast<uint8_t>((entityType >> 8) & 0xFF),
        static_cast<uint8_t>(entityInstance & 0xFF),
        static_cast<uint8_t>((entityInstance >> 8) & 0xFF),
        0x1,
        0x0,   // containerID=1
        0x0,
        0x0,   // effecterSemanticID
        PLDM_NO_INIT,
        false, // effecterDescriptionPDR
        1,     // compositeEffecterCount
        static_cast<uint8_t>(stateSetId & 0xFF),
        static_cast<uint8_t>((stateSetId >> 8) & 0xFF),
        0x1, // possibleStatesSize
        possibleStates};
}

static std::vector<uint8_t> makeGetStateEffecterStatesResp(
    uint8_t compEffecterCount, uint8_t completionCode = PLDM_SUCCESS,
    pldm_effecter_oper_state effecterOpState =
        EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING,
    uint8_t pendingState = 0, uint8_t presentState = 1)
{
    pldm_get_state_effecter_states_resp respData{};
    respData.completion_code = completionCode;
    respData.comp_effecter_count = compEffecterCount;
    for (size_t i = 0; i < compEffecterCount; ++i)
    {
        respData.field[i].effecter_op_state = effecterOpState;
        respData.field[i].pending_state = pendingState;
        respData.field[i].present_state = presentState;
    }

    size_t payloadLen =
        1 + 1 + (compEffecterCount * sizeof(get_effecter_state_field));
    std::vector<uint8_t> response(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* responseMsg = reinterpret_cast<pldm_msg*>(response.data());
    auto rc = encode_get_state_effecter_states_resp(0, &respData, responseMsg,
                                                    payloadLen);
    EXPECT_EQ(rc, PLDM_SUCCESS);
    return response;
}

static std::vector<uint8_t> makeSetStateEffecterStatesResp(
    uint8_t completionCode = PLDM_SUCCESS)
{
    std::vector<uint8_t> response(
        sizeof(pldm_msg_hdr) + PLDM_SET_STATE_EFFECTER_STATES_RESP_BYTES, 0);
    auto* responseMsg = reinterpret_cast<pldm_msg*>(response.data());
    auto rc =
        encode_set_state_effecter_states_resp(0, completionCode, responseMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);
    return response;
}

class TestStateEffecter : public ::testing::Test
{
  public:
    TestStateEffecter() :
        bus(pldm::utils::DBusHandler::getBus()),
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        terminusManager(event, reqHandler, instanceIdDb, termini, 0x8, nullptr)
    {}

    static const auto& getStateSets(const StateEffecter& stateEffecter)
    {
        return stateEffecter.stateSets;
    }

    sdbusplus::bus_t& bus;
    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler;
    pldm::platform_mc::MockTerminusManager terminusManager;
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
};

TEST_F(TestStateEffecter, verifyStateEffecterClearVariable)
{
    uint16_t sensorId = 0x0820;
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    auto pdr1 = makeStateEffecterPdr(sensorId, PLDM_ENTITY_SYS_BOARD, 1,
                                     PLDM_STATESET_ID_BOOT_REQUEST, 0x7);

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.stateEffecterPdrs.size());

    auto stateEffecter = t1.stateEffecters[0];
    EXPECT_EQ(sensorId, stateEffecter->effecterId);

    auto& stateSets = TestStateEffecter::getStateSets(*stateEffecter);
    EXPECT_EQ(PLDM_STATESET_ID_BOOT_REQUEST, stateSets[0]->getStateSetId());

    // Should be PLDM_STATESET_BOOT_REQUEST_NORMAL
    stateEffecter->updateReading(0, EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING,
                                 0, PLDM_STATESET_BOOT_REQUEST_NORMAL);
    EXPECT_EQ(PLDM_STATESET_BOOT_REQUEST_NORMAL, stateSets[0]->getValue());

    // Test with invalid composite index
    stateEffecter->updateReading(4, EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING,
                                 4, PLDM_STATESET_BOOT_REQUEST_REQUESTED);
    EXPECT_EQ(PLDM_STATESET_BOOT_REQUEST_NORMAL, stateSets[0]->getValue());

    // Should be PLDM_STATESET_BOOT_REQUEST_REQUESTED
    stateEffecter->updateReading(0, EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING,
                                 0, PLDM_STATESET_BOOT_REQUEST_REQUESTED);
    EXPECT_EQ(PLDM_STATESET_BOOT_REQUEST_REQUESTED, stateSets[0]->getValue());
}

TEST_F(TestStateEffecter, stateEffecterOperationCoverage)
{
    const uint16_t effecterId = 0x0821;
    std::string uuid("00000000-0000-0000-0000-000000000002");
    auto terminus =
        Terminus(2, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    auto pdr = makeStateEffecterPdr(effecterId, PLDM_ENTITY_SYS_BOARD, 1,
                                    PLDM_STATESET_ID_BOOT_REQUEST, 0x7);
    terminus.pdrs.emplace_back(pdr);
    ASSERT_TRUE(terminus.parsePDRs());
    ASSERT_EQ(1u, terminus.stateEffecters.size());

    auto effecter = terminus.stateEffecters[0];
    auto& stateSets = TestStateEffecter::getStateSets(*effecter);
    ASSERT_EQ(1u, stateSets.size());
    EXPECT_EQ(effecterId, effecter->effecterId);

    const auto& [containerId, entityType, entityInstance] =
        effecter->getEntityInfo();
    EXPECT_EQ(1, containerId);
    EXPECT_EQ(PLDM_ENTITY_SYS_BOARD, entityType);
    EXPECT_EQ(1, entityInstance);

    std::vector<std::string> inventoryPaths{
        "/xyz/openbmc_project/inventory/system/chassis/chassis0"};
    effecter->setInventoryPaths(inventoryPaths);

    effecter->updateReading(0, EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING,
                            PLDM_STATESET_BOOT_REQUEST_REQUESTED,
                            PLDM_STATESET_BOOT_REQUEST_NORMAL);
    EXPECT_EQ(PLDM_STATESET_BOOT_REQUEST_REQUESTED, stateSets[0]->getValue());
    EXPECT_TRUE(effecter->isUpdatePending());

    effecter->updateReading(0, EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING,
                            PLDM_STATESET_BOOT_REQUEST_REQUESTED,
                            PLDM_STATESET_BOOT_REQUEST_NORMAL);
    EXPECT_EQ(PLDM_STATESET_BOOT_REQUEST_NORMAL, stateSets[0]->getValue());
    EXPECT_FALSE(effecter->isUpdatePending());

    effecter->updateReading(0, EFFECTER_OPER_STATE_DISABLED,
                            PLDM_STATESET_BOOT_REQUEST_REQUESTED,
                            PLDM_STATESET_BOOT_REQUEST_NORMAL);
    effecter->updateReading(0, EFFECTER_OPER_STATE_INITIALIZING,
                            PLDM_STATESET_BOOT_REQUEST_REQUESTED,
                            PLDM_STATESET_BOOT_REQUEST_NORMAL);
    effecter->updateReading(0, EFFECTER_OPER_STATE_UNAVAILABLE,
                            PLDM_STATESET_BOOT_REQUEST_REQUESTED,
                            PLDM_STATESET_BOOT_REQUEST_NORMAL);
    effecter->updateReading(0, EFFECTER_OPER_STATE_STATUSUNKNOWN,
                            PLDM_STATESET_BOOT_REQUEST_REQUESTED,
                            PLDM_STATESET_BOOT_REQUEST_NORMAL);
    effecter->updateReading(0, EFFECTER_OPER_STATE_FAILED,
                            PLDM_STATESET_BOOT_REQUEST_REQUESTED,
                            PLDM_STATESET_BOOT_REQUEST_NORMAL);
    effecter->updateReading(0, EFFECTER_OPER_STATE_SHUTTINGDOWN,
                            PLDM_STATESET_BOOT_REQUEST_REQUESTED,
                            PLDM_STATESET_BOOT_REQUEST_NORMAL);
    effecter->updateReading(0, EFFECTER_OPER_STATE_INTEST,
                            PLDM_STATESET_BOOT_REQUEST_REQUESTED,
                            PLDM_STATESET_BOOT_REQUEST_NORMAL);
    effecter->updateReading(5, EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING, 0,
                            PLDM_STATESET_BOOT_REQUEST_NORMAL);

    EXPECT_EQ(StateType::Enabled, effecter->getOperationalStatus());
    effecter->setAvailable(true);
    effecter->setAvailable(false);
    effecter->handleErrGetStateEffecterStates();

    auto [messageId, arg, level, eventId,
          impactedComponent] = stateSets[0]->getEventData(nullptr);
    EXPECT_FALSE(messageId.empty());
    EXPECT_TRUE(eventId.empty());
    EXPECT_TRUE(impactedComponent.empty());
    EXPECT_EQ("ClearNonvolatileVariable", stateSets[0]->getStringStateType());
}

TEST_F(TestStateEffecter, stateEffecterRequestValidationCoverage)
{
    const uint16_t effecterId = 0x0822;
    std::string uuid("00000000-0000-0000-0000-000000000003");
    auto terminus =
        Terminus(3, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    auto pdr = makeStateEffecterPdr(effecterId, PLDM_ENTITY_SYS_BOARD, 1,
                                    PLDM_STATESET_ID_BOOT_REQUEST, 0x7);
    terminus.pdrs.emplace_back(pdr);
    ASSERT_TRUE(terminus.parsePDRs());
    ASSERT_EQ(1u, terminus.stateEffecters.size());
    auto effecter = terminus.stateEffecters[0];

    auto getRc = stdexec::sync_wait(effecter->getStateEffecterStates());
    ASSERT_TRUE(getRc.has_value());
    EXPECT_NE(PLDM_SUCCESS, std::get<0>(*getRc));

    auto setInvalidCmpRc =
        stdexec::sync_wait(effecter->setStateEffecterStates(3, 1));
    ASSERT_TRUE(setInvalidCmpRc.has_value());
    EXPECT_EQ(PLDM_ERROR_INVALID_DATA, std::get<0>(*setInvalidCmpRc));

    std::vector<set_effecter_state_field> wrongSizeFields{};
    auto setWrongSizeRc =
        stdexec::sync_wait(effecter->setStateEffecterStates(wrongSizeFields));
    ASSERT_TRUE(setWrongSizeRc.has_value());
    EXPECT_EQ(PLDM_ERROR_INVALID_DATA, std::get<0>(*setWrongSizeRc));

    std::string path = "/xyz/openbmc_project/control/PLDM_Effecter_creator/0";
    pldm::dbus::PathAssociation association = {
        "chassis", "all_controls", "/xyz/openbmc_project/inventory/test"};
    auto createdInvalidId = StateSetCreator::createEffecter(
        0xFFFE, 0, path, association, effecter.get());
    EXPECT_EQ(nullptr, createdInvalidId);

    auto createdInvalidEffecter = StateSetCreator::createEffecter(
        PLDM_STATESET_ID_BOOT_REQUEST, 0, path, association, nullptr);
    EXPECT_EQ(nullptr, createdInvalidEffecter);
}

TEST_F(TestStateEffecter, clearNonVolatileInterfacesCoverage)
{
    StateSetData bootRequestData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_BOOT_REQUEST),
                        PossibleStates{PLDM_STATESET_BOOT_REQUEST_NORMAL,
                                       PLDM_STATESET_BOOT_REQUEST_REQUESTED});
    StateSetInfo effecterInfo =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
                        std::vector<StateSetData>{bootRequestData});

    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis3"};
    StateEffecter effecter(3, false, 0x0901, effecterInfo, nullptr,
                           associationPath, terminusManager);

    auto& bus = pldm::utils::DBusHandler::getBus();
    ClearNonVolatileVariablesStateIntf stateIntf(
        bus, "/xyz/openbmc_project/control/coverage/cnvv_state", 0);
    stateIntf.update(false);
    stateIntf.update(true);

    ClearNonVolatileVariablesEffecterIntf effecterIntf(
        bus, "/xyz/openbmc_project/control/coverage/cnvv_effecter", 0,
        effecter);
    EXPECT_TRUE(effecterIntf.clear(true));
    EXPECT_FALSE(effecterIntf.clear(false));
}

TEST_F(TestStateEffecter, stateSetCreatorOemEffecterCoverage)
{
    const uint16_t effecterId = 0x0823;
    std::string uuid("00000000-0000-0000-0000-000000000004");
    auto terminus =
        Terminus(4, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    auto pdr = makeStateEffecterPdr(effecterId, PLDM_ENTITY_SYS_BOARD, 1,
                                    PLDM_STATESET_ID_BOOT_REQUEST, 0x7);
    terminus.pdrs.emplace_back(pdr);
    ASSERT_TRUE(terminus.parsePDRs());
    ASSERT_EQ(1u, terminus.stateEffecters.size());
    auto effecter = terminus.stateEffecters[0];

    pldm::dbus::PathAssociation association = {
        "chassis", "all_controls", "/xyz/openbmc_project/inventory/test"};
    std::string debugPath =
        "/xyz/openbmc_project/control/PLDM_Effecter_creator/oem_debug";
    auto debugStateSet = StateSetCreator::createEffecter(
        PLDM_NVIDIA_OEM_STATE_SET_DEBUG_STATE, 0, debugPath, association,
        effecter.get());
    EXPECT_NE(nullptr, debugStateSet);

    std::string cpuDiagPath =
        "/xyz/openbmc_project/control/PLDM_Effecter_creator/oem_cpu_diag";
    auto cpuDiagStateSet = StateSetCreator::createEffecter(
        oem_nvidia::PLDM_NVIDIA_OEM_STATE_SET_CPU_DIAG_REFRESH, 1, cpuDiagPath,
        association, effecter.get());
    EXPECT_NE(nullptr, cpuDiagStateSet);
}

TEST_F(TestStateEffecter, cpuDiagnosticsRefreshCoverage)
{
    const uint16_t effecterId = 0x0824;
    std::string uuid("00000000-0000-0000-0000-000000000005");
    auto terminus =
        Terminus(5, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    auto pdr = makeStateEffecterPdr(
        effecterId, PLDM_ENTITY_SYS_BOARD, 1,
        oem_nvidia::PLDM_NVIDIA_OEM_STATE_SET_CPU_DIAG_REFRESH, 0x3);
    terminus.pdrs.emplace_back(pdr);
    ASSERT_TRUE(terminus.parsePDRs());
    ASSERT_EQ(1u, terminus.stateEffecters.size());

    auto effecter = terminus.stateEffecters[0];
    ASSERT_EQ(1u, effecter->stateSets.size());
    auto cpuDiagSet =
        std::dynamic_pointer_cast<oem_nvidia::StateSetCpuDiagnosticsRefresh>(
            effecter->stateSets[0]);
    ASSERT_NE(nullptr, cpuDiagSet);

    cpuDiagSet->setDefaultValue();
    EXPECT_EQ(oem_nvidia::PLDM_STATE_SET_CPU_DIAG_REFRESH_IDLE,
              cpuDiagSet->getValue());
    cpuDiagSet->setValue(oem_nvidia::PLDM_STATE_SET_CPU_DIAG_REFRESH_REQUESTED);
    EXPECT_EQ(oem_nvidia::PLDM_STATE_SET_CPU_DIAG_REFRESH_REQUESTED,
              cpuDiagSet->getValue());
    cpuDiagSet->setValue(oem_nvidia::PLDM_STATE_SET_CPU_DIAG_REFRESH_IDLE);
    EXPECT_EQ(oem_nvidia::PLDM_STATE_SET_CPU_DIAG_REFRESH_IDLE,
              cpuDiagSet->getValue());
    auto [msg, arg, level, eventId,
          impacted] = cpuDiagSet->getEventData(nullptr);
    EXPECT_TRUE(msg.empty());
    EXPECT_TRUE(arg.empty());
    EXPECT_EQ("CpuDiagnosticsRefresh", cpuDiagSet->getStringStateType());

    auto& bus = pldm::utils::DBusHandler::getBus();
    oem_nvidia::CpuDiagnosticsRefreshStateIntf stateIntf(
        bus, "/xyz/openbmc_project/control/coverage/cpu_diag_state", 0);
    stateIntf.update(true);
    stateIntf.update(false);
    EXPECT_FALSE(stateIntf.refresh());
    EXPECT_TRUE(stateIntf.refresh(true));

    oem_nvidia::CpuDiagnosticsRefreshEffecterIntf effecterIntf(
        bus, "/xyz/openbmc_project/control/coverage/cpu_diag_effecter", 0,
        *effecter);
    effecterIntf.update(true);
    effecterIntf.update(false);
    EXPECT_FALSE(effecterIntf.refresh(true));
    EXPECT_FALSE(effecterIntf.refresh(false));
    EXPECT_FALSE(effecterIntf.refresh());
}

TEST_F(TestStateEffecter, cpuDiagnosticsRefreshStateOnlyCoverage)
{
    pldm::dbus::PathAssociation association = {
        "chassis", "all_controls", "/xyz/openbmc_project/inventory/test"};
    std::string objectPath{
        "/xyz/openbmc_project/control/PLDM_Effecter_creator/oem_cpu_diag_state"};

    oem_nvidia::StateSetCpuDiagnosticsRefresh cpuDiagSet(
        oem_nvidia::PLDM_NVIDIA_OEM_STATE_SET_CPU_DIAG_REFRESH, 0, objectPath,
        association, nullptr);

    EXPECT_EQ(oem_nvidia::PLDM_STATE_SET_CPU_DIAG_REFRESH_IDLE,
              cpuDiagSet.getValue());
    cpuDiagSet.setValue(oem_nvidia::PLDM_STATE_SET_CPU_DIAG_REFRESH_REQUESTED);
    EXPECT_EQ(oem_nvidia::PLDM_STATE_SET_CPU_DIAG_REFRESH_REQUESTED,
              cpuDiagSet.getValue());
    cpuDiagSet.setValue(oem_nvidia::PLDM_STATE_SET_CPU_DIAG_REFRESH_IDLE);
    EXPECT_EQ(oem_nvidia::PLDM_STATE_SET_CPU_DIAG_REFRESH_IDLE,
              cpuDiagSet.getValue());
    EXPECT_EQ("CpuDiagnosticsRefresh", cpuDiagSet.getStringStateType());
}

TEST_F(TestStateEffecter, stateEffecterResponseCoverage)
{
    constexpr pldm::tid_t tid = 0x42;
    const pldm::MctpInfo mctpInfo(
        15, "00000000-0000-0000-0000-000000000042",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Endpoint.BindingTypes.PCIe", std::nullopt);
    ASSERT_TRUE(terminusManager.mapTid(mctpInfo, tid).has_value());

    const uint16_t effecterId = 0x0825;
    std::string uuid("00000000-0000-0000-0000-000000000006");
    auto terminus = Terminus(tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                             terminusManager);
    auto pdr = makeStateEffecterPdr(effecterId, PLDM_ENTITY_SYS_BOARD, 1,
                                    PLDM_STATESET_ID_BOOT_REQUEST, 0x7);
    terminus.pdrs.emplace_back(pdr);
    ASSERT_TRUE(terminus.parsePDRs());
    ASSERT_EQ(1u, terminus.stateEffecters.size());
    auto effecter = terminus.stateEffecters[0];

    auto response = makeGetStateEffecterStatesResp(
        1, PLDM_SUCCESS, EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING,
        PLDM_STATESET_BOOT_REQUEST_REQUESTED,
        PLDM_STATESET_BOOT_REQUEST_NORMAL);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    auto getOkRc = stdexec::sync_wait(effecter->getStateEffecterStates());
    ASSERT_TRUE(getOkRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*getOkRc));
    EXPECT_TRUE(effecter->isUpdatePending());

    response = {0x0, PLDM_PLATFORM, PLDM_GET_STATE_EFFECTER_STATES, 0x0};
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    auto getDecodeRc = stdexec::sync_wait(effecter->getStateEffecterStates());
    ASSERT_TRUE(getDecodeRc.has_value());
    EXPECT_NE(PLDM_SUCCESS, std::get<0>(*getDecodeRc));

    response = makeGetStateEffecterStatesResp(
        1, PLDM_ERROR, EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING,
        PLDM_STATESET_BOOT_REQUEST_REQUESTED,
        PLDM_STATESET_BOOT_REQUEST_NORMAL);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    auto getCcRc = stdexec::sync_wait(effecter->getStateEffecterStates());
    ASSERT_TRUE(getCcRc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*getCcRc));

    std::vector<set_effecter_state_field> stateField{
        {PLDM_REQUEST_SET, PLDM_STATESET_BOOT_REQUEST_REQUESTED}};

    response = makeSetStateEffecterStatesResp(PLDM_SUCCESS);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    response = makeGetStateEffecterStatesResp(
        1, PLDM_SUCCESS, EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING,
        PLDM_STATESET_BOOT_REQUEST_REQUESTED,
        PLDM_STATESET_BOOT_REQUEST_NORMAL);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    auto setOkRc =
        stdexec::sync_wait(effecter->setStateEffecterStates(stateField));
    ASSERT_TRUE(setOkRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*setOkRc));

    response = makeSetStateEffecterStatesResp(PLDM_ERROR);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    response = makeGetStateEffecterStatesResp(
        1, PLDM_SUCCESS, EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING,
        PLDM_STATESET_BOOT_REQUEST_REQUESTED,
        PLDM_STATESET_BOOT_REQUEST_NORMAL);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    auto setCcRc =
        stdexec::sync_wait(effecter->setStateEffecterStates(stateField));
    ASSERT_TRUE(setCcRc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*setCcRc));
}
