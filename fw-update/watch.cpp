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

#include <sys/inotify.h>
#include <unistd.h>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace pldm
{

namespace fw_update
{

// using namespace phosphor::logging;
using namespace std::string_literals;
namespace fs = std::filesystem;

Watch::Watch(sd_event* loop, std::function<int(std::string&)> imageCallback) :
    imageCallback(imageCallback)
{
    // Check if IMAGE DIR exists.
    fs::path imgDirPath(FIRMWARE_PACKAGE_STAGING_DIR);
    if (!fs::is_directory(imgDirPath))
    {
        fs::create_directories(imgDirPath);
    }

    fd = inotify_init1(IN_NONBLOCK);
    if (-1 == fd)
    {
        // Store a copy of errno, because the string creation below will
        // invalidate errno due to one more system calls.
        auto error = errno;
        throw std::runtime_error("inotify_init1 failed, errno="s +
                                 std::strerror(error));
    }

    wd = inotify_add_watch(fd, FIRMWARE_PACKAGE_STAGING_DIR, IN_CLOSE_WRITE);
    if (-1 == wd)
    {
        auto error = errno;
        close(fd);
        throw std::runtime_error("inotify_add_watch failed, errno="s +
                                 std::strerror(error));
    }

    auto rc = sd_event_add_io(loop, nullptr, fd, EPOLLIN, callback, this);
    if (0 > rc)
    {
        throw std::runtime_error("failed to add to event loop, rc="s +
                                 std::strerror(-rc));
    }
}

Watch::~Watch()
{
    if (-1 != fd)
    {
        if (-1 != wd)
        {
            inotify_rm_watch(fd, wd);
        }
        close(fd);
    }
}

int Watch::callback(sd_event_source* /* s */, int fd, uint32_t revents,
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
            auto rc = static_cast<Watch*>(userdata)->imageCallback(tarballPath);
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

} // namespace fw_update
} // namespace pldm
