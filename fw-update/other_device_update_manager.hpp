#pragma once

#include "common/types.hpp"
#include "common/utils.hpp"

#include <xyz/openbmc_project/Software/Activation/server.hpp>
#include <xyz/openbmc_project/Software/ActivationProgress/server.hpp>


#include <sdbusplus/timer.hpp>

#include <unordered_map>

namespace pldm
{

namespace fw_update
{

namespace MatchRules = sdbusplus::bus::match::rules;

namespace Server = sdbusplus::xyz::openbmc_project::Software::server;

/* dead component identifier*/
const uint16_t deadComponent = 0xDEAD;

/**
 * @brief Other device activation information used for storing the activation
 *        state of each of the non-pldm updates currently occuring. Mirrors
 *        the dbus state.
 *
 */
struct OtherDeviceUpdateActivation
{
    std::string uuid;
    Server::Activation::Activations activationState;
    Server::Activation::RequestedActivations requestedActivation;
};

/**
 * @brief ComponentMap contains version and component name. This information
 *        will be used in non pldm message registry mapping.
 */
struct ComponentMap
{
    std::string version;
    std::string componentName;
};

class UpdateManager;

/**
 * @brief Other Device manager
 *          Following are the funtionalities
 *          1) process pldm pkg and extract them to destination
 *          2) activate image
 *          3) Give progress state of the each of the other device
 */
class OtherDeviceUpdateManager
{
  public:
    /**
     * @brief Activation interface creation timeout in seconds
     *
     */
    auto static constexpr UPDATER_ACTIVATION_WAIT_PER_IMAGE_SEC = 3;

    OtherDeviceUpdateManager() = delete;
    OtherDeviceUpdateManager(const OtherDeviceUpdateManager&) = delete;
    OtherDeviceUpdateManager(OtherDeviceUpdateManager&&) = delete;
    OtherDeviceUpdateManager&
        operator=(const OtherDeviceUpdateManager&) = delete;
    OtherDeviceUpdateManager& operator=(OtherDeviceUpdateManager&&) = delete;
    ~OtherDeviceUpdateManager() = default;

    /**
     * @brief Construct a new Other Device Update Manager object
     *
     * @param bus sdbusplus referance
     * @param createActivationObjectCallback call back to create the object
     */
    explicit OtherDeviceUpdateManager(
        sdbusplus::bus::bus& bus, UpdateManager* upMan,
        std::vector<sdbusplus::message::object_path> targets) :
        updateManager(upMan),
        bus(bus), timer(nullptr), targets(targets)
    {
        /* cache number of valid targets */
        updateValidTargets();
    }

    /**
     * @brief Activates all other devices
     *
     * @return true if successfull
     * @return false otherwise
     */
    bool activate();

    /**
     * @brief Async call to monitor the activate change in d-bus
     *
     * @param msg msg
     */
    void onActivationChangedMsg(sdbusplus::message::message& msg);

    /**
     * @brief parser activation message
     *
     * @param objPath path of the activation object
     * @param properties propertis of the activation object
     */
    void onActivationChanged(const std::string& objPath,
                             const pldm::dbus::PropertyMap& properties);

    /**
     * @brief Set the Update Policy object
     *
     * @param path - other software object path
     * @return true if setting update policy is success else false
     */
    bool setUpdatePolicy(const std::string& path);
    /**
     * @brief method to add the dbus activation object paths to dbus watch
     *
     * @param m message
     */
    void interfaceAdded(sdbusplus::message::message& m);

    /**
     * @brief From pldm image extracts the other device images and copies to
     * respective location
     *
     * @param fwDeviceIDRecords - Device records
     * @param componentImageInfos - Image info like offset, size
     * @param package - pldm image input stream
     * @return size_t - number of other device images
     */
    size_t
        extractOtherDevicePkgs(const FirmwareDeviceIDRecords& fwDeviceIDRecords,
                               const ComponentImageInfos& componentImageInfos,
                               std::istream& package);

    /**
     * @brief Get the Number Of Processed Images object
     *
     * @return int
     */
    int getNumberOfProcessedImages();

    /**
     * @brief Get the number of valid UUIDs for non-pldm updates
     *
     * @return count of valid targets
     *
     */
    size_t getValidTargets(void);

  private:
    /**
     * @brief Start timer for interface addition
     *
     */
    void startTimer(int timerExpiryTime);

    /**
     * @brief Interface Addition monitoring
     *
     */
    void startWatchingInterfaceAddition();

    /**
     * @brief Get Activation State of all other devices
     *          if any one to the activation state is activation then it
     *          returns State as activating otherwise Fail / Active
     *
     * @return activation State
     */
    Server::Activation::Activations getOverAllActivationState();

    /**
     * @brief Get file path based on UUID
     *
     * @param UUID UUID to find file path for
     * @return pair with filepath and object path, returns {} on no match
     *
     */
    std::pair<std::string, std::string> getFilePath(const std::string& uuid);

    /**
     * @brief Get the Valid Paths that may contain UUIDs
     *
     * @param paths object to store the paths into
     */
    void getValidPaths(std::vector<std::string>& paths);

    /**
     * @brief updates the valid target count
     *
     */
    void updateValidTargets(void);

    UpdateManager* updateManager;

    /**
     * @brief Cache of the valid targets for non-pldm updates
     *
     */
    size_t validTargetCount;

    /**
     * @brief Dbus object referance
     *
     */
    sdbusplus::bus::bus& bus;

    /**
     * @brief Map conatining sw dbus object state
     *
     */
    std::unordered_map<std::string,
                       std::unique_ptr<OtherDeviceUpdateActivation>>
        otherDevices;

    /**
     * @brief Indicates image process state by item updater
     *
     */
    std::unordered_map<std::string, bool> isImageFileProcessed;

    /**
     * @brief matcher rule to check for activation dbus object change
     *
     */
    std::vector<sdbusplus::bus::match_t> activationMatches;

    /**
     * @brief Timer to wait for interface addition
     *
     */
    std::unique_ptr<phosphor::Timer> timer;
    /**
     * @brief Software object path matcher for interface addition
     *
     */
    std::unique_ptr<sdbusplus::bus::match_t> interfaceAddedMatch;

    /**
     * @brief List of states which are valid for a FW update to be
     *        considered done.
     *
     */
    static constexpr std::array<Server::Activation::Activations, 2>
        validTerminalActivationStates = {
            Server::Activation::Activations::Active,
            Server::Activation::Activations::Failed};
    /**
     * @brief map to match uuid to version string and component name
     *
     */
    std::unordered_map<std::string, ComponentMap> uuidMappings;
    std::vector<sdbusplus::message::object_path> targets;
};

} // namespace fw_update

} // namespace pldm