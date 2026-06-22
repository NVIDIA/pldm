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
#include "../../../libpldm/base.h"

#include "../../../libpldmresponder/base.hpp"
#include "../../../pldmd/invoker.hpp"
#include "../../../pldmd/socket_handler.hpp"
#include "../../../pldmd/socket_manager.hpp"
#include "../../manager.hpp"
#include "common_utils.hpp"
#include "xyz/openbmc_project/Common/UnixSocket/server.hpp"
#include "xyz/openbmc_project/MCTP/Endpoint/server.hpp"
#include "xyz/openbmc_project/ObjectMapper/server.hpp"
#include "xyz/openbmc_project/PLDM/Requester/server.hpp"

#include <getopt.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <sdbusplus/bus.hpp>
#include <sdbusplus/sdbus.hpp>
#include <sdbusplus/server/interface.hpp>
#include <sdbusplus/vtable.hpp>
#include <sdeventplus/event.hpp>
#include <sdeventplus/source/io.hpp>
#include <sdeventplus/source/signal.hpp>
#include <stdplus/signal.hpp>

#include <chrono>
#include <thread>
#include <variant>

using namespace pldm;
using namespace sdeventplus;
using namespace pldm::responder;
using namespace pldm::utils;

using Endpoint_inherit = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::MCTP::server::Endpoint>;

using ActivationIntf = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Software::server::Activation>;

using ActivationProgressIntf = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Software::server::ActivationProgress>;

static constexpr auto MCTP_PATH = "/xyz/openbmc_project/mctp";
static constexpr auto MCTP_BUSNAME = "xyz.openbmc_project.MCTP.Control";
static constexpr auto PCIE_SOCK = "/tmp/pcie";

static constexpr auto MCTP_CTRL_NW_OBJ_PATH{"/xyz/openbmc_project/mctp/0/"};
static constexpr auto MCTP_CTRL_DBUS_EP_INTERFACE{
    "xyz.openbmc_project.MCTP.Endpoint"};
static constexpr auto MCTP_CTRL_DBUS_UUID_INTERFACE{
    "xyz.openbmc_project.Common.UUID"};
static constexpr auto MCTP_CTRL_DBUS_SOCK_INTERFACE{
    "xyz.openbmc_project.Common.UnixSocket"};

using ObjectMapper_inherit = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::server::ObjectMapper>;

static constexpr auto OBJECT_MAPPER_PATH = "/xyz/openbmc_project/object_mapper";
static constexpr auto OBJECT_MAPPER_BUSNAME =
    "xyz.openbmc_project.ObjectMapper";

static constexpr uint8_t EID_DEV = 100;
static constexpr uint8_t MCTP_MSG_TYPE_PLDM = 1;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond_var = PTHREAD_COND_INITIALIZER;
bool is_ready = false;
bool randomInput = false;
bool verbose = false;

#define LOG                                                                    \
    if (verbose)                                                               \
    std::cout

unsigned cmdCount = 1;
bool pldmFwUpdateDeviceUpdaterTest = false;
bool pldmBaseTest = false;
bool pldmInventoryOnlyTest = false;
bool pldmFlipRandCmd = false;
bool pldmKeepValidOutput = true;
std::string keepValidOutputUntil = "";

float flipProbability = 0.01;
long int recvTimeout = 500000;
std::string logPath = "";

struct ActivationMock : public ActivationIntf
{
    ActivationMock(sdbusplus::bus_t& bus, const char* path) :
        ActivationIntf(bus, path)
    {}

    Activations activation(Activations) override
    {
        std::cout << "[PLDMD_MOCKUP] activation" << std::endl;

        return ActivationIntf::activation(Activations::Active);
    }
};

struct MockObjectMapper : public ObjectMapper_inherit
{
    MockObjectMapper(sdbusplus::bus_t& bus, const char* path) :
        ObjectMapper_inherit(bus, path)
    {}

    std::map<std::string, std::vector<std::string>> getObject(
        std::string path, std::vector<std::string> interfaces) override
    {
        return std::map<std::string, std::vector<std::string>>();
    }

    std::map<std::string, std::map<std::string, std::vector<std::string>>>
        getAncestors(std::string path,
                     std::vector<std::string> interfaces) override
    {
        return std::map<std::string,
                        std::map<std::string, std::vector<std::string>>>();
    }

    std::map<std::string, std::map<std::string, std::vector<std::string>>>
        getSubTree(std::string subtree, int32_t depth,
                   std::vector<std::string> interfaces) override
    {
        return std::map<std::string,
                        std::map<std::string, std::vector<std::string>>>();
    }

    std::vector<std::string> getSubTreePaths(
        std::string subtree, int32_t depth,
        std::vector<std::string> interfaces) override
    {
        return std::vector<std::string>();
    }

    std::map<std::string, std::map<std::string, std::vector<std::string>>>
        getAssociatedSubTree(sdbusplus::object_path path,
                             sdbusplus::object_path associationPath,
                             int32_t depth,
                             std::vector<std::string> interfaces) override
    {
        return std::map<std::string,
                        std::map<std::string, std::vector<std::string>>>();
    }

    std::vector<std::string> getAssociatedSubTreePaths(
        std::string path, std::string associationPath, int32_t depth,
        std::vector<std::string> interfaces) override
    {
        return std::vector<std::string>();
    }
};
class MockMctpEndpoint : public Endpoint_inherit
{
  public:
    MockMctpEndpoint(sdbusplus::bus_t& bus, const char* path, int eid) :
        Endpoint_inherit(
            bus,
            (std::string(MCTP_CTRL_NW_OBJ_PATH) + std::to_string(eid)).c_str()),
        objmgr(bus, "/xyz/openbmc_project/mctp"),
        uuid(
            bus,
            (std::string(MCTP_CTRL_NW_OBJ_PATH) + std::to_string(eid)).c_str()),
        socket(
            bus,
            (std::string(MCTP_CTRL_NW_OBJ_PATH) + std::to_string(eid)).c_str())
    {
        (void)path;
        LOG << "MockMctpEndpoint" << std::endl;

        this->eid(eid);
    }

    sdbusplus::server::manager::manager objmgr;
    sdbusplus::xyz::openbmc_project::Common::server::UUID uuid;
    sdbusplus::xyz::openbmc_project::Common::server::UnixSocket socket;
};

size_t getMctpHeaderSize()
{
    return sizeof(EID_DEV) + sizeof(MCTP_MSG_TYPE_PLDM);
}

void encodeSetTidResp(void* ptr)
{
    pldm_msg_hdr hdr;

    hdr.type = 5;
    hdr.instance_id = 0;
    hdr.command = PLDM_SET_TID;
    hdr.header_ver = 0;
    hdr.datagram = 0;
    hdr.reserved = 0;

    memcpy((uint8_t*)ptr + getMctpHeaderSize(), &hdr, sizeof(pldm_msg_hdr));

    ((uint8_t*)ptr + getMctpHeaderSize() + sizeof(pldm_msg_hdr))[0] =
        PLDM_SUCCESS;
}

void flipRandomBits(std::vector<uint8_t>& data)
{
    for (auto& byte : data)
    {
        for (int i = 0; i < 8; i++)
        {
            int seed = getUint8t(randomInput);
            if (((long unsigned int)seed) < 255 * flipProbability)
            {
                byte ^= (1 << i);
            }
        }
    }
}

bool getRandomBool()
{
    return getUint8t(randomInput) > (sizeof(uint8_t) / 2);
}

void putMctpHeader(void* ptr)
{
    ((uint8_t*)(ptr))[0] = EID_DEV;
    ((uint8_t*)(ptr))[1] = MCTP_MSG_TYPE_PLDM;
}

void putPldmRandomPacket(void* ptr)
{
    pldm_msg_hdr hdr;

    putMctpHeader(ptr);

    // hdr.type = getRandomBool() ? PLDM_REQUEST : PLDM_RESPONSE;
    hdr.type = PLDM_REQUEST;
    hdr.instance_id = 0;
    hdr.command = getUint8t(randomInput);

    memcpy((uint8_t*)ptr + getMctpHeaderSize(), &hdr, sizeof(hdr));
}

// FW UPDATE COMMANDS
void encodeQueryDeviceIdentifiersResp(std::vector<uint8_t>& response)
{
    static constexpr size_t respPayloadLength1 = 49;
    static constexpr std::array<uint8_t,
                                sizeof(pldm_msg_hdr) + respPayloadLength1>
        queryDeviceIdentifiersResp1{
            0x00, 0x05, 0x01, 0x00, 0x2b, 0x00, 0x00, 0x00, 0x03, 0x01, 0x00,
            0x04, 0x00, 0x0a, 0x0b, 0x0c, 0x0d, 0x02, 0x00, 0x10, 0x00, 0x16,
            0x20, 0x23, 0xc9, 0x3e, 0xc5, 0x41, 0x15, 0x95, 0xf4, 0x48, 0x70,
            0x1d, 0x49, 0xd6, 0x75, 0xFF, 0xFF, 0x0B, 0x00, 0x01, 0x07, 0x4f,
            0x70, 0x65, 0x6e, 0x42, 0x4d, 0x43, 0x01, 0x02};
    auto responseMsg1 =
        reinterpret_cast<const pldm_msg*>(queryDeviceIdentifiersResp1.data());

    response.resize(
        getMctpHeaderSize() + sizeof(pldm_msg_hdr) + respPayloadLength1);

    memcpy(response.data() + getMctpHeaderSize(), responseMsg1,
           respPayloadLength1);
}

void encodeFirmwareParametersResp(std::vector<uint8_t>& response)
{
    constexpr size_t respPayloadLength1 = 119;
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + respPayloadLength1>
        getFirmwareParametersResp1{
            0x00, 0x05, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01,
            0x0c, 0x00, 0x00, 0x44, 0x65, 0x76, 0x69, 0x63, 0x65, 0x56, 0x65,
            0x72, 0x32, 0x2e, 0x30, 0x02, 0x00, 0x2e, 0x01, 0x28, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x43,
            0x6f, 0x6d, 0x70, 0x33, 0x76, 0x34, 0x2e, 0x30};
    auto responseMsg1 =
        reinterpret_cast<const pldm_msg*>(getFirmwareParametersResp1.data());

    response.resize(
        getMctpHeaderSize() + sizeof(pldm_msg_hdr) + respPayloadLength1);

    memcpy(response.data() + getMctpHeaderSize(), responseMsg1,
           respPayloadLength1);
}

void encodeQueryDownstreamDevices(std::vector<uint8_t>& response)
{
    response.resize(getMctpHeaderSize() + sizeof(pldm_msg_hdr));

    auto request =
        reinterpret_cast<pldm_msg*>(response.data() + getMctpHeaderSize());

    encode_pldm_header_only(PLDM_REQUEST, 0, PLDM_FWUP, 0x03, request);

    putMctpHeader(response.data());
}

void encodeRequestUpdate(std::vector<uint8_t>& response)
{
    size_t respLen =
        sizeof(pldm_msg_hdr) + sizeof(struct pldm_request_update_resp);

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(struct pldm_request_update_resp)>
        requestUpdateResponse1{0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x01};

    pldm_msg_hdr hdr;
    hdr.command = PLDM_REQUEST_UPDATE;
    hdr.request = 0;
    hdr.instance_id = 0;
    hdr.type = PLDM_FWUP;

    memcpy((void*)&requestUpdateResponse1, &hdr, sizeof(pldm_msg_hdr));

    response.resize(getMctpHeaderSize() + respLen);

    memcpy(response.data() + getMctpHeaderSize(), &requestUpdateResponse1,
           respLen);
}

void encodePassComponentTableResponse(std::vector<uint8_t>& response)
{
    size_t respLen =
        sizeof(pldm_msg_hdr) + sizeof(struct pldm_pass_component_table_resp);

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_pass_component_table_resp)>
        passCompTableResponse1{0x00, 0x00, 0x00, 0x00, 0x00, 0x01};

    pldm_msg_hdr hdr;
    hdr.command = PLDM_PASS_COMPONENT_TABLE;
    hdr.request = 0;
    hdr.instance_id = 0;
    hdr.type = PLDM_FWUP;

    memcpy((void*)&passCompTableResponse1, &hdr, sizeof(pldm_msg_hdr));

    response.resize(getMctpHeaderSize() + respLen);

    memcpy(response.data() + getMctpHeaderSize(), &passCompTableResponse1,
           respLen);
}

void encodeUpdateComponentResponse(std::vector<uint8_t>& response)
{
    size_t respLen =
        sizeof(pldm_msg_hdr) + sizeof(struct pldm_update_component_resp);

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_update_component_resp)>
        updateComponentResponse1{0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                 0x01, 0x00, 0x00, 0x00, 0x64, 0x00};

    pldm_msg_hdr hdr;
    hdr.command = PLDM_UPDATE_COMPONENT;
    hdr.request = 0;
    hdr.instance_id = 0;
    hdr.type = PLDM_FWUP;

    memcpy((void*)&updateComponentResponse1, &hdr, sizeof(pldm_msg_hdr));

    response.resize(getMctpHeaderSize() + respLen);

    memcpy(response.data() + getMctpHeaderSize(), &updateComponentResponse1,
           respLen);
}

void putDeviceUpdaterFirst512MBRequest(std::vector<uint8_t>& data)
{
    pldm_msg_hdr hdr;
    hdr.command = PLDM_REQUEST_FIRMWARE_DATA;
    hdr.request = 1;
    hdr.instance_id = 0;
    hdr.type = PLDM_FWUP;

    pldm_request_firmware_data_req req;
    req.length = getUint8t(randomInput) * 1024;
    req.offset = getUint8t(randomInput) * 1024; // fix me

    data.resize(getMctpHeaderSize() + sizeof(pldm_msg_hdr) +
                sizeof(pldm_request_firmware_data_req));

    memcpy(data.data() + getMctpHeaderSize(), &hdr, sizeof(pldm_msg_hdr));
    memcpy(data.data() + getMctpHeaderSize() + sizeof(pldm_msg_hdr), &req,
           sizeof(pldm_request_firmware_data_req));
}

void encodeTransferComplete(std::vector<uint8_t>& response)
{
    size_t respLen = sizeof(pldm_msg_hdr) + sizeof(PLDM_FWUP_TRANSFER_SUCCESS);

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(PLDM_FWUP_TRANSFER_SUCCESS)>
        transferCompleteReq1{0x00, 0x00, 0x00, 0x00};

    pldm_msg_hdr hdr;
    hdr.command = PLDM_TRANSFER_COMPLETE;
    hdr.request = 0;
    hdr.instance_id = 0;
    hdr.type = PLDM_FWUP;

    memcpy((void*)&transferCompleteReq1, &hdr, sizeof(pldm_msg_hdr));

    response.resize(getMctpHeaderSize() + respLen);

    memcpy(response.data() + getMctpHeaderSize(), &transferCompleteReq1,
           respLen);
}

// FW UPDATE COMMANDS END

// BASE COMMANDS
void encodeBaseGetVersion(std::vector<uint8_t>& response)
{
    response.resize(getMctpHeaderSize() + sizeof(pldm_msg_hdr) +
                    PLDM_GET_VERSION_REQ_BYTES);

    auto request =
        reinterpret_cast<pldm_msg*>(response.data() + getMctpHeaderSize());

    encode_get_version_req(0, 0, PLDM_GET_FIRSTPART, PLDM_BASE, request);

    putMctpHeader(response.data());
}

void encodeBaseGetCommands(std::vector<uint8_t>& response)
{
    response.resize(getMctpHeaderSize() + sizeof(pldm_msg_hdr) +
                    PLDM_GET_COMMANDS_REQ_BYTES);

    auto request =
        reinterpret_cast<pldm_msg*>(response.data() + getMctpHeaderSize());

    ver32_t version{0xFF, 0xFF, 0xFF, 0xFF};
    encode_get_commands_req(0, PLDM_BASE, version, request);

    putMctpHeader(response.data());
}

void encodeBaseSetTid(std::vector<uint8_t>& response)
{
    response.resize(getMctpHeaderSize() + sizeof(pldm_msg_hdr) + 1);

    auto request =
        reinterpret_cast<pldm_msg*>(response.data() + getMctpHeaderSize());
    encode_set_tid_req(0, getUint8t(randomInput), request);

    putMctpHeader(response.data());
}

void encodeBaseGetTid(std::vector<uint8_t>& response)
{
    response.resize(getMctpHeaderSize() + sizeof(pldm_msg_hdr));

    auto request =
        reinterpret_cast<pldm_msg*>(response.data() + getMctpHeaderSize());
    encode_get_tid_req(0, request);

    putMctpHeader(response.data());
}

void encodeBaseGetTypes(std::vector<uint8_t>& response)
{
    response.resize(getMctpHeaderSize() + sizeof(pldm_msg_hdr));

    auto request =
        reinterpret_cast<pldm_msg*>(response.data() + getMctpHeaderSize());
    encode_get_types_req(0, request);

    putMctpHeader(response.data());
}
// BASE COMMANDS END

void* runFuzzerReceiver(void*)
{
    auto event = Event::get_default();

    int server_fd, client_fd;
    struct sockaddr_un server_addr, client_addr;
    socklen_t client_len;

    std::fstream fileStream;
    fileStream.open(logPath, std::ios::app);

    // create the server socket
    if ((server_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0)) == -1)
    {
        std::cerr << "[FuzzerReceiver]: Error creating socket." << std::endl;
        return 0;
    }

    // set up the server address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sun_family = AF_UNIX;
    strcpy(server_addr.sun_path, PCIE_SOCK);

    // bind the server socket to the address
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) ==
        -1)
    {
        std::cerr << "[FuzzerReceiver]: Error binding socket." << std::endl;
        return 0;
    }

    // listen for connections
    if (listen(server_fd, 5) == -1)
    {
        std::cerr << "[FuzzerReceiver]: Error listening on socket."
                  << std::endl;
        return 0;
    }

    LOG << "[FuzzerReceiver]: Fuzz server is listening..." << std::endl;

    // accept client connection
    client_len = sizeof(client_addr);

    if ((client_fd = accept(server_fd, (struct sockaddr*)&client_addr,
                            &client_len)) == -1)
    {
        std::cerr << "Error accepting client connection." << std::endl;
        return 0;
    }

    LOG << "[FuzzerReceiver]: Client connected." << std::endl;

    unsigned idx = 0;
    bool skipNextRecv = false;

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = recvTimeout;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    while (true)
    {
        if (!skipNextRecv)
        {
            std::vector<uint8_t> buf(1024);

            LOG << "[FuzzerReceiver]: recv ..." << std::endl;
            int bytes_received = recv(client_fd, buf.data(), buf.size(), 0);

            if (bytes_received < 0)
            {
                if (errno != EAGAIN)
                {
                    LOG << "[FuzzerReceiver]: error receiving data, errno="
                        << errno << std::endl;
                    return nullptr;
                }
                else
                {
                    LOG << "[FuzzerReceiver]: recv timeout" << std::endl;
                    // exit(0);
                    return nullptr;
                }
            }
            else if (bytes_received > 0)
            {
                buf.resize(bytes_received);

                LOG << "[FuzzerReceiver]: recv succeed. Message length: "
                    << bytes_received << std::endl;

                if (bytes_received == 0)
                {
                    LOG << "[FuzzerReceiver]: client disconnected" << std::endl;
                    return 0;
                }

                LOG << "[FuzzerReceiver]: received message content: ";

                fileStream << "RX> ";
                for (auto byte : buf)
                {
                    LOG << " " << std::hex << std::setfill('0') << std::setw(2)
                        << static_cast<int>(byte) << std::dec;
                    fileStream << " " << std::hex << std::setfill('0')
                               << std::setw(2) << static_cast<int>(byte);
                }
                fileStream << "\n" << std::dec;

                LOG << " end" << std::endl;
            }
            else
            {
                LOG << "[FuzzerReceiver]: recv - 0 length response"
                    << std::endl;
            }
        }

        skipNextRecv = false;

        if (idx >= cmdCount)
            break;

        std::vector<uint8_t> response;

        std::cout << "[FuzzerReceiver]: idx=" << idx << std::endl;
        if (pldmBaseTest && idx == 0)
        {
            LOG << "[FuzzerReceiver]: Preparing PLDM_GET_COMMANDS" << std::endl;

            encodeBaseGetCommands(response);
        }
        else if (pldmBaseTest && idx == 1)
        {
            LOG << "[FuzzerReceiver]: Preparing PLDM_GET_VERSION" << std::endl;

            encodeBaseGetVersion(response);
        }
        else if (pldmBaseTest && idx == 2)
        {
            LOG << "[FuzzerReceiver]: Preparing PLDM_SET_TID" << std::endl;

            encodeBaseSetTid(response);
        }
        else if (pldmBaseTest && idx == 3)
        {
            LOG << "[FuzzerReceiver]: Preparing PLDM_GET_TID" << std::endl;

            encodeBaseGetTid(response);
        }
        else if (pldmBaseTest && idx >= 4)
        {
            LOG << "[FuzzerReceiver]: Preparing PLDM_GET_TYPES" << std::endl;

            encodeBaseGetTypes(response);
        }
        else if ((pldmFwUpdateDeviceUpdaterTest || pldmInventoryOnlyTest) &&
                 idx == 0)
        {}
        else if ((pldmFwUpdateDeviceUpdaterTest || pldmInventoryOnlyTest) &&
                 idx == 1)
        {
            LOG << "[FuzzerReceiver]: Preparing QUERY_DEVICE_IDENTIFIERS_RESP"
                << std::endl;

            encodeQueryDeviceIdentifiersResp(response);
            putMctpHeader(response.data());
        }
        else if ((pldmFwUpdateDeviceUpdaterTest || pldmInventoryOnlyTest) &&
                 idx == 2)
        {
            LOG << "[FuzzerReceiver]: Preparing FIRMWARE_PARAMETERS_RESP"
                << std::endl;

            encodeFirmwareParametersResp(response);
            putMctpHeader(response.data());

            skipNextRecv = true;
        }
        else if (pldmInventoryOnlyTest && idx == 3)
        {
            LOG << "[FuzzerReceiver]: Preparing QUERY_DOWNSTREAM_DEVICES_REQ"
                << std::endl;

            encodeQueryDownstreamDevices(response);
            putMctpHeader(response.data());
        }
        else if (pldmFwUpdateDeviceUpdaterTest && idx == 3)
        {
            LOG << "[FuzzerReceiver]: Triggering package_parser" << std::endl;

            std::filesystem::copy("/tmp/test_pkg", "/tmp/images/test_pkg");
        }
        else if (pldmFwUpdateDeviceUpdaterTest && idx == 4)
        {
            LOG << "[FuzzerReceiver]: Preparing REQUEST_UPDATE_RESP"
                << std::endl;

            encodeRequestUpdate(response);
            putMctpHeader(response.data());
        }
        else if (pldmFwUpdateDeviceUpdaterTest && idx == 5)
        {
            LOG << "[FuzzerReceiver]: Preparing PASS_COMPONENT_TABLE_RESP"
                << std::endl;

            encodePassComponentTableResponse(response);
            putMctpHeader(response.data());
        }
        else if (pldmFwUpdateDeviceUpdaterTest && idx == 6)
        {
            LOG << "[FuzzerReceiver]: Preparing UPDATE_COMPONENT_RESP"
                << std::endl;

            encodeUpdateComponentResponse(response);
            putMctpHeader(response.data());

            skipNextRecv = true;
        }
        else if (pldmFwUpdateDeviceUpdaterTest && idx >= 7 && idx < 20)
        {
            LOG << "[FuzzerReceiver]: Preparing REQUEST_FIRMWARE_DATA_REQ"
                << std::endl;

            putDeviceUpdaterFirst512MBRequest(response);
            putMctpHeader(response.data());
        }
        else if (pldmFwUpdateDeviceUpdaterTest && idx == 20)
        {
            LOG << "[FuzzerReceiver]: Preparing TRANSFER_COMPLETE_REQ"
                << std::endl;

            encodeTransferComplete(response);
            putMctpHeader(response.data());
        }
        else if (pldmFlipRandCmd)
        {
            std::vector<std::function<void(std::vector<uint8_t>&)>> functions =
                {encodeQueryDeviceIdentifiersResp,
                 encodeFirmwareParametersResp,
                 encodeQueryDownstreamDevices,
                 encodeRequestUpdate,
                 encodePassComponentTableResponse,
                 encodeUpdateComponentResponse,
                 putDeviceUpdaterFirst512MBRequest,
                 encodeTransferComplete};
        }
        else if (!pldmFwUpdateDeviceUpdaterTest && !pldmBaseTest &&
                 !pldmInventoryOnlyTest)
        {
            LOG << "[FuzzerReceiver]: Preparing random packet" << std::endl;

            auto size = getUint8t(randomInput);
            size += (sizeof(uint8_t) * 2) + sizeof(pldm_msg_hdr);

            response.resize(size);

            getFromCin(response.data(), size, randomInput);
            putPldmRandomPacket(response.data());
        }

        idx++;
        flipRandomBits(response);

        if ((!pldmFwUpdateDeviceUpdaterTest || (idx > 0 && idx < 21)) &&
            response.size() > 0)
        {
            LOG << "[FuzzerReceiver]: Sending response:";
            fileStream << "TX> ";
            for (auto byte : response)
            {
                LOG << " " << std::hex << std::setfill('0') << std::setw(2)
                    << static_cast<int>(byte) << std::dec;
                fileStream << " " << std::hex << std::setfill('0')
                           << std::setw(2) << static_cast<int>(byte)
                           << std::dec;
            }
            fileStream << "\n";
            LOG << " end" << std::endl;

            int sendReturnCode = 0;
            if ((sendReturnCode =
                     write(client_fd, response.data(), response.size())) == -1)
            {
                LOG << "[FuzzerReceiver]: Write error " << sendReturnCode
                    << std::endl;
                return 0;
            }

            LOG << "[FuzzerReceiver]: Response sent" << std::endl;
        }
    }

    LOG << "[FuzzerReceiver]: Closing" << std::endl;
    fileStream << "\n" << std::endl;
    fileStream.close();
    exit(0);

    return 0;
}

void* runObjMapperMock(void*)
{
    auto bus = sdbusplus::bus::new_default();

    MockObjectMapper objectMapper(bus, pldm::utils::mapperPath);
    bus.request_name(pldm::utils::mapperService);

    // Handle dbus processing till keepBus.
    int i = 0;
    while (true)
    {
        try
        {
            bus.process();
            bus.wait();
        }
        catch (const sdbusplus::exception::exception& e)
        {
            ++i;

            if (i == 5)
                return 0;

            LOG << "[runObjMapperMock] " << e.name() << ":" << e.description()
                << std::endl;
        }
    }

    LOG << "[runObjMapperMock] end" << std::endl;
    return 0;
}

void* runMctpMock(void*)
{
    pthread_mutex_lock(&mutex);

    auto bus = sdbusplus::bus::new_default();

    MockMctpEndpoint mctpObj1(
        bus,
        std::string("/xyz/openbmc_project/inventory/chassis/DeviceName1")
            .c_str(),
        100);
    // MockMctpEndpoint mctpObj2(bus,
    //                          (MCTP_PATH + std::string("/spi/obj2")).c_str(),
    //                          18);

    mctpObj1.mediumType(sdbusplus::xyz::openbmc_project::MCTP::server::
                            Endpoint::MediaTypes::PCIe);

    // mctpObj2.mediumType(sdbusplus::xyz::openbmc_project::MCTP::server::
    //                    Endpoint::MediaTypes::SMBus);

    auto types = std::vector<uint8_t>{0, 1};

    if (pldmFwUpdateDeviceUpdaterTest || pldmInventoryOnlyTest)
    {
        types.push_back(5);
    }

    mctpObj1.eid(EID_DEV);
    mctpObj1.networkId(0);
    mctpObj1.supportedMessageTypes(types);
    mctpObj1.uuid.uuid("f53dca18-6180-4b92-af93-0162795dc5b7");
    mctpObj1.socket.address({'/', 't', 'm', 'p', '/', 'p', 'c', 'i', 'e'});
    mctpObj1.socket.protocol(0);
    mctpObj1.socket.type(0);

    bus.request_name(MCTP_BUSNAME);
    pthread_mutex_unlock(&mutex);
    pthread_mutex_lock(&mutex);
    is_ready = true;
    pthread_cond_signal(&cond_var); // Notify waiting thread
    pthread_mutex_unlock(&mutex);

    // Handle dbus processing till keepBus.
    int i = 0;
    while (true)
    {
        try
        {
            bus.process();
            bus.wait();
        }
        catch (const sdbusplus::exception::exception& e)
        {
            ++i;

            if (i == 5)
                return 0;

            LOG << "[runMctpMock] error: " << e.name() << ":" << e.description()
                << std::endl;
        }
    }

    LOG << "[runMctpMock] end" << std::endl;
    return 0;
}

void* runPldmdMock(void*)
{
    Invoker invoker{};
    auto event = Event::get_default();
    auto bus = sdbusplus::bus::new_default();

    mctp_socket::Manager sockManager;
    event.set_watchdog(true);

    dbus_api::Requester dbusImplReq(bus, "/xyz/openbmc_project/pldm");

    sdbusplus::xyz::openbmc_project::Software::server::UpdatePolicy policy(
        bus,
        std::string("/xyz/openbmc_project/software/ComponentName1").c_str());

    policy.targets({sdbusplus::object_path(
        "/xyz/openbmc_project/inventory/chassis/DeviceName1")});

    DBusHandler dbusHandler;
    requester::Handler<requester::Request> reqHandler(event, dbusImplReq,
                                                      sockManager, true);

    std::unique_ptr<fw_update::Manager> fwManager =
        std::make_unique<fw_update::Manager>(
            event, reqHandler, dbusImplReq, "fuzz_test_fw_update_config.json",
            &dbusHandler, true);

    pldm::mctp_socket::Handler sockHandler(
        event, reqHandler, invoker, *(fwManager.get()), sockManager, true);

    std::unique_ptr<MctpDiscovery> mctpDiscoveryHandler =
        std::make_unique<MctpDiscovery>(
            bus, sockHandler,
            std::initializer_list<MctpDiscoveryHandlerIntf*>{
#ifdef PLDM_TYPE2
                platformManager.get(),
#endif
                fwManager.get()});

    bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);
    bus.request_name("xyz.openbmc_project.PLDM");

    stdplus::signal::block(SIGUSR1);

    // auto& deviceUpdaterMap =
    // fwManager.get()->getUpdateManager().getDeviceUpdaterMap();
    // deviceUpdaterMap[DEV_EID] = DeviceUpdater(17, );

    auto returnCode = event.loop();

    if (returnCode)
    {
        LOG << "[runPldmdMock] Loop error " << returnCode << std::endl;

        exit(EXIT_FAILURE);
    }

    LOG << "[runPldmdMock] Loop end " << returnCode << std::endl;
    return 0;
}

void* runActivationMock(void*)
{
    LOG << "[runActivationMock] start" << std::endl;

    auto bus = sdbusplus::bus::new_default();
    bus.request_name("xyz.openbmc_project.Software.Activation");

    ActivationMock aMock(bus,
                         "/xyz/openbmc_project/software/10581898802734046340");

    int i = 0;
    while (true)
    {
        try
        {
            bus.process();
            bus.wait();
        }
        catch (const sdbusplus::exception::exception& e)
        {
            ++i;

            if (i == 5)
                return 0;

            LOG << "[runActivationMock] error: " << e.name() << ":"
                << e.description() << std::endl;
        }
    }

    LOG << "[runActivationMock] end" << std::endl;

    return 0;
}

void print_help(char* program_name)
{
    std::cerr
        << "Usage: " << program_name << " [options] " << std::endl
        << "Options:" << std::endl
        << "  -r                 Run PLDM logic only" << std::endl
        << "  -d                 Mock only DBUS objects" << std::endl
        << "  -u                 Use input from /dev/urandom" << std::endl
        << "  -f                 Byte flip probability" << std::endl
        << "  -n <NUMBER>        Messages send in a row [default 1]. Works only with random mode."
        << std::endl
        << "  -a                 Enable device updater test" << std::endl
        << "  -i                 Enable inventory test only" << std::endl
        << "  -b                 Enable base commands test" << std::endl
        << "  -o                 Enable firmware update commands order flip NOT IMPLEMENTED YET"
        << std::endl
        << "  -s <COMMAND_NAME>  Send valid output until a given state is reached. NOT IMPLEMENTED YET"
        << std::endl
        << "  -t <NUMBER>        The time in <usec> after which a timeout will occur when the mockup is waiting for a response from PLDM. [default 500000]"
        << std::endl
        << "  -c <PATH>          Track packets to file." << std::endl
        << "COMMAND_NAME:" << std::endl
        << " [ RequestUpdate | PassComponentTable | "
        << "UpdateComponent | RequestFirmwareData | TransferComplete | "
        << "VerifyComplete | ApplyComplete | ActivateFirmware ]" << std::endl
        << "  -v                 Verbose" << std::endl
        << "  -h                 Show this help message" << std::endl;
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

#ifdef FUZZ_TEST
    std::cout << "FUZZ_TEST ENABLED" << std::endl;
#endif

    int option;
    bool onlyPldm = false;
    bool onlyMock = false;

    while ((option = getopt(argc, argv, "rduvf:n:aibt:c:h?")) != -1)
    {
        switch (option)
        {
            case 'r':
                onlyPldm = true;
                break;
            case 'd':
                onlyMock = true;
                break;
            case 'u':
                randomInput = true;
                break;
            case 'v':
                verbose = true;
                break;
            case 'f':
                flipProbability = std::atof(optarg);

                LOG << "set flipProbability=" << flipProbability << std::endl;
                break;
            case 'n':
                cmdCount = std::atoi(optarg);

                LOG << "set cmdCout=" << cmdCount << std::endl;
                break;
            case 'a':
                pldmFwUpdateDeviceUpdaterTest = true;
                cmdCount = 9;
                break;
            case 'i':
                pldmInventoryOnlyTest = true;
                cmdCount = 4;
                break;
            case 'b':
                pldmBaseTest = true;
                cmdCount = 5;
                break;
            case 'o':
                pldmFlipRandCmd = true;
                break;
            case 's':
                pldmKeepValidOutput = true;
                keepValidOutputUntil = std::string(optarg);
                cmdCount = 9;
                break;
            case 't':
                recvTimeout = std::atol(optarg);
                LOG << "set recvTimeout=" << recvTimeout << std::endl;
                break;
            case 'c':
                logPath = std::string(optarg);
                LOG << "log tracking on " << std::endl;
                break;
            case 'h':
                print_help(argv[0]);
                return 0;
            case '?':
                std::cerr << "Unknown option: -" << static_cast<char>(optopt)
                          << std::endl;
                print_help(argv[0]);
                return 1;
            default:
                std::cerr << "Invalid option" << std::endl;
                print_help(argv[0]);
                return 1;
        }
    }

    if (onlyMock && onlyPldm)
    {
        std::cerr << "Cannot use -d and -r flag same time." << std::endl;
        print_help(argv[0]);
        return 1;
    }

    pthread_t pldmLogicThread;
    pthread_t fuzzerReceiverThread;
    pthread_t objMapperMockThread;
    pthread_t mctpMockThread;
    pthread_t activationMockThread;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    if (!onlyMock)
    {
        std::remove(PCIE_SOCK);
        ::pthread_create(&fuzzerReceiverThread, nullptr, &runFuzzerReceiver,
                         nullptr);
    }

    if (!onlyPldm)
    {
        ::pthread_create(&objMapperMockThread, nullptr, &runObjMapperMock,
                         nullptr);

        ::pthread_mutex_lock(&mutex);
        ::pthread_create(&mctpMockThread, &attr, &runMctpMock, nullptr);
        ::pthread_create(&activationMockThread, &attr, &runActivationMock,
                         nullptr);
        while (!is_ready)
        {
            pthread_cond_wait(&cond_var,
                              &mutex); // Wait for worker thread to complete
        }
    }

    if (!onlyMock)
    {
        ::pthread_create(&pldmLogicThread, &attr, &runPldmdMock, nullptr);
    }

    if (!onlyPldm)
    {
        ::pthread_join(objMapperMockThread, NULL);
        ::pthread_join(mctpMockThread, NULL);
    }

    if (!onlyMock)
    {
        ::pthread_join(fuzzerReceiverThread, NULL);
        ::pthread_join(pldmLogicThread, NULL);
    }
    return 0;
}
