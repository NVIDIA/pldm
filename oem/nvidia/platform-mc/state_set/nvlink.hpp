#pragma once

#include "config.h"

#include "libpldm/state_set_oem_nvidia.h"

#include "common/types.hpp"
#include "platform-mc/state_set.hpp"

#include <xyz/openbmc_project/Inventory/Decorator/Instance/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Endpoint/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/PortInfo/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/PortState/server.hpp>
#include <xyz/openbmc_project/State/Decorator/SecureState/server.hpp>

#include <filesystem>

namespace pldm
{
namespace platform_mc
{
namespace oem_nvidia
{

using PortInfoIntf = sdbusplus::server::object_t<
    sdbusplus::server::xyz::openbmc_project::inventory::decorator::PortInfo>;
using PortStateIntf = sdbusplus::server::object_t<
    sdbusplus::server::xyz::openbmc_project::inventory::decorator::PortState>;

using PortType =
    sdbusplus::server::xyz::openbmc_project::inventory::decorator::PortInfo::PortType;
using PortProtocol =
    sdbusplus::server::xyz::openbmc_project::inventory::decorator::PortInfo::PortProtocol;
using PortLinkStates =
    sdbusplus::server::xyz::openbmc_project::inventory::decorator::PortState::LinkStates;
using PortLinkStatus =
    sdbusplus::server::xyz::openbmc_project::inventory::decorator::PortState::LinkStatusType;

using EndpointIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Inventory::Item::server::Endpoint>;
using InstanceIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Inventory::Decorator::server::Instance>;

class StateSetNvlink : public StateSet
{
  private:
    std::unique_ptr<PortInfoIntf> ValuePortInfoIntf = nullptr;
    std::unique_ptr<PortStateIntf> ValuePortStateIntf = nullptr;
    std::unique_ptr<EndpointIntf> endpointIntf = nullptr;
    std::unique_ptr<AssociationDefinitionsInft>
        endpointAssociationDefinitionsIntf = nullptr;
    std::unique_ptr<InstanceIntf> endpointInstanceIntf = nullptr;

    // C2CLink fabric prefix
    const std::string fabricsObjectPath =
        "/xyz/openbmc_project/inventory/system/fabrics/";
#ifdef PLATFORM_PREFIX
    const std::string c2clinkFabricPrefix = PLATFORM_PREFIX "_C2CLinkFabric_";
#else
    const std::string c2clinkFabricPrefix = "C2CLinkFabric_";
#endif

  public:
    StateSetNvlink(uint16_t stateSetId, std::string& objectPath,
                   dbus::PathAssociation& stateAssociation) :
        StateSet(stateSetId)
    {
        auto& bus = pldm::utils::DBusHandler::getBus();
        associationDefinitionsIntf =
            std::make_unique<AssociationDefinitionsInft>(bus,
                                                         objectPath.c_str());
        associationDefinitionsIntf->associations(
            {{stateAssociation.forward.c_str(),
              stateAssociation.reverse.c_str(),
              stateAssociation.path.c_str()}});
        ValuePortInfoIntf = std::make_unique<PortInfoIntf>(bus, objectPath.c_str());
        ValuePortStateIntf = std::make_unique<PortStateIntf>(bus, objectPath.c_str());

        setDefaultValue();
    }

    ~StateSetNvlink() = default;

    void setValue(uint8_t value) override
    {
        switch (value)
        {
            case PLDM_STATE_SET_NVLINK_INACTIVE:
                ValuePortStateIntf->linkState(PortLinkStates::Disabled);
                ValuePortStateIntf->linkStatus(PortLinkStatus::LinkDown);
                break;
            case PLDM_STATE_SET_NVLINK_ACTIVE:
                ValuePortStateIntf->linkState(PortLinkStates::Enabled);
                ValuePortStateIntf->linkStatus(PortLinkStatus::LinkUp);
                break;
            case PLDM_STATE_SET_NVLINK_ERROR:
                ValuePortStateIntf->linkState(PortLinkStates::Error);
                ValuePortStateIntf->linkStatus(PortLinkStatus::NoLink);
                break;
            default:
                ValuePortStateIntf->linkState(PortLinkStates::Unknown);
                ValuePortStateIntf->linkStatus(PortLinkStatus::NoLink);
                break;
        }
    }

    void setDefaultValue() override
    {
        ValuePortInfoIntf->type(PortType::BidirectionalPort);
        ValuePortInfoIntf->protocol(PortProtocol::NVLink);
        ValuePortStateIntf->linkState(PortLinkStates::Unknown);
        ValuePortStateIntf->linkStatus(PortLinkStatus::NoLink);
    }

    std::tuple<std::string, std::string> getEventData() const override
    {
        if (ValuePortStateIntf->linkStatus() == PortLinkStatus::LinkUp)
        {
            return {std::string("ResourceEvent.1.0.ResourceStatusChanged"),
                    std::string("LinkUp")};
        }
        else if (ValuePortStateIntf->linkStatus() == PortLinkStatus::LinkDown)
        {
            return {
                std::string("ResourceEvent.1.0.ResourceStatusChangedWarning"),
                std::string("LinkDown")};
        }
        else if (ValuePortStateIntf->linkState() == PortLinkStates::Error)
        {
            return {
                std::string("ResourceEvent.1.0.ResourceStatusChangedCritical"),
                std::string("Error")};
        }
        else
        {
            return {std::string("ResourceEvent.1.0.ResourceStatusChanged"),
                    std::string("Unknown")};
        }
    }

    std::string getStringStateType() const override
    {
        return std::string("NVLink");
    }

    virtual void setAssociation(dbus::PathAssociation& stateAssociation)
    {
        if (!associationDefinitionsIntf)
        {
            return;
        }

        if (stateAssociation.path.empty())
        {
            return;
        }

        associationDefinitionsIntf->associations(
            {{stateAssociation.forward.c_str(),
              stateAssociation.reverse.c_str(),
              stateAssociation.path.c_str()}});

        if (stateAssociation.path.empty())
        {
            return;
        }

        pldm::pdr::EntityInstance instanceNumber = 0;
        constexpr auto instanceInterface =
            "xyz.openbmc_project.Inventory.Decorator.Instance";
        constexpr auto instanceProperty = "InstanceNumber";

        try
        {
            // C2C NVLink instanceNumber should pick processorModule SMBIOS
            // instanceNumber instead of CPU SMBIOS instanceNumber.
            // CPU is counted per processorModule so all CPU
            // SMBIOS instanceNumber is 0 on CG4.
            // ProcessModule is counted per baseboard so its instanceNumber is
            // 0~3 on CG4.
            std::string parentPath =
                std::filesystem::path(stateAssociation.path).parent_path();
            instanceNumber = utils::DBusHandler().getDbusProperty<uint64_t>(
                parentPath.c_str(), instanceProperty, instanceInterface);
        }
        catch (const std::exception& e)
        {
            return;
        }

        std::string endpointName =
            std::filesystem::path(stateAssociation.path).filename();
        std::string endpointObjectPath =
            fabricsObjectPath + c2clinkFabricPrefix +
            std::to_string(instanceNumber) + "/Endpoints/" + endpointName;

        auto& bus = pldm::utils::DBusHandler::getBus();

        if (!endpointIntf)
        {
            endpointIntf =
                std::make_unique<EndpointIntf>(bus, endpointObjectPath.c_str());
        }

        if (!endpointInstanceIntf)
        {
            endpointInstanceIntf =
                std::make_unique<InstanceIntf>(bus, endpointObjectPath.c_str());
            endpointInstanceIntf->instanceNumber(instanceNumber);
        }

        if (!endpointAssociationDefinitionsIntf)
        {
            endpointAssociationDefinitionsIntf =
                std::make_unique<AssociationDefinitionsInft>(
                    bus, endpointObjectPath.c_str());
            endpointAssociationDefinitionsIntf->associations(
                {{"entity_link", "", stateAssociation.path.c_str()}});
        }
    }
};

} // namespace oem_nvidia
} // namespace platform_mc
} // namespace pldm
