#pragma once

#include "platform-mc/state_set.hpp"

#include <com/nvidia/MemorySpareChannel/server.hpp>

namespace pldm
{
namespace platform_mc
{

using MemorySpareChannelIntf = sdbusplus::server::object_t<
    sdbusplus::com::nvidia::server::MemorySpareChannel>;

class StateSetMemorySpareChannel : public StateSet
{
  private:
    uint8_t compId = 0;

  public:
    StateSetMemorySpareChannel(uint16_t stateSetId, uint8_t compId,
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
        ValueIntf =
            std::make_unique<MemorySpareChannelIntf>(bus, objectPath.c_str());
        setDefaultValue();
    }

    ~StateSetMemorySpareChannel() = default;

    void setValue(uint8_t value) override
    {
        switch (value)
        {
            case PLDM_STATESET_PRESENCE_PRESENT:
                ValueIntf->memorySpareChannelPresence(true);
                break;
            case PLDM_STATESET_PRESENCE_NOT_PRESENT:
            default:
                ValueIntf->memorySpareChannelPresence(false);
                break;
        }
    }

    void setDefaultValue() override
    {
        ValueIntf->memorySpareChannelPresence(false);
    }

    std::tuple<std::string, std::string> getEventData() const override
    {
        if (ValueIntf->memorySpareChannelPresence())
        {
            return {std::string("ResourceEvent.1.0.ResourceStatusChangedOK"),
                    std::string("True")};
        }
        else
        {
            return {std::string("ResourceEvent.1.0.ResourceStatusChangedOK"),
                    std::string("False")};
        }
    }

    std::string getStringStateType() const override
    {
        return std::string("MemorySpareChannelPresence");
    }

    std::unique_ptr<MemorySpareChannelIntf> ValueIntf = nullptr;
};

} // namespace platform_mc
} // namespace pldm