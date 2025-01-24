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
#include "watch.hpp"

#include "update_manager.hpp"

#include <sys/inotify.h>
#include <unistd.h>

#include <phosphor-logging/lg2.hpp>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <regex>
#include <stdexcept>
#include <string>

namespace pldm
{

namespace fw_update
{

// using namespace phosphor::logging;
using namespace std::string_literals;
namespace fs = std::filesystem;

Watch::Watch(sd_event* loop,
             std::function<int(std::string&)> imageCallbackImmediate) :
    imageCallbackImmediate(imageCallbackImmediate), loop(loop)
{}

Watch::~Watch()
{
    if (-1 != fdImmediate)
    {
        if (-1 != wdImmediate)
        {
            inotify_rm_watch(fdImmediate, wdImmediate);
        }
        close(fdImmediate);
    }
}

int Watch::callbackImmediate(sd_event_source* /* s */, int fd, uint32_t revents,
                             void* userdata)
{
    if (!(revents & EPOLLIN))
    {
        return 0;
    }

    constexpr auto maxBytes = 1024;
    uint8_t buffer[maxBytes];
    auto bytes = read(fd, buffer, maxBytes);
    if (0 > bytes)
    {
        auto error = errno;
        throw std::runtime_error("failed to read inotify event, errno="s +
                                 std::strerror(error));
    }

    auto offset = 0;
    while (offset < bytes)
    {
        auto event = reinterpret_cast<inotify_event*>(&buffer[offset]);
        if ((event->mask & IN_CLOSE_WRITE) && !(event->mask & IN_ISDIR))
        {
            auto tarballPath =
                std::string{FIRMWARE_PACKAGE_STAGING_DIR} + '/' + event->name;
            info("Received event for new file in immediate path "
                 "{IMMEDIATE_FILE_PATH}",
                 "IMMEDIATE_FILE_PATH", tarballPath);
            auto rc = static_cast<Watch*>(userdata)->imageCallbackImmediate(
                tarballPath);
            if (rc < 0)
            {
                // log<level::ERR>("Error processing image",
                //                 entry("IMAGE=%s", tarballPath.c_str()));
            }
        }

        offset += offsetof(inotify_event, name) + event->len;
        if (0 >= offset) // check to ensure tainted input from buffer
        {
            break;
        }
    }

    return 0;
}

void Watch::initImmediateUpdateWatch()
{
    fs::path imgDirPath(FIRMWARE_PACKAGE_STAGING_DIR);
    std::string mountService(FIRMWARE_PACKAGE_STAGING_DIR_MOUNT_SERVICE);
    if (mountService.empty())
    {
        if (!fs::is_directory(imgDirPath))
        {
            fs::create_directories(imgDirPath);
        }
        addFileEventWatchImmediate();
    }
    else
    {
        if (isServiceCompleted(mountService))
        {
            info("Mount service {MOUNT_SERVICE} is completed.", "MOUNT_SERVICE",
                 mountService);
            if (!fs::is_directory(imgDirPath))
            {
                fs::create_directories(imgDirPath);
            }
            addFileEventWatchImmediate();
        }
        else
        {
            info("Mount service {MOUNT_SERVICE} is not completed."
                 " Subscribing to systemd event.",
                 "MOUNT_SERVICE", mountService);
            subscribeToServiceStateChange(mountService, imgDirPath);
        }
    }
}

void Watch::addFileEventWatchImmediate()
{
    fs::path imgImmediateDirPath(FIRMWARE_PACKAGE_STAGING_DIR);
    if (!fs::is_directory(imgImmediateDirPath))
    {
        fs::create_directories(imgImmediateDirPath);
    }
    fdImmediate = inotify_init1(IN_NONBLOCK);
    if (-1 == fdImmediate)
    {
        // Store a copy of errno, because the string creation below will
        // invalidate errno due to one more system calls.
        auto error = errno;
        throw std::runtime_error("inotify_init1 failed, errno="s +
                                 std::strerror(error));
    }

    wdImmediate = inotify_add_watch(fdImmediate, FIRMWARE_PACKAGE_STAGING_DIR,
                                    IN_CLOSE_WRITE);
    if (-1 == wdImmediate)
    {
        auto error = errno;
        close(fdImmediate);
        throw std::runtime_error("inotify_add_watch failed, errno="s +
                                 std::strerror(error));
    }

    auto rc = sd_event_add_io(loop, nullptr, fdImmediate, EPOLLIN,
                              callbackImmediate, this);
    if (0 > rc)
    {
        throw std::runtime_error("failed to add to event loop, rc="s +
                                 std::strerror(-rc));
    }
}

bool Watch::isServiceCompleted(const std::string& serviceName)
{
    using namespace pldm::utils;
    std::string serviceStatus;
    std::string dbusPath = "/org/freedesktop/systemd1/unit/" + serviceName;
    dbusPath = std::regex_replace(dbusPath, std::regex("\\-"), "_2d");
    dbusPath = std::regex_replace(dbusPath, std::regex("\\."), "_2e");
    auto dbusHandler = pldm::utils::DBusHandler();
    auto& bus = dbusHandler.getBus();
    auto method = bus.new_method_call("org.freedesktop.systemd1",
                                      dbusPath.c_str(), dbusProperties, "Get");
    method.append("org.freedesktop.systemd1.Unit", "ActiveState");

    PropertyValue value{};
    auto reply = bus.call(method);
    reply.read(value);
    serviceStatus = std::get<std::string>(value);

    if (serviceStatus == "active")
    {
        return true;
    }
    // other states are reloading, inactive(yet to start), failed, activating,
    // and deactivating
    return false;
}

void Watch::subscribeToServiceStateChange(const std::string& serviceName,
                                          const std::string& imagePath)
{
    std::string dbusPath = "/org/freedesktop/systemd1/unit/" + serviceName;
    dbusPath = std::regex_replace(dbusPath, std::regex("\\-"), "_2d");
    dbusPath = std::regex_replace(dbusPath, std::regex("\\."), "_2e");
    if (imagePath == FIRMWARE_PACKAGE_STAGING_DIR)
    {
        auto stateChangeHandler = [this, serviceName, imagePath](
                                      sdbusplus::message::message& msg) {
            using Interface = std::string;
            Interface interface;
            pldm::dbus::PropertyMap properties;
            std::string objPath = msg.get_path();

            msg.read(interface, properties);
            auto prop = properties.find("ActiveState");
            if (prop != properties.end())
            {
                auto activeState = std::get<std::string>(prop->second);
                if (activeState == "active")
                {
                    auto stateChangeTimestamp =
                        properties.find("StateChangeTimestampMonotonic");
                    if (stateChangeTimestamp != properties.end())
                    {
                        auto stateChangeTime =
                            std::get<uint64_t>(stateChangeTimestamp->second);
                        if (stateChangeTime != stateChangeTimeImmediate)
                        {
                            stateChangeTimeImmediate = stateChangeTime;
                            info("Received mount service completion signal for "
                                 "{MOUNT_SERVICE_NAME} and PATH={IMAGE_PATH}",
                                 "MOUNT_SERVICE_NAME", serviceName,
                                 "IMAGE_PATH", imagePath);
                            if (-1 != fdImmediate)
                            {
                                if (-1 != wdImmediate)
                                {
                                    inotify_rm_watch(fdImmediate, wdImmediate);
                                }
                                close(fdImmediate);
                            }
                            this->addFileEventWatchImmediate();
                        }
                    }
                }
                // else -> other task states are activating, deactivating,
                // reloading which maps to running. Remaining states are,
                // failed, inactive maps to failed.
            }
        };
        immediateUpdateEvent = std::make_unique<sdbusplus::bus::match_t>(
            pldm::utils::DBusHandler().getBus(),
            "type='signal',interface='org.freedesktop.DBus.Properties',"
            "member='PropertiesChanged',path='" +
                dbusPath + "',arg0='org.freedesktop.systemd1.Unit'",
            stateChangeHandler);
    }
}

} // namespace fw_update
} // namespace pldm
