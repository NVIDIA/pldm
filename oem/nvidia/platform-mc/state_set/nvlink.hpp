#pragma once

#include "config.h"

#include "libpldm/state_set_oem_nvidia.h"

#include "common/types.hpp"
#include "platform-mc/state_set.hpp"

#include <xyz/openbmc_project/Inventory/Decorator/Instance/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Endpoint/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Port/server.hpp>
#include <xyz/openbmc_project/State/Decorator/SecureState/server.hpp>
namespace pldm
{
namespace platform_mc
{
namespace oem_nvidia
{

using PortValueIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Inventory::Item::server::Port>;

using PortType =
    sdbusplus::xyz::openbmc_project::Inventory::Item::server::Port::PortType;
using PortProtocol = sdbusplus::xyz::openbmc_project::Inventory::Item::server::
    Port::PortProtocol;
using PortLinkStates =
    sdbusplus::xyz::openbmc_project::Inventory::Item::server::Port::LinkStates;
using PortLinkStatus = sdbusplus::xyz::openbmc_project::Inventory::Item::
    server::Port::LinkStatusType;

using EndpointIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Inventory::Item::server::Endpoint>;
using InstanceIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Inventory::Decorator::server::Instance>;

class StateSetNvlink : public StateSet
{
  private:
    std::unique_ptr<PortValueIntf> ValueIntf = nullptr;
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
        ValueIntf = std::make_unique<PortValueIntf>(bus, objectPath.c_str());

        setDefaultValue();
    }

    ~StateSetNvlink() = default;

    void setValue(uint8_t value) override
    {
        switch (value)
        {
            case PLDM_STATE_SET_NVLINK_INACTIVE:
                ValueIntf->linkState(PortLinkStates::Disabled);
                ValueIntf->linkStatus(PortLinkStatus::LinkDown);
                break;
            case PLDM_STATE_SET_NVLINK_ACTIVE:
                ValueIntf->linkState(PortLinkStates::Enabled);
                ValueIntf->linkStatus(PortLinkStatus::LinkUp);
                break;
            case PLDM_STATE_SET_NVLINK_ERROR:
                ValueIntf->linkState(PortLinkStates::Error);
                ValueIntf->linkStatus(PortLinkStatus::NoLink);
                break;
            default:
                ValueIntf->linkState(PortLinkStates::Unknown);
                ValueIntf->linkStatus(PortLinkStatus::NoLink);
                break;
        }
    }

    void setDefaultValue() override
    {
        ValueIntf->type(PortType::BidirectionalPort);
        ValueIntf->protocol(PortProtocol::NVLink);
        ValueIntf->linkState(PortLinkStates::Unknown);
        ValueIntf->linkStatus(PortLinkStatus::NoLink);
    }

    std::tuple<std::string, std::string> getEventData() const override
    {
        if (ValueIntf->linkStatus() == PortLinkStatus::LinkUp)
        {
            return {std::string("ResourceEvent.1.0.ResourceStatusChanged"),
                    std::string("LinkUp")};
        }
        else if (ValueIntf->linkStatus() == PortLinkStatus::LinkDown)
        {
            return {
                std::string("ResourceEvent.1.0.ResourceStatusChangedWarning"),
                std::string("LinkDown")};
        }
        else if (ValueIntf->linkState() == PortLinkStates::Error)
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

        pldm::pdr::EntityInstance instanceNumber = 0;
        constexpr auto instanceInterface =
            "xyz.openbmc_project.Inventory.Decorator.Instance";
        constexpr auto instanceProperty = "InstanceNumber";

        try
        {
            instanceNumber = utils::DBusHandler().getDbusProperty<uint64_t>(
                stateAssociation.path.c_str(), instanceProperty,
                instanceInterface);
        }
        catch (const std::exception& e)
        {
            instanceNumber = 0xFFFF;
        }

        std::string endpointName =
            std::filesystem::path(stateAssociation.path).filename();
        std::string endpointObjectPath = fabricsObjectPath + c2clinkFabricPrefix +
                                         std::to_string(instanceNumber) +
                                         "/Endpoints/" + endpointName;

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
