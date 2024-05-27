#pragma once

#include <stdint.h>

#include <sdbusplus/message/types.hpp>

#include <bitset>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace pldm
{

using EID = uint8_t;
using UUID = std::string;
using SKU = std::string;
using Request = std::vector<uint8_t>;
using Response = std::vector<uint8_t>;
using Command = uint8_t;

using MctpMedium = std::string;
using MctpBinding = std::string;
using NetworkId = uint8_t;
using MctpInfo = std::tuple<EID, UUID, MctpMedium, NetworkId, MctpBinding>;
using MctpInfos = std::vector<MctpInfo>;
using tid_t = uint8_t;
using VendorIANA = uint32_t;

namespace dbus
{

using ObjectPath = std::string;
using Service = std::string;
using Interface = std::string;
using Interfaces = std::vector<std::string>;
using Property = std::string;
using PropertyType = std::string;
using Value =
    std::variant<bool, uint8_t, int16_t, uint16_t, int32_t, uint32_t, int64_t,
                 uint64_t, double, std::string, std::vector<uint8_t>>;

using PropertyMap = std::map<Property, Value>;
using InterfaceMap = std::map<Interface, PropertyMap>;
using ObjectValueTree = std::map<sdbusplus::message::object_path, InterfaceMap>;
using MctpInterfaces = std::map<UUID, InterfaceMap>;
typedef struct _pathAssociation
{
    std::string forward;
    std::string reverse;
    std::string path;
} PathAssociation;

} // namespace dbus

namespace fw_update
{

// Descriptor definition
using DescriptorType = uint16_t;
using DescriptorData = std::vector<uint8_t>;
using VendorDefinedDescriptorTitle = std::string;
using VendorDefinedDescriptorData = std::vector<uint8_t>;
using VendorDefinedDescriptorInfo =
    std::tuple<VendorDefinedDescriptorTitle, VendorDefinedDescriptorData>;
using DescriptorValue =
    std::variant<DescriptorData, VendorDefinedDescriptorInfo>;
using Descriptors = std::multiset<std::pair<DescriptorType, DescriptorValue>>;

using DescriptorMap = std::unordered_map<EID, Descriptors>;

// Component information
using CompClassification = uint16_t;
using CompIdentifier = uint16_t;
using CompKey = std::pair<CompClassification, CompIdentifier>;
using CompClassificationIndex = uint8_t;
using CompVersion = std::string;
using CompInfo = std::tuple<CompClassificationIndex, CompVersion>;
using ComponentInfo = std::map<CompKey, CompInfo>;
using ComponentInfoMap = std::unordered_map<EID, ComponentInfo>;

// PackageHeaderInformation
using PackageHeaderSize = size_t;
using PackageVersion = std::string;
using ComponentBitmapBitLength = uint16_t;
using PackageHeaderChecksum = uint32_t;

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
using ComponentImageInfo =
    std::tuple<CompClassification, CompIdentifier, CompComparisonStamp,
               CompOptions, ReqCompActivationMethod, CompLocationOffset,
               CompSize, CompVersion>;
using ComponentImageInfos = std::vector<ComponentImageInfo>;

// DeviceInventory
using DeviceObjPath = std::string;
using Associations =
    std::vector<std::tuple<std::string, std::string, std::string>>;
using DBusIntfMatch = std::pair<dbus::Interface, dbus::PropertyMap>;
using CreateDeviceInfo = std::tuple<DeviceObjPath, Associations>;
using UpdateDeviceInfo = DeviceObjPath;
using DeviceInfo = std::tuple<CreateDeviceInfo, UpdateDeviceInfo>;
using MatchDeviceInfo = std::vector<std::tuple<DBusIntfMatch, DeviceInfo>>;

// FirmwareInventory
using ComponentName = std::string;
using ComponentIdNameMap = std::unordered_map<CompIdentifier, ComponentName>;
using ComponentObject = std::tuple<ComponentName, Associations>;
using CreateComponentIdNameMap = std::unordered_map<CompIdentifier, ComponentObject>;
using UpdateComponentIdNameMap = ComponentIdNameMap;
using FirmwareInfo = std::tuple<CreateComponentIdNameMap, UpdateComponentIdNameMap>;
using MatchFirmwareInfo = std::vector<std::tuple<DBusIntfMatch, FirmwareInfo>>;

// ComponentInformation
using MatchComponentNameMapInfo = std::vector<std::tuple<DBusIntfMatch, ComponentIdNameMap>>;
using ComponentNameMap = std::unordered_map<EID, ComponentIdNameMap>;

/** @struct MatchEntryInfo
 *  @brief the template struct to find the matched configured info for an dbus interface from mctp endpoint
 */
template <typename T, typename U>
struct MatchEntryInfo
{
    MatchEntryInfo(const MatchEntryInfo&) = delete;
    MatchEntryInfo& operator=(const MatchEntryInfo&) = delete;
    MatchEntryInfo(MatchEntryInfo&&) = delete;
    MatchEntryInfo& operator=(MatchEntryInfo&&) = delete;

    MatchEntryInfo(const T &i) : infos(i){}
    MatchEntryInfo(){}
    ~MatchEntryInfo(){}

    T infos;

    bool matchInventoryEntry(dbus::InterfaceMap interfaceMap, U& entry) const
    {
        for(uint16_t i = 0; i < infos.size(); i++)
        {
            auto match = std::get<0>(infos[i]);

            if(interfaceMap.contains(match.first) && interfaceMap[match.first] == match.second)
            {
                entry = std::get<1>(infos[i]);
                return true;
            }
        }
        return false;
    }
};

/** @struct DeviceInventoryInfo
 *  @brief the Device inventory infor parsed from config file and find the matched configured info for an dbus interface from mctp endpoint
 */
using DeviceInventoryInfo = MatchEntryInfo<MatchDeviceInfo, DeviceInfo>;
/** @struct FirmwareInventoryInfo
 *  @brief the Firmware inventory info parsed from config file and find the matched configured info for an dbus interface from mctp endpoint
 */
using FirmwareInventoryInfo = MatchEntryInfo<MatchFirmwareInfo, FirmwareInfo>;
/** @struct ComponentNameMapInfo
 *  @brief the Component name info parsed from config file and find the matched configured info for an dbus interface from mctp endpoint
 */
using ComponentNameMapInfo = MatchEntryInfo<MatchComponentNameMapInfo, ComponentIdNameMap>;

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
using EffecterID = uint16_t;
using EntityType = uint16_t;
using EntityInstance = uint16_t;
using ContainerID = uint16_t;
using StateSetId = uint16_t;
using CompositeCount = uint8_t;
using SensorOffset = uint8_t;
using EventState = uint8_t;
using TerminusValidity = uint8_t;

//!< Subset of the State Set that is supported by a effecter/sensor
using PossibleStates = std::set<uint8_t>;
//!< Subset of the State Set that is supported by each effecter/sensor in a
//!< composite effecter/sensor
using CompositeSensorStates = std::vector<PossibleStates>;
using EntityInfo = std::tuple<ContainerID, EntityType, EntityInstance>;
using SensorInfo = std::tuple<EntityInfo, CompositeSensorStates>;
using StateSetData = std::tuple<StateSetId, PossibleStates>;
using StateSetInfo = std::tuple<EntityInfo, std::vector<StateSetData>>;

using DbusVariantType = std::variant<
    std::vector<std::tuple<std::string, std::string, std::string>>,
    std::vector<std::string>, std::vector<double>, std::string, int64_t,
    uint64_t, double, int32_t, uint32_t, int16_t, uint16_t, uint8_t, bool,
    sdbusplus::message::unix_fd, std::vector<uint32_t>, std::vector<uint16_t>,
    sdbusplus::message::object_path,
    std::tuple<uint64_t, std::vector<std::tuple<std::string, std::string,
                                                double, uint64_t>>>,
    std::vector<std::tuple<std::string, std::string>>,
    std::vector<std::tuple<uint32_t, std::vector<uint32_t>>>,
    std::vector<std::tuple<uint32_t, size_t>>,
    std::vector<std::tuple<sdbusplus::message::object_path, std::string,
                           std::string, std::string>>,
    std::vector<sdbusplus::message::object_path>, std::vector<uint8_t>,
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
using SensorAuxiliaryNames = std::tuple<pdr::SensorID, SensorCnt, AuxiliaryNames>;
using EffecterAuxiliaryNames = SensorAuxiliaryNames;
using EnitityAssociations =
    std::map<pdr::ContainerID, std::pair<pdr::EntityInfo, std::set<pdr::EntityInfo>>>;
} // namespace platform_mc

} // namespace pldm
