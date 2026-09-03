#include "common/utils.hpp"
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "libpldmresponder/platform_config.hpp"
#undef private

#include <systemd/sd-bus.h>

#include <xyz/openbmc_project/Inventory/Decorator/Compatible/common.hpp>

#include <filesystem>

#include <gtest/gtest.h>

using namespace pldm::responder::platform_config;
namespace fs = std::filesystem;

using InventoryDecoratorCompatible =
    sdbusplus::common::xyz::openbmc_project::inventory::decorator::Compatible;

namespace
{

// Seal a freshly-appended message and rewind it so the target callback can
// read the payload back out (same idiom used by the platform-mc tests).
void sealAndRewind(sdbusplus::message::message& msg)
{
    ASSERT_GE(sd_bus_message_seal(msg.get(), 0, 0), 0);
    ASSERT_GE(sd_bus_message_rewind(msg.get(), true), 0);
}

// Build an interfacesAdded-style message carrying the supplied interface map.
sdbusplus::message::message makeInterfacesAddedMsg(
    const std::string& objPath, const pldm::utils::InterfaceMap& interfaces)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    auto msg = bus.new_method_call("org.test", "/org/test",
                                   "org.test.Interface", "Method");
    msg.append(sdbusplus::object_path(objPath), interfaces);
    return msg;
}

class PlatformConfigTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        sysDir = fs::temp_directory_path() /
                 ("pldm_platform_config_" + std::to_string(getpid()));
        fs::create_directories(sysDir);
        // A directory named after a compatible system type.
        fs::create_directories(sysDir / systemName);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(sysDir, ec);
    }

    fs::path sysDir;
    const std::string systemName = "TestSystemType";
};

} // namespace

TEST_F(PlatformConfigTest, GetSysSpecificJsonDirMatches)
{
    Handler handler(sysDir);
    auto result = handler.getSysSpecificJsonDir(sysDir, {systemName});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, systemName);
}

TEST_F(PlatformConfigTest, GetSysSpecificJsonDirNoMatch)
{
    Handler handler(sysDir);
    auto result = handler.getSysSpecificJsonDir(sysDir, {"Nonexistent"});
    EXPECT_FALSE(result.has_value());
}

TEST_F(PlatformConfigTest, GetSysSpecificJsonDirEmptyPath)
{
    Handler handler(sysDir);
    auto result = handler.getSysSpecificJsonDir(fs::path{}, {systemName});
    EXPECT_FALSE(result.has_value());
}

TEST_F(PlatformConfigTest, RegisterSystemTypeCallback)
{
    Handler handler(sysDir);
    bool invoked = false;
    handler.registerSystemTypeCallback([&invoked](const std::string&, bool) {
        invoked = true;
    });
    EXPECT_FALSE(invoked); // registration alone does not invoke
}

TEST_F(PlatformConfigTest, GetPlatformNameReturnsCachedType)
{
    Handler handler(sysDir);
    handler.systemType = systemName;
    auto name = handler.getPlatformName();
    ASSERT_TRUE(name.has_value());
    EXPECT_EQ(name->string(), systemName);
}

TEST_F(PlatformConfigTest, GetPlatformNameWithoutEntityManager)
{
    // With no cached type and no EntityManager on the bus the D-Bus subtree
    // lookup either returns empty or throws; both paths yield nullopt.
    Handler handler(sysDir);
    EXPECT_NO_THROW({ (void)handler.getPlatformName(); });
}

TEST_F(PlatformConfigTest, SystemCompatibleCallbackSetsSystemType)
{
    Handler handler(sysDir);
    bool invoked = false;
    std::string reported;
    handler.registerSystemTypeCallback([&](const std::string& t, bool) {
        invoked = true;
        reported = t;
    });

    pldm::utils::InterfaceMap interfaces;
    pldm::utils::PropertyMap props;
    props.emplace("Names", std::vector<std::string>{"AnotherType", systemName});
    interfaces.emplace(InventoryDecoratorCompatible::interface, props);

    auto msg = makeInterfacesAddedMsg("/xyz/openbmc_project/inventory/system",
                                      interfaces);
    sealAndRewind(msg);

    EXPECT_NO_THROW(handler.systemCompatibleCallback(msg));
    EXPECT_EQ(handler.systemType, systemName);
    EXPECT_TRUE(invoked);
    EXPECT_EQ(reported, systemName);
}

TEST_F(PlatformConfigTest, SystemCompatibleCallbackIgnoresOtherInterface)
{
    Handler handler(sysDir);
    pldm::utils::InterfaceMap interfaces;
    interfaces.emplace("xyz.openbmc_project.Some.Other.Interface",
                       pldm::utils::PropertyMap{});
    auto msg = makeInterfacesAddedMsg("/xyz/openbmc_project/inventory/other",
                                      interfaces);
    sealAndRewind(msg);

    EXPECT_NO_THROW(handler.systemCompatibleCallback(msg));
    EXPECT_TRUE(handler.systemType.empty());
}

TEST_F(PlatformConfigTest, SystemCompatibleCallbackNoNamesProperty)
{
    Handler handler(sysDir);
    pldm::utils::InterfaceMap interfaces;
    // Compatible interface present but without the "Names" property.
    interfaces.emplace(InventoryDecoratorCompatible::interface,
                       pldm::utils::PropertyMap{});
    auto msg = makeInterfacesAddedMsg("/xyz/openbmc_project/inventory/noname",
                                      interfaces);
    sealAndRewind(msg);

    EXPECT_NO_THROW(handler.systemCompatibleCallback(msg));
    EXPECT_TRUE(handler.systemType.empty());
}

TEST_F(PlatformConfigTest, SystemCompatibleCallbackEmptyNames)
{
    Handler handler(sysDir);
    pldm::utils::InterfaceMap interfaces;
    pldm::utils::PropertyMap props;
    props.emplace("Names", std::vector<std::string>{});
    interfaces.emplace(InventoryDecoratorCompatible::interface, props);
    auto msg = makeInterfacesAddedMsg("/xyz/openbmc_project/inventory/empty",
                                      interfaces);
    sealAndRewind(msg);

    EXPECT_NO_THROW(handler.systemCompatibleCallback(msg));
    EXPECT_TRUE(handler.systemType.empty());
}
