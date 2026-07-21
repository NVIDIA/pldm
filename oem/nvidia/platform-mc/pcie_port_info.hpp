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

#include <cstddef>
#include <cstdint>
#include <string>

namespace pldm
{
namespace oem_nvidia
{

/**
 * @brief Handle the OEM PLDM "PCIePortInfo" event (event class 0xF4).
 *
 * SatMC sends this dedicated slim event carrying, per PCIe root port, the
 * current/max link speed and width. This handler decodes the payload and
 * publishes the values on a per-RP inventory D-Bus object composing the
 * existing PortInfo (speed, Gbps) and PortWidth (width, lanes) decorators,
 * with the same chassis/CPU association as the PCIe port. It is fully
 * self-contained (owns its own objects on the daemon bus obtained via
 * pldm::utils::DBusHandler::getBus()) and does not modify existing PCIe port
 * link-state objects.
 *
 * Wire payload — a fixed pcie_rp_link_info[PCIE_MAX_RP_NUM] array
 * (PCIE_MAX_RP_NUM = 48); each record is 7 packed bytes:
 *   bool     is_enabled      whether the root port is enabled
 *   bool     data_valid      whether link info was read successfully
 *   uint8_t  rp_num          root port (link) number, 0-47
 *   uint8_t  link_width      current negotiated link width (lanes)
 *   uint8_t  link_speed      current negotiated link speed (GT/s)
 *   uint8_t  max_link_speed  maximum supported link speed (GT/s)
 *   uint8_t  max_link_width  maximum supported link width (lanes)
 * Records with data_valid == false are skipped.
 *
 * @param[in] terminus      Terminus name (e.g. "ProcessorModule_0")
 * @param[in] eventData     Pointer to the OEM event payload
 * @param[in] eventDataSize Size of the payload in bytes
 * @return true on success, false on a hard failure (empty/invalid payload)
 */
bool handlePciePortInfoEvent(const std::string& terminus,
                             const uint8_t* eventData, size_t eventDataSize);

} // namespace oem_nvidia
} // namespace pldm
