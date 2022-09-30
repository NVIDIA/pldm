#include "common/types.hpp"
#include "common/utils.hpp"

#include <sdbusplus/bus.hpp>

constexpr auto dbusProperties = "org.freedesktop.DBus.Properties";
constexpr auto mapperService = "xyz.openbmc_project.ObjectMapper";
constexpr auto mapperPath = "/xyz/openbmc_project/object_mapper";
constexpr auto mapperInterface = "xyz.openbmc_project.ObjectMapper";

/**
 * @brief Get the D-Bus service using mapper lookup
 *
 * @param[in] bus
 * @param[in] path
 * @param[in] interface
 * @return std::string
 */
inline std::string getService(sdbusplus::bus::bus& bus, const char* path,
                       const char* interface)
{
    using DbusInterfaceList = std::vector<std::string>;
    std::map<std::string, std::vector<std::string>> mapperResponse;

    auto mapper = bus.new_method_call(mapperService, mapperPath,
                                      mapperInterface, "GetObject");
    mapper.append(path, DbusInterfaceList({interface}));

    auto mapperResponseMsg = bus.call(mapper);
    mapperResponseMsg.read(mapperResponse);
    return mapperResponse.begin()->first;
}

/**
 * @brief set D-Bus property. New bus will be used for every set to avoid
 * contention with single thread using same bus
 *
 * @param[in] dbusMap - D-Bus mappings
 * @param[in] value - value to set
 */
inline void setDBusProperty(const pldm::utils::DBusMapping& dbusMap,
                     const std::string& value)
{
    auto bus = sdbusplus::bus::new_default();
    std::string dBusService =
        getService(bus, dbusMap.objectPath.c_str(), dbusMap.interface.c_str());
    auto method = bus.new_method_call(
        dBusService.c_str(), dbusMap.objectPath.c_str(), dbusProperties, "Set");
    pldm::utils::PropertyValue propertyValue = value;
    method.append(dbusMap.interface.c_str(), dbusMap.propertyName.c_str(),
                  propertyValue);
    bus.call_noreply(method);
}