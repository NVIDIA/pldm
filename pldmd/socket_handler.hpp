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
#include "invoker.hpp"
#include "requester/handler.hpp"
#include "socket_manager.hpp"

#include <sdeventplus/event.hpp>
#include <sdeventplus/source/io.hpp>

#include <map>
#include <optional>
#include <unordered_map>

namespace pldm::fw_update
{
class Manager;
}

namespace pldm::mctp_socket
{

using PathName = std::string;

using namespace sdeventplus;
using namespace sdeventplus::source;
using namespace pldm::responder;

/** @class Handler
 *
 *  The Handler class abstracts the communication with multiple MCTP Tx/Rx
 *  daemons which supports different transport mechanisms. The initialisation of
 *  this class is driven by the discovery of MCTP.Endpoint interface which
 *  exposes the socket information to communicate with the endpoints.  This
 *  manager class handles the data to be read on the communication sockets by
 *  registering callbacks for EPOLLIN.
 */
class Handler
{
  public:
    Handler() = delete;
    Handler(const Handler&) = delete;
    Handler(Handler&&) = default;
    Handler& operator=(const Handler&) = delete;
    Handler& operator=(Handler&&) = default;
    virtual ~Handler() = default;

    const uint8_t MCTP_MSG_TYPE_PLDM = 1;

    /** @brief Constructor
     *
     *  @param[in] event - PLDM daemon's main event loop
     *  @param[in] handler - PLDM request handler
     *  @param[in] invoker - PLDM responder handler
     *  @param[in] fwManager - Firmware update manager
     *  @param[in] verbose - Verbose tracing flag
     *  @param[in/out] manager - MCTP socket manager
     */
    explicit Handler(sdeventplus::Event& event,
                     requester::Handler<requester::Request>& handler,
                     Invoker& invoker, fw_update::Manager& fwManager,
                     Manager& manager, bool verbose) :
        event(event),
        handler(handler), invoker(invoker), fwManager(fwManager),
        manager(manager), verbose(verbose)
    {}

    int registerMctpEndpoint(EID eid, int type, int protocol,
                             const std::vector<uint8_t>& pathName);

  private:
    sdeventplus::Event& event;
    requester::Handler<requester::Request>& handler;
    Invoker& invoker;
    fw_update::Manager& fwManager;
    Manager& manager;
    bool verbose;

    SocketInfo initSocket(int type, int protocol,
                          const std::vector<uint8_t>& pathName);

    std::optional<Response>
        processRxMsg(const std::vector<uint8_t>& requestMsg);

    /** @brief Socket information for MCTP Tx/Rx daemons */
    std::map<std::vector<uint8_t>,
             std::tuple<std::unique_ptr<pldm::utils::CustomFD>, SendBufferSize,
                        std::unique_ptr<IO>>>
        socketInfoMap;
};

} // namespace pldm::mctp_socket