#pragma once

#include "../state_set.hpp"

#include <xyz/openbmc_project/State/ProcessorPerformance/server.hpp>

namespace pldm
{
namespace platform_mc
{

using ProcessorPerformanceIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::State::server::ProcessorPerformance>;

using ProcessorPerformanceStates = sdbusplus::xyz::openbmc_project::State::
    server::ProcessorPerformance::PerformanceStates;

class StateSetPerformance : public StateSet
{
  public:
    StateSetPerformance(uint16_t stateSetId, uint8_t compId,
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
            std::make_unique<ProcessorPerformanceIntf>(bus, objectPath.c_str());
        setDefaultValue();
    }

    ~StateSetPerformance() = default;

    void setValue(uint8_t value) override
    {
        switch (value)
        {
            case PLDM_STATESET_PERFORMANCE_NORMAL:
                ValueIntf->value(ProcessorPerformanceStates::Normal);
                break;
            case PLDM_STATESET_PERFORMANCE_THROTTLED:
                ValueIntf->value(ProcessorPerformanceStates::Throttled);
                break;
            default:
                ValueIntf->value(ProcessorPerformanceStates::Unknown);
                break;
        }
    }

    void setDefaultValue() override
    {
        ValueIntf->value(ProcessorPerformanceStates::Unknown);
    }

    std::tuple<std::string, std::string> getEventData() const override
    {
        if (ValueIntf->value() == ProcessorPerformanceStates::Normal)
        {
            return {std::string("ResourceEvent.1.0.ResourceStatusChangedOK"),
                    std::string("Normal")};
        }
        else
        {
            return {
                std::string("ResourceEvent.1.0.ResourceStatusChangedWarning"),
                std::string("Throttled")};
        }
    }

    std::string getStringStateType() const override
    {
        return std::string("Performance");
    }

  private:
    std::unique_ptr<ProcessorPerformanceIntf> ValueIntf = nullptr;
    uint8_t compId = 0;
};

} // namespace platform_mc
} // namespace pldm