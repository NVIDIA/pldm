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

#include "common/instance_id.hpp"

#include <unistd.h>

#include <cstring>
#include <filesystem>

static constexpr uintmax_t pldmMaxInstanceIds = 32;

class TestInstanceIdDb : public pldm::InstanceIdDb
{
  public:
    TestInstanceIdDb() : TestInstanceIdDb(createDb())
    {}

    ~TestInstanceIdDb()
    {
        std::filesystem::remove(dbPath);
    };

  private:
    static std::filesystem::path createDb()
    {
        static const char dbTmpl[] = "/tmp/db.XXXXXX";
        char dbName[sizeof(dbTmpl)] = {};

        ::strncpy(dbName, dbTmpl, sizeof(dbName));
        ::close(::mkstemp(dbName));

        std::filesystem::path dbPath(dbName);
        std::filesystem::resize_file(
            dbPath, static_cast<uintmax_t>(PLDM_MAX_TIDS) * pldmMaxInstanceIds);

        return dbPath;
    };

    TestInstanceIdDb(std::filesystem::path dbPath) :
        InstanceIdDb(dbPath), dbPath(dbPath)
    {}

    std::filesystem::path dbPath;
};
