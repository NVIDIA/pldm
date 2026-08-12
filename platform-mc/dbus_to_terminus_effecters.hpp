#pragma once

#include "common/types.hpp"
#include "common/utils.hpp"
#include "platform-mc/manager.hpp"
#include "requester/handler.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace pldm
{

namespace host_effecters
{

using DbusChgHostEffecterProps =
    std::map<dbus::Property, pldm::utils::PropertyValue>;

struct PossibleState
{
    uint16_t stateSetId;
    std::vector<uint8_t> states;
};

struct DBusEffecterMapping
{
    pldm::utils::DBusMapping dbusMap;
    std::vector<pldm::utils::PropertyValue> propertyValues;
    PossibleState state;
};

struct DBusNumericEffecterMapping
{
    pldm::utils::DBusMapping dbusMap;
    uint8_t dataSize;
    int8_t unitModifier;
    double resolution;
    double offset;
    double propertyValue;
};

struct EffecterInfo
{
    uint8_t mctpEid;
    std::string terminusName;
    uint8_t effecterPdrType;
    uint16_t containerId;
    uint16_t entityType;
    uint16_t entityInstance;
    uint8_t compEffecterCnt;
    bool checkHostState;
    std::vector<DBusEffecterMapping> dbusInfo;
    std::vector<DBusNumericEffecterMapping> dbusNumericEffecterInfo;
};

class HostEffecterParser
{
  public:
    HostEffecterParser() = delete;
    HostEffecterParser(const HostEffecterParser&) = delete;
    HostEffecterParser& operator=(const HostEffecterParser&) = delete;
    HostEffecterParser(HostEffecterParser&&) = delete;
    HostEffecterParser& operator=(HostEffecterParser&&) = delete;
    virtual ~HostEffecterParser() = default;

    explicit HostEffecterParser(
        pldm::InstanceIdDb* instanceIdDb, int fd, const pldm_pdr* repo,
        pldm::utils::DBusHandler* const dbusHandler,
        const std::string& jsonPath,
        pldm::requester::Handler<pldm::requester::Request>* handler,
        pldm::platform_mc::Manager* platformManager = nullptr) :
        instanceIdDb(instanceIdDb), sockFd(fd), pdrRepo(repo),
        dbusHandler(dbusHandler), handler(handler),
        platformManager(platformManager)
    {
        try
        {
            parseEffecterJson(jsonPath);
        }
        catch (const std::exception& e)
        {
            std::cerr << "The json file does not exist or malformed, ERROR="
                      << e.what() << "\n";
        }
    }

    void parseEffecterJson(const std::string& jsonPath);

    void processHostEffecterChangeNotification(
        const DbusChgHostEffecterProps& chProperties, size_t effecterInfoIndex,
        size_t dbusInfoIndex, uint16_t effecterId);

    void processTerminusNumericEffecterChangeNotification(
        const DbusChgHostEffecterProps& chProperties, size_t effecterInfoIndex,
        size_t dbusInfoIndex, uint16_t effecterId);

    void populatePropVals(
        const pldm::utils::Json& dBusValues,
        std::vector<pldm::utils::PropertyValue>& propertyValues,
        const std::string& propertyType);

    virtual int setHostStateEffecter(
        size_t effecterInfoIndex,
        std::vector<set_effecter_state_field>& stateField, uint16_t effecterId);

    int setTerminusNumericEffecter(size_t effecterInfoIndex,
                                   uint16_t effecterId, uint8_t dataSize,
                                   double rawValue);

    uint8_t findNewStateValue(size_t effecterInfoIndex, size_t dbusInfoIndex,
                              const pldm::utils::PropertyValue& propertyValue);

    virtual void createHostEffecterMatch(
        const std::string& objectPath, const std::string& interface,
        size_t effecterInfoIndex, size_t dbusInfoIndex, uint16_t effecterId);

    bool isHostOn();

    static double adjustValue(double value, double offset, double resolution,
                              int8_t modify);

  protected:
    pldm::InstanceIdDb* instanceIdDb; //!< Reference to the InstanceIdDb object
                                      //!< to obtain instance id
    [[maybe_unused]] int sockFd;      //!< Socket fd to send message to host
    const pldm_pdr* pdrRepo;          //!< Reference to PDR repo
    std::vector<EffecterInfo> hostEffecterInfo; //!< Parsed effecter information
    std::vector<std::unique_ptr<sdbusplus::match>>
        effecterInfoMatch; //!< vector to catch the D-Bus property change
                           //!< signals for the effecters
    const pldm::utils::DBusHandler* dbusHandler; //!< D-bus Handler
    /** @brief PLDM request handler */
    pldm::requester::Handler<pldm::requester::Request>* handler;
    pldm::platform_mc::Manager* platformManager;
};

} // namespace host_effecters
} // namespace pldm
