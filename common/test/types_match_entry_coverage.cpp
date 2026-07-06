#include "common/types.hpp"

#include <array>
#include <type_traits>

#include <gtest/gtest.h>

using namespace pldm::fw_update;
namespace dbus = pldm::dbus;

namespace
{

const std::array<dbus::Value, 12>& allDbusValues()
{
    static const std::array<dbus::Value, 12> values = {
        dbus::Value{true},
        dbus::Value{uint8_t(1)},
        dbus::Value{int16_t(-1)},
        dbus::Value{uint16_t(2)},
        dbus::Value{int32_t(-3)},
        dbus::Value{uint32_t(4)},
        dbus::Value{int64_t(-5)},
        dbus::Value{uint64_t(6)},
        dbus::Value{double(7.7)},
        dbus::Value{std::string("hello")},
        dbus::Value{std::vector<uint8_t>{1, 2}},
        dbus::Value{std::vector<uint64_t>{3, 4}},
    };
    return values;
}

bool expectedCompareResult(const dbus::Value& expected,
                           const dbus::Value& actual)
{
    return std::visit(
        [](const auto& lhs, const auto& rhs) {
            using Lhs = std::decay_t<decltype(lhs)>;
            using Rhs = std::decay_t<decltype(rhs)>;

            if constexpr (std::is_same_v<Lhs, Rhs>)
            {
                if constexpr (std::is_arithmetic_v<Lhs>)
                {
                    return static_cast<int64_t>(lhs) ==
                           static_cast<int64_t>(rhs);
                }
                else
                {
                    return lhs == rhs;
                }
            }
            else if constexpr (std::is_arithmetic_v<Lhs> &&
                               std::is_arithmetic_v<Rhs>)
            {
                return static_cast<int64_t>(lhs) == static_cast<int64_t>(rhs);
            }
            else
            {
                return false;
            }
        },
        expected, actual);
}

template <typename InventoryInfoType>
__attribute__((noinline)) bool invokeIsPropertyMatchNoInline(
    const InventoryInfoType& inv, const dbus::InterfaceMap& interfaceMap,
    const std::pair<dbus::Property, dbus::Value>& cfgProp,
    const dbus::Interface& interface)
{
    auto fn = &InventoryInfoType::isPropertyMatch;
    return (inv.*fn)(interfaceMap, cfgProp, interface);
}

template <typename InventoryInfoType, typename EntryType>
__attribute__((noinline)) bool invokeMatchInventoryEntryNoInline(
    const InventoryInfoType& inv, const dbus::InterfaceMap& interfaceMap,
    EntryType& entry)
{
    auto fn = &InventoryInfoType::matchInventoryEntry;
    return (inv.*fn)(interfaceMap, entry);
}

MatchFirmwareInfo makeFirmwareMatchInfos(const dbus::Interface& interface,
                                         const dbus::PropertyMap& cfgProps)
{
    MatchFirmwareInfo firmwareMatchInfos;
    CreateComponentIdNameMap createMap{
        {1, ComponentObject{std::string("ComponentOne"), Associations{},
                            std::string{}, false}}};
    UpdateComponentIdNameMap updateMap{{1, "ComponentOne"}};
    firmwareMatchInfos.push_back(
        {DBusIntfMatch{interface, cfgProps},
         FirmwareInfo{std::move(createMap), std::move(updateMap)}});
    return firmwareMatchInfos;
}

MatchComponentNameMapInfo makeComponentNameMapInfos(
    const dbus::Interface& interface, const dbus::PropertyMap& cfgProps)
{
    MatchComponentNameMapInfo componentMatchInfos;
    componentMatchInfos.push_back({DBusIntfMatch{interface, cfgProps},
                                   ComponentIdNameMap{{1, "ComponentOne"}}});
    return componentMatchInfos;
}

using GenericMatchInfo = std::pair<DBusIntfMatch, int>;
using GenericMatchInfos = std::vector<GenericMatchInfo>;
using GenericEntryMatcher = MatchEntryInfo<GenericMatchInfos, int>;

GenericMatchInfos makeGenericMatchInfos(const dbus::Interface& interface,
                                        const dbus::PropertyMap& cfgProps)
{
    GenericMatchInfos genericMatchInfos;
    genericMatchInfos.push_back({DBusIntfMatch{interface, cfgProps}, 42});
    return genericMatchInfos;
}

} // namespace

TEST(MatchEntryInfoCoverage, DeviceAndFirmwareAllValueTypePairs)
{
    constexpr const char* interface = "xyz.openbmc_project.Test.Interface";
    constexpr const char* property = "Value";

    for (const auto& expectedVal : allDbusValues())
    {
        for (const auto& actualVal : allDbusValues())
        {
            const bool expectedMatch =
                expectedCompareResult(expectedVal, actualVal);

            dbus::PropertyMap cfgProps = {{property, expectedVal}};
            dbus::InterfaceMap interfaceMap = {
                {interface,
                 {{property, actualVal},
                  // Force property-match path instead of direct-match path.
                  {"Extra", dbus::Value{uint32_t(1)}}}}};
            std::pair<dbus::Property, dbus::Value> cfgProp{property,
                                                           expectedVal};

            MatchFirmwareInfo firmwareMatchInfos;
            firmwareMatchInfos.push_back(
                {DBusIntfMatch{interface, cfgProps},
                 FirmwareInfo{CreateComponentIdNameMap{},
                              UpdateComponentIdNameMap{}}});
            FirmwareInventoryInfo firmwareInventoryInfo(firmwareMatchInfos);
            EXPECT_EQ(
                invokeIsPropertyMatchNoInline(firmwareInventoryInfo,
                                              interfaceMap, cfgProp, interface),
                expectedMatch);
        }
    }
}

TEST(MatchEntryInfoCoverage, ComponentNameMapAllValueTypePairs)
{
    constexpr const char* interface = "xyz.openbmc_project.Test.Interface";
    constexpr const char* property = "Value";

    for (const auto& expectedVal : allDbusValues())
    {
        for (const auto& actualVal : allDbusValues())
        {
            const bool expectedMatch =
                expectedCompareResult(expectedVal, actualVal);

            dbus::PropertyMap cfgProps = {{property, expectedVal}};
            dbus::InterfaceMap interfaceMap = {
                {interface,
                 {{property, actualVal}, {"Extra", dbus::Value{uint32_t(1)}}}}};
            std::pair<dbus::Property, dbus::Value> cfgProp{property,
                                                           expectedVal};

            ComponentNameMapInfo componentNameMapInfo(
                makeComponentNameMapInfos(interface, cfgProps));
            ComponentIdNameMap componentNameMap;
            EXPECT_EQ(
                invokeIsPropertyMatchNoInline(componentNameMapInfo,
                                              interfaceMap, cfgProp, interface),
                expectedMatch);
            EXPECT_TRUE(componentNameMap.empty());
        }
    }
}

TEST(MatchEntryInfoCoverage, MatchInventoryEntryCoversAllAliases)
{
    constexpr const char* interface = "xyz.openbmc_project.Test.Interface";
    dbus::PropertyMap cfgProps = {{"Name", dbus::Value{std::string("GPU0")}},
                                  {"Enabled", dbus::Value{true}}};
    dbus::InterfaceMap directMatchMap = {{interface, cfgProps}};
    dbus::InterfaceMap propertyMatchMap = {
        {interface,
         {{"Name", dbus::Value{std::string("GPU0")}},
          {"Enabled", dbus::Value{true}},
          {"Extra", dbus::Value{uint32_t(9)}}}}};
    dbus::InterfaceMap missingPropertyMap = {
        {interface, {{"Name", dbus::Value{std::string("GPU0")}}}}};
    dbus::InterfaceMap missingInterfaceMap = {
        {"xyz.openbmc_project.Other.Interface", cfgProps}};

    FirmwareInventoryInfo firmwareInfo(
        makeFirmwareMatchInfos(interface, cfgProps));
    FirmwareInfo firmwareEntry;
    EXPECT_TRUE(invokeMatchInventoryEntryNoInline(firmwareInfo, directMatchMap,
                                                  firmwareEntry));
    EXPECT_TRUE(invokeMatchInventoryEntryNoInline(
        firmwareInfo, propertyMatchMap, firmwareEntry));
    EXPECT_FALSE(invokeMatchInventoryEntryNoInline(
        firmwareInfo, missingPropertyMap, firmwareEntry));
    EXPECT_FALSE(invokeMatchInventoryEntryNoInline(
        firmwareInfo, missingInterfaceMap, firmwareEntry));

    ComponentNameMapInfo componentInfo(
        makeComponentNameMapInfos(interface, cfgProps));
    ComponentIdNameMap componentEntry;
    EXPECT_TRUE(invokeMatchInventoryEntryNoInline(componentInfo, directMatchMap,
                                                  componentEntry));
    EXPECT_TRUE(invokeMatchInventoryEntryNoInline(
        componentInfo, propertyMatchMap, componentEntry));
    EXPECT_FALSE(invokeMatchInventoryEntryNoInline(
        componentInfo, missingPropertyMap, componentEntry));
    EXPECT_FALSE(invokeMatchInventoryEntryNoInline(
        componentInfo, missingInterfaceMap, componentEntry));
}

TEST(MatchEntryInfoCoverage, GenericEntryMatcherAllValueTypePairs)
{
    constexpr const char* interface = "xyz.openbmc_project.Test.Interface";
    constexpr const char* property = "Value";

    for (const auto& expectedVal : allDbusValues())
    {
        for (const auto& actualVal : allDbusValues())
        {
            const bool expectedMatch =
                expectedCompareResult(expectedVal, actualVal);

            dbus::PropertyMap cfgProps = {{property, expectedVal}};
            dbus::InterfaceMap interfaceMap = {
                {interface,
                 {{property, actualVal}, {"Extra", dbus::Value{uint32_t(1)}}}}};
            std::pair<dbus::Property, dbus::Value> cfgProp{property,
                                                           expectedVal};

            GenericEntryMatcher matcher(
                makeGenericMatchInfos(interface, cfgProps));
            EXPECT_EQ(invokeIsPropertyMatchNoInline(matcher, interfaceMap,
                                                    cfgProp, interface),
                      expectedMatch);
        }
    }
}

TEST(MatchEntryInfoCoverage, GenericEntryMatcherPropertyPathCoverage)
{
    constexpr const char* interface = "xyz.openbmc_project.Test.Interface";
    dbus::PropertyMap cfgProps = {{"Name", dbus::Value{std::string("GPU0")}},
                                  {"Enabled", dbus::Value{true}}};
    dbus::InterfaceMap directMatchMap = {{interface, cfgProps}};
    dbus::InterfaceMap propertyMatchMap = {
        {interface,
         {{"Name", dbus::Value{std::string("GPU0")}},
          {"Enabled", dbus::Value{true}},
          {"Extra", dbus::Value{uint32_t(9)}}}}};
    dbus::InterfaceMap missingPropertyMap = {
        {interface, {{"Name", dbus::Value{std::string("GPU0")}}}}};
    dbus::InterfaceMap missingInterfaceMap = {
        {"xyz.openbmc_project.Other.Interface", cfgProps}};

    GenericEntryMatcher matcher(makeGenericMatchInfos(interface, cfgProps));
    int entry = 0;
    EXPECT_TRUE(
        invokeMatchInventoryEntryNoInline(matcher, directMatchMap, entry));
    EXPECT_EQ(entry, 42);

    entry = 0;
    EXPECT_TRUE(
        invokeMatchInventoryEntryNoInline(matcher, propertyMatchMap, entry));
    EXPECT_EQ(entry, 42);

    entry = 0;
    EXPECT_FALSE(
        invokeMatchInventoryEntryNoInline(matcher, missingPropertyMap, entry));
    EXPECT_EQ(entry, 0);

    EXPECT_FALSE(
        invokeMatchInventoryEntryNoInline(matcher, missingInterfaceMap, entry));
    EXPECT_EQ(entry, 0);
}

TEST(MatchEntryInfoCoverage, DefaultConstructedMatchersReturnFalse)
{
    constexpr const char* interface = "xyz.openbmc_project.Test.Interface";
    dbus::InterfaceMap interfaceMap = {
        {interface, {{"Value", dbus::Value{uint8_t(1)}}}}};

    FirmwareInventoryInfo firmwareInfo;
    FirmwareInfo firmwareEntry{};
    EXPECT_FALSE(invokeMatchInventoryEntryNoInline(firmwareInfo, interfaceMap,
                                                   firmwareEntry));

    ComponentNameMapInfo componentInfo;
    ComponentIdNameMap componentEntry{};
    EXPECT_FALSE(invokeMatchInventoryEntryNoInline(componentInfo, interfaceMap,
                                                   componentEntry));

    GenericEntryMatcher matcher;
    int genericEntry = 0;
    EXPECT_FALSE(
        invokeMatchInventoryEntryNoInline(matcher, interfaceMap, genericEntry));
    EXPECT_EQ(genericEntry, 0);
}

TEST(MatchEntryInfoCoverage, MatchInventoryEntryUsesSecondCandidate)
{
    constexpr const char* interface = "xyz.openbmc_project.Test.Interface";
    dbus::PropertyMap firstProps = {{"Name", dbus::Value{std::string("GPU1")}}};
    dbus::PropertyMap secondProps = {{"Name", dbus::Value{std::string("GPU0")}},
                                     {"Enabled", dbus::Value{true}}};
    dbus::InterfaceMap interfaceMap = {
        {interface,
         {{"Name", dbus::Value{std::string("GPU0")}},
          {"Enabled", dbus::Value{true}},
          {"Extra", dbus::Value{uint32_t(9)}}}}};

    MatchFirmwareInfo firmwareInfos;
    firmwareInfos.push_back(
        {DBusIntfMatch{interface, firstProps},
         FirmwareInfo{CreateComponentIdNameMap{}, UpdateComponentIdNameMap{}}});
    firmwareInfos.push_back(
        {DBusIntfMatch{interface, secondProps},
         FirmwareInfo{
             CreateComponentIdNameMap{
                 {2, ComponentObject{std::string("ComponentTwo"),
                                     Associations{}, std::string{}, false}}},
             UpdateComponentIdNameMap{{2, "ComponentTwo"}}}});
    FirmwareInventoryInfo firmwareMatcher(firmwareInfos);
    FirmwareInfo firmwareEntry{};
    EXPECT_TRUE(invokeMatchInventoryEntryNoInline(firmwareMatcher, interfaceMap,
                                                  firmwareEntry));
    EXPECT_TRUE(std::get<UpdateComponentIdNameMap>(firmwareEntry).contains(2));

    MatchComponentNameMapInfo componentInfos;
    componentInfos.push_back(
        {DBusIntfMatch{interface, firstProps}, ComponentIdNameMap{{1, "One"}}});
    componentInfos.push_back({DBusIntfMatch{interface, secondProps},
                              ComponentIdNameMap{{2, "Two"}}});
    ComponentNameMapInfo componentMatcher(componentInfos);
    ComponentIdNameMap componentEntry{};
    EXPECT_TRUE(invokeMatchInventoryEntryNoInline(
        componentMatcher, interfaceMap, componentEntry));
    EXPECT_TRUE(componentEntry.contains(2));

    GenericMatchInfos genericInfos;
    genericInfos.push_back({DBusIntfMatch{interface, firstProps}, 1});
    genericInfos.push_back({DBusIntfMatch{interface, secondProps}, 2});
    GenericEntryMatcher genericMatcher(genericInfos);
    int genericEntry = 0;
    EXPECT_TRUE(invokeMatchInventoryEntryNoInline(genericMatcher, interfaceMap,
                                                  genericEntry));
    EXPECT_EQ(genericEntry, 2);
}

TEST(MatchEntryInfoCoverage, EmptyConfiguredPropertiesMatchPresentInterface)
{
    constexpr const char* interface = "xyz.openbmc_project.Test.Interface";
    dbus::InterfaceMap interfaceMap = {
        {interface, {{"Name", dbus::Value{std::string("GPU0")}}}}};
    dbus::PropertyMap emptyProps{};

    FirmwareInventoryInfo firmwareMatcher(
        makeFirmwareMatchInfos(interface, emptyProps));
    FirmwareInfo firmwareEntry{};
    EXPECT_TRUE(invokeMatchInventoryEntryNoInline(firmwareMatcher, interfaceMap,
                                                  firmwareEntry));

    ComponentNameMapInfo componentMatcher(
        makeComponentNameMapInfos(interface, emptyProps));
    ComponentIdNameMap componentEntry{};
    EXPECT_TRUE(invokeMatchInventoryEntryNoInline(
        componentMatcher, interfaceMap, componentEntry));

    GenericEntryMatcher genericMatcher(
        makeGenericMatchInfos(interface, emptyProps));
    int genericEntry = 0;
    EXPECT_TRUE(invokeMatchInventoryEntryNoInline(genericMatcher, interfaceMap,
                                                  genericEntry));
    EXPECT_EQ(genericEntry, 42);
}

TEST(MatchEntryInfoCoverage, CompareValuesCoversNonNumericBranches)
{
    EXPECT_TRUE(GenericEntryMatcher::compareValues(std::string("match"),
                                                   std::string("match")));
    EXPECT_FALSE(GenericEntryMatcher::compareValues(std::string("match"),
                                                    std::string("other")));

    const std::vector<uint8_t> expectedBytes{1, 2, 3};
    const std::vector<uint8_t> actualBytes{1, 2, 3};
    const std::vector<uint64_t> otherNumeric{1, 2, 3};
    EXPECT_TRUE(GenericEntryMatcher::compareValues(expectedBytes, actualBytes));
    EXPECT_FALSE(
        GenericEntryMatcher::compareValues(expectedBytes, otherNumeric));
    EXPECT_FALSE(
        GenericEntryMatcher::compareValues(std::string("match"), uint8_t{1}));
}
