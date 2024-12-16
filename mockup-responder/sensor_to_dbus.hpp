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

#include "common/types.hpp"

#include <boost/asio.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/asio/sd_event.hpp>

#include <string>
#include <vector>

/**
 * @class Sensor
 * @brief Represents a sensor and manages its state and properties.
 *
 * This class allows for managing a sensor's properties, including its state,
 * value, and D-Bus interfaces.
 */
class Sensor
{
  public:
    /**
     * @brief Constructor
     *
     * @param sensorId The unique identifier for the sensor.
     * @param server The D-Bus object server to which the sensor interfaces are
     * added.
     */
    Sensor(uint16_t sensorId, sdbusplus::asio::object_server& server);

    /**
     * @brief Updates the sensor's operational state.
     *
     * @param newState The new state to be set for the sensor.
     */
    void updateState(const std::string& newState);

    uint16_t sensorId;
    double value;
    uint8_t composite_count;
    bool functionalValue;
    std::string stateValue;
    std::shared_ptr<sdbusplus::asio::dbus_interface> iface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> operationalIface;
};

/**
 * @class Effecter
 * @brief Represents an effecter and manages its state and properties.
 *
 * This class allows for managing an effecter's properties, including its state,
 * value, and D-Bus interfaces.
 */
class Effecter
{
  public:
    /**
     * @brief Constructor
     *
     * @param effecterId The unique identifier for the effecter.
     * @param server The D-Bus object server to which the effecter interfaces
     * are added.
     */
    Effecter(uint16_t effecterId, sdbusplus::asio::object_server& server);

    /**
     * @brief Updates the effecter's operational state.
     *
     * @param newState The new state to be set for the effecter.
     */
    void updateState(const std::string& newState);

    uint16_t effecterId;
    double value;
    uint8_t composite_count;
    bool functionalValue;
    std::string stateValue;
    std::shared_ptr<sdbusplus::asio::dbus_interface> iface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> operationalIface;
};

extern std::vector<std::shared_ptr<Sensor>> sensors;
extern std::vector<std::shared_ptr<Effecter>> effecters;
