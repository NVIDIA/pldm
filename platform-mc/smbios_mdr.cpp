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
#include "event_manager.hpp"
#include "smbios_mdr.hpp"

namespace mdr
{
namespace fs = std::filesystem;

bool saveSmbiosData(uint16_t smbiosEventDataLength, uint8_t* smbiosEventData) {
    std::string defaultDir =
        std::filesystem::path(mdr::defaultFile).parent_path();
    mdr::MDRSMBIOSHeader mdrHdr;
    mdrHdr.dirVer = mdr::dirVersion;
    mdrHdr.mdrType = mdr::typeII;
    mdrHdr.timestamp = std::time(nullptr);
    mdrHdr.dataSize = smbiosEventDataLength;

    std::string dirName{defaultDir};
    auto dirStatus = fs::status(dirName);
    if (fs::exists(dirStatus))
    {
        if (!fs::is_directory(dirStatus))
        {
            lg2::error("Failed to create {DIRNAME} directory", "DIRNAME",
                       dirName);
            return false;
        }
    }
    else
    {
        fs::create_directory(dirName);
    }

    try
    {
        std::ofstream smbiosFile(mdr::defaultFile,
                                 std::ios_base::binary | std::ios_base::trunc);
        if (!smbiosFile.good())
        {
            lg2::error("Failed to open SMBIOS table file");
            return false;
        }
        smbiosFile.exceptions(std::ofstream::badbit | std::ofstream::failbit);
        smbiosFile.write(reinterpret_cast<char*>(&mdrHdr),
                         sizeof(mdr::MDRSMBIOSHeader));
        smbiosFile.write(reinterpret_cast<char*>(smbiosEventData),
                         mdrHdr.dataSize);
        smbiosFile.close();
    }
    catch (const std::ofstream::failure& e)
    {
        lg2::error("Failed to write SMBIOS data, error={ERROR}", "ERROR", e.what());
        return false;
    }

    return true;
}

bool syncSmbiosData()
{
    bool status = false;
    auto& bus = pldm::utils::DBusHandler::getBus();

    try
    {
        auto method = bus.new_method_call(
            mdr::service, mdr::objectPath,
            mdr::interface, "AgentSynchronizeData");
        auto reply = bus.call(method);
        reply.read(status);
    }
    catch (const std::exception& e)
    {
        lg2::error("Error Sync data with service"
                   " ERROR={ERROR}, SERVICE={SERVICE}, PATH={PATH}",
                   "ERROR", e.what(), "SERVICE", mdr::service, "PATH", mdr::objectPath);
        return false;
    }

    if (!status)
    {
        lg2::error("Sync data with service failure");
        return false;
    }

    return true;
}
} // namespace mdr
