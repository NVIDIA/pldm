/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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

#include <sdbusplus/exception.hpp>

#include <string>
#include <string_view>

namespace errors
{
using namespace std::literals::string_literals;
class InvalidArgument final : public sdbusplus::exception::internal_exception
{
    public:
        explicit InvalidArgument(std::string_view propertyNameArg):
            propertyName(propertyNameArg),
            errWhatDetailed("Invalid argument was given for property: "s +
                             description()){}
        InvalidArgument(std::string_view propertyNameArg, std::string_view info):
            propertyName(propertyNameArg),
            errWhatDetailed(("Invalid argument was given for property: "s +
                            description() + ". "s).append(info)){}

        const char* name() const noexcept override
        {
            return "xyz.openbmc_project.Common.Error.InvalidArgument";
        }

        const char* description() const noexcept override
        {
            return "Out of range";
        }

        const char* what() const noexcept override
        {
            return errWhatDetailed.c_str();
        }

        int get_errno() const noexcept override
        {
            return static_cast<int>(std::errc::invalid_argument);
        }

        std::string propertyName;

    private:
        std::string errWhatDetailed;
};

} // namespace errors
