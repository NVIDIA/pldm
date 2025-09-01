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

#include "dbusutil.hpp"
#include "fw-update/update_manager.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>
#include <xyz/openbmc_project/Object/Delete/server.hpp>
#include <xyz/openbmc_project/Software/Activation/server.hpp>
#include <xyz/openbmc_project/Software/ActivationBlocksTransition/server.hpp>
#include <xyz/openbmc_project/Software/ActivationProgress/server.hpp>
#include <xyz/openbmc_project/Software/UpdatePolicy/server.hpp>

#include <string>
constexpr auto systemdBusname = "org.freedesktop.systemd1";
constexpr auto systemdInterface = "org.freedesktop.systemd1.Manager";
constexpr auto systemdPath = "/org/freedesktop/systemd1";

namespace pldm
{

namespace fw_update
{

using ActivationIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Software::server::Activation>;
using ActivationProgressIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Software::server::ActivationProgress>;
using DeleteIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Object::server::Delete>;
using UpdatePolicyIntf = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Software::server::UpdatePolicy>;
using ActivationBlocksTransitionInherit = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Software::server::
        ActivationBlocksTransition>;

/** @class ActivationProgress
 *
 *  Concrete implementation of xyz.openbmc_project.Software.ActivationProgress
 *  D-Bus interface
 */
class ActivationProgress : public ActivationProgressIntf
{
  public:
    /** @brief Constructor
     *
     * @param[in] bus - Bus to attach to
     * @param[in] objPath - D-Bus object path
     */
    ActivationProgress(sdbusplus::bus_t& bus, const std::string& objPath) :
        ActivationProgressIntf(bus, objPath.c_str(),
                               action::emit_interface_added)
    {
        progress(0);
    }
};

/** @class Delete
 *
 *  Concrete implementation of xyz.openbmc_project.Object.Delete D-Bus interface
 */
class Delete : public DeleteIntf
{
  public:
    /** @brief Constructor
     *
     *  @param[in] bus - Bus to attach to
     *  @param[in] objPath - D-Bus object path
     *  @param[in] updateManager - Reference to FW update manager
     */
    Delete(sdbusplus::bus_t& bus, const std::string& objPath,
           UpdateManager* updateManager) :
        DeleteIntf(bus, objPath.c_str(), action::emit_interface_added),
        updateManager(updateManager), objPath(objPath)
    {}

    /** @brief Delete the Activation D-Bus object for the FW update package */
    void delete_() override
    {
        updateManager->clearActivationInfo();
    }

  private:
    UpdateManager* updateManager;
    const std::string objPath;
};

/** @class Activation
 *
 *  Concrete implementation of xyz.openbmc_project.Object.Activation D-Bus
 *  interface
 */
class Activation : public ActivationIntf
{
  public:
    /** @brief Constructor
     *
     *  @param[in] bus - Bus to attach to
     *  @param[in] objPath - D-Bus object path
     *  @param[in] updateManager - Reference to FW update manager
     */
    Activation(sdbusplus::bus_t& bus, std::string objPath,
               Activations activationStatus, UpdateManager* updateManager) :
        ActivationIntf(bus, objPath.c_str(), action::defer_emit), bus(bus),
        objPath(objPath), updateManager(updateManager)
    {
        deleteImpl = std::make_unique<Delete>(bus, objPath, updateManager);
        activation(activationStatus);
        emit_object_added();
    }

    using sdbusplus::xyz::openbmc_project::Software::server::Activation::
        activation;
    using sdbusplus::xyz::openbmc_project::Software::server::Activation::
        requestedActivation;

    /** @brief Overriding Activation property setter function
     */
    Activations activation(Activations value) override
    {
        if (value == Activations::Activating)
        {
            deleteImpl.reset();
            namespace software =
                sdbusplus::xyz::openbmc_project::Software::server;
            updateManager->performSecurityChecksAsync(
                [this, updateManager(updateManager)](bool securityCheck) {
                    if (!securityCheck)
                    {
                        lg2::error(
                            "Security checks failed setting activation to fail");
                        updateManager->resetActivationBlocksTransition();
                        updateManager->clearFirmwareUpdatePackage();

                        ActivationIntf::activation(
                            software::Activation::Activations::Failed);
                    }
                    else
                    {
                        auto state = updateManager->activatePackage();

                        if (state == Activations::Failed)
                        {
                            lg2::error(
                                "Activation failed setting activation to fail");
                            updateManager->resetActivationBlocksTransition();
                            updateManager->clearFirmwareUpdatePackage();
                        }
                        else if (state == Activations::Active)
                        {
                            lg2::info("Activation set to active");
                            updateManager->clearFirmwareUpdatePackage();
                        }
                    }
                },
                [this,
                 updateManager(updateManager)](const std::string& errorMsg) {
                    lg2::error(
                        "Security checks failed setting activation to fail");
                    lg2::error(
                        "Exception during activation security check: {ERRORMSG}",
                        "ERRORMSG", errorMsg);
                    updateManager->resetActivationBlocksTransition();
                    updateManager->clearFirmwareUpdatePackage();

                    ActivationIntf::activation(
                        software::Activation::Activations::Failed);
                });
        }
        else if (value == Activations::Active || value == Activations::Failed)
        {
            if (!deleteImpl)
            {
                deleteImpl =
                    std::make_unique<Delete>(bus, objPath, updateManager);
            }
        }
        return ActivationIntf::activation(value);
    }

    /** @brief Overriding RequestedActivations property setter function
     */
    RequestedActivations requestedActivation(
        RequestedActivations value) override
    {
        if ((value == RequestedActivations::Active) &&
            (requestedActivation() != RequestedActivations::Active))
        {
            if ((ActivationIntf::activation() == Activations::Invalid))
            {
                std::string compName = "Firmware Update Service";
                std::string messageError = "Invalid FW Package";
                std::string resolution =
                    "Retry firmware update operation with valid FW package.";
                createLogEntry(resourceErrorDetected, compName, messageError,
                               resolution);
                updateManager->clearFirmwareUpdatePackage();
                activation(Activations::Failed);
            }
            else
            {
                activation(Activations::Activating);
            }
        }
        return ActivationIntf::requestedActivation(value);
    }

  private:
    sdbusplus::bus_t& bus;
    const std::string objPath;
    UpdateManager* updateManager;
    std::unique_ptr<Delete> deleteImpl;
};

/** @class UpdatePolicy
 *
 *  Concrete implementation of xyz.openbmc_project.Software.UpdatePolicy D-Bus
 *  interface
 */
class UpdatePolicy : public UpdatePolicyIntf
{
  public:
    /** @brief Constructor
     *
     *  @param[in] bus - Bus to attach to
     *  @param[in] objPath - D-Bus object path
     *  @param[in] updateManager - Reference to FW update manager
     */
    UpdatePolicy(sdbusplus::bus::bus& bus, const std::string& objPath) :
        UpdatePolicyIntf(bus, objPath.c_str(), action::emit_interface_added)

    {}
};

/**
 * @brief activation block transition implementation
 *
 */
class ActivationBlocksTransition : public ActivationBlocksTransitionInherit
{
  public:
    /**
     * @brief Construct a new Activation Blocks Transition object
     *
     * @param bus
     * @param path
     * @param updateManager
     */
    ActivationBlocksTransition(sdbusplus::bus::bus& bus,
                               const std::string& path,
                               UpdateManager* updateManager) :
        ActivationBlocksTransitionInherit(bus, path.c_str(),
                                          action::emit_interface_added),
        bus(bus), updateManager(updateManager)
    {
        enableRebootGuard();
    }

    /**
     * @brief Destroy the Activation Blocks Transition object
     *
     */
    ~ActivationBlocksTransition()
    {
        disableRebootGuard();
    }

  private:
    sdbusplus::bus::bus& bus;
    UpdateManager* updateManager;

    /**
     * @brief Enable rebootguard
     *
     */
    void enableRebootGuard()
    {
        if (updateManager->fwDebug)
        {
            info("Activating PLDM firmware update package - BMC reboots are "
                 "disabled.");
        }
        try
        {
            auto method = bus.new_method_call(systemdBusname, systemdPath,
                                              systemdInterface, "StartUnit");
            method.append("reboot-guard-enable.service", "replace");
            bus.call_noreply_noerror(method);
        }
        catch (const std::exception& e)
        {
            error("Error starting service.", "ERROR", e);
        }
    }

    /**
     * @brief disable reboot guard
     *
     */
    void disableRebootGuard()
    {
        if (updateManager->fwDebug)
        {
            if (updateManager->isStageOnlyUpdate)
            {
                info("PLDM firmware update package is staged - BMC reboots are "
                     "re-enabled.");
            }
            else
            {
                info(
                    "Activating PLDM firmware update package - BMC reboots are "
                    "re-enabled.");
            }
        }
        try
        {
            auto method = bus.new_method_call(systemdBusname, systemdPath,
                                              systemdInterface, "StartUnit");
            method.append("reboot-guard-disable.service", "replace");
            bus.call_noreply_noerror(method);
        }
        catch (const std::exception& e)
        {
            error("Error starting service.", "ERROR", e);
        }
    }
};

} // namespace fw_update

} // namespace pldm
