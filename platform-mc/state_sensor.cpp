#include "state_sensor.hpp"

#include "libpldm/platform.h"

#include "common/utils.hpp"

#include <math.h>

#include <phosphor-logging/lg2.hpp>
#include <xyz/openbmc_project/Logging/Entry/server.hpp>

#include <limits>
#include <regex>

namespace pldm
{
namespace platform_mc
{

using namespace sdbusplus::xyz::openbmc_project::Logging::server;
using Level = sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;

StateSensor::StateSensor(
    const uint8_t tid, const bool sensorDisabled, const uint16_t sensorId,
    StateSetInfo sensorInfo,
    std::vector<std::vector<std::pair<std::string, std::string>>>* sensorNames,
    std::string& associationPath) :
    tid(tid),
    sensorId(sensorId), sensorInfo(sensorInfo), needUpdate(true), async(false)
{
    std::string path = "/xyz/openbmc_project/state/PLDM_Sensor_" +
                       std::to_string(sensorId) + "_" + std::to_string(tid);

    auto& bus = pldm::utils::DBusHandler::getBus();
    availabilityIntf = std::make_unique<AvailabilityIntf>(bus, path.c_str());
    availabilityIntf->available(true);

    operationalStatusIntf =
        std::make_unique<OperationalStatusIntf>(bus, path.c_str());
    operationalStatusIntf->functional(!sensorDisabled);

    auto stateSensors = std::get<1>(sensorInfo);
    uint8_t idx = 0;
    for (auto& sensor : stateSensors)
    {
        auto stateSetId = std::get<0>(sensor);
        dbus::PathAssociation association = {"chassis", "all_states",
                                             associationPath};
        auto compositeSensorId = stateSets.size();
        std::string compositeSensorName = "Id_" + std::to_string(compositeSensorId);
        // pick first en langTag sensor aux name
        if (sensorNames && sensorNames->size() > compositeSensorId)
        {
            for (const auto& [tag, name] : sensorNames->at(compositeSensorId))
            {
                if (tag == "en")
                {
                    compositeSensorName = name;
                }
            }
        }

        std::string objPath = path + "/" + compositeSensorName;
        objPath =
            std::regex_replace(objPath, std::regex("[^a-zA-Z0-9_/]+"), "_");
        auto stateSet = StateSetCreator::createSensor(stateSetId, idx++,
                                                      objPath, association, this);
        if (stateSet != nullptr)
        {
            stateSets.emplace_back(std::move(stateSet));
        }
    }
    sdbusplus::message::object_path entityPath(associationPath);
    associationEntityId = entityPath.filename();
    transform(associationEntityId.begin(), associationEntityId.end(),
              associationEntityId.begin(), ::toupper);
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
        lg2::error(
            "State Sensor id:{SENSORID} updateReading index out of range",
            "SENSORID", sensorId);
    }
}

void StateSensor::handleSensorEvent(uint8_t sensorOffset, uint8_t eventState)
{
    if (sensorOffset < stateSets.size())
    {
        stateSets[sensorOffset]->setValue(eventState);
        std::string arg1 = getAssociationEntityId() + " " +
                           stateSets[sensorOffset]->getStringStateType();
        auto [messageID, arg2] = stateSets[sensorOffset]->getEventData();
        std::string resolution = "None";
        createLogEntry(messageID, arg1, arg2, resolution);
    }
    else
    {
        lg2::error("State Sensor id:{SENSORID} event offset out of range",
                   "SENSORID", sensorId);
    }
}

void StateSensor::createLogEntry(std::string& messageID, std::string& arg1,
                                 std::string& arg2, std::string& resolution)
{
    auto createLog = [&messageID](std::map<std::string, std::string>& addData,
                                  Level& level) {
        static constexpr auto logObjPath = "/xyz/openbmc_project/logging";
        static constexpr auto logInterface =
            "xyz.openbmc_project.Logging.Create";
        auto& bus = pldm::utils::DBusHandler::getBus();

        try
        {
            auto service =
                pldm::utils::DBusHandler().getService(logObjPath, logInterface);
            auto severity = sdbusplus::xyz::openbmc_project::Logging::server::
                convertForMessage(level);
            auto method = bus.new_method_call(service.c_str(), logObjPath,
                                              logInterface, "Create");
            method.append(messageID, severity, addData);
            bus.call_noreply(method);
        }
        catch (const std::exception& e)
        {
            lg2::error(
                "Failed to create D-Bus log entry for sensor message registry, {ERROR}.",
                "ERROR", e);
        }
    };

    std::map<std::string, std::string> addData;
    addData["REDFISH_MESSAGE_ID"] =
        "ResourceEvent.1.0.ResourceStatusChangedWarning";
    Level level = Level::Informational;
    addData["REDFISH_MESSAGE_ARGS"] = arg1 + "," + arg2;
    addData["xyz.openbmc_project.Logging.Entry.Resolution"] = resolution;
    createLog(addData, level);
    return;
}

} // namespace platform_mc
} // namespace pldm