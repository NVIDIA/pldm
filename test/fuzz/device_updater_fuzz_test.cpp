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
#include "libpldm/firmware_update.h"

#include "common/utils.hpp"
#include "common_utils.hpp"
#include "fw-update/device_updater.hpp"
#include "fw-update/package_parser.hpp"
#include "fw-update/update_manager.hpp"
#include "pldmd/dbus_impl_requester.hpp"
#include "requester/handler.hpp"

#include <cassert>

// #include <sdeventplus/test/sdevent.hpp>

using namespace pldm;
using namespace pldm::fw_update;

class DeviceUpdaterFuzzTest
{

  public:
    DeviceUpdaterFuzzTest(bool (*dataReader)(char* value, int size) =
                              [](char* value, int size) -> bool {
        return (bool)std::cin.get(value, size);
    }) :
        package("./test_pkg", std::ios::binary | std::ios::in | std::ios::ate),
        event(sdeventplus::Event::get_default()),
        dbusImplRequester(pldm::utils::DBusHandler::getBus(),
                          "/xyz/openbmc_project/pldm"),
        reqHandler(event, dbusImplRequester, sockManager, false,
                   std::chrono::seconds(1), 2, std::chrono::milliseconds(100)),
        updateManager(event, reqHandler, dbusImplRequester, descriptorMap,
                      componentInfoMap, componentNameMap, true),
        readDataFun(dataReader)
    {
        char versionString1[11]{0};
        getFromCin(versionString1, 10);
        versionString1[10] = '\0';
        pldm_firmware_update_descriptor_types type;
        getFromCin(reinterpret_cast<char*>(&type),
                   sizeof(pldm_firmware_update_descriptor_types));

        std::vector<uint8_t> data(16);
        getFromCin(reinterpret_cast<char*>(data.data()), 16);

        char componentName1[12]{0};
        getFromCin(componentName1, 12);
        char componentName2[12]{0};
        getFromCin(componentName2, 12);

        char componentName3[12]{0};
        getFromCin(componentName3, 12);

        char componentName4[12]{0};
        getFromCin(componentName4, 12);
        fwDeviceIDRecord = {1, {0x00}, versionString1, {{type, data}}, {}};
        compImageInfos = {
            {10, 100, 0xFFFFFFFF, 0, 0, 139, 1024, versionString1}};
        compInfo = {
            {std::make_pair(10, 100), std::make_tuple(1, "comp1Version")}};
        compIdNameInfo = {{11, componentName1},
                          {55555, componentName2},
                          {12, componentName3},
                          {66666, componentName4}};
    }

    void validatePackage()
    {
        uintmax_t packageSize = package.tellg();
        package.seekg(0);
        std::vector<uint8_t> packageHeader(
            sizeof(pldm_package_header_information));
        package.read(reinterpret_cast<char*>(packageHeader.data()),
                     sizeof(pldm_package_header_information));

        auto pkgHeaderInfo =
            reinterpret_cast<const pldm_package_header_information*>(
                packageHeader.data());
        auto pkgHeaderInfoSize = sizeof(pldm_package_header_information) +
                                 pkgHeaderInfo->package_version_string_length;
        packageHeader.clear();
        packageHeader.resize(pkgHeaderInfoSize);
        package.seekg(0);
        package.read(reinterpret_cast<char*>(packageHeader.data()),
                     pkgHeaderInfoSize);
        auto parser = parsePkgHeader(packageHeader);

        package.seekg(0);
        packageHeader.resize(parser->pkgHeaderSize);
        package.read(reinterpret_cast<char*>(packageHeader.data()),
                     parser->pkgHeaderSize);
        parser->parse(packageHeader, packageSize);
    }

    void readPackage512B()
    {
        DeviceUpdater deviceUpdater(0, package, fwDeviceIDRecord,
                                    compImageInfos, compInfo, compIdNameInfo,
                                    512, &updateManager, false);
        constexpr std::array<uint8_t,
                             sizeof(pldm_msg_hdr) +
                                 sizeof(pldm_request_firmware_data_req)>
            reqFwDataReq{0x8A, 0x05, 0x15, 0x00, 0x00, 0x00,
                         0x00, 0x00, 0x02, 0x00, 0x00};
        auto requestMsg =
            reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());
        auto response = deviceUpdater.requestFwData(
            requestMsg, sizeof(pldm_request_firmware_data_req));
    }

    std::ifstream package;
    FirmwareDeviceIDRecord fwDeviceIDRecord;
    ComponentImageInfos compImageInfos;
    ComponentInfo compInfo;
    ComponentIdNameMap compIdNameInfo;
    sdeventplus::Event event;
    pldm::dbus_api::Requester dbusImplRequester;
    pldm::mctp_socket::Manager sockManager;
    requester::Handler<requester::Request> reqHandler;
    DescriptorMap descriptorMap;
    ComponentInfoMap componentInfoMap;
    ComponentNameMap componentNameMap;
    UpdateManager updateManager;
    bool (*readDataFun)(char* value, int size);
};

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    DeviceUpdaterFuzzTest duft;
    duft.validatePackage();
    duft.readPackage512B();
}
