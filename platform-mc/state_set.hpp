#pragma once
#include "libpldm/platform.h"

#include "common/types.hpp"
#include "common/utils.hpp"

#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/State/Decorator/PowerSystemInputs/server.hpp>
#include <xyz/openbmc_project/State/ProcessorPerformance/server.hpp>

namespace pldm
{
namespace platform_mc
{

using PerformanceValueIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::State::server::ProcessorPerformance>;
using PowerSupplyValueIntf =
    sdbusplus::server::object_t<sdbusplus::xyz::openbmc_project::State::
                                    Decorator::server::PowerSystemInputs>;
using PerformanceStates = sdbusplus::xyz::openbmc_project::State::server::
    ProcessorPerformance::PerformanceStates;
using PowerSupplyInputStatus = sdbusplus::xyz::openbmc_project::State::
    Decorator::server::PowerSystemInputs::Status;
using AssociationDefinitionsInft = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Association::server::Definitions>;

class StateSet
{
  protected:
    uint16_t id;
    std::unique_ptr<AssociationDefinitionsInft> associationDefinitionsIntf =
        nullptr;

  public:
    StateSet(uint16_t id) : id(id)
    {}
    virtual ~StateSet() = default;
    virtual void setValue(uint8_t value) const = 0;
    virtual void setDefaultValue() const = 0;

    virtual void setAssociation(dbus::PathAssociation& stateAssociation)
    {
        if (!associationDefinitionsIntf)
        {
            return;
        }

        associationDefinitionsIntf->associations(
            {{stateAssociation.forward.c_str(),
              stateAssociation.reverse.c_str(),
              stateAssociation.path.c_str()}});
    }
};

class StateSetPerformance : public StateSet
{
  private:
    std::unique_ptr<PerformanceValueIntf> ValueIntf = nullptr;

  public:
    StateSetPerformance(uint16_t stateSetId, std::string& objectPath,
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
        ValueIntf =
            std::make_unique<PerformanceValueIntf>(bus, objectPath.c_str());
        setDefaultValue();
    }
    ~StateSetPerformance() = default;
    void setValue(uint8_t value) const override
    {
        switch (value)
        {
            case PLDM_STATESET_PERFORMANCE_NORMAL:
                ValueIntf->value(PerformanceStates::Normal);
                break;
            case PLDM_STATESET_PERFORMANCE_THROTTLED:
                ValueIntf->value(PerformanceStates::Throttled);
                break;
            default:
                ValueIntf->value(PerformanceStates::Unknown);
                break;
        }
    }
    void setDefaultValue() const override
    {
        ValueIntf->value(PerformanceStates::Unknown);
    }
};

class StateSetPowerSupplyInput : public StateSet
{
  private:
    std::unique_ptr<PowerSupplyValueIntf> ValueIntf = nullptr;

  public:
    StateSetPowerSupplyInput(uint16_t stateSetId, std::string& objectPath,
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
        ValueIntf =
            std::make_unique<PowerSupplyValueIntf>(bus, objectPath.c_str());
        setDefaultValue();
    }
    ~StateSetPowerSupplyInput() = default;
    void setValue(uint8_t value) const override
    {
        switch (value)
        {
            case PLDM_STATESET_POWERSUPPLY_NORMAL:
                ValueIntf->status(PowerSupplyInputStatus::Good);
                break;
            case PLDM_STATESET_POWERSUPPLY_OUTOFRANGE:
                ValueIntf->status(PowerSupplyInputStatus::InputOutOfRange);
                break;
            default:
                ValueIntf->status(PowerSupplyInputStatus::Unknown);
                break;
        }
    }
    void setDefaultValue() const override
    {
        ValueIntf->status(PowerSupplyInputStatus::Unknown);
    }
};

class StateSetCreator
{
  public:
    static std::unique_ptr<StateSet>
        create(uint16_t stateSetId, std::string& path,
               dbus::PathAssociation& stateAssociation)
    {
        if (stateSetId == PLDM_STATESET_ID_PERFORMANCE)
        {
            return std::make_unique<StateSetPerformance>(stateSetId, path,
                                                         stateAssociation);
        }
        else if (stateSetId == PLDM_STATESET_ID_POWERSUPPLY)
        {
            return std::make_unique<StateSetPowerSupplyInput>(stateSetId, path,
                                                              stateAssociation);
        }
        else
        {
            std::cout << "State Sensor PDR Info, Set ID is unknonw"
                      << std::endl;
            return nullptr;
        }
    }
};

} // namespace platform_mc
} // namespace pldm
