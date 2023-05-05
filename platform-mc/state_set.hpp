#pragma once
#include "libpldm/platform.h"

#include "common/types.hpp"
#include "common/utils.hpp"
#include "platform-mc/oem_base.hpp"
#include <xyz/openbmc_project/Association/Definitions/server.hpp>

namespace pldm
{
namespace platform_mc
{

class StateEffecter;
class StateSensor;
class StateSet;

using StateSets = std::vector<std::shared_ptr<StateSet>>;
using namespace sdbusplus;
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
    virtual void setValue(uint8_t value) = 0;
    virtual void setDefaultValue() = 0;
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

class StateSetCreator
{
  public:
    static std::unique_ptr<StateSet>
        createSensor(uint16_t stateSetId, uint8_t compId, std::string& path,
                     dbus::PathAssociation& stateAssociation, StateSensor* sensor);

    static std::unique_ptr<StateSet>
        createEffecter(uint16_t stateSetId, uint8_t compId, std::string& path,
                       dbus::PathAssociation& stateAssociation,
                       StateEffecter* effecter);
};

} // namespace platform_mc
} // namespace pldm
