/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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
#include "common/utils.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/server/object.hpp>
#include <sdeventplus/event.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>

namespace pldm
{
namespace platform_mc
{
using AssociationDefinitionsIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Association::server::Definitions>;

using AssociationsType =
    std::vector<std::tuple<std::string, std::string, std::string>>;
/**
 * @brief Entity
 *
 * Entity class holds the related D-Bus inventory path and its parent.
 */
class Entity
{
  public:
    Entity(std::string& inventory, std::string& ContainerInventory) :
        inventory(inventory), ContainerInventory(ContainerInventory)
    {}

    auto getInventory()
    {
        return inventory;
    }

    auto getClosestInventory()
    {
        if (inventory.size())
        {
            return inventory;
        }
        else
        {
            return ContainerInventory;
        }
    }

  private:
    void createAssociation()
    {
        if (inventory.size() == 0 || ContainerInventory.size() == 0)
        {
            inventoryAssociationInft.reset();
            return;
        }

        AssociationsType associations = {
            {"parent_chassis", "all_chassis", ContainerInventory.c_str()}};
        if (inventoryAssociationInft == nullptr)
        {
            try
            {
                inventoryAssociationInft =
                    std::make_unique<AssociationDefinitionsIntf>(
                        utils::DBusHandler::getBus(), inventory.c_str());
            }
            catch (const std::exception& e)
            {
                lg2::error(
                    "Failed to create AssociationDefinitionsIntf to {INVENTORY}.",
                    "INVENTORY", inventory, "ERROR", e);
            }
            if (inventoryAssociationInft == nullptr)
            {
                return;
            }
        }

        inventoryAssociationInft->associations(associations);

        return;
    }

    std::string inventory;
    std::string ContainerInventory;
    std::unique_ptr<AssociationDefinitionsIntf> inventoryAssociationInft =
        nullptr;
};
} // namespace platform_mc
} // namespace pldm
