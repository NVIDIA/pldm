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

#include <sdbusplus/async.hpp>
#include <sdbusplus/timer.hpp>
#include <xyz/openbmc_project/Software/Activation/server.hpp>
#include <xyz/openbmc_project/Software/ActivationProgress/server.hpp>

namespace pldm
{
namespace fw_update
{

namespace MatchRules = sdbusplus::bus::match::rules;
namespace Server = sdbusplus::xyz::openbmc_project::Software::server;
class UpdateManager;
const std::string InstallTokenUUID = "76910DFA1E4C11ED861D0242AC120002";
const std::string EraseTokenUUID = "76910DFA1E4C11ED861D0242AE52A53E";
const std::string transferFailedResolution =
    "Debug Token Service is not ready, retry the firmware update operation "
    "after the management controller is ready. If the issue still persists"
    " reset the baseboard.";

/**
 * @brief Debug Token implementation in pldm for token installation and erase
 */
class DebugToken
{
  public:
    /**
     * @brief Timeout for debug token install or erase operation
     *
     */
    auto static constexpr debugTokenTimeout = std::chrono::seconds(60);

    DebugToken() = delete;
    DebugToken(const DebugToken&) = delete;
    DebugToken(DebugToken&&) = delete;
    DebugToken& operator=(const DebugToken&) = delete;
    DebugToken& operator=(DebugToken&&) = delete;
    /**
     * @brief Drain any in-flight coroutines spawned by startTokenUpdate() so
     *        suspended frames see `*this` alive through their full await
     *        chain.
     *
     *  Defined inline so the destructor symbol is emitted in every TU that
     *  includes this header. The pldmd target builds debug_token.cpp only
     *  when get_option('debug-token').enabled(), but UpdateManager holds a
     *  unique_ptr<DebugToken> whenever OEM_NVIDIA is on — without the inline
     *  body, builds with OEM_NVIDIA && !DEBUG_TOKEN fail to link
     *  ~UpdateManager.
     */
    ~DebugToken()
    {
        stdexec::sync_wait(tokenScope.on_empty());
    }

    /**
     * @brief Debug token object for install or erase token
     *
     * @param[in] bus - sdbusplus referance
     * @param[in] updateManager - update manager reference
     */
    explicit DebugToken(
        sdbusplus::bus::bus& bus, UpdateManager* updateManager,
        pldm::utils::DBusHandlerInterface& dbusHandler = defaultDbusHandler()) :
        updateManager(updateManager), bus(bus), dbusHandler(dbusHandler),
        timer(nullptr), tokenStatus(false)
    {}

    /**
     * @brief From pldm image extracts the debug token image and copies to
     * respective location. Drives D-Bus property writes asynchronously.
     *
     *  fwDeviceIDRecords and componentImageInfos are taken by value so the
     *  coroutine frame owns its own copies — references would dangle if the
     *  caller-side parser is reset (e.g. via clearFirmwareUpdatePackage)
     *  while the coroutine is suspended on a D-Bus await.
     *
     *  `package` is kept as a reference and is only safe to use in the
     *  synchronous prologue (before the first co_await). With the
     *  inline_scheduler used by startTokenUpdate, the prologue runs to the
     *  first suspension while the caller is still on the stack, so the
     *  stream is guaranteed live during the prologue. Do NOT add reads from
     *  `package` after any co_await in this function — those would be a
     *  use-after-free if the caller has since dropped its hold on the
     *  stream.
     *
     * @param[in] fwDeviceIDRecords - Device records (taken by value)
     * @param[in] componentImageInfos - Image info (taken by value)
     * @param[in] package - pldm image input stream (synchronous-prologue use
     *                      only; must outlive startTokenUpdate's call)
     * @return coroutine - debug token install/erase status is ignored
     */
    exec::task<void> updateDebugToken(FirmwareDeviceIDRecords fwDeviceIDRecords,
                                      ComponentImageInfos componentImageInfos,
                                      std::istream& package);

    /**
     * @brief Spawn updateDebugToken as a detached coroutine on the internal
     *        async_scope. Returns immediately; the coroutine completes when
     *        the underlying D-Bus calls finish or fail.
     *
     *        Callers that need to wait for completion (e.g. tests) should use
     *        getTokenScope().on_empty() with stdexec::sync_wait.
     */
    void startTokenUpdate(const FirmwareDeviceIDRecords& fwDeviceIDRecords,
                          const ComponentImageInfos& componentImageInfos,
                          std::istream& package);

    /** @brief Accessor for the async scope owning in-flight token coroutines.
     *  Used by UpdateManager / tests that need to wait for completion before
     *  destroying or inspecting state.
     */
    exec::async_scope& getTokenScope()
    {
        return tokenScope;
    }

    /**
     * @brief Checks if the debug token component is present
     *
     */
    bool isDebugTokenComponentPresent() const
    {
        return installToken;
    }

  private:
    static pldm::utils::DBusHandlerInterface& defaultDbusHandler();

    UpdateManager* updateManager;
    /* install or erase token path */
    std::string tokenPath;
    /* debug token install or erase version */
    std::string tokenVersion;
    /**
     * @brief Dbus object referance
     *
     */
    sdbusplus::bus::bus& bus;
    pldm::utils::DBusHandlerInterface& dbusHandler;

    /**
     * @brief matcher rule to check for activation dbus object change
     *
     */
    std::vector<sdbusplus::bus::match_t> activationMatches;

    /**
     * @brief Timer for debug token install or erase
     *
     */
    std::unique_ptr<sdbusplus::Timer> timer;

    /* contains install or erase completion status */
    bool tokenStatus;

    bool installToken = false;

    /**
     * @brief Async call to monitor the activate change in D-Bus
     *
     * @param[in] msg - msg
     */
    void onActivationChangedMsg(sdbusplus::message::message& msg);
    /**
     * @brief Timer for debug token install or erase timeout
     *
     * @param[in] - expiry time
     */
    void startTimer(auto timerExpiryTime);

    /**
     * @brief Get file path based on UUID
     *
     * @param[in] UUID - UUID to find file path for
     * @return pair with filepath and object path, returns {} on no match
     *
     */
    std::pair<std::string, std::string> getFilePath(const std::string& uuid);

    /**
     * @brief Get valid D-Bus object paths that may contain UUIDs
     *
     * @param[in] paths - object to store the paths into
     */
    void getValidPaths(std::vector<std::string>& paths);

    /**
     * @brief Drive the RequestedActivation property write on the debug-token
     *        D-Bus object.
     *
     * @return true if the property set succeeded; false if it failed. On
     *         failure the function has already run the synchronous cleanup
     *         (log entry + startUpdate()) so the caller must NOT arm the
     *         activation timeout — that timer only makes sense while there
     *         is an in-flight activation waiting on a property-change
     *         signal.
     */
    exec::task<bool> activate();

    /**
     * @brief triggers pldm and non-pldm updates
     *
     */
    void startUpdate();
    /**
     * @brief set the extended version for item updater to update
    message registry and pass this to token installer
     *
     */
    exec::task<void> setVersion();

    /** @brief Owns in-flight coroutines spawned by startTokenUpdate(). The
     *  destructor sync_waits this scope so any suspended coroutine sees a
     *  live `this` for the full chain. Replaces the previous aliveFlag
     *  pattern that was needed for the callback-based async writes.
     *  MUST remain the last data member so it is destroyed first, after the
     *  destructor has drained it. */
    exec::async_scope tokenScope;
};

} // namespace fw_update
} // namespace pldm
