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
#include "sensor_to_dbus.hpp"

std::string operationalStatusPath =
    "xyz.openbmc_project.State.Decorator.OperationalStatus.StateType.";
Sensor::Sensor(uint16_t sensorId, sdbusplus::asio::object_server& server) :
    sensorId(sensorId), composite_count(1), functionalValue(true),
    stateValue(operationalStatusPath + "Enabled")
{
    value = 0;
    std::string path =
        "/xyz/openbmc_project/sensors/id_" + std::to_string(sensorId);

    iface = server.add_interface(path, "xyz.openbmc_project.reading");
    iface->register_property(
        "value", value,
        [&](const double& req, double& propertyValue) {
            value = req;
            propertyValue = value;
            return true;
        },
        [&](const double& /*property*/) { return value; });
    iface->initialize();

    operationalIface = server.add_interface(
        path, "xyz.openbmc_project.State.Decorator.OperationalStatus");

    operationalIface->register_property(
        "State", stateValue,
        [&](const std::string& req, std::string& propertyValue) {
            stateValue = req;
            propertyValue = stateValue;
            return true;
        },
        [&](const std::string& /*property*/) { return stateValue; });

    operationalIface->register_property(
        "Functional", functionalValue,
        [&](const bool& req, bool& propertyValue) {
            functionalValue = req;
            propertyValue = functionalValue;
            return true;
        },
        [&](const bool& /*property*/) { return functionalValue; });

    operationalIface->initialize();
}

void Sensor::updateState(const std::string& newState)
{
    stateValue = operationalStatusPath + newState;
    operationalIface->set_property("State", stateValue);
}

Effecter::Effecter(uint16_t effecterId,
                   sdbusplus::asio::object_server& server) :
    effecterId(effecterId),
    composite_count(1), functionalValue(true),
    stateValue(operationalStatusPath + "Enabled")
{
    value = 0;
    std::string path =
        "/xyz/openbmc_project/effecters/id_" + std::to_string(effecterId);

    iface = server.add_interface(path, "xyz.openbmc_project.reading");
    iface->register_property(
        "value", value,
        [&](const double& req, double& propertyValue) {
            value = req;
            propertyValue = value;
            return true;
        },
        [&](const double& /*property*/) { return value; });
    iface->initialize();

    operationalIface = server.add_interface(
        path, "xyz.openbmc_project.State.Decorator.OperationalStatus");

    operationalIface->register_property(
        "State", stateValue,
        [&](const std::string& req, std::string& propertyValue) {
            stateValue = req;
            propertyValue = stateValue;
            return true;
        },
        [&](const std::string& /*property*/) { return stateValue; });

    operationalIface->register_property(
        "Functional", functionalValue,
        [&](const bool& req, bool& propertyValue) {
            functionalValue = req;
            propertyValue = functionalValue;
            return true;
        },
        [&](const bool& /*property*/) { return functionalValue; });

    operationalIface->initialize();
}

void Effecter::updateState(const std::string& newState)
{
    stateValue = operationalStatusPath + newState;
    operationalIface->set_property("State", stateValue);
}
