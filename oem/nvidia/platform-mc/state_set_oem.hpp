#pragma once

#include "libpldm/state_set_oem_nvidia.h"

#include "platform-mc/state_set.hpp"

#include <xyz/openbmc_project/Inventory/Decorator/PortInfo/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/PortState/server.hpp>

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

class StateSetNvlink : public StateSet
{
  private:
    std::unique_ptr<PortInfoIntf> ValuePortInfoIntf = nullptr;
    std::unique_ptr<PortStateIntf> ValuePortStateIntf = nullptr;

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
    void setValue(uint8_t value) const override
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
    void setDefaultValue() const override
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
            return {std::string("ResourceEvent.1.0.ResourceStatusChangedOK"),
                    std::string("Active")};
        }
        else if (ValuePortStateIntf->linkStatus() == PortLinkStatus::LinkDown)
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
