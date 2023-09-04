#pragma once

#include "../state_set.hpp"

#include <xyz/openbmc_project/State/Decorator/Health/server.hpp>

namespace pldm
{
namespace platform_mc
{

using HealthIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::State::Decorator::server::Health>;
using HealthType = sdbusplus::xyz::openbmc_project::State::Decorator::server::
    Health::HealthType;

class StateSetHealthState : public StateSet
{
  private:
    std::unique_ptr<HealthIntf> ValueIntf = nullptr;
    uint8_t compId = 0;

  public:
    StateSetHealthState(uint16_t stateSetId, uint8_t compId,
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
        ValueIntf = std::make_unique<HealthIntf>(bus, objectPath.c_str());
        setDefaultValue();
    }

    ~StateSetHealthState() = default;

    void setValue(uint8_t value) override
    {
        switch (value)
        {
            case PLDM_STATESET_HEALTH_STATE_NORMAL:
                ValueIntf->health(HealthType::OK);
                break;
            case PLDM_STATESET_HEALTH_STATE_NON_CRITICAL:
            case PLDM_STATESET_HEALTH_STATE_UPPER_NON_CRITICAL:
            case PLDM_STATESET_HEALTH_STATE_LOWER_NON_CRITICAL:
                ValueIntf->health(HealthType::Warning);
                break;
            case PLDM_STATESET_HEALTH_STATE_CRITICAL:
            case PLDM_STATESET_HEALTH_STATE_UPPER_CRITICAL:
            case PLDM_STATESET_HEALTH_STATE_LOWER_CRITICAL:
            case PLDM_STATESET_HEALTH_STATE_FATAL:
            case PLDM_STATESET_HEALTH_STATE_LOWER_FATAL:
            case PLDM_STATESET_HEALTH_STATE_UPPER_FATAL:
            default:
                ValueIntf->health(HealthType::Critical);
                break;
        }
    }

    void setDefaultValue() override
    {
        ValueIntf->health(HealthType::OK);
    }

    std::tuple<std::string, std::string> getEventData() const override
    {
        switch (ValueIntf->health())
        {
            case HealthType::Critical:
                return {std::string(
                            "ResourceEvent.1.0.ResourceStatusChangedCritical"),
                        std::string("Critical")};
                break;
            case HealthType::Warning:
                return {std::string(
                            "ResourceEvent.1.0.ResourceStatusChangedWarning"),
                        std::string("Warning")};
                break;
            case HealthType::OK:
            default:
                return {
                    std::string("ResourceEvent.1.0.ResourceStatusChangedOK"),
                    std::string("OK")};
                break;
        }
    }

    std::string getStringStateType() const override
    {
        return std::string("Health");
    }
};

} // namespace platform_mc
} // namespace pldm
