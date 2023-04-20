#pragma once

#include "platform-mc/state_set.hpp"

#include <xyz/openbmc_project/Inventory/Item/Port/server.hpp>
#include <xyz/openbmc_project/State/Decorator/SecureState/server.hpp>

namespace pldm
{
namespace platform_mc
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

class StateSetPciePortLinkState : public StateSet
{
  public:
    StateSetPciePortLinkState(uint16_t stateSetId, uint8_t compId,
                      std::string& objectPath,
                      dbus::PathAssociation& stateAssociation) :
        StateSet(stateSetId),
        compId(compId)
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

    ~StateSetPciePortLinkState() = default;

    void setValue(uint8_t value) override
    {
        switch (value)
        {
            case PLDM_STATESET_LINK_STATE_DISCONNECTED:
                ValueIntf->linkState(PortLinkStates::Disabled);
                ValueIntf->linkStatus(PortLinkStatus::LinkDown);
                break;
            case PLDM_STATESET_LINK_STATE_CONNECTED:
                ValueIntf->linkState(PortLinkStates::Enabled);
                ValueIntf->linkStatus(PortLinkStatus::LinkUp);
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
        ValueIntf->protocol(PortProtocol::PCIe);
        ValueIntf->linkState(PortLinkStates::Unknown);
        ValueIntf->linkStatus(PortLinkStatus::NoLink);
    }

    std::tuple<std::string, std::string> getEventData() const override
    {
        if (ValueIntf->linkStatus() == PortLinkStatus::LinkUp)
        {
            return {std::string("ResourceEvent.1.0.ResourceStatusChangedOK"),
                    std::string("Active")};
        }
        else if (ValueIntf->linkStatus() == PortLinkStatus::LinkDown)
        {
            return {
                std::string("ResourceEvent.1.0.ResourceStatusChangedWarning"),
                std::string("Inactive")};
        }
        else if (ValueIntf->linkState() == PortLinkStates::Error)
        {
            return {
                std::string("ResourceEvent.1.0.ResourceStatusChangedCritical"),
                std::string("Error")};
        }
        else
        {
            return {
                std::string("ResourceEvent.1.0.ResourceStatusChanged"),
                std::string("Unknown")};
        }
    }

    std::string getStringStateType() const override
    {
        return std::string("PCIe");
    }

  private:
    std::unique_ptr<PortValueIntf> ValueIntf = nullptr;
    uint8_t compId = 0;
};

} // namespace platform_mc
} // namespace pldm
