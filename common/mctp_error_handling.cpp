/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION &
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
#include "mctp_error_handling.hpp"

#include "fw-update/dbusutil.hpp"
#include "utils.hpp"

#include <linux/mctp.h>

#include <phosphor-logging/lg2.hpp>
#include <phosphor-logging/mctp_error_registry.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>
#include <optional>

namespace pldm
{
namespace transport
{

int readMctpErrorQueue(int fd, MctpError& error)
{
    char controlBuf[512];
    struct iovec iov = {.iov_base = &error, .iov_len = sizeof(error)};

    struct msghdr msg = {.msg_name = nullptr,
                         .msg_namelen = 0,
                         .msg_iov = &iov,
                         .msg_iovlen = 1,
                         .msg_control = controlBuf,
                         .msg_controllen = sizeof(controlBuf),
                         .msg_flags = 0};

    ssize_t ret = recvmsg(fd, &msg, MSG_ERRQUEUE);
    if (ret < 0)
    {
        int rc = errno;
        // EAGAIN/EWOULDBLOCK is not an error, just means no errors in queue
        if (rc == EAGAIN || rc == EWOULDBLOCK)
        {
            return -EAGAIN;
        }
        return -rc;
    }

    // The error data is in the iovec buffer (error struct)
    // But we need to verify MCTP_RECVERR is present in control messages
    bool foundError = false;
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
         cmsg = CMSG_NXTHDR(&msg, cmsg))
    {
        if (cmsg->cmsg_level == SOL_MCTP && cmsg->cmsg_type == MCTP_RECVERR)
        {
            foundError = true;
            break;
        }
    }

    if (!foundError)
    {
        return -EAGAIN;
    }

    if (error.payload_len < sizeof(pldm_msg_hdr))
    {
        lg2::error(
            "MCTP Error Queue: Invalid payload_len={LEN}, minimum 2 bytes required",
            "LEN", static_cast<int>(error.payload_len));
        return -EINVAL;
    }

    size_t len = std::min(static_cast<size_t>(error.payload_len),
                          static_cast<size_t>(MCTP_ERROR_PAYLOAD_SIZE));
    std::string payloadHex;
    for (size_t i = 0; i < len; i++)
    {
        payloadHex += std::format("{:02x} ", error.payload[i]);
    }

    lg2::info("MCTP Error Queue: dir={DIR} EID {SRCEID}->{DESTEID} "
              "errno={ERRNO} payloadLen={LEN} payload=[{PAYLOAD}]",
              "DIR", static_cast<int>(error.direction), "SRCEID",
              static_cast<int>(error.src_eid), "DESTEID",
              static_cast<int>(error.dest_eid), "ERRNO",
              static_cast<int>(error.error_code), "LEN",
              static_cast<int>(error.payload_len), "PAYLOAD",
              payloadHex.c_str());

    return 0;
}

uint8_t extractPldmType(const MctpError& error)
{
    if (error.msg_type != MCTP_MSG_TYPE_PLDM)
    {
        return 0xFF;
    }

    if (error.payload_len < 2)
    {
        return 0xFF;
    }

    uint8_t typeAndVersion = error.payload[1];
    uint8_t pldmType = typeAndVersion & 0x3F;

    return pldmType;
}

MctpError createMctpErrorObject(mctp_eid_t destEid, int errorCode,
                                uint8_t binding,
                                const std::vector<uint8_t>& payload)
{
    MctpError mctpErr{};
    mctpErr.msg_type = MCTP_MSG_TYPE_PLDM;
    mctpErr.direction = MCTP_DIR_TX;
    mctpErr.src_eid = 0; // BMC (local)
    mctpErr.dest_eid = destEid;
    mctpErr.error_code = errorCode;
    mctpErr.binding = binding;
    mctpErr.timestamp_ns = 0;
    mctpErr.payload_len =
        std::min(payload.size(), static_cast<size_t>(MCTP_ERROR_PAYLOAD_SIZE));
    std::copy(payload.begin(), payload.begin() + mctpErr.payload_len,
              mctpErr.payload);

    return mctpErr;
}

void createMctpTransportRedfishEvent(
    mctp_eid_t eid, const std::string& commandName, uint32_t errorCode,
    uint8_t binding, uint8_t direction, const std::string& logNamespace)
{
    // PLDM FW Update Config Migration (DGXOPENBMC-25121): device identity is no
    // longer resolved from an EID-keyed fw_update_config.json. The transport
    // error event below is diagnostic only; the device name is left unresolved
    // (surfaces as "Unknown") rather than reintroducing an EID-keyed lookup.
    std::optional<std::string> componentName = std::nullopt;

    bool isAsync = (binding != MCTP_BINDING_UNKNOWN);

    lg2::error(
        "MCTP Transport Error - {COMMAND} to EID {EID}, Binding={BINDING}, "
        "Direction={DIR}, Async={ASYNC} (error code: {ERROR_CODE})",
        "COMMAND", commandName, "EID", eid, "BINDING", binding, "DIR",
        direction, "ASYNC", isAsync, "ERROR_CODE", errorCode);

    auto mctpBinding = static_cast<phosphor::logging::mctp::Binding>(binding);
    auto mctpDirection =
        static_cast<phosphor::logging::mctp::Direction>(direction);

    auto registry = phosphor::logging::mctp::errorToRedfishRegistry(
        errorCode, mctpDirection, mctpBinding, eid, commandName, componentName);

    if (registry)
    {
        // MCTP transport sync APIs errors can be ignored if the device
        // communication is already bad
        if (registry->isDeviceError && !isAsync)
        {
            auto hasErrors = queryDeviceStatus(eid);
            if (hasErrors)
            {
                lg2::info(
                    "Device has errors, skipping transport error Redfish event "
                    "creation for {DEVICE_NAME} and EID {EID}",
                    "DEVICE_NAME", componentName.value_or("Unknown"), "EID",
                    eid);
                return;
            }
        }

        std::string argsStr;
        for (size_t i = 0; i < registry->args.size(); i++)
        {
            if (i > 0)
            {
                argsStr += ",";
            }
            argsStr += registry->args[i];
        }

        // Forward the device name and RAS catalog error ID so bmcweb can
        // populate Oem.Nvidia.Device and Oem.Nvidia.ErrorId on the log entry.
        std::map<std::string, std::string> extraData;
        extraData["DEVICE_NAME"] =
            componentName.value_or(phosphor::logging::mctp::getDeviceNameByEid(
                static_cast<uint8_t>(eid)));
        if (!registry->errorId.empty())
        {
            extraData["ERROR_ID"] = registry->errorId;
        }

        using Level =
            sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;
        createLogEntry(registry->registryId, argsStr, registry->resolution,
                       logNamespace, Level::Informational, extraData);
    }
}

} // namespace transport
} // namespace pldm
