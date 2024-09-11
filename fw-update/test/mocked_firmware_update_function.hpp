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
#pragma once

#include "libpldm/firmware_update.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using namespace ::testing;
using ::testing::Return;

#define MOCK_METHOD11(m, ...)                                                  \
    GMOCK_INTERNAL_MOCK_METHODN(, , m, 11, __VA_ARGS__)

#define MOCK_METHOD12(m, ...)                                                  \
    GMOCK_INTERNAL_MOCK_METHODN(, , m, 12, __VA_ARGS__)

class MockedFirmwareUpdateFunction
{
  public:
    virtual ~MockedFirmwareUpdateFunction()
    {}

    MOCK_METHOD10(encode_request_update_req,
                  int(uint8_t, uint32_t, uint16_t, uint8_t, uint16_t, uint8_t,
                      uint8_t, const struct variable_field*, struct pldm_msg*,
                      size_t));

    MOCK_METHOD4(encode_request_firmware_data_resp,
                 int(uint8_t, uint8_t, struct pldm_msg*, size_t));

    MOCK_METHOD4(decode_request_firmware_data_req,
                 int(const struct pldm_msg*, size_t, uint32_t*, uint32_t*));

    MOCK_METHOD11(encode_pass_component_table_req,
                  int(uint8_t, uint8_t, uint16_t, uint16_t, uint8_t, uint32_t,
                      uint8_t, uint8_t, const struct variable_field*,
                      struct pldm_msg*, size_t));

    MOCK_METHOD5(decode_pass_component_table_resp,
                 int(const struct pldm_msg*, size_t, uint8_t*, uint8_t*,
                     uint8_t*));

    MOCK_METHOD7(decode_update_component_resp,
                 int(const struct pldm_msg*, size_t, uint8_t*, uint8_t*,
                     uint8_t*, bitfield32_t*, uint16_t*));

    MOCK_METHOD4(decode_apply_complete_req,
                 int(const struct pldm_msg*, size_t, uint8_t*, bitfield16_t*));

    MOCK_METHOD4(encode_apply_complete_resp,
                 int(uint8_t, uint8_t, struct pldm_msg*, size_t));

    MOCK_METHOD5(decode_request_update_resp,
                 int(const struct pldm_msg*, size_t, uint8_t*, uint16_t*,
                     uint8_t*));

    MOCK_METHOD12(encode_update_component_req,
                  int(uint8_t, uint16_t, uint16_t, uint8_t, uint32_t, uint32_t,
                      bitfield32_t, uint8_t, uint8_t,
                      const struct variable_field*, struct pldm_msg*, size_t));
};