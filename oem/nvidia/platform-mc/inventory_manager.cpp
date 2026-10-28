/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "inventory_manager.hpp"

namespace pldm::platform_mc::oem_nvidia
{

void InventoryManager::reset()
{
    terminusData.clear();
}

void InventoryManager::setInstance(tid_t tid, std::optional<uint16_t> instance)
{
    terminusData[tid].instance = instance;
}

std::optional<uint16_t> InventoryManager::instance(tid_t tid) const
{
    auto itr = terminusData.find(tid);
    if (itr == terminusData.end())
    {
        return std::nullopt;
    }
    return itr->second.instance;
}

void InventoryManager::setTerminusName(tid_t tid, const std::string& name)
{
    terminusData[tid].name = name;
}

void InventoryManager::forget(tid_t tid)
{
    terminusData.erase(tid);
}

EntityTree::AddResult InventoryManager::addEntityAssociation(
    tid_t tid, std::span<const uint8_t> pdr)
{
    return terminusData[tid].entities.addPdr(pdr);
}

EntityTree& InventoryManager::entityTree(tid_t tid)
{
    return terminusData[tid].entities;
}

void InventoryManager::unbindAll(tid_t tid)
{
    auto itr = terminusData.find(tid);
    if (itr != terminusData.end())
    {
        itr->second.entities.unbindAll();
    }
}

} // namespace pldm::platform_mc::oem_nvidia
