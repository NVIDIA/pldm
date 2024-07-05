/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION &
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

#include <phosphor-logging/lg2.hpp>
#include <xyz/openbmc_project/State/ServiceReady/common.hpp>
#include <xyz/openbmc_project/State/ServiceReady/server.hpp>

#include <memory>

using ServiceReadyIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::State::server::ServiceReady>;

class PldmServiceReadyIntf
{
  public:
    // Singleton access method that simply returns the instance
    static PldmServiceReadyIntf& getInstance()
    {
        if (!instance)
        {
            throw std::runtime_error(
                "PldmServiceReadyIntf instance is not initialized yet");
        }
        return *instance;
    }

    // Initialization method to create and setup the singleton instance
    static void initialize(sdbusplus::bus::bus& bus, const char* path)
    {
        if (instance)
        {
            throw std::logic_error(
                "Initialize called on an already initialized PldmServiceReadyIntf");
        }
        static PldmServiceReadyIntf inst(bus, path);
        instance = &inst;
    }

    PldmServiceReadyIntf(const PldmServiceReadyIntf&) = delete;
    PldmServiceReadyIntf& operator=(const PldmServiceReadyIntf&) = delete;
    PldmServiceReadyIntf(PldmServiceReadyIntf&&) = delete;
    PldmServiceReadyIntf& operator=(PldmServiceReadyIntf&&) = delete;

    void setStateEnabled()
    {
        serviceIntf->state(ServiceReadyIntf::States::Enabled);
    }

    void setStateStarting()
    {
        serviceIntf->state(ServiceReadyIntf::States::Starting);
    }

  private:
    // Private constructor to prevent direct instantiation
    PldmServiceReadyIntf(sdbusplus::bus::bus& bus, const char* path)
    {
        serviceIntf = std::make_unique<ServiceReadyIntf>(bus, path);
        serviceIntf->state(ServiceReadyIntf::States::Starting);
        serviceIntf->serviceType(ServiceReadyIntf::ServiceTypes::PLDM);
    }

    static inline PldmServiceReadyIntf* instance = nullptr;

    std::unique_ptr<ServiceReadyIntf> serviceIntf = nullptr;
};
