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

#include "utils.hpp"

#include <phosphor-logging/lg2.hpp>

#include <queue>
namespace pldm
{
namespace utils
{

constexpr auto entityManagerService = "xyz.openbmc_project.EntityManager";

#ifndef MOCK_DBUS_ASYNC_UTILS
/** @struct coGetDbusProperty
 *
 * An awaitable object needed by co_await operator to get D-Bus Property value
 * e.g.
 * rc = co_await
 * coGetDbusProperty<std::string>("/xyz/openbmc_project/inventory/system/chassis/HGX_GPU_SXM_1/HGX_GPU_SXM_1",
 * "UUID", "xyz.openbmc_project.Configuration.NSM_Chassis",
 * "xyz.openbmc_project.EntityManager");
 *
 * @tparam type - property data type
 */
template <typename type>
struct coGetDbusProperty
{
    const std::string service;
    const std::string objectPath;
    const std::string interface;
    const std::string property;

    /** @brief For keeping the return value.
     */
    type ret;

    /** @brief Returning false to make await_suspend() to be called.
     */
    bool await_ready() noexcept
    {
        return false;
    }

    /** @brief Called by co_await operator before suspending coroutine. The
     * method will send out NSM request message, register a call back function
     * for the event when D-Bus method done.
     */
    bool await_suspend(std::coroutine_handle<> handle)
    {
        auto& asioConnection = utils::DBusHandler::getAsioConnection();

        asioConnection->async_method_call(
            [resumeHandle = handle, &ret = ret,
             this](boost::system::error_code ec, PropertyValue value) {
                if (ec)
                {
                    lg2::error(
                        "error while DbusProperties.Get for intf={INTERFACE}, prop={PROPERTY} and path={OBJECT_PATH}. {ERROR_MESSAGE} ",
                        "INTERFACE", interface, "PROPERTY", property,
                        "OBJECT_PATH", objectPath, "ERROR_MESSAGE",
                        ec.message());
                    ret = type();
                }
                else
                {
                    try
                    {
                        ret = std::get<type>(value);
                    }
                    catch (const std::bad_variant_access& e)
                    {
                        lg2::error(
                            "bad_variant_access while DbusProperties.Get for intf={INTERFACE}, prop={PROPERTY} and path={OBJECT_PATH}: {ERROR_MESSAGE}",
                            "INTERFACE", interface, "PROPERTY", property,
                            "OBJECT_PATH", objectPath, "ERROR_MESSAGE",
                            e.what());
                        ret = type();
                    }
                }
                resumeHandle();
            },
            service.c_str(), objectPath.c_str(),
            "org.freedesktop.DBus.Properties", "Get", interface.c_str(),
            property.c_str());

        return true;
    }

    /** @brief Called by co_await operator to get return value when awaitable
     * object completed.
     */
    type await_resume() const noexcept
    {
        return ret;
    }

    /** @brief Constructor of awaitable object to initialize necessary member
     * variables.
     */
    coGetDbusProperty(const std::string& objectPath,
                      const std::string& property, const std::string& interface,
                      const std::string service = entityManagerService) :
        service(service), objectPath(objectPath), interface(interface),
        property(property), ret{}
    {}
};

/** @struct coGetServiceMap
 *
 * An awaitable object needed by co_await operator to get service map which has
 * the interfaces e.g. auto mapperResponse = co_await
 * utils::coGetServiceMap(objPath, dbus::Interfaces{});
 *
 * @tparam type - property data type
 */
struct coGetServiceMap
{
    const std::string objectPath;
    const dbus::Interfaces ifaceList;

    /** @brief For keeping the return value.
     */
    MapperServiceMap ret;

    /** @brief Returning false to make await_suspend() to be called.
     */
    bool await_ready() noexcept
    {
        return false;
    }

    /** @brief Called by co_await operator before suspending coroutine. The
     * method will send out NSM request message, register a call back function
     * for the event when D-Bus method done.
     */
    bool await_suspend(std::coroutine_handle<> handle) noexcept
    {
        auto& asioConnection = utils::DBusHandler::getAsioConnection();

        asioConnection->async_method_call(
            [resumeHandle = handle, &ret = ret,
             this](boost::system::error_code ec, MapperServiceMap value) {
                if (ec)
                {
                    lg2::debug("coGetServiceMap: GetObject failed for "
                               "path={OBJECT_PATH}: {ERROR_MESSAGE}",
                               "OBJECT_PATH", objectPath, "ERROR_MESSAGE",
                               ec.message());
                }
                else
                {
                    ret = value;
                }
                resumeHandle();
            },
            mapperService, mapperPath, mapperInterface, "GetObject",
            objectPath.c_str(), ifaceList);

        return true;
    }

    /** @brief Called by co_await operator to get return value when awaitable
     * object completed.
     */
    MapperServiceMap await_resume() const noexcept
    {
        return ret;
    }

    /** @brief Constructor of awaitable object to initialize necessary member
     * variables.
     */
    coGetServiceMap(const std::string& objectPath,
                    const dbus::Interfaces& ifaceList) :
        objectPath(objectPath), ifaceList(ifaceList)
    {}
};

/** @struct coSetDbusProperty
 *
 * An awaitable object needed by co_await operator to set a D-Bus property.
 * Resolves the owning service via ObjectMapper.GetObject, then issues
 * Properties.Set. The co_await result is a bool: true on success, false on
 * any failure (mapper lookup, missing owner, or Set call error).
 *
 * Example:
 *   bool ok = co_await coSetDbusProperty<std::string>(
 *       objectPath, interface, property, value);
 *
 * @tparam type - property data type (must match the actual D-Bus signature)
 */
template <typename type>
struct coSetDbusProperty
{
    const std::string& objectPath;
    const std::string& interface;
    const std::string& property;
    const type value;

    /** @brief Result captured from the async chain. */
    bool ret{false};

    /** @brief Returning false to make await_suspend() to be called. */
    bool await_ready() noexcept
    {
        return false;
    }

    /** @brief Called by co_await operator before suspending the coroutine.
     *  Issues mapper GetObject; on success, issues Properties.Set; resumes
     *  the coroutine when both complete (or as soon as either fails).
     */
    bool await_suspend(std::coroutine_handle<> handle) noexcept
    {
        auto& asioConnection = utils::DBusHandler::getAsioConnection();
        using MapperResponse = std::map<std::string, std::vector<std::string>>;
        pldm::utils::PropertyValue dbusValue{value};

        asioConnection->async_method_call(
            [resumeHandle = handle, &ret = ret, asioConnection,
             objectPath = objectPath, interface = interface,
             property = property,
             dbusValue](boost::system::error_code ec, MapperResponse response) {
                if (ec || response.empty())
                {
                    lg2::error(
                        "coSetDbusProperty: mapper lookup failed for path={OBJECT_PATH} intf={INTERFACE}: {ERROR_MESSAGE}",
                        "OBJECT_PATH", objectPath, "INTERFACE", interface,
                        "ERROR_MESSAGE", ec.message());
                    ret = false;
                    resumeHandle();
                    return;
                }
                auto serviceName = response.begin()->first;
                asioConnection->async_method_call(
                    [resumeHandle, &ret, objectPath,
                     property](boost::system::error_code ec2) {
                        if (ec2)
                        {
                            lg2::error(
                                "coSetDbusProperty: Properties.Set failed for prop={PROPERTY} path={OBJECT_PATH}: {ERROR_MESSAGE}",
                                "PROPERTY", property, "OBJECT_PATH", objectPath,
                                "ERROR_MESSAGE", ec2.message());
                            ret = false;
                        }
                        else
                        {
                            ret = true;
                        }
                        resumeHandle();
                    },
                    serviceName, objectPath, "org.freedesktop.DBus.Properties",
                    "Set", interface, property, dbusValue);
            },
            mapperService, mapperPath, mapperInterface, "GetObject", objectPath,
            std::vector<std::string>{interface});

        return true;
    }

    /** @brief Called by co_await operator to retrieve the success flag. */
    bool await_resume() const noexcept
    {
        return ret;
    }

    coSetDbusProperty(const std::string& objectPath,
                      const std::string& interface, const std::string& property,
                      type value) :
        objectPath(objectPath), interface(interface), property(property),
        value(std::move(value))
    {}
};

/** @struct coGetSubtree
 *
 * An awaitable object needed by co_await operator to get service map which has
 * the interfaces e.g. auto subTreeResponse = co_await
 * utils::coGetSubTree(objPath, 0, dbus::Interfaces{});
 *
 * @tparam type - property data type
 */
struct coGetSubTree
{
    const std::string objectPath;
    int depth;
    const dbus::Interfaces ifaceList;

    /** @brief For keeping the return value.
     */
    GetSubTreeResponse ret;

    /** @brief Returning false to make await_suspend() to be called.
     */
    bool await_ready() noexcept
    {
        return false;
    }

    /** @brief Called by co_await operator before suspending coroutine. The
     * method will send out NSM request message, register a call back function
     * for the event when D-Bus method done.
     */
    bool await_suspend(std::coroutine_handle<> handle) noexcept
    {
        auto& asioConnection = utils::DBusHandler::getAsioConnection();

        asioConnection->async_method_call(
            [resumeHandle = handle, &ret = ret,
             this](boost::system::error_code ec, GetSubTreeResponse value) {
                if (ec)
                {
                    lg2::error(
                        "error while xyz.openbmc_project.ObjectMapper.GetSubTree for intf={INTERFACE} and path={OBJECT_PATH}. {ERROR_MESSAGE} ",
                        "OBJECT_PATH", objectPath, "ERROR_MESSAGE",
                        ec.message());
                }
                else
                {
                    ret = value;
                }
                resumeHandle();
            },
            mapperService, mapperPath, mapperInterface, "GetSubTree",
            objectPath.c_str(), depth, ifaceList);

        return true;
    }

    /** @brief Called by co_await operator to get return value when awaitable
     * object completed.
     */
    GetSubTreeResponse await_resume() const noexcept
    {
        return ret;
    }

    /** @brief Constructor of awaitable object to initialize necessary member
     * variables.
     */
    coGetSubTree(const std::string& objectPath, int depth,
                 const dbus::Interfaces& ifaceList) :
        objectPath(objectPath), depth(depth), ifaceList(ifaceList)
    {}
};

#else

template <typename type>
struct coGetDbusProperty
{
    const std::string service;
    const std::string objectPath;
    const std::string interface;
    const std::string property;

    type ret;

    bool await_ready()
    {
        return true;
    }

    bool await_suspend([[maybe_unused]] std::coroutine_handle<> handle) noexcept
    {
        return true;
    }

    type await_resume() const noexcept
    {
        return ret;
    }

    coGetDbusProperty(const std::string& objectPath,
                      const std::string& property, const std::string& interface,
                      const std::string service = entityManagerService) :
        service(service), objectPath(objectPath), interface(interface),
        property(property), ret{}
    {}
};

struct coGetServiceMap
{
    const std::string objectPath;
    const dbus::Interfaces ifaceList;

    MapperServiceMap ret;

    bool await_ready() noexcept
    {
        return true;
    }

    bool await_suspend([[maybe_unused]] std::coroutine_handle<> handle) noexcept
    {
        return true;
    }

    MapperServiceMap await_resume() const noexcept
    {
        return ret;
    }

    coGetServiceMap(const std::string& objectPath,
                    const dbus::Interfaces& ifaceList) :
        objectPath(objectPath), ifaceList(ifaceList)
    {}
};

struct coGetSubTree
{
    const std::string objectPath;
    int depth;
    const dbus::Interfaces ifaceList;

    GetSubTreeResponse ret;

    bool await_ready() noexcept
    {
        return true;
    }

    bool await_suspend([[maybe_unused]] std::coroutine_handle<> handle) noexcept
    {
        return true;
    }

    GetSubTreeResponse await_resume() const noexcept
    {
        return ret;
    }

    coGetSubTree(const std::string& objectPath, int depth,
                 const dbus::Interfaces& ifaceList) :
        objectPath(objectPath), depth(depth), ifaceList(ifaceList)
    {}
};

template <typename type>
struct coSetDbusProperty
{
    const std::string& objectPath;
    const std::string& interface;
    const std::string& property;
    const type value;

    bool ret{true};

    bool await_ready() noexcept
    {
        return true;
    }

    bool await_suspend([[maybe_unused]] std::coroutine_handle<> handle) noexcept
    {
        return true;
    }

    bool await_resume() const noexcept
    {
        return ret;
    }

    coSetDbusProperty(const std::string& objectPath,
                      const std::string& interface, const std::string& property,
                      type value) :
        objectPath(objectPath), interface(interface), property(property),
        value(std::move(value))
    {}
};

#endif

} // namespace utils
} // namespace pldm
