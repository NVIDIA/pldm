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
#include "libpldm/requester/pldm.h"

#include "common/types.hpp"
#include "fw_update_utility.hpp"
#include "mockup_responder.hpp"
#include "pldmd/dbus_impl_requester.hpp"
#include "requester/handler.hpp"
#include "requester/mctp_endpoint_discovery.hpp"

#include <err.h>
#include <getopt.h>

#include <boost/algorithm/string.hpp>
#include <phosphor-logging/lg2.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/server.hpp>
#include <sdeventplus/event.hpp>

#include <iostream>

using namespace phosphor::logging;

void optionUsage(void)
{
    std::cerr << "Usage: mockup_responder [options]\n";
    std::cerr << "Options:\n";
    std::cerr
        << " [--verbose] - would enable verbosity\n"
        << " [--eid <EID>] - assign EID to mockup responder\n"
        << " [--pdrFile <Path>] - path to PDR file\n"
        << " [--terminusMaxBufferSize <size>] - set the terminus max buffer size\n";
}

bool uuidStringToBytes(const std::string& uuidStr, uint8_t uuid[16])
{
    if (uuidStr.length() != 36 || uuidStr[8] != '-' || uuidStr[13] != '-' ||
        uuidStr[18] != '-' || uuidStr[23] != '-')
    {
        return false;
    }

    std::string hexStr = uuidStr;
    hexStr.erase(std::remove(hexStr.begin(), hexStr.end(), '-'), hexStr.end());

    if (hexStr.length() != 32)
    {
        return false;
    }

    for (int i = 0; i < 16; ++i)
    {
        uuid[i] = static_cast<uint8_t>(
            std::stoul(hexStr.substr(i * 2, 2), nullptr, 16));
    }

    return true;
}

int main(int argc, char** argv)
{
    bool verbose = false;
    int eid = 0;
    uint16_t terminusMaxBufferSize = 0;
    int argflag;
    std::string pdrpath;
    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"verbose", no_argument, 0, 'v'},
        {"eid", required_argument, 0, 'e'},
        {"pdrFile", required_argument, 0, 'p'},
        {"terminusMaxBufferSize", required_argument, 0, 's'},
        {0, 0, 0, 0}};

    while ((argflag = getopt_long(argc, argv, "hve:p:s:", long_options,
                                  nullptr)) >= 0)
    {
        switch (argflag)
        {
            case 'h':
                optionUsage();
                exit(EXIT_FAILURE);
                break;
            case 'v':
                verbose = true;
                break;
            case 'e':
                eid = std::stoi(optarg);
                if (eid < 0 || eid > 255)
                {
                    optionUsage();
                    exit(EXIT_FAILURE);
                }
                break;
            case 'p':
                pdrpath = optarg;
                break;
            case 's':
                terminusMaxBufferSize =
                    static_cast<uint16_t>(std::stoi(optarg));
                break;
            default:
                exit(EXIT_FAILURE);
        }
    }

    if (verbose)
    {
        lg2::info("start a Mockup Responder EID={EID}", "EID", eid);
        lg2::info("PDR file path={PATH}", "PATH", pdrpath);
        lg2::info("Terminus Max Buffer Size={SIZE}", "SIZE",
                  terminusMaxBufferSize);
    }

    try
    {
        boost::asio::io_context io;
        auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
        sdbusplus::asio::object_server objServer(systemBus);
        auto bus = sdbusplus::bus::new_default();
        auto event = sdeventplus::Event::get_default();

        bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);
        sdbusplus::server::manager::manager objManager(bus, "/");
        std::string serviceName =
            "xyz.openbmc_project.PLDM.eid_" + std::to_string(eid);
        bus.request_name(serviceName.c_str());

        std::string service = "xyz.openbmc_project.MCTP.Control.PCIe";
        std::string objectPath =
            "/xyz/openbmc_project/mctp/0/" + std::to_string(eid);
        std::string interface = "xyz.openbmc_project.Common.UUID";
        std::string property = "UUID";

        auto methodCall =
            bus.new_method_call(service.c_str(), objectPath.c_str(),
                                "org.freedesktop.DBus.Properties", "Get");

        methodCall.append(interface, property);

        auto reply = bus.call(methodCall);

        std::variant<std::string> uuidVariant;
        reply.read(uuidVariant);

        std::string uuidStr = std::get<std::string>(uuidVariant);
        uint8_t uuid[16] = {0};

        if (!uuidStringToBytes(uuidStr, uuid))
        {
            lg2::error("Unable to fetch UUID");
            // If unable to fetch UUID, assign default value
            uint8_t defaultUuid[16] = {0x11, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x11};
            std::memcpy(uuid, defaultUuid, sizeof(uuid));
        }

        MockupResponder::MockupResponder mockupResponder(
            verbose, event, objServer, eid, pdrpath, terminusMaxBufferSize,
            uuid);
        return event.loop();
    }
    catch (const std::exception& e)
    {
        lg2::error("Exception: {HANDLER_EXCEPTION}", "HANDLER_EXCEPTION",
                   e.what());
        exit(EXIT_FAILURE);
    }
    exit(EXIT_SUCCESS);
}