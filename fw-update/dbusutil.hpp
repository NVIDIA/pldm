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
#include "common/types.hpp"
#include "common/utils.hpp"
#include "config.hpp"

#include <libpldm/pldm.h>

#include <com/nvidia/State/DeviceState/server.hpp>
#include <phosphor-logging/device_error_log.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/async.hpp>
#include <sdbusplus/bus.hpp>
#include <xyz/openbmc_project/Logging/Entry/server.hpp>

#include <optional>

PHOSPHOR_LOG2_USING;

constexpr auto dbusProperties = "org.freedesktop.DBus.Properties";
constexpr auto mapperService = "xyz.openbmc_project.ObjectMapper";
constexpr auto mapperPath = "/xyz/openbmc_project/object_mapper";
constexpr auto mapperInterface = "xyz.openbmc_project.ObjectMapper";
const std::string transferFailed{"Update.1.0.TransferFailed"};
const std::string transferringToComponent{"Update.1.0.TransferringToComponent"};
const std::string verificationFailed{"Update.1.0.VerificationFailed"};
const std::string updateSuccessful{"Update.1.0.UpdateSuccessful"};
const std::string awaitToActivate{"Update.1.0.AwaitToActivate"};
const std::string applyFailed{"Update.1.0.ApplyFailed"};
const std::string activateFailed{"Update.1.0.ActivateFailed"};
const std::string targetDetermined{"Update.1.0.TargetDetermined"};
const std::string resourceErrorDetected{
    "ResourceEvent.1.0.ResourceErrorsDetected"};
const std::string componentUpdateSkipped{
    "NvidiaUpdate.1.0.ComponentUpdateSkipped"};
const std::string stageSuccessful{"NvidiaUpdate.1.0.StageSuccessful"};
const std::string activateSuccessful{"NvidiaUpdate.1.0.ActivateSuccessful"};
const std::string debugTokenEraseFailed{
    "NvidiaUpdate.1.0.DebugTokenEraseFailed"};
const std::string componentUpdateTime{"NvidiaUpdate.1.0.ComponentUpdateTime"};
/**
 * @brief Get the D-Bus service using mapper lookup
 *
 * @param[in] bus
 * @param[in] path
 * @param[in] interface
 * @return std::string
 */
inline std::string getService(sdbusplus::bus::bus& bus, const char* path,
                              const char* interface)
{
    using DbusInterfaceList = std::vector<std::string>;
    std::map<std::string, std::vector<std::string>> mapperResponse;

    auto mapper = bus.new_method_call(mapperService, mapperPath,
                                      mapperInterface, "GetObject");
    mapper.append(path, DbusInterfaceList({interface}));

    auto mapperResponseMsg = bus.call(mapper);
    mapperResponseMsg.read(mapperResponse);
    return mapperResponse.begin()->first;
}

/**
 * @brief set D-Bus property. New bus will be used for every set to avoid
 * contention with single thread using same bus
 *
 * @param[in] dbusMap - D-Bus mappings
 * @param[in] value - value to set
 */
inline void setDBusProperty(const pldm::utils::DBusMapping& dbusMap,
                            const std::string& value)
{
    auto bus = sdbusplus::bus::new_default();
    std::string dBusService =
        getService(bus, dbusMap.objectPath.c_str(), dbusMap.interface.c_str());
    auto method = bus.new_method_call(
        dBusService.c_str(), dbusMap.objectPath.c_str(), dbusProperties, "Set");
    pldm::utils::PropertyValue propertyValue = value;
    method.append(dbusMap.interface.c_str(), dbusMap.propertyName.c_str(),
                  propertyValue);
    bus.call_noreply(method);
}

/** @brief Create log entry with explicit severity and pre-formatted args
 *
 *  @param[in] messageID - Message ID
 *  @param[in] messageArgs - Pre-formatted message arguments string
 *  @param[in] resolution - Resolution field
 *  @param[in] logNamespace - Logging namespace
 *  @param[in] level - Severity level for the log entry
 */
inline void createLogEntry(
    const std::string& messageID, const std::string& messageArgs,
    const std::string& resolution, const std::string& logNamespace,
    sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level level)
{
    std::map<std::string, std::string> addData;
    addData["REDFISH_MESSAGE_ID"] = messageID;
    addData["REDFISH_MESSAGE_ARGS"] = messageArgs;

    if (!resolution.empty())
    {
        addData["xyz.openbmc_project.Logging.Entry.Resolution"] = resolution;
    }

    if (!logNamespace.empty())
    {
        addData["namespace"] = logNamespace;
    }

    auto& asioConnection = pldm::utils::DBusHandler::getAsioConnection();
    auto severity =
        sdbusplus::xyz::openbmc_project::Logging::server::convertForMessage(
            level);
    asioConnection->async_method_call(
        [](boost::system::error_code ec) {
            if (ec)
            {
                error("error while logging message registry: ", "ERROR_MESSAGE",
                      ec.message());
                return;
            }
        },
        "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
        "xyz.openbmc_project.Logging.Create", "Create", messageID, severity,
        addData);
}

/** @brief Create the D-Bus log entry for message registry
 *
 *  @param[in] messageID - Message ID
 *  @param[in] arg0 - argument 0
 *  @param[in] arg1 - argument 1
 *  @param[in] resolution - Resolution field
 *  @param[in] logNamespace - Logging namespace, default is FWUpdate
 *  @param[in] overrideSeverity - Overwrites the severity for the Log
 */
inline void createLogEntry(
    const std::string& messageID, const std::string& arg0,
    const std::string& arg1, const std::string& resolution,
    const std::string logNamespace = "FWUpdate", bool overrideSeverity = false)
{
    using namespace sdbusplus::xyz::openbmc_project::Logging::server;
    using Level =
        sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;

    std::string messageArgs;
    Level level = Level::Informational;

    if (messageID == targetDetermined || messageID == updateSuccessful ||
        messageID == componentUpdateSkipped || messageID == stageSuccessful ||
        messageID == componentUpdateTime || messageID == activateSuccessful)
    {
        messageArgs = arg0 + "," + arg1;
    }
    else if (messageID == transferFailed || messageID == verificationFailed ||
             messageID == applyFailed || messageID == activateFailed)
    {
        messageArgs = arg1 + "," + arg0;
        level = Level::Critical;
    }
    else if (messageID == transferringToComponent ||
             messageID == awaitToActivate)
    {
        messageArgs = arg1 + "," + arg0;
    }
    else if (messageID == resourceErrorDetected)
    {
        messageArgs = arg0 + "," + arg1;
        if (overrideSeverity)
        {
            level = Level::Informational;
        }
        else
        {
            level = Level::Critical;
        }
    }
    else
    {
        info("Generic message ID using default ordering for args", "MESSAGEID",
             messageID);
        level = Level::Critical;
        messageArgs = arg0 + "," + arg1;
    }

    createLogEntry(messageID, messageArgs, resolution, logNamespace, level);
}

/** @brief Simple structure to hold Redfish error information */
struct RedfishErrorInfo
{
    std::string messageId;
    std::string arg0;
    std::string arg1;
    std::string resolution;
};

/** @brief Query device status via D-Bus and get Redfish error info
 *
 *  @param[in] eid - endpoint ID of the device
 *  @return vector of Redfish error info for all errors found, empty if no
 * errors
 */
inline std::vector<RedfishErrorInfo> queryDeviceStatusError(
    mctp_eid_t eid) noexcept
{
    using DeviceState = sdbusplus::server::com::nvidia::state::DeviceState;

    std::string objectPath = "/com/nvidia/state/device_status/" +
                             std::to_string(static_cast<unsigned int>(eid));

    static constexpr auto deviceStatusInterface =
        "com.nvidia.State.DeviceState";
    static constexpr auto deviceStatusService = "xyz.openbmc_project.Logging";

    std::variant<pldm::fw_update::DeviceStatusMap> propertyValue;

    try
    {
        auto& bus = pldm::utils::DBusHandler::getBus();
        auto method =
            bus.new_method_call(deviceStatusService, objectPath.c_str(),
                                "org.freedesktop.DBus.Properties", "Get");
        method.append(deviceStatusInterface, "DeviceStatus");

        auto reply = bus.call(method);

        reply.read(propertyValue);
    }
    catch (const std::exception& e)
    {
        error("Failed to query device status for EID {EID}: {ERROR}", "EID",
              eid, "ERROR", e.what());
        return {};
    }

    auto statusMap = std::get<pldm::fw_update::DeviceStatusMap>(propertyValue);

    auto commIt = statusMap.find(DeviceState::StatusType::Communication);
    if (commIt == statusMap.end())
    {
        return {};
    }

    DeviceState::DeviceHealth health = std::get<0>(commIt->second);
    if (health == DeviceState::DeviceHealth::Healthy)
    {
        return {};
    }

    const auto& errors = std::get<1>(commIt->second);
    if (errors.empty())
    {
        return {};
    }

    std::vector<RedfishErrorInfo> errorInfos;
    for (const auto& error : errors)
    {
        const auto& additionalData = std::get<2>(error);

        auto messageIdIt = additionalData.find("REDFISH_MESSAGE_ID");
        auto messageArgsIt = additionalData.find("REDFISH_MESSAGE_ARGS");

        if (messageIdIt != additionalData.end())
        {
            RedfishErrorInfo errorInfo;
            errorInfo.messageId = messageIdIt->second;

            std::string messageArgs = (messageArgsIt != additionalData.end())
                                          ? messageArgsIt->second
                                          : "";

            size_t commaPos = messageArgs.find(',');
            if (commaPos != std::string::npos)
            {
                errorInfo.arg0 = messageArgs.substr(0, commaPos);
                errorInfo.arg1 = messageArgs.substr(commaPos + 1);
                errorInfo.arg1.erase(0,
                                     errorInfo.arg1.find_first_not_of(" \t"));
                errorInfo.arg1.erase(
                    errorInfo.arg1.find_last_not_of(" \t") + 1);
            }
            else
            {
                errorInfo.arg0 = "";
                errorInfo.arg1 = messageArgs;
            }
            if (additionalData.find("REDFISH_RESOLUTION") !=
                additionalData.end())
            {
                errorInfo.resolution = additionalData.at("REDFISH_RESOLUTION");
            }

            errorInfos.push_back(errorInfo);
        }
    }

    return errorInfos;
}

/** @brief Query device status via D-Bus to check if device has errors
 *
 *  @param[in] eid - endpoint ID of the device
 *  @return bool - true if device has errors, false otherwise
 */
inline bool queryDeviceStatus(mctp_eid_t eid)
{
    auto errorInfos = queryDeviceStatusError(eid);
    return !errorInfos.empty();
}

/** @brief Query device status via D-Bus and create event logs for timeouts
 *         for a specific component
 *
 *  @param[in] eid - endpoint ID of the device
 *  @return bool - true if error was logged, false otherwise
 */
inline bool queryDeviceStatusAndLog(mctp_eid_t eid)
{
    auto errorInfos = queryDeviceStatusError(eid);
    if (errorInfos.empty())
    {
        return false;
    }

    for (const auto& errorInfo : errorInfos)
    {
        createLogEntry(errorInfo.messageId, errorInfo.arg0, errorInfo.arg1,
                       errorInfo.resolution);
    }
    return true;
}
