#include "state_sensor.hpp"

#include "libpldm/platform.h"

#include "common/utils.hpp"

#include <math.h>

#include <limits>
#include <regex>

namespace pldm
{
namespace platform_mc
{

StateSensor::StateSensor(const uint8_t tid,
                             const bool sensorDisabled,
                             const uint16_t sensorId,
                             StateSetSensorInfo sensorInfo,
                             std::string& sensorName,
                             std::string& associationPath) :
    tid(tid), sensorId(sensorId), sensorInfo(sensorInfo)
{
    std::string path = "/xyz/openbmc_project/state/" + sensorName;
    path = std::regex_replace(path, std::regex("[^a-zA-Z0-9_/]+"), "_");

    auto& bus = pldm::utils::DBusHandler::getBus();    
    availabilityIntf = std::make_unique<AvailabilityIntf>(bus, path.c_str());
    availabilityIntf->available(true);

    operationalStatusIntf =
        std::make_unique<OperationalStatusIntf>(bus, path.c_str());
    operationalStatusIntf->functional(!sensorDisabled);

    auto stateSensors = std::get<1>(sensorInfo);
    for (auto& sensor : stateSensors)
    {
        auto stateSetId = std::get<0>(sensor);
        dbus::PathAssociation association = {
            "chassis", "all_states",
            associationPath};
        std::string stateSetPath = path + "/Id_" + std::to_string(stateSets.size());
        auto stateSet = StateSetCreator::create(stateSetId, stateSetPath, association);
        if (stateSet != nullptr)
        {
            stateSets.emplace_back(std::move(stateSet));
        }
    }

    //TODO : Polling must be based on event support 
    updateTime = 10000000;
}

void StateSensor::handleErrGetSensorReading()
{
    operationalStatusIntf->functional(false);
    for (auto& stateSet : stateSets)
    {
        stateSet->setDefaultValue();
    }
}

void StateSensor::updateReading(bool available, bool functional,
                                uint8_t compSensorIndex, uint8_t value)
{
    availabilityIntf->available(available);
    operationalStatusIntf->functional(functional);

    if (compSensorIndex < stateSets.size())
    {
        stateSets[compSensorIndex]->setValue(value);
    }
    else
    {
        std::cerr << "State Sensor updateReading index out of range"
                  << std::endl;
    }
}

} // namespace platform_mc
} // namespace pldm
