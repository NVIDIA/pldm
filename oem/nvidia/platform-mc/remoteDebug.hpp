#pragma once

#include "libpldm/state_set_oem_nvidia.h"

#include "platform-mc/numeric_effecter.hpp"
#include "platform-mc/oem_base.hpp"
#include "platform-mc/state_effecter.hpp"
#include "platform-mc/state_sensor.hpp"

#include <xyz/openbmc_project/Common/error.hpp>
#include <xyz/openbmc_project/Control/Processor/RemoteDebug/server.hpp>

namespace pldm
{
namespace platform_mc
{
namespace oem_nvidia
{

using RemoteDebugIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Control::Processor::server::RemoteDebug>;

using DebugState = sdbusplus::xyz::openbmc_project::Control::Processor::server::
    RemoteDebug::DebugState;

using DebugPolicy = sdbusplus::xyz::openbmc_project::Control::Processor::
    server::RemoteDebug::DebugPolicy;

class StateSetDebugState : public StateSet
{
  public:
    StateSetDebugState(uint16_t stateSetId, uint8_t compId,
                       [[maybe_unused]] std::string& objectPath,
                       [[maybe_unused]] dbus::PathAssociation& stateAssociation,
                       [[maybe_unused]] StateEffecter* effecter) :
        StateSet(stateSetId),
        compId(compId)
    {
        auto& bus = pldm::utils::DBusHandler::getBus();
        setDefaultValue();
        associationDefinitionsIntf =
            std::make_unique<AssociationDefinitionsInft>(bus,
                                                         objectPath.c_str());
    }

    ~StateSetDebugState() = default;

    void setValue(uint8_t v) override
    {
        value = v;
    }

    void setDefaultValue() override
    {
        value = PLDM_STATE_SET_DEBUG_STATE_OFFLINE;
    }

    uint8_t getValue()
    {
        return value;
    }

    std::string getStringStateType() const override
    {
        return std::string("DebugState");
    }

    std::tuple<std::string, std::string> getEventData() const override
    {
        if (value == PLDM_STATE_SET_DEBUG_STATE_DISABLED)
        {
            return {std::string("ResourceEvent.1.0.ResourceStatusChanged"),
                    std::string("Disable")};
        }
        else if (value == PLDM_STATE_SET_DEBUG_STATE_ENABLED)
        {
            return {std::string("ResourceEvent.1.0.ResourceStatusChanged"),
                    std::string("Enabled")};
        }
        else if (value == PLDM_STATE_SET_DEBUG_STATE_OFFLINE)
        {
            return {std::string("ResourceEvent.1.0.ResourceStatusChanged"),
                    std::string("Offline")};
        }
        else
        {
            return {std::string("ResourceEvent.1.0.ResourceStatusChanged"),
                    std::string("Unknown")};
        }
    }

  private:
    uint8_t value;
    uint8_t compId;
};

class OemRemoteDebugIntf : public OemIntf, public RemoteDebugIntf
{
    enum DebugCompId : uint8_t
    {
        JTAGEN_COMP_ID = 0,
        DEVEN_COMP_ID,
        SPNIDEN_COMP_ID,
        SPIDEN_COMP_ID,
        NIDEN_COMP_ID,
        DBGEN_COMP_ID,
        INVALID_COMP_ID = 255
    };

  public:
    OemRemoteDebugIntf(bus::bus& bus, const char* path,
                       std::shared_ptr<StateEffecter> stateEffecter,
                       std::shared_ptr<NumericEffecter> numericEffecter,
                       std::shared_ptr<StateSensor> stateSensor) :
        RemoteDebugIntf(bus, path),
        stateEffecter(stateEffecter), numericEffecter(numericEffecter),
        stateSensor(stateSensor)
    {}

    DebugState jtagDebug() const override
    {
        return getDebugState(JTAGEN_COMP_ID);
    }

    DebugState deviceDebug() const override
    {
        return getDebugState(DEVEN_COMP_ID);
    }

    DebugState securePrivilegeNonInvasiveDebug() const override
    {
        return getDebugState(SPNIDEN_COMP_ID);
    }

    DebugState securePrivilegeInvasiveDebug() const override
    {
        return getDebugState(SPIDEN_COMP_ID);
    }

    DebugState nonInvasiveDebug() const override
    {
        return getDebugState(NIDEN_COMP_ID);
    }

    DebugState invasiveDebug() const override
    {
        return getDebugState(DBGEN_COMP_ID);
    }

    uint32_t timeout(uint32_t value, bool skipSignal)
    {
        numericEffecter
            ->setNumericEffecterValue(numericEffecter->baseToRaw(value))
            .detach();
        return RemoteDebugIntf::timeout(value, skipSignal);
    }

    uint32_t timeout() const override
    {
        numericEffecter->getNumericEffecterValue().detach();
        return numericEffecter->rawToBase(numericEffecter->getValue());
    }

    void enable(std::vector<DebugPolicy> debugPolicy) override
    {
        uint8_t cmpEffCnt = stateEffecter->stateSets.size();
        std::vector<set_effecter_state_field> stateField(cmpEffCnt,
                                                         {PLDM_NO_CHANGE, 0});
        for (auto& v : debugPolicy)
        {
            auto compId = toCompId(v);
            if (compId == INVALID_COMP_ID)
            {
                throw sdbusplus::xyz::openbmc_project::Common::Error::
                    InvalidArgument();
            }

            if (stateSensor->stateSets[compId]->getValue() ==
                PLDM_STATE_SET_DEBUG_STATE_OFFLINE)
            {
                throw sdbusplus::xyz::openbmc_project::Common::Error::
                    NotAllowed();
            }
            stateField[compId] = {PLDM_REQUEST_SET,
                                  PLDM_STATE_SET_DEBUG_STATE_ENABLED};
        }

        stateEffecter->setStateEffecterStates(stateField).detach();
    }

    void disable(std::vector<DebugPolicy> debugPolicy) override
    {
        uint8_t cmpEffCnt = stateEffecter->stateSets.size();
        std::vector<set_effecter_state_field> stateField(cmpEffCnt,
                                                         {PLDM_NO_CHANGE, 0});

        for (auto& v : debugPolicy)
        {
            auto compId = toCompId(v);
            if (compId == INVALID_COMP_ID)
            {
                throw sdbusplus::xyz::openbmc_project::Common::Error::
                    InvalidArgument();
            }

            if (stateSensor->stateSets[compId]->getValue() ==
                PLDM_STATE_SET_DEBUG_STATE_OFFLINE)
            {
                throw sdbusplus::xyz::openbmc_project::Common::Error::
                    NotAllowed();
            }
            stateField[compId] = {PLDM_REQUEST_SET,
                                  PLDM_STATE_SET_DEBUG_STATE_DISABLED};
        }

        stateEffecter->setStateEffecterStates(stateField).detach();
    }

    uint8_t toCompId(DebugPolicy value)
    {
        switch (value)
        {
            case DebugPolicy::JtagDebug:
                return JTAGEN_COMP_ID;
            case DebugPolicy::DeviceDebug:
                return DEVEN_COMP_ID;
            case DebugPolicy::SecurePrivilegeNonInvasiveDebug:
                return SPNIDEN_COMP_ID;
            case DebugPolicy::SecurePrivilegeInvasiveDebug:
                return SPIDEN_COMP_ID;
            case DebugPolicy::NonInvasiveDebug:
                return NIDEN_COMP_ID;
            case DebugPolicy::InvasiveDebug:
                return DBGEN_COMP_ID;
            default:
                return INVALID_COMP_ID;
        }
    }

    DebugState toDebugState(uint8_t value) const
    {
        switch (value)
        {
            case PLDM_STATE_SET_DEBUG_STATE_ENABLED:
                return DebugState::Enabled;
            case PLDM_STATE_SET_DEBUG_STATE_DISABLED:
                return DebugState::Disabled;
            case PLDM_STATE_SET_DEBUG_STATE_OFFLINE:
                return DebugState::Offline;
            default:
                return DebugState::Unknown;
        }
    }

    DebugState getDebugState(uint8_t compositeId) const
    {
        if (stateEffecter->stateSets[compositeId]->getOpState() ==
            EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING)
        {
            return DebugState::Pending;
        }
        return toDebugState(stateSensor->stateSets[compositeId]->getValue());
    }

  private:
    std::shared_ptr<StateEffecter> stateEffecter;
    std::shared_ptr<NumericEffecter> numericEffecter;
    std::shared_ptr<StateSensor> stateSensor;
};

} // namespace oem_nvidia
} // namespace platform_mc
} // namespace pldm
