#include "state_effecter.hpp"

#include "libpldm/platform.h"

#include "common/utils.hpp"

#include <math.h>

#include <limits>
#include <regex>

namespace pldm
{
namespace platform_mc
{

StateEffecter::StateEffecter(const uint8_t tid,
                  const bool effecterDisabled,
                  const uint16_t effecterId,
                  StateSetInfo effecterInfo,
                  std::string& effecterName, std::string& associationPath) :
    tid(tid), effecterId(effecterId), effecterInfo(effecterInfo)
{
    std::string path = "/xyz/openbmc_project/control/" + effecterName;
    path = std::regex_replace(path, std::regex("[^a-zA-Z0-9_/]+"), "_");

    auto& bus = pldm::utils::DBusHandler::getBus();
    availabilityIntf = std::make_unique<AvailabilityIntf>(bus, path.c_str());
    availabilityIntf->available(true);

    operationalStatusIntf =
        std::make_unique<OperationalStatusIntf>(bus, path.c_str());
    operationalStatusIntf->functional(!effecterDisabled);

    auto effecters = std::get<1>(effecterInfo);
    for (auto& effecter : effecters)
    {
        auto stateSetId = std::get<0>(effecter);
        dbus::PathAssociation association = {
            "chassis", "all_controls",
            associationPath};
        std::string stateSetPath = path + "/Id_" + std::to_string(stateSets.size());
        auto stateSet = StateSetCreator::create(stateSetId, stateSetPath, association);
        if (stateSet != nullptr)
        {
            stateSets.emplace_back(std::move(stateSet));
        }
    }
}

} // namespace platform_mc
} // namespace pldm
