/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved.
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
#include "common/utils.hpp"
#include "pldmd/dbus_impl_requester.hpp"

using namespace pldm;

class DBusInterfaceFuzzTest
{
  public:
    DBusInterfaceFuzzTest() :
        bus(sdbusplus::bus::new_default()),
        dbusImpl(bus, "/xyz/openbmc_project/pldm")
    {}

    void fuzzDBusMessages(const uint8_t* data, size_t size)
    {
        if (size < sizeof(pldm_msg_hdr))
            return;

        // Fuzz D-Bus message handling
        std::vector<uint8_t> message(data, data + size);
        dbusImpl.sendRecvPldmMessage(message);
    }

  private:
    sdbusplus::bus_t bus;
    dbus_api::Requester dbusImpl;
};

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    DBusInterfaceFuzzTest test;
    test.fuzzDBusMessages(data, size);
    return 0;
}
