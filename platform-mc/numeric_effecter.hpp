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

#include "libpldm/platform.h"
#include "libpldm/requester/pldm.h"

#include "common/types.hpp"
#include "platform-mc/numeric_effecter_base_unit.hpp"
#include "platform-mc/oem_base.hpp"
#include "requester/handler.hpp"

#include <sdbusplus/server/object.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/Area/server.hpp>
#include <xyz/openbmc_project/Sensor/Value/server.hpp>
#include <xyz/openbmc_project/State/Decorator/Availability/server.hpp>
#include <xyz/openbmc_project/State/Decorator/OperationalStatus/server.hpp>

namespace pldm
{
namespace platform_mc
{

class TerminusManager;

using namespace std::chrono;
using namespace pldm::pdr;
using SensorUnit = sdbusplus::xyz::openbmc_project::Sensor::server::Value::Unit;
using Associations =
    std::vector<std::tuple<std::string, std::string, std::string>>;
using StateType = sdbusplus::xyz::openbmc_project::State::Decorator::server::
    OperationalStatus::StateType;
using ValueIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Sensor::server::Value>;
using OperationalStatusIntf =
    sdbusplus::server::object_t<sdbusplus::xyz::openbmc_project::State::
                                    Decorator::server::OperationalStatus>;
using AvailabilityIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::State::Decorator::server::Availability>;
using AssociationDefinitionsInft = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Association::server::Definitions>;
using PhysicalContextType = sdbusplus::xyz::openbmc_project::Inventory::
    Decorator::server::Area::PhysicalContextType;
using InventoryDecoratorAreaIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Inventory::Decorator::server::Area>;

/**
 * @brief NumericEffecter
 *
 * This class handles sensor reading updated by sensor manager and export
 * status to D-Bus interface.
 */
class NumericEffecter
{
  public:
    NumericEffecter(const tid_t tid, const bool effecterDisabled,
                    std::shared_ptr<pldm_numeric_effecter_value_pdr> pdr,
                    std::string& effecerName, std::string& associationPath,
                    TerminusManager& terminusManager);
    ~NumericEffecter(){};

    /** @brief The function called by Sensor Manager to set sensor to
     * error status.
     */
    void handleErrGetNumericEffecterValue();

    /** @brief Updating the effecter status to D-Bus interface
     */
    void updateValue(pldm_effecter_oper_state effecterOperState,
                     double pendingValue, double presentValue);

    /** @brief Getting the effecter state
     */
    StateType state()
    {
        return operationalStatusIntf->state();
    }

    /** @brief Sending setNumericEffecterEnable command for the effecter
     *
     *  @param[in] state - the effecter state to be set
     */
    requester::Coroutine
        setNumericEffecterEnable(pldm_effecter_oper_state state);

    /** @brief Sending setNumericEffecterValue command for the effecter
     *
     *  @param[in] effecterValue - the effecter value to be set
     */
    requester::Coroutine setNumericEffecterValue(double effecterValue);

    /** @brief Sending getNumericEffecterValue command for the effecter
     */
    requester::Coroutine getNumericEffecterValue();

    /**
     * raw: raw value, read from/set to effecter.
     * unit: effecter unit value, converted from raw value with conversion
     * formula.
     * base: base unit value, removing unitModifier from effecter unit value.
     * e.g. baseUnit=Volts(V), unitModifier=-3, resolution=5, offset=0.
     * effecter unit = millivolts(mV).
     * raw value 1000 == effecter unit value 5000 mV == base unit value 5 V.
     */
    /** @brief rawToUnit is used to convert raw value to effecter unit
     *
     *  @param[in] value - raw value
     *  @return double - converted effecter unit value
     */
    double rawToUnit(double value);

    /** @brief unitToRaw is used to convert effecter units to raw value
     *
     *  @param[in] value - effecter unit value
     *  @return double - converted raw value
     */
    double unitToRaw(double value);

    /** @brief unitToBase is used to convert effecter unit value to base unit
     * value
     *
     *  @param[in] value - base unit value
     *  @return double - converted base unit value
     */
    double unitToBase(double value);

    /** @brief BaseToUnit is used to convert base unit value to effecter unit
     * value
     *
     *  @param[in] value - base unit value
     *  @return double - converted effecter unit value
     */
    double baseToUnit(double value);

    inline double rawToBase(double value)
    {
        return unitToBase(rawToUnit(value));
    }

    inline double baseToRaw(double value)
    {
        return unitToRaw(baseToUnit(value));
    }

    /** @brief Get the ContainerID, EntityType, EntityInstance of the PLDM
     * Entity which the sensor belongs to
     *  @return EntityInfo - Entity ID
     */
    inline auto getEntityInfo()
    {
        return entityInfo;
    }

    /** @brief Updating the association to D-Bus interface
     *  @param[in] inventoryPath - inventory path of the entity
     */
    inline void setInventoryPaths(const std::vector<std::string>& inventoryPath)
    {
        if (associationDefinitionsIntf)
        {
            std::map<std::pair<std::string, std::string>, bool> assocMap;
            Associations assocs{};

            auto associations = associationDefinitionsIntf->associations();
            for (auto& association : associations)
            {
                auto& [forward, reverse, objectPath] = association;
                auto iter = assocMap.find(std::make_pair(forward, reverse));
                if (iter == assocMap.end())
                {
                    for (const auto& path : inventoryPath)
                    {
                        assocs.emplace_back(std::make_tuple(
                            forward.c_str(), reverse.c_str(), path.c_str()));
                    }
                    assocMap[{forward, reverse}] = true;
                }
            }
            associationDefinitionsIntf->associations(assocs);
        }
    }

    /** @brief Getter of value member variable */
    double getValue()
    {
        return value;
    }

    /** @brief Setter of value member variable */
    void setValue(double v)
    {
        value = v;
    }

    /** @brief getter of baseUnit member variable */
    uint8_t getBaseUnit()
    {
        return baseUnit;
    }

    /** @brief Updating the physicalContext to D-Bus interface
     *  @param[in] type - physical context type
     */
    inline void setPhysicalContext(PhysicalContextType type)
    {
        if (inventoryDecoratorAreaIntf)
        {
            inventoryDecoratorAreaIntf->physicalContext(type);
        }
    }

    /** @brief get the association */
    auto getAssociation() const
    {
        return associationDefinitionsIntf->associations();
    }

    /** @brief Terminus ID which the sensor belongs to */
    tid_t tid;

    /** @brief Effecter ID */
    uint16_t effecterId;

    /** @brief ContainerID, EntityType, EntityInstance of the PLDM Entity which
     * the sensor belongs to */
    EntityInfo entityInfo;

    /** @brief  The PLDM defined effecterDataSize enum */
    uint8_t dataSize;

    /** @brief  The DBus path of effecter */
    std::string path;

    /** @brief  A container to store OemIntf, it allows us to add additional OEM
     * sdbusplus object as extra attribute */
    std::vector<std::shared_ptr<platform_mc::OemIntf>> oemIntfs;

    /** @brief NumericEffecterBaseUnit is a base class of all different units,
     * it provides uniform APIs to NumericEffecter */
    std::unique_ptr<NumericEffecterBaseUnit> unitIntf = nullptr;

    /** @brief flag to update the value once */
    bool needUpdate;

  private:
    std::unique_ptr<AvailabilityIntf> availabilityIntf = nullptr;
    std::unique_ptr<OperationalStatusIntf> operationalStatusIntf = nullptr;
    std::unique_ptr<AssociationDefinitionsInft> associationDefinitionsIntf =
        nullptr;
    std::unique_ptr<InventoryDecoratorAreaIntf> inventoryDecoratorAreaIntf =
        nullptr;

    /** @brief The resolution of sensor in Units */
    double resolution;

    /** @brief A constant value that is added in as part of conversion process
     * of converting a raw sensor reading to Units */
    double offset;

    /** @brief A power-of-10 multiplier for baseUnit */
    int8_t unitModifier;

    /** @brief reference of TerminusManager */
    TerminusManager& terminusManager;

    /** @brief raw value of numeric effecter */
    double value;

    /** @brief baseUnit of numeric effecter */
    uint8_t baseUnit;
};
} // namespace platform_mc
} // namespace pldm
