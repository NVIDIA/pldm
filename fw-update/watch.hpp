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

#include <systemd/sd-event.h>

#include <functional>
#include <string>
#include "config.h"

namespace pldm
{

namespace fw_update
{

/** @class Watch
 *
 *  @brief Adds inotify watch on software image upload directory
 *
 *  The inotify watch is hooked up with sd-event, so that on call back,
 *  appropriate actions related to a software image upload can be taken.
 */
class Watch
{
  public:
    /** @brief ctor - hook inotify watch with sd-event
     *
     *  @param[in] loop - sd-event object
     *  @param[in] imageCallbackImmediate - The callback function for processing
     * the immdidate update image
     *  @param[in] imageCallbackSplitStage - The callback function for
     * processing the split-stage update image
     */
    Watch(sd_event* loop,
          std::function<int(std::string&)> imageCallbackImmediate,
          std::function<int(std::string&)> imageCallbackSplitStage);

    Watch(const Watch&) = delete;
    Watch& operator=(const Watch&) = delete;
    Watch(Watch&&) = delete;
    Watch& operator=(Watch&&) = delete;

    /** @brief dtor - remove inotify watch and close fd's
     */
    ~Watch();

  private:
    /** @brief sd-event callback for immediate update
     *
     *  @param[in] s - event source, floating (unused) in our case
     *  @param[in] fd - inotify fd
     *  @param[in] revents - events that matched for fd
     *  @param[in] userdata - pointer to Watch object
     *  @returns 0 on success, -1 on fail
     */
    static int callbackImmediate(sd_event_source* s, int fd, uint32_t revents,
                                 void* userdata);

    /** @brief sd-event callback for split-stage update
     *
     *  @param[in] s - event source, floating (unused) in our case
     *  @param[in] fd - inotify fd
     *  @param[in] revents - events that matched for fd
     *  @param[in] userdata - pointer to Watch object
     *  @returns 0 on success, -1 on fail
     */
    static int callbackSplitStaged(sd_event_source* s, int fd, uint32_t revents,
                                   void* userdata);

    /** @brief image upload directory watch descriptor */
    int wd = -1;

    /** @brief inotify file descriptor */
    int fdImmediate = -1;
    int fdSplitStage = -1;

    /** @brief The callback function for processing the immediate update. */
    std::function<int(std::string&)> imageCallbackImmediate;

    /** @brief The callback function for processing the split stage and update
     * image. */
    std::function<int(std::string&)> imageCallbackSplitStage;

    /**
     * @brief initialize file watchers for immediate update
     *
     * @param[in] loop - event loop
     */
    void initializeImmediateUpdateWatch(sd_event* loop);

    /**
     * @brief initialize file watchers for split-stage update
     *
     * @param[in] loop - event loop
     */
    void initializeStagedUpdateWatch(sd_event* loop);
};

} // namespace fw_update
} // namespace pldm
