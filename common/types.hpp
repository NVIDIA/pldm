#pragma once

#include <com/nvidia/State/DeviceState/server.hpp>
#include <sdbusplus/message/types.hpp>

#include <algorithm>
#include <bitset>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace pldm
{

using Availability = bool;
using eid = uint8_t;
using UUID = std::string;
using Request = std::vector<uint8_t>;
using Response = std::vector<uint8_t>;
using MCTPMsgTypes = std::vector<uint8_t>;
using Command = uint8_t;

/** @brief MCTP Endpoint Medium type in string
 *         Reserved for future purpose
 */

using MctpMedium = std::string;
/** @brief Type definition of MCTP Network Index.
 *         uint32_t is used as defined in MCTP Endpoint D-Bus Interface
 */
using NetworkId = uint32_t;

/** @brief Type definition of MCTP name in string
 */
using MctpInfoName = std::optional<std::string>;

/** @brief MCTP Endpoint Binding type in string ▎*/
using MctpBinding = std::string;

/** @brief Type definition for an optional local MCTP EID */
using LocalEid = std::optional<uint8_t>;

/** @brief Type definition of MCTP interface information between two endpoints.
 *         eid : Endpoint EID in byte. Defined to match with MCTP D-Bus
 *               interface
 *         UUID : Endpoint UUID which is used to different the endpoints
 *         MctpMedium: Endpoint MCTP Medium info (Resersed)
 *         NetworkId: MCTP network index
 *         name: Alias name of the endpoint, e.g. BMC, NIC, etc.
 *         MctpBinding: The MCTP binding type of the endpoint
 *         LocalEid: The BMC's own MCTP EID on this endpoint's network
 */
using MctpInfo = std::tuple<eid, UUID, MctpMedium, NetworkId, MctpInfoName,
                            MctpBinding, LocalEid>;

/** @brief Type definition of MCTP endpoint D-Bus properties in
 *         xyz.openbmc_project.MCTP.Endpoint D-Bus interface.
 *
 *         NetworkId: MCTP network index
 *         eid : Endpoint EID in byte. Defined to match with MCTP D-Bus
 *               interface
 *         MCTPMsgTypes: MCTP message types
 *         MctpMedium: Endpoint MCTP Medium info
 *         MctpBinding: The MCTP binding type of the endpoint
 *         LocalEid: The BMC's own MCTP EID on this endpoint's network
 */
using MctpEndpointProps =
    std::tuple<NetworkId, eid, MCTPMsgTypes, MctpMedium, MctpBinding, LocalEid>;

/** @brief Type defined for list of MCTP interface information
 */
using MctpInfos = std::vector<MctpInfo>;

using SKU = std::string;
using tid_t = uint8_t;
using VendorIANA = uint32_t;

/**
 * In `Table 2 - Special endpoint IDs` of DSP0236.
 * EID from 1 to 7 is reserved EID. So the start valid EID is 8
 */
#define MCTP_START_VALID_EID 8
constexpr uint8_t BmcMctpEid = 8;

#define PLDM_PLATFORM_GETPDR_MAX_RECORD_BYTES 1024
/* default the max event message buffer size BMC supported to 4K bytes */
#define PLDM_PLATFORM_EVENT_MSG_MAX_BUFFER_SIZE 4096
/* DSP0248 section16.9 EventMessageBufferSize Command, the default message
 * buffer size is 256 bytes
 */
#define PLDM_PLATFORM_DEFAULT_MESSAGE_BUFFER_SIZE 256

inline constexpr uint16_t HEARTBEAT_TIMEOUT = 120;
inline constexpr uint8_t TERMINUS_ID = 1;
inline constexpr uint16_t TERMINUS_HANDLE = 1;

namespace dbus
{

using ObjectPath = std::string;
using Service = std::string;
using Interface = std::string;
using Interfaces = std::vector<std::string>;
using Property = std::string;
using PropertyType = std::string;
using Value = std::variant<bool, uint8_t, int16_t, uint16_t, int32_t, uint32_t,
                           int64_t, uint64_t, double, std::string,
                           std::vector<uint8_t>, std::vector<uint64_t>>;

using PropertyMap = std::map<Property, Value>;
using InterfaceMap = std::map<Interface, PropertyMap>;
using ObjectValueTree = std::map<sdbusplus::object_path, InterfaceMap>;

using MctpInterfaces = std::map<UUID, InterfaceMap>;
typedef struct _pathAssociation
{
    std::string forward;
    std::string reverse;
    std::string path;
} PathAssociation;

} // namespace dbus

/** @brief Binding of MCTP endpoint EID to Entity Manager's D-Bus object path
 */
using Configurations = std::map<dbus::ObjectPath, MctpInfo>;

namespace fw_update
{
using InventoryPath = std::string;
using SoftwareName = std::string;

// Descriptor definition
using DescriptorType = uint16_t;
using DescriptorData = std::vector<uint8_t>;
using VendorDefinedDescriptorTitle = std::string;
using VendorDefinedDescriptorData = std::vector<uint8_t>;
using VendorDefinedDescriptorInfo =
    std::tuple<VendorDefinedDescriptorTitle, VendorDefinedDescriptorData>;
using Descriptors =
    std::multimap<DescriptorType,
                  std::variant<DescriptorData, VendorDefinedDescriptorInfo>>;
using DownstreamDeviceIndex = uint16_t;
using DownstreamDeviceInfo =
    std::unordered_map<DownstreamDeviceIndex, Descriptors>;

using DescriptorMap = std::unordered_map<eid, Descriptors>;
using DownstreamDescriptorMap = std::unordered_map<eid, DownstreamDeviceInfo>;

// Component information
using CompClassification = uint16_t;
using CompIdentifier = uint16_t;
using SoftwareIdentifier = std::pair<eid, CompIdentifier>;
using CompKey = std::pair<CompClassification, CompIdentifier>;
using CompClassificationIndex = uint8_t;
using CompVersion = std::string;
using CompActivationMethods = uint16_t;
using CompInfo =
    std::tuple<CompClassificationIndex, CompVersion, CompActivationMethods>;
using ComponentInfo = std::map<CompKey, CompInfo>;
using ComponentInfoMap = std::unordered_map<eid, ComponentInfo>;

// PackageHeaderInformation
using PackageHeaderSize = size_t;
using PackageVersion = std::string;
using ComponentBitmapBitLength = uint16_t;
using PackageHeaderChecksum = uint32_t;
using PackagePayloadChecksum = uint32_t;

// FirmwareDeviceIDRecords
using DeviceIDRecordCount = uint8_t;
using DeviceUpdateOptionFlags = std::bitset<32>;
using ApplicableComponents = std::vector<size_t>;
using ComponentImageSetVersion = std::string;
using FirmwareDevicePackageData = std::vector<uint8_t>;
using FirmwareDeviceIDRecord =
    std::tuple<DeviceUpdateOptionFlags, ApplicableComponents,
               ComponentImageSetVersion, Descriptors,
               FirmwareDevicePackageData>;
using FirmwareDeviceIDRecords = std::vector<FirmwareDeviceIDRecord>;

// ComponentImageInformation
using ComponentImageCount = uint16_t;
using CompComparisonStamp = uint32_t;
using CompOptions = std::bitset<16>;
using ReqCompActivationMethod = std::bitset<16>;
using CompLocationOffset = uint32_t;
using CompSize = uint32_t;
using CompOpaqueDataLength = uint32_t;
using ComponentImageInfo =
    std::tuple<CompClassification, CompIdentifier, CompComparisonStamp,
               CompOptions, ReqCompActivationMethod, CompLocationOffset,
               CompSize, CompVersion>;
using ComponentImageInfos = std::vector<ComponentImageInfo>;

using DownstreamDeviceIDRecordCount = uint8_t;

// Shared inventory helper types. RoT chassis objects are owned by
// entity-manager via Configuration.PLDMDeviceInventory.
using DeviceObjPath = std::string;
using Associations =
    std::vector<std::tuple<std::string, std::string, std::string>>;
using DBusIntfMatch = std::pair<dbus::Interface, dbus::PropertyMap>;

// FirmwareInventory
using ComponentName = std::string;
using Manufacturer = std::string;
using ComponentIdNameMap = std::unordered_map<CompIdentifier, ComponentName>;
// UpdateOnly marks a component whose Software.Version object is owned by
// another service (e.g. BMC firmware component 16, owned by BMC.Inventory).
// firmware inventory only stamps SoftwareId on it via updateSwId() and does
// not create a competing object.
using UpdateOnly = bool;
using ComponentObject =
    std::tuple<ComponentName, Associations, Manufacturer, UpdateOnly>;
using CreateComponentIdNameMap =
    std::unordered_map<CompIdentifier, ComponentObject>;
using UpdateComponentIdNameMap = ComponentIdNameMap;
using FirmwareInfo =
    std::tuple<CreateComponentIdNameMap, UpdateComponentIdNameMap>;
using MatchFirmwareInfo = std::vector<std::tuple<DBusIntfMatch, FirmwareInfo>>;

// ComponentInformation
using MatchComponentNameMapInfo =
    std::vector<std::tuple<DBusIntfMatch, ComponentIdNameMap>>;
using ComponentNameMap = std::unordered_map<eid, ComponentIdNameMap>;
using ComponentTargetList =
    std::unordered_map<eid, std::vector<CompIdentifier>>;

// DeviceStatus return type
using AdditionalData = std::map<std::string, std::string>;
using DeviceStatusErrorCode = int64_t;
using DeviceState = sdbusplus::server::com::nvidia::state::DeviceState;
using DeviceStatusMap = std::map<
    DeviceState::StatusType,
    std::tuple<
        DeviceState::DeviceHealth,
        std::vector<std::tuple<DeviceStatusErrorCode, DeviceState::ErrorClass,
                               AdditionalData>>>>;

/** @struct MatchEntryInfo
 *  @brief the template struct to find the matched configured info for an dbus
 * interface from mctp endpoint
 */
template <typename T, typename U>
struct MatchEntryInfo
{
    MatchEntryInfo(const MatchEntryInfo&) = delete;
    MatchEntryInfo& operator=(const MatchEntryInfo&) = delete;
    MatchEntryInfo(MatchEntryInfo&&) = delete;
    MatchEntryInfo& operator=(MatchEntryInfo&&) = delete;

    MatchEntryInfo(const T& i) : infos(i) {}
    MatchEntryInfo() {}
    ~MatchEntryInfo() {}

    T infos;

    /**
     * @brief Method to compare whether config and D-Bus properties are direct
     * match.
     *
     * @param[in] interfaceMap - D-Bus properties
     * @param[in] cfgIntfName - config interface from json
     * @param[in] cfgProperty - config property from json
     * @return true
     * @return false
     */
    bool isDirectMatch(const dbus::InterfaceMap& interfaceMap,
                       const dbus::Interface& cfgIntfName,
                       const dbus::PropertyMap& cfgProperty) const
    {
        auto interfaceProp = interfaceMap.find(cfgIntfName);
        if (interfaceProp == interfaceMap.end())
        {
            return false;
        }
        return interfaceProp->second == cfgProperty;
    }

    /**
     * @brief Helper template to compare two values
     */
    template <typename T1, typename T2>
    static bool compareValues(const T1& val1, const T2& val2)
    {
        if constexpr (std::is_same_v<T1, T2>)
        {
            if constexpr (std::is_arithmetic_v<T1>)
            {
                return static_cast<int64_t>(val1) == static_cast<int64_t>(val2);
            }
            else
            {
                return val1 == val2;
            }
        }
        else if constexpr (std::is_arithmetic_v<T1> && std::is_arithmetic_v<T2>)
        {
            return static_cast<int64_t>(val1) == static_cast<int64_t>(val2);
        }
        else
        {
            return false;
        }
    }

    /**
     * @brief Method to compare individual property from json are matching with
     * D-Bus property.
     *
     * @param[in] interfaceMap - D-Bus properties
     * @param[in] cfgProperty - config property from jso
     * @param[in] cfgIntfName - config interface from json
     * @return true
     * @return false
     */
    bool isPropertyMatch(
        const dbus::InterfaceMap& interfaceMap,
        const std::pair<dbus::Property, dbus::Value>& cfgProperty,
        const dbus::Interface& cfgIntfName) const
    {
        const auto& [cfgPropertyName, cfgPropertyValue] = cfgProperty;
        auto interfaceProp = interfaceMap.find(cfgIntfName);
        if (interfaceProp == interfaceMap.end())
        {
            return false;
        }

        auto propIt = interfaceProp->second.find(cfgPropertyName);
        if (propIt == interfaceProp->second.end())
        {
            return false;
        }

        bool matches = false;
        std::visit(
            [&matches, &propIt](const auto& expected) {
                std::visit(
                    [&matches, &expected](const auto& actual) {
                        matches = compareValues(expected, actual);
                    },
                    propIt->second);
            },
            cfgPropertyValue);

        return matches;
    }

    bool matchInventoryEntry(const dbus::InterfaceMap& interfaceMap,
                             U& entry) const
    {
        for (size_t i = 0; i < infos.size(); i++)
        {
            const auto [cfgIntfName, cfgProps] = std::get<0>(infos[i]);

            if (isDirectMatch(interfaceMap, cfgIntfName, cfgProps))
            {
                entry = std::get<1>(infos[i]);
                return true;
            }

            if (interfaceMap.contains(cfgIntfName))
            {
                if (std::all_of(cfgProps.begin(), cfgProps.end(),
                                [&, cfgIntfName = cfgIntfName](
                                    const auto& cfgProperty) {
                                    return isPropertyMatch(
                                        interfaceMap, cfgProperty, cfgIntfName);
                                }))
                {
                    entry = std::get<1>(infos[i]);
                    return true;
                }
            }
        }
        return false;
    }
};

/** @struct FirmwareInventoryInfo
 *  @brief the Firmware inventory info parsed from config file and find the
 * matched configured info for an dbus interface from mctp endpoint
 */
using FirmwareInventoryInfo = MatchEntryInfo<MatchFirmwareInfo, FirmwareInfo>;
/** @struct ComponentNameMapInfo
 *  @brief the Component name info parsed from config file and find the matched
 * configured info for an dbus interface from mctp endpoint
 */
using ComponentNameMapInfo =
    MatchEntryInfo<MatchComponentNameMapInfo, ComponentIdNameMap>;

enum class ComponentImageInfoPos : size_t
{
    CompClassificationPos = 0,
    CompIdentifierPos = 1,
    CompComparisonStampPos = 2,
    CompOptionsPos = 3,
    ReqCompActivationMethodPos = 4,
    CompLocationOffsetPos = 5,
    CompSizePos = 6,
    CompVersionPos = 7,
};

// PackageSignatureFormat
using PackageSignatureVersion = uint8_t;
using PackageSignatureSecurityVersion = uint8_t;
using PackageSignaturePayloadSize = size_t;
using PackageSignatureSignatureType = uint8_t;
using PackageSignatureSignatureSize = uint16_t;
using PackageSignatureSignature = std::vector<uint8_t>;
using PackageSignatureMinorVersion = uint8_t;
using PackageSignatureOffsetToSignature = uint16_t;
using PackageSignatureOffsetToPublicKey = uint16_t;
using PackageSignaturePublicKeySize = uint16_t;
using PackageSignaturePublicKey = std::vector<uint8_t>;

} // namespace fw_update

namespace pdr
{

using EID = uint8_t;
using TerminusHandle = uint16_t;
using TerminusID = uint8_t;
using SensorID = uint16_t;
using EntityType = uint16_t;
using EntityInstance = uint16_t;
using ContainerID = uint16_t;
using StateSetId = uint16_t;
using CompositeCount = uint8_t;
using SensorOffset = uint8_t;
using EventState = uint8_t;
using TerminusValidity = uint8_t;
using EffecterID = uint16_t;
using EntityName = std::string;
using SensorCount = uint8_t;
using NameLanguageTag = std::string;
using SensorName = std::string;
using SensorAuxiliaryNames = std::tuple<
    SensorID, SensorCount,
    std::vector<std::vector<std::pair<NameLanguageTag, SensorName>>>>;
using AuxiliaryNames = std::vector<std::pair<NameLanguageTag, std::string>>;

/** @struct EntityKey
 *
 *  EntityKey uniquely identifies the PLDM entity and a combination of Entity
 *  Type, Entity Instance Number, Entity Container ID
 *
 */
struct EntityKey
{
    EntityType type;            //!< Entity type
    EntityInstance instanceIdx; //!< Entity instance number
    ContainerID containerId;    //!< Entity container ID

    bool operator==(const EntityKey& e) const
    {
        return ((type == e.type) && (instanceIdx == e.instanceIdx) &&
                (containerId == e.containerId));
    }
};

using EntityKey = struct EntityKey;
using EntityAuxiliaryNames = std::tuple<EntityKey, AuxiliaryNames>;

//!< Subset of the State Set that is supported by a effecter/sensor
using PossibleStates = std::set<uint8_t>;
//!< Subset of the State Set that is supported by each effecter/sensor in a
//!< composite effecter/sensor
using CompositeSensorStates = std::vector<PossibleStates>;
using EntityInfo = std::tuple<ContainerID, EntityType, EntityInstance>;
using SensorInfo =
    std::tuple<EntityInfo, CompositeSensorStates, std::vector<StateSetId>>;

using StateSetData = std::tuple<StateSetId, PossibleStates>;
using StateSetInfo = std::tuple<EntityInfo, std::vector<StateSetData>>;

using DbusVariantType = std::variant<
    std::vector<std::tuple<std::string, std::string, std::string>>,
    std::vector<std::string>, std::vector<double>, std::string, int64_t,
    uint64_t, double, int32_t, uint32_t, int16_t, uint16_t, uint8_t, bool,
    sdbusplus::message::unix_fd, std::vector<uint32_t>, std::vector<uint16_t>,
    sdbusplus::object_path,
    std::tuple<uint64_t, std::vector<std::tuple<std::string, std::string,
                                                double, uint64_t>>>,
    std::vector<std::tuple<std::string, std::string>>,
    std::vector<std::tuple<uint32_t, std::vector<uint32_t>>>,
    std::vector<std::tuple<uint32_t, size_t>>,
    std::vector<std::tuple<sdbusplus::object_path, std::string, std::string,
                           std::string>>,
    std::vector<sdbusplus::object_path>, std::vector<uint8_t>,
    std::vector<std::tuple<uint8_t, std::string>>, std::tuple<size_t, bool>,
    std::tuple<bool, uint32_t>, std::map<std::string, uint64_t>,
    std::tuple<std::string, std::string, std::string, uint64_t>>;
} // namespace pdr

namespace platform_mc
{
using SensorCnt = uint8_t;
using EffecterCnt = SensorCnt;
using NameLanguageTag = std::string;
using SensorName = std::string;
using EffecterName = SensorName;
using AuxiliaryNames =
    std::vector<std::vector<std::pair<NameLanguageTag, SensorName>>>;
using SensorAuxiliaryNames =
    std::tuple<pdr::SensorID, SensorCnt, AuxiliaryNames>;
using EffecterAuxiliaryNames = SensorAuxiliaryNames;
using EnitityAssociations =
    std::map<pdr::ContainerID,
             std::pair<pdr::EntityInfo, std::set<pdr::EntityInfo>>>;
using ParentObjPath = std::string;
} // namespace platform_mc

namespace bios
{

using AttributeName = std::string;
using AttributeType = std::string;
using ReadonlyStatus = bool;
using DisplayName = std::string;
using Description = std::string;
using MenuPath = std::string;
using CurrentValue = std::variant<int64_t, std::string>;
using DefaultValue = std::variant<int64_t, std::string>;
using OptionString = std::string;
using OptionValue = std::variant<int64_t, std::string>;
using ValueDisplayName = std::string;
using Option =
    std::vector<std::tuple<OptionString, OptionValue, ValueDisplayName>>;
using BIOSTableObj =
    std::tuple<AttributeType, ReadonlyStatus, DisplayName, Description,
               MenuPath, CurrentValue, DefaultValue, Option>;
using BaseBIOSTable = std::map<AttributeName, BIOSTableObj>;
using PendingObj = std::tuple<AttributeType, CurrentValue>;
using PendingAttributes = std::map<AttributeName, PendingObj>;
using Callback = std::function<void()>;

} // namespace bios

} // namespace pldm
