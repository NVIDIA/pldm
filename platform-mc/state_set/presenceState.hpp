#pragma once

#include "../state_set.hpp"

#include <xyz/openbmc_project/State/PresenceState/server.hpp>

namespace pldm
{
namespace platform_mc
{

using PresenceStateIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::State::server::PresenceState>;

class StateSetPresenceState : public StateSet
{
  private:
    std::unique_ptr<PresenceStateIntf> ValueIntf = nullptr;
    uint8_t compId = 0;

  public:
    StateSetPresenceState(uint16_t stateSetId, uint8_t compId,
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
            std::make_unique<PresenceStateIntf>(bus, objectPath.c_str());
        setDefaultValue();
    }

    ~StateSetPresenceState() = default;

    void setValue(uint8_t value) override
    {
        switch (value)
        {
            case PLDM_STATESET_PRESENCE_PRESENT:
                ValueIntf->presence(true);
                break;
            case PLDM_STATESET_PRESENCE_NOT_PRESENT:
            default:
                ValueIntf->presence(false);
                break;
        }
    }

    void setDefaultValue() override
    {
        ValueIntf->presence(false);
    }

    std::tuple<std::string, std::string> getEventData() const override
    {
        if (ValueIntf->presence())
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
        return std::string("Presence");
    }
};

} // namespace platform_mc
} // namespace pldm