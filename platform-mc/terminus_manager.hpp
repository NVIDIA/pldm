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
#include "requester/handler.hpp"
#include "terminus.hpp"

#include <nlohmann/json_fwd.hpp>

#include <queue>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

namespace pldm
{

enum SupportedTransportLayer
{
    MCTP
};

namespace platform_mc
{
constexpr size_t tidPoolSize = std::numeric_limits<tid_t>::max() + 1;
using RequesterHandler = requester::Handler<requester::Request>;

/** @brief The config of a terminus: instance, terminus name and CPU index. */
using TerminusConfigEntry =
    std::tuple<int, std::string, std::optional<uint16_t>>;

class Manager;
/**
 * @brief TerminusManager
 *
 * TerminusManager class handle the task to discover and initialize PLDM
 * terminus.
 *
 * The config of a terminus (TerminusName, instance, CPU index) is keyed by
 * EID and comes from, strongest first: an Entity Manager PLDM device
 * record, an entry of the static configuration file
 * (pldm_static_configuration.json), and, with OEM_NVIDIA, the CPU device of
 * the platform topology (NvDevice). It is applied when the terminus is
 * created, and to a terminus still being initialized when it arrives later.
 * A config that changes after the terminus is initialized is only reported;
 * it takes effect at the next pldmd start.
 */
class TerminusManager
{
  public:
    TerminusManager() = delete;
    TerminusManager(const TerminusManager&) = delete;
    TerminusManager(TerminusManager&&) = delete;
    TerminusManager& operator=(const TerminusManager&) = delete;
    TerminusManager& operator=(TerminusManager&&) = delete;
    virtual ~TerminusManager() = default;

    explicit TerminusManager(
        sdeventplus::Event& event,
        requester::Handler<requester::Request>& handler,
        InstanceIdDb& instanceIdDb,
        std::map<tid_t, std::shared_ptr<Terminus>>& termini,
        mctp_eid_t localEid, Manager* manager,
        bool numericSensorsWithoutAuxName = false);

    /** @brief start a coroutine to discover terminus
     *
     *  @param[in] mctpInfos - list of mctpInfo to be checked
     */
    void discoverMctpTerminus(const MctpInfos& mctpInfos);

    /** @brief Load static PLDM terminus configuration from JSON file
     *
     *  @param[in] configPath - path to the configuration JSON file
     */
    void loadStaticTerminusConfig(const std::string& configPath);

    /** @brief Load the terminus config of the named endpoints from EM.
     *
     *  Sources, strongest first:
     *  - a PLDM device record whose MCTPTargetName is the endpoint's name:
     *    TerminusName, TerminusInstance and CpuIndex. It replaces an entry
     *    loaded from the static configuration file, with a warning.
     *  - an entry of the static configuration file for the EID.
     *  - with OEM_NVIDIA, the CPU NvDevice of that name: ParentChassis
     *    without the platform prefix, Instance and CpuIndex. It is a guess,
     *    so it is skipped for an EID the file configures.
     *
     *  The config is applied to the terminus of the EID when it is created.
     *  If the terminus exists already, a config that differs from its own
     *  is applied while it is not initialized, and is reported with a
     *  warning once it is: it then takes effect at the next pldmd start.
     *
     *  Each named endpoint (name and EID) is read once: EM is read only for
     *  the endpoints of @p configurations not read before, and not at all
     *  when there are none, also when an endpoint never gets a terminus.
     *  A failed D-Bus query reads as no records, so it counts as a read
     *  too. A record that arrives later is read by reloadTerminusConfig().
     *
     *  @param[in] configurations - EM config path to MctpInfo, with the
     *                              endpoint name resolved
     *  @param[in] dbusHandler - D-Bus handler used to read the EM records
     *  @param[in] force - read every named endpoint, also one read before
     */
    void loadTerminusConfigFromEm(
        const Configurations& configurations,
        const pldm::utils::DBusHandlerInterface& dbusHandler,
        bool force = false);

    /** @brief Read the terminus config of the terminus @p tid from EM
     *         again, as loadTerminusConfigFromEm() does for its endpoint,
     *         also when the endpoint was read before. Used when a record
     *         naming the endpoint is added while the terminus is being
     *         initialized. No-op for a terminus without an MCTP endpoint or
     *         without an endpoint name, and for an initialized terminus,
     *         to which a new config could no longer apply.
     *
     *  @param[in] tid - Terminus ID
     *  @param[in] dbusHandler - D-Bus handler used to read the EM records
     */
    void reloadTerminusConfig(
        tid_t tid, const pldm::utils::DBusHandlerInterface& dbusHandler =
                       pldm::utils::DBusHandler());

    /** @brief Send request PLDM message to tid. The function will
     *         return when received the response message from terminus.
     *
     *  @param[in] tid - tid
     *  @param[in] request - request PLDM message
     *  @param[out] responseMsg - response PLDM message
     *  @param[out] responseLen - length of response PLDM message
     *  @return coroutine return_value - PLDM completion code
     */
    exec::task<int> SendRecvPldmMsg(tid_t tid, Request& request,
                                    const pldm_msg** responseMsg,
                                    size_t* responseLen);

    /** @brief Send request PLDM message to eid. The function will
     *         return when received the response message from terminus.
     *
     *  @param[in] eid - eid
     *  @param[in] request - request PLDM message
     *  @param[out] responseMsg - response PLDM message
     *  @param[out] responseLen - length of response PLDM message
     *  @return coroutine return_value - PLDM completion code
     */
    virtual exec::task<int> SendRecvPldmMsgOverMctp(
        mctp_eid_t eid, Request& request, const pldm_msg** responseMsg,
        size_t* responseLen);

    /** @brief member functions to map/unmap tid
     */
    std::optional<MctpInfo> toMctpInfo(const tid_t& tid);
    std::optional<tid_t> toTid(const MctpInfo& mctpInfo);
    std::optional<tid_t> mapTid(const MctpInfo& mctpInfo);
    std::optional<tid_t> mapTid(const MctpInfo& mctpInfo, tid_t tid);
    void unmapTid(const tid_t& tid);

    /** @brief getter of local EID
     *
     *  @return uint8_t - local EID
     */
    mctp_eid_t getLocalEid() const
    {
        return localEid;
    }

    /** @brief Get the event loop reference
     *  @return Reference to the sdeventplus event loop
     */
    sdeventplus::Event& getEvent()
    {
        return event;
    }

    /** @brief return terminus by uuid
     */
    std::shared_ptr<Terminus> getTerminus(const UUID& uuid);

    /** @brief The managed termini, by TID. */
    const std::map<tid_t, std::shared_ptr<Terminus>>& getTermini() const
    {
        return termini;
    }

    /** @brief resume Terminus's ID by send setTID command again
     *
     *  @param[in] tid - Terminus ID
     *  @return coroutine return_value - PLDM completion code
     */
    exec::task<int> resumeTid(tid_t tid);

    /** @brief Show Numeric Sensors without Aux Names **/
    bool numericSensorsWithoutAuxName;

  private:
    /** @brief The coroutine task execute by discoverMctpTerminus()
     *
     *  @return coroutine return_value - PLDM completion code
     */
    exec::task<int> discoverMctpTerminusTask();

    /** @brief Initialize terminus and then instantiate terminus object to keeps
     *         the data fetched from terminus
     *
     *  @param[in] mctpInfo - NetworkId, EID and UUID
     */
    exec::task<int> initMctpTerminus(const MctpInfo& mctpInfo);

    /** @brief Apply the configured instance, terminus name and CPU index for
     *         an EID, if any, to a freshly created Terminus
     *
     *  @param[in] tid - Terminus ID of the terminus to configure
     *  @param[in] eid - EID the configuration is keyed on
     */
    void applyTerminusConfig(tid_t tid, mctp_eid_t eid);

    /** @brief Store one PLDMTermini entry of the static configuration file
     *
     *  @param[in] terminus - the JSON object describing one terminus
     */
    void storeStaticTerminusEntry(const nlohmann::json& terminus);

    /** @brief Store the terminus config @p config of @p source for @p eid,
     *         and apply it to the terminus of @p endpoints if it exists:
     *         while it is not initialized; a change after that is only
     *         reported.
     *
     *  @param[in] eid - EID the configuration is keyed on
     *  @param[in] config - instance, terminus name and CPU index
     *  @param[in] source - where the config comes from, for the logs
     *  @param[in] endpoints - the configured endpoints with that EID
     */
    void storeTerminusConfig(mctp_eid_t eid, const TerminusConfigEntry& config,
                             const std::string& source,
                             const std::vector<MctpInfo>& endpoints);

    /** @brief Refresh the cached types of a known terminus and act on a
     *         PLDM Type 2 change; keep the cache on GetPLDMTypes failure. */
    exec::task<void> refreshTerminusSupportedTypes(
        std::shared_ptr<Terminus> terminus, mctp_eid_t eid);

    /** @brief Send getTID PLDM command to destination EID and then return the
     *         value of tid in reference parameter.
     *
     *  @param[in] eid - Destination EID
     *  @param[out] tid - TID returned from terminus
     *  @return coroutine return_value - PLDM completion code
     */
    exec::task<int> getTidOverMctp(mctp_eid_t eid, tid_t& tid);

    /** @brief Send setTID command to destination EID.
     *
     *  @param[in] eid - Destination EID
     *  @param[in] tid - Terminus ID
     *  @return coroutine return_value - PLDM completion code
     */
    exec::task<int> setTidOverMctp(mctp_eid_t eid, tid_t tid);

    /** @brief Wrapper around getTidOverMctp() that retries the getTID exchange
     *         a bounded number of times on transient MCTP failure, so a single
     *         non-response during discovery does not permanently drop the
     *         terminus for this boot.
     *
     *  @param[in] eid - Destination EID
     *  @param[out] tid - TID returned from terminus
     *  @return coroutine return_value - PLDM completion code
     */
    exec::task<int> getTidOverMctpWithRetry(mctp_eid_t eid, tid_t& tid);

    /** @brief Wrapper around setTidOverMctp() that retries the setTID exchange
     *         a bounded number of times on transient MCTP failure. An
     *         unsupported-command response is treated as terminal (not
     * retried).
     *
     *  @param[in] eid - Destination EID
     *  @param[in] tid - Terminus ID
     *  @return coroutine return_value - PLDM completion code
     */
    exec::task<int> setTidOverMctpWithRetry(mctp_eid_t eid, tid_t tid);

    /** @brief Max attempts for the MCTP TID-discovery commands (getTID/setTID)
     *         before giving up on a terminus for this discovery pass. A single
     *         transient MCTP non-response (e.g. a CX still stabilising after
     *         reset release) must not permanently drop the endpoint for the
     *         boot. Overridable by unit tests to disable retry/backoff.
     */
    uint8_t mctpTidDiscoveryMaxAttempts = 3;

    /** @brief Delay between MCTP TID-discovery retry attempts (500 ms).
     *         Overridable by unit tests.
     */
    uint64_t mctpTidDiscoveryRetryDelayUsec = 500000;

    /** @brief Send getPLDMTypes command to destination EID and then return the
     *         value of supportedTypes in reference parameter.
     *
     *  @param[in] tid - Destination TID
     *  @param[out] supportedTypes - Supported Types returned from terminus
     *  @return coroutine return_value - PLDM completion code
     */
    exec::task<int> getPLDMTypes(tid_t tid, uint64_t& supportedTypes);

    /** @brief getTerminusUID command
     *
     *  @param[in] tid - Destination TID
     *  @param[out] uuid - UUID in string format xxxxxxxx-xxxx-xxxx-xxxxxxxxxxxx
     *  @return coroutine return_value - PLDM completion code
     */
    exec::task<int> getTerminusUID(tid_t tid, UUID& uuidStr);

    sdeventplus::Event& event;
    RequesterHandler& handler;
    InstanceIdDb& instanceIdDb;

    /** @brief Managed termini list */
    std::map<tid_t, std::shared_ptr<Terminus>>& termini;

    /** @brief local EID */
    mctp_eid_t localEid;

    /** @brief tables for maintaining assigned TID */
    std::vector<bool> tidPool;
    std::map<tid_t, SupportedTransportLayer> transportLayerTable;
    std::map<tid_t, MctpInfo> mctpInfoTable;

    /** @brief Mapping of EID to terminus config (instance, name, cpuIndex) */
    std::unordered_map<mctp_eid_t, TerminusConfigEntry> eidToTerminusConfigMap;

    /** @brief EIDs configured by the static configuration file */
    std::set<mctp_eid_t> fileConfiguredEids;

    /** @brief Named endpoints (name, EID) whose terminus config
     *         loadTerminusConfigFromEm() has read from EM */
    std::set<std::pair<std::string, mctp_eid_t>> emConfigEndpointsRead;

    /** @brief A queue of MctpInfos to be discovered **/
    std::queue<MctpInfos> queuedMctpInfos{};

    /** @brief coroutine handle of discoverTerminusTask */
    std::optional<std::pair<exec::async_scope, std::optional<int>>>
        discoverMctpTerminusTaskHandle;

    /** @brief A Manager interface for calling the hook functions **/
    Manager* manager;

    /** @brief Rate-limit PLDM send/recv error logs per EID to avoid flooding
     *  when a terminus repeatedly fails to respond. Cleared on success.
     */
    pldm::utils::LogRateLimiter<mctp_eid_t> pldmErrorLogLimiter;
};
} // namespace platform_mc
} // namespace pldm
