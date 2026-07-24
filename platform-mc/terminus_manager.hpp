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

#include "requester/handler.hpp"
#include "terminus.hpp"

#include <queue>
#include <unordered_map>

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

class Manager;
/**
 * @brief TerminusManager
 *
 * TerminusManager class handle the task to discover and initialize PLDM
 * terminus.
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

    /** @brief Member functions to store the mctp info and tid to terminus info
     *         list.
     *
     *  @param[in] mctpInfos - list information of the MCTP endpoints
     *  @param[in] tid - Destination TID
     *
     *  @return tid - Terminus tid
     */
    std::optional<pldm_tid_t> storeTerminusInfo(const MctpInfo& mctpInfo,
                                                pldm_tid_t tid);

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
    std::unordered_map<mctp_eid_t,
                       std::tuple<int, std::string, std::optional<uint16_t>>>
        eidToTerminusConfigMap;

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
