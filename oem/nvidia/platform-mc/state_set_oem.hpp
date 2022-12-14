#pragma once

#include "libpldm/state_set_oem_nvidia.h"

#include "platform-mc/state_set.hpp"

#include <xyz/openbmc_project/Inventory/Item/Port/server.hpp>

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

class StateSetNvlink : public StateSet
{
  private:
    std::unique_ptr<PortValueIntf> ValueIntf = nullptr;

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
    void setValue(uint8_t value) const override
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
    void setDefaultValue() const override
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
            return {std::string("ResourceEvent.1.0.ResourceStatusChangedOK"),
                    std::string("Active")};
        }
        else if (ValueIntf->linkStatus() == PortLinkStatus::LinkDown)
        {
            return {
                std::string("ResourceEvent.1.0.ResourceStatusChangedWarning"),
                std::string("Inactive")};
        }
        else
        {
            return {
                std::string("ResourceEvent.1.0.ResourceStatusChangedCritical"),
                std::string("Error")};
        }
    }

    std::string getStringStateType() const override
    {
        return std::string("NVLink");
    }
};

} // namespace oem_nvidia
} // namespace platform_mc
} // namespace pldm
