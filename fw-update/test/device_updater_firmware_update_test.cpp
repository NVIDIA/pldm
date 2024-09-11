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
#include "libpldm/firmware_update.h"

#include "common/utils.hpp"
#define private public
#include "fw-update/device_updater.hpp"
#include "fw-update/package_parser.hpp"
#include "fw-update/update_manager.hpp"
#include "mocked_firmware_update_function.hpp"
#include "pldmd/dbus_impl_requester.hpp"
#include "requester/handler.hpp"

#include <sdbusplus/bus.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>
#include <sdeventplus/test/sdevent.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pldm;
using namespace pldm::fw_update;
using ::testing::_;

class DeviceUpdaterTestWithMockedFirmwareUpdateFunctions : public testing::Test
{
  public:
    DeviceUpdaterTestWithMockedFirmwareUpdateFunctions() :
        package("./test_pkg", std::ios::binary | std::ios::in | std::ios::ate),
        event(sdeventplus::Event::get_default()),
        dbusImplRequester(pldm::utils::DBusHandler::getBus(),
                          "/xyz/openbmc_project/pldm"),
        reqHandler(event, dbusImplRequester, sockManager, false,
                   std::chrono::seconds(1), 2, std::chrono::milliseconds(100)),
        updateManager(event, reqHandler, dbusImplRequester, descriptorMap,
                      componentInfoMap, componentNameMap, true)
    {
        fwDeviceIDRecord = {
            1,
            {0x00},
            "VersionString2",
            {{PLDM_FWUP_UUID,
              std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41,
                                   0x15, 0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49,
                                   0xD6, 0x75}}},
            {}};
        compImageInfos = {
            {10, 100, 0xFFFFFFFF, 0, 0, 139, 1024, "VersionString3"}};
        compInfo = {
            {std::make_pair(10, 100), std::make_tuple(1, "comp1Version")}};
        compIdNameInfo = {{11, "ComponentName1"},
                          {55555, "ComponentName2"},
                          {12, "ComponentName3"},
                          {66666, "ComponentName4"}};

        _mockedFirmwareUpdateFunction.reset(
            new ::testing::NiceMock<MockedFirmwareUpdateFunction>());
    }

    ~DeviceUpdaterTestWithMockedFirmwareUpdateFunctions()
    {
        _mockedFirmwareUpdateFunction.reset();
    }

    static std::unique_ptr<MockedFirmwareUpdateFunction>
        _mockedFirmwareUpdateFunction;

  protected:
    std::ifstream package;
    FirmwareDeviceIDRecord fwDeviceIDRecord;
    ComponentImageInfos compImageInfos;
    ComponentInfo compInfo;
    ComponentIdNameMap compIdNameInfo;
    sdeventplus::Event event;
    pldm::dbus_api::Requester dbusImplRequester;
    pldm::mctp_socket::Manager sockManager;
    requester::Handler<requester::Request> reqHandler;
    DescriptorMap descriptorMap;
    ComponentInfoMap componentInfoMap;
    ComponentNameMap componentNameMap;
    UpdateManager updateManager;
};

std::unique_ptr<MockedFirmwareUpdateFunction>
    DeviceUpdaterTestWithMockedFirmwareUpdateFunctions::
        _mockedFirmwareUpdateFunction;

int encode_request_firmware_data_resp(uint8_t instance_id,
                                      uint8_t completion_code,
                                      struct pldm_msg* msg,
                                      size_t payload_length)
{
    return DeviceUpdaterTestWithMockedFirmwareUpdateFunctions::
        _mockedFirmwareUpdateFunction->encode_request_firmware_data_resp(
            instance_id, completion_code, msg, payload_length);
};

int decode_request_firmware_data_req(const struct pldm_msg* msg,
                                     size_t payload_length, uint32_t* offset,
                                     uint32_t* length)
{
    return DeviceUpdaterTestWithMockedFirmwareUpdateFunctions::
        _mockedFirmwareUpdateFunction->decode_request_firmware_data_req(
            msg, payload_length, offset, length);
}

int encode_request_update_req(uint8_t instance_id, uint32_t max_transfer_size,
                              uint16_t num_of_comp,
                              uint8_t max_outstanding_transfer_req,
                              uint16_t pkg_data_len,
                              uint8_t comp_image_set_ver_str_type,
                              uint8_t comp_image_set_ver_str_len,
                              const struct variable_field* comp_img_set_ver_str,
                              struct pldm_msg* msg, size_t payload_length)
{
    return DeviceUpdaterTestWithMockedFirmwareUpdateFunctions::
        _mockedFirmwareUpdateFunction->encode_request_update_req(
            instance_id, max_transfer_size, num_of_comp,
            max_outstanding_transfer_req, pkg_data_len,
            comp_image_set_ver_str_type, comp_image_set_ver_str_len,
            comp_img_set_ver_str, msg, payload_length);
};

int encode_pass_component_table_req(
    uint8_t instance_id, uint8_t transfer_flag, uint16_t comp_classification,
    uint16_t comp_identifier, uint8_t comp_classification_index,
    uint32_t comp_comparison_stamp, uint8_t comp_ver_str_type,
    uint8_t comp_ver_str_len, const struct variable_field* comp_ver_str,
    struct pldm_msg* msg, size_t payload_length)
{
    return DeviceUpdaterTestWithMockedFirmwareUpdateFunctions::
        _mockedFirmwareUpdateFunction->encode_pass_component_table_req(
            instance_id, transfer_flag, comp_classification, comp_identifier,
            comp_classification_index, comp_comparison_stamp, comp_ver_str_type,
            comp_ver_str_len, comp_ver_str, msg, payload_length);
}

int decode_pass_component_table_resp(const struct pldm_msg* msg,
                                     size_t payload_length,
                                     uint8_t* completion_code,
                                     uint8_t* comp_resp,
                                     uint8_t* comp_resp_code)
{
    return DeviceUpdaterTestWithMockedFirmwareUpdateFunctions::
        _mockedFirmwareUpdateFunction->decode_pass_component_table_resp(
            msg, payload_length, completion_code, comp_resp, comp_resp_code);
}

int decode_update_component_resp(const struct pldm_msg* msg,
                                 size_t payload_length,
                                 uint8_t* completion_code,
                                 uint8_t* comp_compatability_resp,
                                 uint8_t* comp_compatability_resp_code,
                                 bitfield32_t* update_option_flags_enabled,
                                 uint16_t* time_before_req_fw_data)
{
    return DeviceUpdaterTestWithMockedFirmwareUpdateFunctions::
        _mockedFirmwareUpdateFunction->decode_update_component_resp(
            msg, payload_length, completion_code, comp_compatability_resp,
            comp_compatability_resp_code, update_option_flags_enabled,
            time_before_req_fw_data);
}

int decode_apply_complete_req(
    const struct pldm_msg* msg, size_t payload_length, uint8_t* apply_result,
    bitfield16_t* comp_activation_methods_modification)
{
    return DeviceUpdaterTestWithMockedFirmwareUpdateFunctions::
        _mockedFirmwareUpdateFunction->decode_apply_complete_req(
            msg, payload_length, apply_result,
            comp_activation_methods_modification);
}

int encode_apply_complete_resp(uint8_t instance_id, uint8_t completion_code,
                               struct pldm_msg* msg, size_t payload_length)
{
    return DeviceUpdaterTestWithMockedFirmwareUpdateFunctions::
        _mockedFirmwareUpdateFunction->encode_apply_complete_resp(
            instance_id, completion_code, msg, payload_length);
}

int decode_request_update_resp(const struct pldm_msg* msg,
                               size_t payload_length, uint8_t* completion_code,
                               uint16_t* fd_meta_data_len,
                               uint8_t* fd_will_send_pkg_data)
{
    return DeviceUpdaterTestWithMockedFirmwareUpdateFunctions::
        _mockedFirmwareUpdateFunction->decode_request_update_resp(
            msg, payload_length, completion_code, fd_meta_data_len,
            fd_will_send_pkg_data);
}

int encode_update_component_req(
    uint8_t instance_id, uint16_t comp_classification, uint16_t comp_identifier,
    uint8_t comp_classification_index, uint32_t comp_comparison_stamp,
    uint32_t comp_image_size, bitfield32_t update_option_flags,
    uint8_t comp_ver_str_type, uint8_t comp_ver_str_len,
    const struct variable_field* comp_ver_str, struct pldm_msg* msg,
    size_t payload_length)
{
    return DeviceUpdaterTestWithMockedFirmwareUpdateFunctions::
        _mockedFirmwareUpdateFunction->encode_update_component_req(
            instance_id, comp_classification, comp_identifier,
            comp_classification_index, comp_comparison_stamp, comp_image_size,
            update_option_flags, comp_ver_str_type, comp_ver_str_len,
            comp_ver_str, msg, payload_length);
}

TEST_F(DeviceUpdaterTestWithMockedFirmwareUpdateFunctions,
       requestFwData_decode_request_firmware_failed)
{
    mctp_eid_t eid = 0;
    size_t componentOffset = 0;
    DeviceUpdater deviceUpdater(eid, package, fwDeviceIDRecord, compImageInfos,
                                compInfo, compIdNameInfo, 512, &updateManager,
                                false);
    ComponentUpdater componentUpdater(eid, package, fwDeviceIDRecord,
                                      compImageInfos, compInfo, compIdNameInfo,
                                      512, &updateManager, &deviceUpdater,
                                      componentOffset, false);

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_request_firmware_data_req)>
        reqFwDataReq{0x8A, 0x05, 0x15, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x02, 0x00, 0x00};

    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());
    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::RequestFirmwareData);

    EXPECT_CALL(*_mockedFirmwareUpdateFunction,
                encode_request_firmware_data_resp(_, _, _, _))
        .WillRepeatedly(testing::Return(1));

    EXPECT_CALL(*_mockedFirmwareUpdateFunction,
                decode_request_firmware_data_req(_, _, _, _))
        .WillRepeatedly(testing::Return(1));

    EXPECT_NO_THROW({
        componentUpdater.requestFwData(requestMsg,
                                       sizeof(pldm_request_firmware_data_req));
    });
}

TEST_F(DeviceUpdaterTestWithMockedFirmwareUpdateFunctions, startFwUpdateFlow)
{
    mctp_eid_t eid = 0;

    EXPECT_CALL(*_mockedFirmwareUpdateFunction,
                encode_request_update_req(_, _, _, _, _, _, _, _, _, _))
        .WillRepeatedly(testing::Return(0));

    DeviceUpdater deviceUpdater(eid, package, fwDeviceIDRecord, compImageInfos,
                                compInfo, compIdNameInfo, 64, &updateManager,
                                false);
    EXPECT_NO_THROW({ deviceUpdater.startFwUpdateFlow(); });
}

TEST_F(DeviceUpdaterTestWithMockedFirmwareUpdateFunctions,
       startFwUpdateFlow_encode_request_failed)
{
    mctp_eid_t eid = 0;

    EXPECT_CALL(*_mockedFirmwareUpdateFunction,
                encode_request_update_req(_, _, _, _, _, _, _, _, _, _))
        .WillRepeatedly(testing::Return(1));

    DeviceUpdater deviceUpdater(eid, package, fwDeviceIDRecord, compImageInfos,
                                compInfo, compIdNameInfo, 64, &updateManager,
                                false);

    EXPECT_NO_THROW({ deviceUpdater.startFwUpdateFlow(); });
}

TEST_F(DeviceUpdaterTestWithMockedFirmwareUpdateFunctions,
       private_method_sendPassCompTableRequest_encode_pass_component_table_req)
{
    mctp_eid_t eid = 0;
    size_t offset = 0;

    DeviceUpdater deviceUpdater(eid, package, fwDeviceIDRecord, compImageInfos,
                                compInfo, compIdNameInfo, 512, &updateManager,
                                false);

    EXPECT_CALL(
        *_mockedFirmwareUpdateFunction,
        encode_pass_component_table_req(_, _, _, _, _, _, _, _, _, _, _))
        .WillRepeatedly(testing::Return(1));

    EXPECT_NO_THROW({ deviceUpdater.sendPassCompTableRequest(offset); });
}

TEST_F(DeviceUpdaterTestWithMockedFirmwareUpdateFunctions,
       passCompTabl_decode_pass_component_table_resp_rc)
{
    mctp_eid_t eid = 0;

    DeviceUpdater deviceUpdater(eid, package, fwDeviceIDRecord, compImageInfos,
                                compInfo, compIdNameInfo, 512, &updateManager,
                                false);

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_request_firmware_data_req)>
        reqFwDataReq{0x8A, 0x05, 0x15, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x02, 0x00, 0x00};

    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    EXPECT_CALL(*_mockedFirmwareUpdateFunction,
                decode_pass_component_table_resp(_, _, _, _, _))
        .WillRepeatedly(testing::Return(1));

    EXPECT_NO_THROW({
        deviceUpdater.processPassCompTableResponse(
            eid, requestMsg, sizeof(struct pldm_pass_component_table_resp));
    });
}

TEST_F(DeviceUpdaterTestWithMockedFirmwareUpdateFunctions,
       updateComponent_decode_update_component_resp_rc)
{
    mctp_eid_t eid = 0;
    size_t componentOffset = 0;
    DeviceUpdater deviceUpdater(eid, package, fwDeviceIDRecord, compImageInfos,
                                compInfo, compIdNameInfo, 512, &updateManager,
                                false);
    ComponentUpdater componentUpdater(eid, package, fwDeviceIDRecord,
                                      compImageInfos, compInfo, compIdNameInfo,
                                      512, &updateManager, &deviceUpdater,
                                      componentOffset, false);

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_update_component_resp)>
        updateComponentReq{0x8A, 0x05, 0x15, 0x00, 0x00, 0x00};

    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(updateComponentReq.data());

    EXPECT_CALL(*_mockedFirmwareUpdateFunction,
                decode_update_component_resp(_, _, _, _, _, _, _))
        .WillRepeatedly(testing::Return(1));

    EXPECT_NO_THROW({
        componentUpdater.processUpdateComponentResponse(
            eid, requestMsg, sizeof(struct pldm_update_component_resp));
    });
}

TEST_F(DeviceUpdaterTestWithMockedFirmwareUpdateFunctions,
       applyComplete_encode_apply_complete_resp_rc)
{
    mctp_eid_t eid = 0;
    size_t componentOffset = 0;
    DeviceUpdater deviceUpdater(eid, package, fwDeviceIDRecord, compImageInfos,
                                compInfo, compIdNameInfo, 512, &updateManager,
                                false);
    ComponentUpdater componentUpdater(eid, package, fwDeviceIDRecord,
                                      compImageInfos, compInfo, compIdNameInfo,
                                      512, &updateManager, &deviceUpdater,
                                      componentOffset, false);

    constexpr std::array<uint8_t,
                         sizeof(pldm_msg_hdr) + sizeof(pldm_apply_complete_req)>
        applyComplete1{0x00, 0x00, 0x00, 0x01, 0x30, 0x00};

    auto requestMsg1 = reinterpret_cast<const pldm_msg*>(applyComplete1.data());
    componentUpdater.componentUpdaterState.set(
        ComponentUpdaterSequence::ApplyComplete);

    EXPECT_CALL(*_mockedFirmwareUpdateFunction,
                decode_apply_complete_req(_, _, _, _))
        .WillRepeatedly(testing::Return(1));

    EXPECT_CALL(*_mockedFirmwareUpdateFunction,
                encode_apply_complete_resp(_, _, _, _))
        .WillRepeatedly(testing::Return(1));

    EXPECT_NO_THROW({
        componentUpdater.applyComplete(requestMsg1,
                                       sizeof(pldm_apply_complete_req));
    });
    constexpr std::array<uint8_t,
                         sizeof(pldm_msg_hdr) + sizeof(pldm_apply_complete_req)>
        applyComplete2{0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
    auto requestMsg2 = reinterpret_cast<const pldm_msg*>(applyComplete2.data());
    EXPECT_NO_THROW({
        componentUpdater.applyComplete(requestMsg2,
                                       sizeof(pldm_apply_complete_req));
    });
}

TEST_F(DeviceUpdaterTestWithMockedFirmwareUpdateFunctions,
       requestUpdate_decode_request_update_resp_rc)
{
    mctp_eid_t eid = 0;

    DeviceUpdater deviceUpdater(eid, package, fwDeviceIDRecord, compImageInfos,
                                compInfo, compIdNameInfo, 512, &updateManager,
                                false);

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_request_firmware_data_req)>
        reqFwDataReq{0x8A, 0x05, 0x15, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x02, 0x00, 0x00};

    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    EXPECT_CALL(*_mockedFirmwareUpdateFunction,
                encode_request_update_req(_, _, _, _, _, _, _, _, _, _))
        .WillRepeatedly(testing::Return(0));

    EXPECT_CALL(*_mockedFirmwareUpdateFunction,
                decode_request_update_resp(_, _, _, _, _))
        .WillRepeatedly(testing::Return(1));

    EXPECT_NO_THROW({
        deviceUpdater.processRequestUpdateResponse(
            eid, requestMsg, sizeof(struct pldm_request_update_resp));
    });
}

TEST_F(DeviceUpdaterTestWithMockedFirmwareUpdateFunctions,
       sendUpdateComponentRequest_encode_update_component_req_rc)
{
    mctp_eid_t eid = 0;
    size_t componentOffset = 0;

    DeviceUpdater deviceUpdater(eid, package, fwDeviceIDRecord, compImageInfos,
                                compInfo, compIdNameInfo, 512, &updateManager,
                                false);
    ComponentUpdater componentUpdater(eid, package, fwDeviceIDRecord,
                                      compImageInfos, compInfo, compIdNameInfo,
                                      512, &updateManager, &deviceUpdater,
                                      componentOffset, false);

    EXPECT_CALL(*_mockedFirmwareUpdateFunction,
                encode_update_component_req(_, _, _, _, _, _, _, _, _, _, _, _))
        .WillRepeatedly(testing::Return(1));

    EXPECT_NO_THROW(
        { componentUpdater.sendUpdateComponentRequest(componentOffset); });
}