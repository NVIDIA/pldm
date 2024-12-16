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
#pragma once

#include "../libpldmresponder/pdr_utils.hpp"

#include <nlohmann/json.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <string>
#include <vector>

using Json = nlohmann::json;

class PdrJsonParser
{
  public:
    PdrJsonParser(bool verbose, sdbusplus::asio::object_server& server) :
        verbose(verbose), server(server)
    {}

    ::pldm_pdr* parse(Json& json, ::pldm_pdr* pdrRepo);

  private:
    void parseNumericEffecter(Json& json, ::pldm_pdr* pdrRepo);

    void parseStateEffecter(Json& json, ::pldm_pdr* pdrRepo);

    void parseStateSensor(Json& json, ::pldm_pdr* pdrRepo);

    void parseNumericSensor(Json& json, ::pldm_pdr* pdrRepo);

    void parseEntityAssociation(Json& json, ::pldm_pdr* pdrRepo);

    void parseEntry(::pldm_pdr* pdrRepo, Json json);

    pldm_effecter_init parseEffecterInit(std::string string);

    static const std::vector<Json> emptyList;
    static const std::map<std::string, Json> emptyMap;
    static const Json empty;
    static int currentEffecterId;

    bool verbose{false};
    sdbusplus::asio::object_server& server;
};
