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

#include "platform-mc/sensor_manager.hpp"

#include <gmock/gmock.h>

namespace pldm
{
namespace platform_mc
{

class MockSensorManager : public SensorManager
{
  public:
    MockSensorManager(sdeventplus::Event& event,
                      TerminusManager& terminusManager,
                      std::map<mctp_eid_t, std::shared_ptr<Terminus>>& termini,
                      Manager* manager) :
        SensorManager(event, terminusManager, termini, manager){};

    MOCK_METHOD(void, doSensorPolling, (tid_t tid), (override));
};

} // namespace platform_mc
} // namespace pldm