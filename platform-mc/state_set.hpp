#pragma once
#include "libpldm/platform.h"

#include "common/types.hpp"
#include "common/utils.hpp"

#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/Control/Boot/ClearNonVolatileVariables/server.hpp>
#include <xyz/openbmc_project/Control/Processor/RemoteDebug/server.hpp>
#include <xyz/openbmc_project/State/Decorator/PowerSystemInputs/server.hpp>
#include <xyz/openbmc_project/State/Decorator/SecureState/server.hpp>
#include <xyz/openbmc_project/State/PresenceState/server.hpp>
#include <xyz/openbmc_project/State/ProcessorPerformance/server.hpp>

namespace pldm
{
namespace platform_mc
{

class StateEffecter;

using namespace sdbusplus;
using PerformanceValueIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::State::server::ProcessorPerformance>;
using PowerSupplyValueIntf =
    sdbusplus::server::object_t<sdbusplus::xyz::openbmc_project::State::
                                    Decorator::server::PowerSystemInputs>;
using RemoteDebugValueIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Control::Processor::server::RemoteDebug>;
using SecureStateValueIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::State::Decorator::server::SecureState>;
using ClearNonVolatileVariablesValueIntf =
    sdbusplus::server::object_t<sdbusplus::xyz::openbmc_project::Control::Boot::
                                    server::ClearNonVolatileVariables>;
using RemoteDebugInterfaceIntf =
    sdbusplus::xyz::openbmc_project::Control::Processor::server::RemoteDebug;
using PerformanceStates = sdbusplus::xyz::openbmc_project::State::server::
    ProcessorPerformance::PerformanceStates;
using PowerSupplyInputStatus = sdbusplus::xyz::openbmc_project::State::
    Decorator::server::PowerSystemInputs::Status;
using ClearNonVolatileVariablesIntf = sdbusplus::xyz::openbmc_project::Control::
    Boot::server::ClearNonVolatileVariables;
using AssociationDefinitionsInft = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Association::server::Definitions>;
using PresenceStateIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::State::server::PresenceState>;
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

    virtual uint8_t getValue()
    {
        return 0;
    }

    uint16_t getStateSetId()
    {
        return id;
    }
    virtual std::string getStringStateType() const = 0;
    virtual std::tuple<std::string, std::string> getEventData() const = 0;
};

class StateSetPerformance : public StateSet
{
  private:
    std::unique_ptr<PerformanceValueIntf> ValueIntf = nullptr;
    uint8_t compId = 0;

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
    std::tuple<std::string, std::string> getEventData() const override
    {
        if (ValueIntf->value() == PerformanceStates::Normal)
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
};

class StateSetPowerSupplyInput : public StateSet
{
  private:
    std::unique_ptr<PowerSupplyValueIntf> ValueIntf = nullptr;
    uint8_t compId = 0;

  public:
    StateSetPowerSupplyInput(uint16_t stateSetId, uint8_t compId,
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
    std::tuple<std::string, std::string> getEventData() const override
    {
        if (ValueIntf->status() == PowerSupplyInputStatus::Good)
        {
            return {std::string("ResourceEvent.1.0.ResourceStatusChangedOK"),
                    std::string("Normal")};
        }
        else
        {
            return {
                std::string("ResourceEvent.1.0.ResourceStatusChangedWarning"),
                std::string("Current Input out of Range")};
        }
    }
    std::string getStringStateType() const override
    {
        return std::string("EDP VIolation State");
    }
};

class RemoteDebugStateIntf : public RemoteDebugValueIntf
{
  public:
    RemoteDebugStateIntf(bus::bus& bus, const char* path, uint8_t compId) :
        RemoteDebugValueIntf(bus, path), compId(compId)
    {}

    using RemoteDebugInterfaceIntf::enabled;
    virtual void update(bool value)
    {
        RemoteDebugInterfaceIntf::enabled(value);
    }

  protected:
    uint8_t compId = 0;
};

class RemoteDebugEffecterIntf : public RemoteDebugStateIntf
{
  public:
    RemoteDebugEffecterIntf(bus::bus& bus, const char* path, uint8_t compId,
                            StateEffecter& effecter) :
        RemoteDebugStateIntf(bus, path, compId),
        effecter(effecter)
    {}

    using RemoteDebugInterfaceIntf::enabled;
    void update(bool value);
    bool enabled(bool value) override;

  private:
    StateEffecter& effecter;
};

class StateSetRemoteDebug : public StateSet
{
  private:
    std::unique_ptr<RemoteDebugStateIntf> ValueIntf = nullptr;
    uint8_t compId = 0;

  public:
    StateSetRemoteDebug(uint16_t stateSetId, uint8_t compId,
                        std::string& objectPath,
                        dbus::PathAssociation& stateAssociation,
                        StateEffecter* effecter) :
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

        if (effecter != nullptr)
        {
            ValueIntf = std::make_unique<RemoteDebugEffecterIntf>(
                bus, objectPath.c_str(), compId, *effecter);
        }
        else
        {
            ValueIntf = std::make_unique<RemoteDebugStateIntf>(
                bus, objectPath.c_str(), compId);
        }
        setDefaultValue();
    }
    ~StateSetRemoteDebug() = default;
    void setValue(uint8_t value) const override
    {
        if (value == PLDM_STATESET_LINK_STATE_CONNECTED)
        {
            ValueIntf->update(true);
        }
        else
        {
            ValueIntf->update(false);
        }
    }
    void setDefaultValue() const override
    {
        ValueIntf->update(false);
    }
    uint8_t getValue()
    {
        return ValueIntf->enabled();
    }
    std::tuple<std::string, std::string> getEventData() const override
    {
        if (ValueIntf->enabled())
        {
            return {std::string("ResourceEvent.1.0.ResourceStatusChangedOK"),
                    std::string("Enabled")};
        }
        else
        {
            return {std::string("ResourceEvent.1.0.ResourceStatusChangedOK"),
                    std::string("Disabled")};
        }
    }
    std::string getStringStateType() const override
    {
        return std::string("RemoteDebug");
    }
};

class ClearNonVolatileVariablesStateIntf :
    public ClearNonVolatileVariablesValueIntf
{
  public:
    ClearNonVolatileVariablesStateIntf(bus::bus& bus, const char* path,
                                       uint8_t compId) :
        ClearNonVolatileVariablesValueIntf(bus, path),
        compId(compId)
    {}

    using ClearNonVolatileVariablesIntf::clear;
    virtual void update(bool value)
    {
        ClearNonVolatileVariablesIntf::clear(value);
    }

  protected:
    uint8_t compId = 0;
};

class ClearNonVolatileVariablesEffecterIntf :
    public ClearNonVolatileVariablesStateIntf
{
  public:
    ClearNonVolatileVariablesEffecterIntf(bus::bus& bus, const char* path,
                                          uint8_t compId,
                                          StateEffecter& effecter) :
        ClearNonVolatileVariablesStateIntf(bus, path, compId),
        effecter(effecter)
    {}

    using ClearNonVolatileVariablesIntf::clear;
    void update(bool value);
    bool clear(bool value) override;

  private:
    StateEffecter& effecter;
};

class StateSetClearNonvolatileVariable : public StateSet
{
  private:
    std::unique_ptr<ClearNonVolatileVariablesStateIntf> ValueIntf = nullptr;
    uint8_t compId = 0;

  public:
    StateSetClearNonvolatileVariable(uint16_t stateSetId, uint8_t compId,
                                     std::string& objectPath,
                                     dbus::PathAssociation& stateAssociation,
                                     StateEffecter* effecter) :
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

        if (effecter != nullptr)
        {
            ValueIntf = std::make_unique<ClearNonVolatileVariablesEffecterIntf>(
                bus, objectPath.c_str(), compId, *effecter);
        }
        else
        {
            ValueIntf = std::make_unique<ClearNonVolatileVariablesStateIntf>(
                bus, objectPath.c_str(), compId);
        }
        setDefaultValue();
    }
    ~StateSetClearNonvolatileVariable() = default;
    void setValue(uint8_t value) const override
    {
        switch (value)
        {
            case PLDM_STATESET_BOOT_REQUEST_REQUESTED:
                ValueIntf->update(true);
                break;
            default:
            case PLDM_STATESET_BOOT_REQUEST_NORMAL:
                ValueIntf->update(false);
                break;
        }
    }
    void setDefaultValue() const override
    {
        ValueIntf->update(false);
    }
    uint8_t getValue()
    {
        uint8_t state = 0;
        if (ValueIntf->clear())
        {
            state = PLDM_STATESET_BOOT_REQUEST_REQUESTED;
        }
        else
        {
            state = PLDM_STATESET_BOOT_REQUEST_NORMAL;
        }
        return state;
    }
    std::tuple<std::string, std::string> getEventData() const override
    {
        if (ValueIntf->clear())
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
        return std::string("ClearNonvolatileVariable");
    }
};

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

    void setValue(uint8_t value) const override
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

    void setDefaultValue() const override
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

class StateSetCreator
{
  public:
    static std::unique_ptr<StateSet>
        createSensor(uint16_t stateSetId, uint8_t compId, std::string& path,
                     dbus::PathAssociation& stateAssociation);

    static std::unique_ptr<StateSet>
        createEffecter(uint16_t stateSetId, uint8_t compId, std::string& path,
                       dbus::PathAssociation& stateAssociation,
                       StateEffecter* effecter);
};

} // namespace platform_mc
} // namespace pldm
