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

#include "../state_set.hpp"

#include <libpldm/state_set.h>

#include <xyz/openbmc_project/State/Boot/Progress/server.hpp>

#include <filesystem>
#include <regex>

namespace pldm
{
namespace platform_mc
{

using BootProgressIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::State::Boot::server::Progress>;

using BootProgressStages = sdbusplus::xyz::openbmc_project::State::Boot::
    server::Progress::ProgressStages;

class StateSetProcessorOsStates : public StateSet
{
  public:
    StateSetProcessorOsStates(uint16_t stateSetId, uint8_t compId,
                              std::string& objectPath,
                              dbus::PathAssociation& stateAssociation) :
        StateSet(stateSetId), compId(compId), objectPath(objectPath)
    {
        auto& bus = pldm::utils::DBusHandler::getBus();
        associationDefinitionsIntf =
            std::make_unique<AssociationDefinitionsInft>(bus,
                                                         objectPath.c_str());
        associationDefinitionsIntf->associations(
            {{stateAssociation.forward.c_str(),
              stateAssociation.reverse.c_str(),
              stateAssociation.path.c_str()}});
        bootProgressIntf =
            std::make_unique<BootProgressIntf>(bus, objectPath.c_str());
        setDefaultValue();
    }

    ~StateSetProcessorOsStates() = default;

    void setValue(uint8_t value) override
    {
        presentState = value;
        switch (value)
        {
            case PLDM_STATE_SET_EMBEDDED_PROC_OS_RESET_BOOT_ROM:
                bootProgressIntf->bootProgress(
                    BootProgressStages::ResetBootROM);
                break;
            case PLDM_STATE_SET_EMBEDDED_PROC_OS_FW_BOOT_STAGE1:
                bootProgressIntf->bootProgress(
                    BootProgressStages::PrimaryProcInit);
                break;
            case PLDM_STATE_SET_EMBEDDED_PROC_OS_FW_BOOT_STAGE2:
                bootProgressIntf->bootProgress(
                    BootProgressStages::MotherboardInit);
                break;
            case PLDM_STATE_SET_EMBEDDED_PROC_OS_PRE_OS:
                bootProgressIntf->bootProgress(
                    BootProgressStages::SystemInitComplete);
                break;
            case PLDM_STATE_SET_EMBEDDED_PROC_OS_BOOTING:
                bootProgressIntf->bootProgress(BootProgressStages::OSStart);
                break;
            case PLDM_STATE_SET_EMBEDDED_PROC_OS_RUNNING:
                bootProgressIntf->bootProgress(BootProgressStages::OSRunning);
                break;
            case PLDM_STATE_SET_EMBEDDED_PROC_OS_QUIESCED:
                bootProgressIntf->bootProgress(BootProgressStages::OSQuiesced);
                break;
            case PLDM_STATE_SET_EMBEDDED_PROC_OS_FW_UPDATE_IN_PROGRESS:
                bootProgressIntf->bootProgress(
                    BootProgressStages::FWUpdateInProgress);
                break;
            case PLDM_STATE_SET_EMBEDDED_PROC_OS_CRASH_DUMP_IN_PROGRESS:
                bootProgressIntf->bootProgress(
                    BootProgressStages::OSCrashDumpInProgress);
                break;
            case PLDM_STATE_SET_EMBEDDED_PROC_OS_CRASH_DUMP_COMPLETED:
                bootProgressIntf->bootProgress(
                    BootProgressStages::OSCrashDumpCompleted);
                break;
            case PLDM_STATE_SET_EMBEDDED_PROC_OS_FW_FAULT_IN_PROGRESS:
                bootProgressIntf->bootProgress(
                    BootProgressStages::FWFaultInProgress);
                break;
            case PLDM_STATE_SET_EMBEDDED_PROC_OS_FW_FAULT_COMPLETED:
                bootProgressIntf->bootProgress(
                    BootProgressStages::FWFaultCompleted);
                break;
            case PLDM_STATE_SET_EMBEDDED_PROC_OS_RESET_BOOT_ROM_2:
                bootProgressIntf->bootProgress(
                    BootProgressStages::ResetBootROM);
                break;
            default:
                bootProgressIntf->bootProgress(BootProgressStages::Unspecified);
                break;
        }
    }

    void setDefaultValue() override
    {
        presentState = PLDM_STATE_SET_EMBEDDED_PROC_OS_RESET_BOOT_ROM;
        setValue(presentState);
    }

    std::tuple<std::string, std::string, Level, std::string, std::string>
        getEventData([[maybe_unused]] utils::SensorEventInfo* sensorEventInfo)
            const override
    {
        switch (bootProgressIntf->bootProgress())
        {
            case BootProgressStages::OSRunning:
                return {
                    std::string("ResourceEvent.1.0.ResourceStatusChangedOK"),
                    std::string("OSRunning"), Level::Informational, "", ""};
            case BootProgressStages::OSQuiesced:
                return {std::string(
                            "ResourceEvent.1.0.ResourceStatusChangedWarning"),
                        std::string("OSQuiesced"), Level::Warning, "", ""};
            case BootProgressStages::FWUpdateInProgress:
                return {std::string(
                            "ResourceEvent.1.0.ResourceStatusChangedWarning"),
                        std::string("FWUpdateInProgress"), Level::Warning, "",
                        ""};
            case BootProgressStages::OSCrashDumpInProgress:
                return {std::string(
                            "ResourceEvent.1.0.ResourceStatusChangedWarning"),
                        std::string("OSCrashDumpInProgress"), Level::Warning,
                        "", ""};
            case BootProgressStages::OSCrashDumpCompleted:
                return {std::string(
                            "ResourceEvent.1.0.ResourceStatusChangedWarning"),
                        std::string("OSCrashDumpCompleted"), Level::Warning, "",
                        ""};
            case BootProgressStages::FWFaultInProgress:
                return {std::string(
                            "ResourceEvent.1.0.ResourceStatusChangedWarning"),
                        std::string("FWFaultInProgress"), Level::Warning, "",
                        ""};
            case BootProgressStages::FWFaultCompleted:
                return {std::string(
                            "ResourceEvent.1.0.ResourceStatusChangedWarning"),
                        std::string("FWFaultCompleted"), Level::Warning, "",
                        ""};
            case BootProgressStages::OEM:
                return {std::string(
                            "ResourceEvent.1.0.ResourceStatusChangedWarning"),
                        std::string("OEM"), Level::Warning, "", ""};
            default:
                return {
                    std::string("ResourceEvent.1.0.ResourceStatusChangedOK"),
                    std::string("Booting"), Level::Informational, "", ""};
        }
    }

    std::string getStringStateType() const override
    {
        return std::string("ProcessorOSState");
    }

    void setAssociation(
        std::vector<dbus::PathAssociation>& stateAssociations) override
    {
        if (!associationDefinitionsIntf)
        {
            return;
        }
        Associations assocs{};
        for (const auto& assoc : stateAssociations)
        {
            assocs.emplace_back(assoc.forward.c_str(), assoc.reverse.c_str(),
                                assoc.path.c_str());
            if (assoc.forward == "chassis")
            {
                assocs.emplace_back("chassis", "os_states", assoc.path.c_str());
            }
        }
        associationDefinitionsIntf->associations(assocs);
    }

    void updateSensorName([[maybe_unused]] std::string name) override
    {
        objectName = name;
        if (name == objectPath.filename())
        {
            return;
        }
        objectPath = objectPath.parent_path() / name;

        auto& bus = pldm::utils::DBusHandler::getBus();
        auto path = std::regex_replace(objectPath.string(),
                                       std::regex("[^a-zA-Z0-9_/]+"), "_");
        if (associationDefinitionsIntf)
        {
            auto associations = associationDefinitionsIntf->associations();
            associationDefinitionsIntf =
                std::make_unique<AssociationDefinitionsInft>(bus, path.c_str());
            associationDefinitionsIntf->associations(associations);
        }

        if (bootProgressIntf)
        {
            bootProgressIntf =
                std::make_unique<BootProgressIntf>(bus, path.c_str());
        }
        setDefaultValue();
        setValue(presentState);
    }

  private:
    std::unique_ptr<BootProgressIntf> bootProgressIntf = nullptr;
    [[maybe_unused]] uint8_t compId = 0;
    std::filesystem::path objectPath;
    std::string objectName;
    uint8_t presentState;
};

} // namespace platform_mc
} // namespace pldm
