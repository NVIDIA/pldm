#include "../utils.hpp"

#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace
{

using namespace pldm::utils;

class StringPropertyCoverageHandler : public DBusHandler
{
  public:
    mutable PropertyValue nextValue{std::string{}};

    PropertyValue getDbusPropertyVariant(const char*, const char*,
                                         const char*) const override
    {
        return nextValue;
    }
};

struct GetManagedEmptyObject
{
    static ObjectValueTree getManagedObj(const char*, const char*)
    {
        return {};
    }
};

struct GetManagedObject
{
    static ObjectValueTree getManagedObj(const char*, const char*)
    {
        ObjectValueTree objects;
        objects.emplace(
            sdbusplus::message::object_path(
                "/xyz/openbmc_project/inventory/fresh-tu"),
            InterfaceMap{
                {"xyz.openbmc_project.Inventory.Item",
                 PropertyMap{{"PrettyName", std::string("fresh-tu")}}}});
        return objects;
    }
};

struct ThrowingManagedObject
{
    static ObjectValueTree getManagedObj(const char*, const char*)
    {
        throw std::runtime_error("forced initialization failure");
    }
};

struct FlakyManagedObject
{
    static void reset()
    {
        attempts() = 0;
    }

    static ObjectValueTree getManagedObj(const char*, const char*)
    {
        if (attempts()++ == 0)
        {
            throw std::runtime_error("transient initialization failure");
        }

        ObjectValueTree objects;
        objects.emplace(sdbusplus::message::object_path(
                            "/xyz/openbmc_project/inventory/flaky-fresh-tu"),
                        InterfaceMap{});
        return objects;
    }

  private:
    static int& attempts()
    {
        static int value = 0;
        return value;
    }
};

TEST(UtilsInlineCrossTuCoverage, DecimalToBcdAndStaticAccessors)
{
    EXPECT_EQ(decimalToBcd<uint8_t>(0), uint8_t{0});
    EXPECT_EQ(decimalToBcd<uint8_t>(45), uint8_t{0x45});

    auto& bus = DBusHandler::getBus();
    EXPECT_EQ(&bus, &DBusHandler::getBus());

    auto& connection = DBusHandler::getAsioConnection();
    EXPECT_EQ(connection.get(), DBusHandler::getAsioConnection().get());
}

TEST(UtilsInlineCrossTuCoverage, GetDbusPropertyAndInventoryObjects)
{
    StringPropertyCoverageHandler handler;
    handler.nextValue = std::string("fresh-value");
    EXPECT_EQ(handler.getDbusProperty<std::string>(
                  "/xyz/openbmc_project/example", "PrettyName",
                  "xyz.openbmc_project.Inventory.Item"),
              "fresh-value");

    handler.nextValue = uint64_t{9};
    EXPECT_THROW((handler.getDbusProperty<std::string>(
                     "/xyz/openbmc_project/example", "PrettyName",
                     "xyz.openbmc_project.Inventory.Item")),
                 std::bad_variant_access);

    auto& empty = DBusHandler::getInventoryObjects<GetManagedEmptyObject>();
    EXPECT_TRUE(empty.empty());

    auto& first = DBusHandler::getInventoryObjects<GetManagedObject>();
    ASSERT_EQ(first.size(), 1u);
    auto& second = DBusHandler::getInventoryObjects<GetManagedObject>();
    EXPECT_EQ(&first, &second);

    EXPECT_THROW((DBusHandler::getInventoryObjects<ThrowingManagedObject>()),
                 std::runtime_error);

    FlakyManagedObject::reset();
    EXPECT_THROW((DBusHandler::getInventoryObjects<FlakyManagedObject>()),
                 std::runtime_error);

    auto& recovered = DBusHandler::getInventoryObjects<FlakyManagedObject>();
    ASSERT_EQ(recovered.size(), 1u);
    auto& cached = DBusHandler::getInventoryObjects<FlakyManagedObject>();
    EXPECT_EQ(&recovered, &cached);
}

} // namespace
