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

            MatchDeviceInfo deviceMatchInfos;
            deviceMatchInfos.push_back(
                {DBusIntfMatch{interface, cfgProps},
                 DeviceInfo{CreateDeviceInfo{
                                "/xyz/openbmc_project/inventory/coverage", {}},
                            UpdateDeviceInfo{}}});
            DeviceInventoryInfo deviceInventoryInfo(deviceMatchInfos);
            EXPECT_EQ(
                invokeIsPropertyMatchNoInline(deviceInventoryInfo, interfaceMap,
                                              cfgProp, interface),
                expectedMatch);

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
