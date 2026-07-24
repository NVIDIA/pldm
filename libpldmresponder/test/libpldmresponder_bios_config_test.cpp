#include "common/bios_utils.hpp"
#include "common/test/mocked_utils.hpp"
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "libpldmresponder/bios_config.hpp"
#undef private
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "libpldmresponder/bios_enum_attribute.hpp"
#include "libpldmresponder/bios_integer_attribute.hpp"
#include "libpldmresponder/bios_string_attribute.hpp"
#include "libpldmresponder/platform_config.hpp"
#include "mocked_bios.hpp"
#include "test/test_tmp_utils.hpp"

#include <libpldm/edac.h>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_future.hpp>
#include <nlohmann/json.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pldm::bios::utils;
using namespace pldm::responder::bios;
using namespace pldm::utils;

using ::testing::_;
using ::testing::ElementsAreArray;
using ::testing::Return;
using ::testing::StrEq;
using ::testing::Throw;

namespace
{

const pldm_bios_attr_table_entry* findAttrEntryByName(
    const Table& attrTable, const Table& stringTable, const std::string& name)
{
    BIOSStringTable biosStringTable(stringTable);
    auto stringHandle = biosStringTable.findHandle(name);
    return table::attribute::findByStringHandle(attrTable, stringHandle);
}

const pldm_bios_attr_val_table_entry* findAttrValueEntryByHandle(
    const Table& attrValueTable, uint16_t attrHandle)
{
    for (auto entry : BIOSTableIter<PLDM_BIOS_ATTR_VAL_TABLE>(
             attrValueTable.data(), attrValueTable.size()))
    {
        auto [currentHandle, _] = table::attribute_value::decodeHeader(entry);
        if (currentHandle == attrHandle)
        {
            return entry;
        }
    }
    return nullptr;
}

size_t findAttrIndex(const BIOSConfig& biosConfig, const std::string& name)
{
    auto iter = std::find_if(
        biosConfig.biosAttributes.begin(), biosConfig.biosAttributes.end(),
        [&name](const auto& attr) { return attr->name == name; });
    if (iter == biosConfig.biosAttributes.end())
    {
        throw std::invalid_argument("Unknown BIOS attribute");
    }
    return static_cast<size_t>(
        std::distance(biosConfig.biosAttributes.begin(), iter));
}

void drainDbusSignals()
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    for (int i = 0; i < 8; ++i)
    {
        bus.wait(std::chrono::milliseconds(20));
        while (bus.process_discard())
        {}
    }
}

void refreshTableChecksum(Table& table)
{
    auto checksum =
        htole32(pldm_edac_crc32(table.data(), table.size() - sizeof(uint32_t)));
    std::memcpy(table.data() + table.size() - sizeof(checksum), &checksum,
                sizeof(checksum));
}

void overwriteStringValue(Table& stringTable, uint16_t stringHandle,
                          const std::string& value)
{
    auto* entry = const_cast<pldm_bios_string_table_entry*>(
        pldm_bios_table_string_find_by_handle(
            stringTable.data(), stringTable.size(), stringHandle));
    if (entry == nullptr)
    {
        throw std::invalid_argument("Unknown string handle");
    }

    auto stringLength =
        pldm_bios_table_string_entry_decode_string_length(entry);
    if (stringLength != value.size())
    {
        throw std::invalid_argument("Replacement string length mismatch");
    }

    std::copy(value.begin(), value.end(), entry->name);
    refreshTableChecksum(stringTable);
}

Table buildAttrValueTableWithoutHandle(const Table& attrValueTable,
                                       uint16_t skippedHandle)
{
    Table filteredTable;
    for (auto entry : BIOSTableIter<PLDM_BIOS_ATTR_VAL_TABLE>(
             attrValueTable.data(), attrValueTable.size()))
    {
        auto [currentHandle, _] = table::attribute_value::decodeHeader(entry);
        if (currentHandle == skippedHandle)
        {
            continue;
        }

        auto entryLength = pldm_bios_table_attr_value_entry_length(entry);
        auto* entryBytes = reinterpret_cast<const uint8_t*>(entry);
        filteredTable.insert(filteredTable.end(), entryBytes,
                             entryBytes + entryLength);
    }

    table::appendPadAndChecksum(filteredTable);
    return filteredTable;
}

class BIOSConfigDbusFixture
{
  public:
    static constexpr auto serviceName = "xyz.openbmc_project.BIOSConfig.Test";
    static constexpr auto objectPath =
        "/xyz/openbmc_project/bios_config/manager";
    static constexpr auto interfaceName =
        "xyz.openbmc_project.BIOSConfig.Manager";
    static constexpr auto stringObjectPath = "/xyz/abc/def";
    static constexpr auto stringInterfaceName =
        "xyz.openbmc_project.str_example1.value";
    static constexpr auto avsObjectPath = "/xyz/openbmc_project/avsbus";
    static constexpr auto avsInterfaceName = "xyz.openbmc.AvsBus.Manager";
    static constexpr auto enumObjectPath = "/xyz/abc/def";
    static constexpr auto enumInterfaceName = "xyz.openbmc.InBandCodeUpdate";

    static std::string initialStringValue(const BaseBIOSTable& table)
    {
        auto iter = table.find("str_example1");
        if (iter == table.end())
        {
            return "abc";
        }
        return std::get<std::string>(
            std::get<static_cast<uint8_t>(BIOSConfig::Index::currentValue)>(
                iter->second));
    }

    explicit BIOSConfigDbusFixture(BaseBIOSTable initialTable) :
        baseTable(std::move(initialTable)),
        strExample1(initialStringValue(baseTable))
    {
        connection = std::make_shared<sdbusplus::asio::connection>(
            io, sdbusplus::bus::new_bus());
        connection->request_name(serviceName);
        server = std::make_unique<sdbusplus::asio::object_server>(connection);
        iface = server->add_interface(objectPath, interfaceName);
        iface->register_property(
            "BaseBIOSTable", baseTable,
            [this](const BaseBIOSTable& requested, BaseBIOSTable& current) {
                current = requested;
                baseTable = requested;
                ++setCount;
                return true;
            },
            [](const BaseBIOSTable& current) { return current; });
        iface->register_property(
            "PendingAttributes", pendingAttributes,
            [this](const PendingAttributes& requested,
                   PendingAttributes& current) {
                current = requested;
                pendingAttributes = requested;
                ++pendingSetCount;
                return true;
            },
            [](const PendingAttributes& current) { return current; });
        iface->initialize();

        strExample1Iface =
            server->add_interface(stringObjectPath, stringInterfaceName);
        strExample1Iface->register_property(
            "Str_example1", strExample1,
            [this](const std::string& requested, std::string& current) {
                current = requested;
                strExample1 = requested;
                ++strExample1SetCount;
                return true;
            },
            [](const std::string& current) { return current; });
        strExample1Iface->initialize();

        avsIface = server->add_interface(avsObjectPath, avsInterfaceName);
        avsIface->register_property(
            "Rail", avsRail,
            [this](const uint8_t& requested, uint8_t& current) {
                current = requested;
                avsRail = requested;
                ++avsSetCount;
                return true;
            },
            [](const uint8_t& current) { return current; });
        avsIface->initialize();

        enumIface = server->add_interface(enumObjectPath, enumInterfaceName);
        enumIface->register_property(
            "Policy", inbandCodeUpdatePolicy,
            [this](const uint8_t& requested, uint8_t& current) {
                current = requested;
                inbandCodeUpdatePolicy = requested;
                ++policySetCount;
                return true;
            },
            [](const uint8_t& current) { return current; });
        enumIface->initialize();

        ioThread = std::thread([this] { io.run(); });
    }

    ~BIOSConfigDbusFixture()
    {
        io.stop();
        if (ioThread.joinable())
        {
            ioThread.join();
        }
    }

    bool setPendingAttributesProperty(const PendingAttributes& value)
    {
        return setPropertyOnIoThread(iface, "PendingAttributes", value);
    }

    bool setStrExample1Property(const std::string& value)
    {
        return setPropertyOnIoThread(strExample1Iface, "Str_example1", value);
    }

    bool setAvsRailProperty(uint8_t value)
    {
        return setPropertyOnIoThread(avsIface, "Rail", value);
    }

    bool setInbandCodeUpdatePolicy(uint8_t value)
    {
        return setPropertyOnIoThread(enumIface, "Policy", value);
    }

    /** @brief set_property performs sd-bus operations (property update +
     *         PropertiesChanged emission); sd_bus is not thread-safe and
     *         ioThread concurrently dispatches messages on the same bus, so
     *         run the mutation on the io thread instead of racing it from
     *         the test thread. Completion of the future also guarantees the
     *         signal was handed to the daemon, replacing the sleep-based
     *         propagation guesses. */
    template <typename T>
    bool setPropertyOnIoThread(
        const std::shared_ptr<sdbusplus::asio::dbus_interface>& intf,
        const std::string& propertyName, const T& value)
    {
        auto done = boost::asio::post(
            io, boost::asio::use_future([intf, propertyName, value] {
                return intf->set_property(propertyName, value);
            }));
        if (done.wait_for(std::chrono::seconds(10)) !=
            std::future_status::ready)
        {
            ADD_FAILURE() << "Timed out setting property " << propertyName
                          << " on the fixture io thread";
            return false;
        }
        return done.get();
    }

    boost::asio::io_context io;
    // Keep io.run() alive even when the connection is momentarily out of
    // pending async work; without this the io thread can exit mid-test and
    // the server silently stops dispatching.
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type>
        workGuard = boost::asio::make_work_guard(io);
    std::shared_ptr<sdbusplus::asio::connection> connection;
    std::unique_ptr<sdbusplus::asio::object_server> server;
    std::shared_ptr<sdbusplus::asio::dbus_interface> iface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> strExample1Iface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> avsIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> enumIface;
    BaseBIOSTable baseTable;
    PendingAttributes pendingAttributes{};
    std::string strExample1;
    uint8_t avsRail = 0;
    uint8_t inbandCodeUpdatePolicy = 0;
    size_t setCount = 0;
    size_t pendingSetCount = 0;
    size_t strExample1SetCount = 0;
    size_t avsSetCount = 0;
    size_t policySetCount = 0;
    std::thread ioThread;
};

} // namespace

class TestBIOSConfig : public ::testing::Test
{
  public:
    static void SetUpTestCase() // will execute once at the begining of all
                                // TestBIOSConfig objects
    {
        tableDir = pldm::test::makeTempDir("BIOSTables.XXXXXX");

        std::vector<fs::path> paths = {
            "./bios_jsons/string_attrs.json",
            "./bios_jsons/integer_attrs.json",
            "./bios_jsons/enum_attrs.json",
        };

        for (auto& path : paths)
        {
            std::ifstream file;
            file.open(path);
            auto j = Json::parse(file);
            jsons.emplace_back(j);
        }
    }

    std::optional<Json> findJsonEntry(const std::string& name)
    {
        for (auto& json : jsons)
        {
            auto entries = json.at("entries");
            for (auto& entry : entries)
            {
                auto n = entry.at("attribute_name").get<std::string>();
                if (n == name)
                {
                    return entry;
                }
            }
        }
        return std::nullopt;
    }

    static void TearDownTestCase() // will be executed once at th end of all
                                   // TestBIOSConfig objects
    {
        fs::remove_all(tableDir);
    }

    static fs::path tableDir;
    static std::vector<Json> jsons;
};

fs::path TestBIOSConfig::tableDir;
std::vector<Json> TestBIOSConfig::jsons;

TEST_F(TestBIOSConfig, buildTablesTest)
{
    MockdBusHandler dbusHandler;

    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.buildTables();

    auto stringTable = biosConfig.getBIOSTable(PLDM_BIOS_STRING_TABLE);
    auto attrTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_TABLE);
    auto attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);

    EXPECT_TRUE(stringTable);
    EXPECT_TRUE(attrTable);
    EXPECT_TRUE(attrValueTable);

    std::set<std::string> expectedStrings = {
        "HMCManagedState",
        "On",
        "Off",
        "FWBootSide",
        "Perm",
        "Temp",
        "InbandCodeUpdate",
        "Allowed",
        "NotAllowed",
        "CodeUpdatePolicy",
        "Concurrent",
        "Disruptive",
        "VDD_AVSBUS_RAIL",
        "SBE_IMAGE_MINIMUM_VALID_ECS",
        "INTEGER_INVALID_CASE",
        "str_example1",
        "str_example2",
        "str_example3"};
    std::set<std::string> strings;
    for (auto entry : BIOSTableIter<PLDM_BIOS_STRING_TABLE>(
             stringTable->data(), stringTable->size()))
    {
        auto str = table::string::decodeString(entry);
        strings.emplace(str);
    }

    EXPECT_EQ(strings, expectedStrings);

    BIOSStringTable biosStringTable(*stringTable);

    for (auto entry : BIOSTableIter<PLDM_BIOS_ATTR_TABLE>(attrTable->data(),
                                                          attrTable->size()))
    {
        auto header = table::attribute::decodeHeader(entry);
        auto attrName = biosStringTable.findString(header.stringHandle);
        auto jsonEntry = findJsonEntry(attrName);
        EXPECT_TRUE(jsonEntry);
        switch (header.attrType)
        {
            case PLDM_BIOS_STRING:
            case PLDM_BIOS_STRING_READ_ONLY:
            {
                auto stringField = table::attribute::decodeStringEntry(entry);
                auto stringType = BIOSStringAttribute::strTypeMap.at(
                    jsonEntry->at("string_type").get<std::string>());
                EXPECT_EQ(stringField.stringType,
                          static_cast<uint8_t>(stringType));

                EXPECT_EQ(
                    stringField.minLength,
                    jsonEntry->at("minimum_string_length").get<uint16_t>());
                EXPECT_EQ(
                    stringField.maxLength,
                    jsonEntry->at("maximum_string_length").get<uint16_t>());
                EXPECT_EQ(
                    stringField.defLength,
                    jsonEntry->at("default_string_length").get<uint16_t>());
                EXPECT_EQ(stringField.defString,
                          jsonEntry->at("default_string").get<std::string>());
                break;
            }
            case PLDM_BIOS_INTEGER:
            case PLDM_BIOS_INTEGER_READ_ONLY:
            {
                auto integerField = table::attribute::decodeIntegerEntry(entry);
                EXPECT_EQ(integerField.lowerBound,
                          jsonEntry->at("lower_bound").get<uint64_t>());
                EXPECT_EQ(integerField.upperBound,
                          jsonEntry->at("upper_bound").get<uint64_t>());
                EXPECT_EQ(integerField.scalarIncrement,
                          jsonEntry->at("scalar_increment").get<uint32_t>());
                EXPECT_EQ(integerField.defaultValue,
                          jsonEntry->at("default_value").get<uint64_t>());
                break;
            }
            case PLDM_BIOS_ENUMERATION:
            case PLDM_BIOS_ENUMERATION_READ_ONLY:
            {
                auto [pvHdls,
                      defInds] = table::attribute::decodeEnumEntry(entry);
                auto possibleValues = jsonEntry->at("possible_values")
                                          .get<std::vector<std::string>>();
                std::vector<std::string> strings;
                for (auto pv : pvHdls)
                {
                    auto s = biosStringTable.findString(pv);
                    strings.emplace_back(s);
                }
                EXPECT_EQ(strings, possibleValues);
                EXPECT_EQ(defInds.size(), 1);

                auto defValue = biosStringTable.findString(pvHdls[defInds[0]]);
                auto defaultValues = jsonEntry->at("default_values")
                                         .get<std::vector<std::string>>();
                EXPECT_EQ(defValue, defaultValues[0]);

                break;
            }
            default:
                EXPECT_TRUE(false);
                break;
        }
    }

    for (auto entry : BIOSTableIter<PLDM_BIOS_ATTR_VAL_TABLE>(
             attrValueTable->data(), attrValueTable->size()))
    {
        auto header = table::attribute_value::decodeHeader(entry);
        auto attrEntry =
            table::attribute::findByHandle(*attrTable, header.attrHandle);
        auto attrHeader = table::attribute::decodeHeader(attrEntry);
        auto attrName = biosStringTable.findString(attrHeader.stringHandle);
        auto jsonEntry = findJsonEntry(attrName);
        EXPECT_TRUE(jsonEntry);
        switch (header.attrType)
        {
            case PLDM_BIOS_STRING:
            case PLDM_BIOS_STRING_READ_ONLY:
            {
                auto value = table::attribute_value::decodeStringEntry(entry);
                auto defValue =
                    jsonEntry->at("default_string").get<std::string>();
                EXPECT_EQ(value, defValue);
                break;
            }
            case PLDM_BIOS_INTEGER:
            case PLDM_BIOS_INTEGER_READ_ONLY:
            {
                auto value = table::attribute_value::decodeIntegerEntry(entry);
                auto defValue = jsonEntry->at("default_value").get<uint64_t>();
                EXPECT_EQ(value, defValue);
                break;
            }
            case PLDM_BIOS_ENUMERATION:
            case PLDM_BIOS_ENUMERATION_READ_ONLY:
            {
                auto indices = table::attribute_value::decodeEnumEntry(entry);
                EXPECT_EQ(indices.size(), 1);
                auto possibleValues = jsonEntry->at("possible_values")
                                          .get<std::vector<std::string>>();

                auto defValues = jsonEntry->at("default_values")
                                     .get<std::vector<std::string>>();
                EXPECT_EQ(possibleValues[indices[0]], defValues[0]);
                break;
            }
            default:
                EXPECT_TRUE(false);
                break;
        }
    }
}

TEST_F(TestBIOSConfig, setAttrValue)
{
    MockdBusHandler dbusHandler;

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    auto stringTable = biosConfig.getBIOSTable(PLDM_BIOS_STRING_TABLE);
    auto attrTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_TABLE);

    BIOSStringTable biosStringTable(*stringTable);
    BIOSTableIter<PLDM_BIOS_ATTR_TABLE> attrTableIter(attrTable->data(),
                                                      attrTable->size());
    auto stringHandle = biosStringTable.findHandle("str_example1");
    uint16_t attrHandle{};

    for (auto entry : BIOSTableIter<PLDM_BIOS_ATTR_TABLE>(attrTable->data(),
                                                          attrTable->size()))
    {
        auto header = table::attribute::decodeHeader(entry);
        if (header.stringHandle == stringHandle)
        {
            attrHandle = header.attrHandle;
            break;
        }
    }

    EXPECT_NE(attrHandle, 0);

    std::vector<uint8_t> attrValueEntry{
        0,   0,             /* attr handle */
        1,                  /* attr type string read-write */
        4,   0,             /* current string length */
        'a', 'b', 'c', 'd', /* defaut value string handle index */
    };

    attrValueEntry[0] = attrHandle & 0xff;
    attrValueEntry[1] = (attrHandle >> 8) & 0xff;

    DBusMapping dbusMapping{"/xyz/abc/def",
                            "xyz.openbmc_project.str_example1.value",
                            "Str_example1", "string"};
    PropertyValue value = std::string("abcd");
    EXPECT_CALL(dbusHandler, setDbusProperty(dbusMapping, value)).Times(1);

    auto rc =
        biosConfig.setAttrValue(attrValueEntry.data(), attrValueEntry.size());
    EXPECT_EQ(rc, PLDM_SUCCESS);

    auto attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    auto findEntry = [&attrValueTable](uint16_t handle)
        -> const pldm_bios_attr_val_table_entry* {
        for (auto entry : BIOSTableIter<PLDM_BIOS_ATTR_VAL_TABLE>(
                 attrValueTable->data(), attrValueTable->size()))
        {
            auto [attrHandle, _] = table::attribute_value::decodeHeader(entry);
            if (attrHandle == handle)
                return entry;
        }
        return nullptr;
    };

    auto entry = findEntry(attrHandle);
    EXPECT_NE(entry, nullptr);

    auto p = reinterpret_cast<const uint8_t*>(entry);
    EXPECT_THAT(std::vector<uint8_t>(p, p + attrValueEntry.size()),
                ElementsAreArray(attrValueEntry));
}

TEST_F(TestBIOSConfig, setBIOSTableValidationCoverage)
{
    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    auto stringTable = biosConfig.getBIOSTable(PLDM_BIOS_STRING_TABLE);
    auto attrTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_TABLE);
    auto attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(stringTable.has_value());
    ASSERT_TRUE(attrTable.has_value());
    ASSERT_TRUE(attrValueTable.has_value());

    Table badChecksum = *stringTable;
    badChecksum.back() ^= 0xFF;
    EXPECT_EQ(biosConfig.setBIOSTable(PLDM_BIOS_STRING_TABLE, badChecksum),
              PLDM_INVALID_BIOS_TABLE_DATA_INTEGRITY_CHECK);
    EXPECT_EQ(biosConfig.setBIOSTable(0xFF, *stringTable),
              PLDM_INVALID_BIOS_TABLE_TYPE);

    biosConfig.removeTables();
    EXPECT_EQ(biosConfig.setBIOSTable(PLDM_BIOS_ATTR_TABLE, *attrTable),
              PLDM_INVALID_BIOS_TABLE_TYPE);
    EXPECT_EQ(biosConfig.setBIOSTable(PLDM_BIOS_STRING_TABLE, *stringTable),
              PLDM_SUCCESS);
    EXPECT_EQ(
        biosConfig.setBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE, *attrValueTable),
        PLDM_INVALID_BIOS_TABLE_TYPE);

    auto* enumAttrEntry =
        findAttrEntryByName(*attrTable, *stringTable, "CodeUpdatePolicy");
    ASSERT_NE(enumAttrEntry, nullptr);
    auto enumHeader = table::attribute::decodeHeader(enumAttrEntry);
    auto [pvHdls,
          defIndices] = table::attribute::decodeEnumEntry(enumAttrEntry);
    pvHdls[0] = 0xFFFF;
    Table invalidEnumAttrTable;
    pldm_bios_table_attr_entry_enum_info enumInfo = {
        enumHeader.stringHandle,
        enumHeader.attrType == PLDM_BIOS_ENUMERATION_READ_ONLY,
        static_cast<uint8_t>(pvHdls.size()),
        pvHdls.data(),
        static_cast<uint8_t>(defIndices.size()),
        defIndices.data(),
    };
    table::attribute::constructEnumEntry(invalidEnumAttrTable, &enumInfo);
    table::appendPadAndChecksum(invalidEnumAttrTable);
    EXPECT_EQ(
        biosConfig.setBIOSTable(PLDM_BIOS_ATTR_TABLE, invalidEnumAttrTable),
        PLDM_INVALID_BIOS_ATTR_HANDLE);
    EXPECT_EQ(biosConfig.setBIOSTable(PLDM_BIOS_ATTR_TABLE, *attrTable),
              PLDM_SUCCESS);

    Table invalidAttrValueTable = *attrValueTable;
    auto* firstAttrValue = reinterpret_cast<pldm_bios_attr_val_table_entry*>(
        invalidAttrValueTable.data());
    firstAttrValue->attr_handle = htole16(0xFFFF);
    auto checksum = htole32(
        pldm_edac_crc32(invalidAttrValueTable.data(),
                        invalidAttrValueTable.size() - sizeof(uint32_t)));
    std::memcpy(invalidAttrValueTable.data() + invalidAttrValueTable.size() -
                    sizeof(checksum),
                &checksum, sizeof(checksum));
    EXPECT_EQ(biosConfig.setBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE,
                                      invalidAttrValueTable),
              PLDM_INVALID_BIOS_ATTR_HANDLE);

    biosConfig.removeTables();
    biosConfig.storeTable(tableDir / "attributeTable", *attrTable);
    EXPECT_EQ(
        biosConfig.setBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE, *attrValueTable),
        PLDM_INVALID_BIOS_TABLE_TYPE);
}

TEST_F(TestBIOSConfig, checkAttrValueToUpdateCoverage)
{
    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    auto stringTable = biosConfig.getBIOSTable(PLDM_BIOS_STRING_TABLE);
    auto attrTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_TABLE);
    auto attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(stringTable.has_value());
    ASSERT_TRUE(attrTable.has_value());
    ASSERT_TRUE(attrValueTable.has_value());

    auto* enumAttrEntry =
        findAttrEntryByName(*attrTable, *stringTable, "HMCManagedState");
    ASSERT_NE(enumAttrEntry, nullptr);
    auto enumHeader = table::attribute::decodeHeader(enumAttrEntry);

    Table enumInvalidLength;
    table::attribute_value::constructEnumEntry(
        enumInvalidLength, enumHeader.attrHandle, enumHeader.attrType, {0, 1});
    EXPECT_EQ(biosConfig.checkAttrValueToUpdate(
                  reinterpret_cast<const pldm_bios_attr_val_table_entry*>(
                      enumInvalidLength.data()),
                  enumAttrEntry, *stringTable),
              PLDM_ERROR_INVALID_LENGTH);

    Table enumInvalidIndex;
    table::attribute_value::constructEnumEntry(
        enumInvalidIndex, enumHeader.attrHandle, enumHeader.attrType, {9});
    EXPECT_EQ(biosConfig.checkAttrValueToUpdate(
                  reinterpret_cast<const pldm_bios_attr_val_table_entry*>(
                      enumInvalidIndex.data()),
                  enumAttrEntry, *stringTable),
              PLDM_ERROR_INVALID_DATA);

    auto* integerAttrEntry = findAttrEntryByName(*attrTable, *stringTable,
                                                 "SBE_IMAGE_MINIMUM_VALID_ECS");
    ASSERT_NE(integerAttrEntry, nullptr);
    auto integerHeader = table::attribute::decodeHeader(integerAttrEntry);
    auto [lowerBound, upperBound, scalarIncrement, defaultValue] =
        table::attribute::decodeIntegerEntry(integerAttrEntry);
    (void)lowerBound;
    (void)scalarIncrement;
    (void)defaultValue;

    Table integerOutOfRange;
    table::attribute_value::constructIntegerEntry(
        integerOutOfRange, integerHeader.attrHandle, integerHeader.attrType,
        upperBound + 1);
    EXPECT_EQ(biosConfig.checkAttrValueToUpdate(
                  reinterpret_cast<const pldm_bios_attr_val_table_entry*>(
                      integerOutOfRange.data()),
                  integerAttrEntry, *stringTable),
              PLDM_ERROR_INVALID_DATA);

    auto* stringAttrEntry =
        findAttrEntryByName(*attrTable, *stringTable, "str_example1");
    ASSERT_NE(stringAttrEntry, nullptr);
    auto stringHeader = table::attribute::decodeHeader(stringAttrEntry);
    auto stringField = table::attribute::decodeStringEntry(stringAttrEntry);
    Table stringTooLong;
    table::attribute_value::constructStringEntry(
        stringTooLong, stringHeader.attrHandle, stringHeader.attrType,
        std::string(stringField.maxLength + 1, 'x'));
    EXPECT_EQ(biosConfig.checkAttrValueToUpdate(
                  reinterpret_cast<const pldm_bios_attr_val_table_entry*>(
                      stringTooLong.data()),
                  stringAttrEntry, *stringTable),
              PLDM_ERROR_INVALID_LENGTH);

    Table stringTooShort;
    table::attribute_value::constructStringEntry(
        stringTooShort, stringHeader.attrHandle, stringHeader.attrType, "");
    EXPECT_EQ(biosConfig.checkAttrValueToUpdate(
                  reinterpret_cast<const pldm_bios_attr_val_table_entry*>(
                      stringTooShort.data()),
                  stringAttrEntry, *stringTable),
              PLDM_ERROR_INVALID_LENGTH);

    std::array<uint8_t, sizeof(pldm_bios_attr_val_table_entry)> unsupported{};
    auto* unsupportedEntry =
        reinterpret_cast<pldm_bios_attr_val_table_entry*>(unsupported.data());
    unsupportedEntry->attr_handle = enumHeader.attrHandle;
    unsupportedEntry->attr_type = PLDM_BIOS_PASSWORD;
    EXPECT_EQ(biosConfig.checkAttrValueToUpdate(unsupportedEntry, enumAttrEntry,
                                                *stringTable),
              PLDM_ERROR);
}

TEST_F(TestBIOSConfig, checkTablesAndFindAttrHandleCoverage)
{
    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    auto stringTable = biosConfig.getBIOSTable(PLDM_BIOS_STRING_TABLE);
    auto attrTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_TABLE);
    auto attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(stringTable.has_value());
    ASSERT_TRUE(attrTable.has_value());
    ASSERT_TRUE(attrValueTable.has_value());

    Table invalidAttrTable = *attrTable;
    auto* firstAttr =
        reinterpret_cast<pldm_bios_attr_table_entry*>(invalidAttrTable.data());
    firstAttr->string_handle = 0xFFFF;
    EXPECT_EQ(biosConfig.checkAttributeTable(invalidAttrTable),
              PLDM_INVALID_BIOS_ATTR_HANDLE);

    Table invalidAttrValueTable = *attrValueTable;
    auto* firstAttrValue = reinterpret_cast<pldm_bios_attr_val_table_entry*>(
        invalidAttrValueTable.data());
    firstAttrValue->attr_handle = 0xFFFF;
    EXPECT_EQ(biosConfig.checkAttributeValueTable(invalidAttrValueTable),
              PLDM_INVALID_BIOS_ATTR_HANDLE);

    auto attrHandle = biosConfig.findAttrHandle("str_example1");
    auto* attrEntry =
        findAttrEntryByName(*attrTable, *stringTable, "str_example1");
    ASSERT_NE(attrEntry, nullptr);
    EXPECT_EQ(attrHandle, table::attribute::decodeHeader(attrEntry).attrHandle);
    EXPECT_NE(findAttrValueEntryByHandle(*attrValueTable, attrHandle), nullptr);
    EXPECT_NE(biosConfig.findAttrHandle("VDD_AVSBUS_RAIL"), 0);
    EXPECT_NE(biosConfig.findAttrHandle("CodeUpdatePolicy"), 0);
    EXPECT_THROW(biosConfig.findAttrHandle("missing-attribute"),
                 std::invalid_argument);
}

TEST_F(TestBIOSConfig, checkAttributeTableEmptyCoverage)
{
    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    EXPECT_EQ(biosConfig.checkAttributeTable(Table{}), PLDM_SUCCESS);
}

TEST_F(TestBIOSConfig, checkAttributeTableEnumMissingPossibleValueCoverage)
{
    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));
    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);

    Table stringTable;
    const auto nameHandle = table::string::decodeHandle(
        table::string::constructEntry(stringTable, "EmptyEnum"));
    table::appendPadAndChecksum(stringTable);
    biosConfig.storeTable(tableDir / "stringTable", stringTable);

    std::vector<uint16_t> possibleValues{
        static_cast<uint16_t>(nameHandle + 1),
    };
    std::vector<uint8_t> defaultIndices{};
    pldm_bios_table_attr_entry_enum_info enumInfo = {
        nameHandle,
        false,
        static_cast<uint8_t>(possibleValues.size()),
        possibleValues.data(),
        static_cast<uint8_t>(defaultIndices.size()),
        defaultIndices.data(),
    };
    Table attrTable;
    table::attribute::constructEnumEntry(attrTable, &enumInfo);
    table::appendPadAndChecksum(attrTable);

    EXPECT_EQ(biosConfig.checkAttributeTable(attrTable),
              PLDM_INVALID_BIOS_ATTR_HANDLE);
}

TEST_F(TestBIOSConfig, checkAttributeTableEnumWithoutDefaultsCoverage)
{
    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));
    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    auto stringTable = biosConfig.getBIOSTable(PLDM_BIOS_STRING_TABLE);
    auto attrTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_TABLE);
    ASSERT_TRUE(stringTable.has_value());
    ASSERT_TRUE(attrTable.has_value());

    auto* enumAttrEntry =
        findAttrEntryByName(*attrTable, *stringTable, "CodeUpdatePolicy");
    ASSERT_NE(enumAttrEntry, nullptr);
    auto header = table::attribute::decodeHeader(enumAttrEntry);
    auto [possibleValues, _] = table::attribute::decodeEnumEntry(enumAttrEntry);
    std::array<uint8_t, 1> ignoredDefaultIndices{0};
    pldm_bios_table_attr_entry_enum_info enumInfo = {
        header.stringHandle,
        header.attrType == PLDM_BIOS_ENUMERATION_READ_ONLY,
        static_cast<uint8_t>(possibleValues.size()),
        possibleValues.data(),
        0,
        ignoredDefaultIndices.data(),
    };

    Table noDefaultAttrTable(pldm_bios_table_attr_entry_enum_encode_length(
        enumInfo.pv_num, enumInfo.def_num));
    ASSERT_EQ(pldm_bios_table_attr_entry_enum_encode(
                  noDefaultAttrTable.data(), noDefaultAttrTable.size(),
                  &enumInfo),
              PLDM_SUCCESS);
    table::appendPadAndChecksum(noDefaultAttrTable);

    EXPECT_EQ(biosConfig.checkAttributeTable(noDefaultAttrTable), PLDM_SUCCESS);
}

TEST_F(TestBIOSConfig, setAttrValueAdditionalCoverage)
{
    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    auto stringTable = biosConfig.getBIOSTable(PLDM_BIOS_STRING_TABLE);
    auto attrTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_TABLE);
    auto attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(stringTable.has_value());
    ASSERT_TRUE(attrTable.has_value());
    ASSERT_TRUE(attrValueTable.has_value());

    auto attrHandle = biosConfig.findAttrHandle("str_example1");
    auto* attrEntry =
        findAttrEntryByName(*attrTable, *stringTable, "str_example1");
    auto* attrValueEntry =
        findAttrValueEntryByHandle(*attrValueTable, attrHandle);
    ASSERT_NE(attrEntry, nullptr);
    ASSERT_NE(attrValueEntry, nullptr);

    auto integerHandle = biosConfig.findAttrHandle("VDD_AVSBUS_RAIL");
    auto* integerEntry =
        findAttrValueEntryByHandle(*attrValueTable, integerHandle);
    ASSERT_NE(integerEntry, nullptr);
    EXPECT_EQ(biosConfig.setAttrValue(
                  integerEntry,
                  pldm_bios_table_attr_value_entry_length(integerEntry), false,
                  false),
              PLDM_SUCCESS);

    auto enumHandle = biosConfig.findAttrHandle("CodeUpdatePolicy");
    auto* enumEntry = findAttrValueEntryByHandle(*attrValueTable, enumHandle);
    ASSERT_NE(enumEntry, nullptr);
    EXPECT_EQ(biosConfig.setAttrValue(
                  enumEntry, pldm_bios_table_attr_value_entry_length(enumEntry),
                  false, false),
              PLDM_SUCCESS);

    auto entryLength = pldm_bios_table_attr_value_entry_length(attrValueEntry);
    fs::remove(tableDir / "attributeTable");
    EXPECT_EQ(
        biosConfig.setAttrValue(attrValueEntry, entryLength, false, false),
        PLDM_BIOS_TABLE_UNAVAILABLE);

    biosConfig.storeTable(tableDir / "attributeTable", *attrTable);
    fs::remove(tableDir / "stringTable");
    EXPECT_EQ(
        biosConfig.setAttrValue(attrValueEntry, entryLength, false, false),
        PLDM_BIOS_TABLE_UNAVAILABLE);

    biosConfig.storeTable(tableDir / "stringTable", *stringTable);
    auto renamedStringTable = *stringTable;
    auto attrHeader = table::attribute::decodeHeader(attrEntry);
    overwriteStringValue(renamedStringTable, attrHeader.stringHandle,
                         "missingname1");
    EXPECT_EQ(biosConfig.setBIOSTable(PLDM_BIOS_STRING_TABLE,
                                      renamedStringTable, false),
              PLDM_SUCCESS);
    EXPECT_EQ(
        biosConfig.setAttrValue(attrValueEntry, entryLength, false, false),
        PLDM_ERROR);

    biosConfig.storeTable(tableDir / "stringTable", *stringTable);
}

TEST_F(TestBIOSConfig, setAttrValueDbusExceptionCoverage)
{
    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    auto attrTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_TABLE);
    auto stringTable = biosConfig.getBIOSTable(PLDM_BIOS_STRING_TABLE);
    ASSERT_TRUE(attrTable.has_value());
    ASSERT_TRUE(stringTable.has_value());

    auto attrHandle = biosConfig.findAttrHandle("str_example1");
    auto* attrEntry =
        findAttrEntryByName(*attrTable, *stringTable, "str_example1");
    ASSERT_NE(attrEntry, nullptr);

    Table attrValueEntry;
    auto header = table::attribute::decodeHeader(attrEntry);
    table::attribute_value::constructStringEntry(attrValueEntry, attrHandle,
                                                 header.attrType, "dbus-fail");

    EXPECT_CALL(dbusHandler,
                setDbusProperty(DBusMapping{"/xyz/abc/def",
                                            "xyz.openbmc_project.str_example1."
                                            "value",
                                            "Str_example1", "string"},
                                PropertyValue{std::string("dbus-fail")}))
        .WillOnce(Throw(std::runtime_error("dbus write failed")));

    EXPECT_EQ(biosConfig.setAttrValue(attrValueEntry.data(),
                                      attrValueEntry.size(), true, false),
              PLDM_ERROR);
}

TEST_F(TestBIOSConfig, checkAttributeValueTableInvalidEnumPossibleValueCoverage)
{
    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    auto stringTable = biosConfig.getBIOSTable(PLDM_BIOS_STRING_TABLE);
    auto attrTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_TABLE);
    auto attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(stringTable.has_value());
    ASSERT_TRUE(attrTable.has_value());
    ASSERT_TRUE(attrValueTable.has_value());

    Table invalidAttrTable = *attrTable;
    auto* enumAttrEntry =
        findAttrEntryByName(invalidAttrTable, *stringTable, "CodeUpdatePolicy");
    ASSERT_NE(enumAttrEntry, nullptr);
    auto enumHeader = table::attribute::decodeHeader(enumAttrEntry);
    auto [pvHdls,
          defIndices] = table::attribute::decodeEnumEntry(enumAttrEntry);
    pvHdls[0] = 0xFFFF;

    pldm_bios_table_attr_entry_enum_info enumInfo = {
        enumHeader.stringHandle,
        enumHeader.attrType == PLDM_BIOS_ENUMERATION_READ_ONLY,
        static_cast<uint8_t>(pvHdls.size()),
        pvHdls.data(),
        static_cast<uint8_t>(defIndices.size()),
        defIndices.data(),
    };
    Table rewrittenAttrTable;
    for (auto entry : BIOSTableIter<PLDM_BIOS_ATTR_TABLE>(
             invalidAttrTable.data(), invalidAttrTable.size()))
    {
        auto header = table::attribute::decodeHeader(entry);
        if (header.attrHandle == enumHeader.attrHandle)
        {
            table::attribute::constructEnumEntry(rewrittenAttrTable, &enumInfo);
            continue;
        }

        switch (header.attrType)
        {
            case PLDM_BIOS_STRING:
            case PLDM_BIOS_STRING_READ_ONLY:
            {
                auto stringInfo = table::attribute::decodeStringEntry(entry);
                pldm_bios_table_attr_entry_string_info info = {
                    header.stringHandle,
                    header.attrType == PLDM_BIOS_STRING_READ_ONLY,
                    stringInfo.stringType,
                    stringInfo.minLength,
                    stringInfo.maxLength,
                    stringInfo.defLength,
                    stringInfo.defString.c_str(),
                };
                table::attribute::constructStringEntry(rewrittenAttrTable,
                                                       &info);
                break;
            }
            case PLDM_BIOS_INTEGER:
            case PLDM_BIOS_INTEGER_READ_ONLY:
            {
                auto integerInfo = table::attribute::decodeIntegerEntry(entry);
                pldm_bios_table_attr_entry_integer_info info = {
                    header.stringHandle,
                    header.attrType == PLDM_BIOS_INTEGER_READ_ONLY,
                    integerInfo.lowerBound,
                    integerInfo.upperBound,
                    integerInfo.scalarIncrement,
                    integerInfo.defaultValue,
                };
                table::attribute::constructIntegerEntry(rewrittenAttrTable,
                                                        &info);
                break;
            }
            case PLDM_BIOS_ENUMERATION:
            case PLDM_BIOS_ENUMERATION_READ_ONLY:
            {
                auto [possibleValueHandles, defaultIndices] =
                    table::attribute::decodeEnumEntry(entry);
                pldm_bios_table_attr_entry_enum_info info = {
                    header.stringHandle,
                    header.attrType == PLDM_BIOS_ENUMERATION_READ_ONLY,
                    static_cast<uint8_t>(possibleValueHandles.size()),
                    possibleValueHandles.data(),
                    static_cast<uint8_t>(defaultIndices.size()),
                    defaultIndices.data(),
                };
                table::attribute::constructEnumEntry(rewrittenAttrTable, &info);
                break;
            }
            default:
                FAIL() << "Unexpected attribute type in test table";
        }
    }
    table::appendPadAndChecksum(rewrittenAttrTable);

    biosConfig.storeTable(tableDir / "attributeTable", rewrittenAttrTable);
    EXPECT_EQ(biosConfig.checkAttributeValueTable(*attrValueTable),
              PLDM_INVALID_BIOS_ATTR_HANDLE);
    biosConfig.storeTable(tableDir / "attributeTable", *attrTable);
}

TEST_F(TestBIOSConfig, checkAttributeValueTableInvalidAttrNameHandleCoverage)
{
    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    auto stringTable = biosConfig.getBIOSTable(PLDM_BIOS_STRING_TABLE);
    auto attrTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_TABLE);
    auto attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(stringTable.has_value());
    ASSERT_TRUE(attrTable.has_value());
    ASSERT_TRUE(attrValueTable.has_value());

    Table invalidAttrTable = *attrTable;
    auto* firstAttr =
        reinterpret_cast<pldm_bios_attr_table_entry*>(invalidAttrTable.data());
    firstAttr->string_handle = htole16(0xFFFF);
    refreshTableChecksum(invalidAttrTable);

    biosConfig.storeTable(tableDir / "attributeTable", invalidAttrTable);
    EXPECT_EQ(biosConfig.checkAttributeValueTable(*attrValueTable),
              PLDM_INVALID_BIOS_ATTR_HANDLE);
    biosConfig.storeTable(tableDir / "attributeTable", *attrTable);
}

TEST_F(TestBIOSConfig,
       checkAttributeValueTableMinimalEnumMissingPossibleValueCoverage)
{
    auto jsonDir = pldm::test::makeTempDir("BIOSConfigEnumMissingJson.XXXXXX");
    auto localTableDir =
        pldm::test::makeTempDir("BIOSConfigEnumMissingTables.XXXXXX");

    MockdBusHandler dbusHandler;
    BIOSConfig biosConfig(jsonDir.c_str(), localTableDir.c_str(), &dbusHandler,
                          0, 0, nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.biosAttributes.clear();

    Table stringTable;
    const auto nameHandle = table::string::decodeHandle(
        table::string::constructEntry(stringTable, "EnumAttr"));
    table::appendPadAndChecksum(stringTable);
    biosConfig.storeTable(localTableDir / "stringTable", stringTable);

    std::vector<uint16_t> possibleValues{0xFFFF};
    std::vector<uint8_t> defaultIndices{};
    pldm_bios_table_attr_entry_enum_info enumInfo = {
        nameHandle,
        false,
        static_cast<uint8_t>(possibleValues.size()),
        possibleValues.data(),
        static_cast<uint8_t>(defaultIndices.size()),
        defaultIndices.data(),
    };
    Table attrTable;
    table::attribute::constructEnumEntry(attrTable, &enumInfo);
    table::appendPadAndChecksum(attrTable);
    biosConfig.storeTable(localTableDir / "attributeTable", attrTable);

    Table attrValueTable;
    table::attribute_value::constructEnumEntry(attrValueTable, 0,
                                               PLDM_BIOS_ENUMERATION, {0});
    table::appendPadAndChecksum(attrValueTable);

    EXPECT_EQ(biosConfig.checkAttributeValueTable(attrValueTable),
              PLDM_INVALID_BIOS_ATTR_HANDLE);

    fs::remove_all(jsonDir);
    fs::remove_all(localTableDir);
}

TEST_F(TestBIOSConfig, checkAttributeValueTableEnumWithoutCurrentValuesCoverage)
{
    MockdBusHandler dbusHandler;
    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    auto stringTable = biosConfig.getBIOSTable(PLDM_BIOS_STRING_TABLE);
    auto attrTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_TABLE);
    ASSERT_TRUE(stringTable.has_value());
    ASSERT_TRUE(attrTable.has_value());

    auto* enumAttrEntry =
        findAttrEntryByName(*attrTable, *stringTable, "InbandCodeUpdate");
    ASSERT_NE(enumAttrEntry, nullptr);
    auto header = table::attribute::decodeHeader(enumAttrEntry);
    std::array<uint8_t, 1> ignoredCurrentIndices{0};

    Table attrValueTable(
        pldm_bios_table_attr_value_entry_encode_enum_length(0));
    ASSERT_EQ(pldm_bios_table_attr_value_entry_encode_enum(
                  attrValueTable.data(), attrValueTable.size(),
                  header.attrHandle, header.attrType, 0,
                  ignoredCurrentIndices.data()),
              PLDM_SUCCESS);
    table::appendPadAndChecksum(attrValueTable);

    EXPECT_EQ(biosConfig.checkAttributeValueTable(attrValueTable),
              PLDM_SUCCESS);
    ASSERT_TRUE(biosConfig.baseBIOSTableMaps.contains("InbandCodeUpdate"));
    const auto& currentValue =
        std::get<static_cast<uint8_t>(BIOSConfig::Index::currentValue)>(
            biosConfig.baseBIOSTableMaps.at("InbandCodeUpdate"));
    EXPECT_TRUE(std::holds_alternative<int64_t>(currentValue));
    EXPECT_EQ(std::get<int64_t>(currentValue), 0);
}

TEST_F(TestBIOSConfig, findAttrHandleMissingAttrTableEntryCoverage)
{
    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    auto stringTable = biosConfig.getBIOSTable(PLDM_BIOS_STRING_TABLE);
    auto attrTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_TABLE);
    ASSERT_TRUE(stringTable.has_value());
    ASSERT_TRUE(attrTable.has_value());

    Table alteredAttrTable = *attrTable;
    auto* targetEntry = const_cast<pldm_bios_attr_table_entry*>(
        findAttrEntryByName(alteredAttrTable, *stringTable, "str_example1"));
    ASSERT_NE(targetEntry, nullptr);

    BIOSStringTable biosStringTable(*stringTable);
    targetEntry->string_handle =
        htole16(biosStringTable.findHandle("str_example2"));
    refreshTableChecksum(alteredAttrTable);
    biosConfig.storeTable(tableDir / "attributeTable", alteredAttrTable);

    EXPECT_THROW(biosConfig.findAttrHandle("str_example1"),
                 std::invalid_argument);
}

TEST_F(TestBIOSConfig, removeTablesCoverage)
{
    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    ASSERT_TRUE(biosConfig.getBIOSTable(PLDM_BIOS_STRING_TABLE).has_value());
    ASSERT_TRUE(biosConfig.getBIOSTable(PLDM_BIOS_ATTR_TABLE).has_value());
    ASSERT_TRUE(biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE).has_value());

    biosConfig.removeTables();

    EXPECT_FALSE(biosConfig.getBIOSTable(PLDM_BIOS_STRING_TABLE).has_value());
    EXPECT_FALSE(biosConfig.getBIOSTable(PLDM_BIOS_ATTR_TABLE).has_value());
    EXPECT_FALSE(biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE).has_value());
}

TEST_F(TestBIOSConfig, loadCoverage)
{
    MockdBusHandler dbusHandler;
    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);

    auto tempDir = pldm::test::makeTempDir("BIOSConfigLoad.XXXXXX");

    auto validJson = tempDir / "valid.json";
    {
        std::ofstream out(validJson);
        out << R"({"entries":[{"attribute_name":"good-entry"},{"broken":true}]})";
    }

    size_t handledEntries = 0;
    biosConfig.load(validJson, [&handledEntries](const Json& entry) {
        (void)entry.at("attribute_name");
        ++handledEntries;
    });
    EXPECT_EQ(handledEntries, 1u);

    auto emptyEntriesJson = tempDir / "empty_entries.json";
    {
        std::ofstream out(emptyEntriesJson);
        out << R"({"entries":[]})";
    }
    biosConfig.load(emptyEntriesJson,
                    [&handledEntries](const Json&) { ++handledEntries; });
    EXPECT_EQ(handledEntries, 1u);

    auto malformedJson = tempDir / "malformed.json";
    {
        std::ofstream out(malformedJson);
        out << R"({"entries":[)";
    }

    biosConfig.load(malformedJson,
                    [&handledEntries](const Json&) { ++handledEntries; });
    EXPECT_EQ(handledEntries, 1u);

    fs::remove_all(tempDir);
}

TEST_F(TestBIOSConfig, checkAttributeTableAndValueCoverage)
{
    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    auto stringTable = biosConfig.getBIOSTable(PLDM_BIOS_STRING_TABLE);
    auto attrTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_TABLE);
    auto attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(stringTable.has_value());
    ASSERT_TRUE(attrTable.has_value());
    ASSERT_TRUE(attrValueTable.has_value());

    EXPECT_EQ(biosConfig.checkAttributeValueTable(Table{}), PLDM_SUCCESS);

    auto* enumAttrEntry =
        findAttrEntryByName(*attrTable, *stringTable, "CodeUpdatePolicy");
    ASSERT_NE(enumAttrEntry, nullptr);
    auto enumHeader = table::attribute::decodeHeader(enumAttrEntry);
    auto [pvHdls,
          defIndices] = table::attribute::decodeEnumEntry(enumAttrEntry);

    Table invalidEnumAttrTable;
    pvHdls[0] = 0xFFFF;
    pldm_bios_table_attr_entry_enum_info invalidEnumInfo = {
        enumHeader.stringHandle,
        enumHeader.attrType == PLDM_BIOS_ENUMERATION_READ_ONLY,
        static_cast<uint8_t>(pvHdls.size()),
        pvHdls.data(),
        static_cast<uint8_t>(defIndices.size()),
        defIndices.data(),
    };
    table::attribute::constructEnumEntry(invalidEnumAttrTable,
                                         &invalidEnumInfo);
    EXPECT_EQ(biosConfig.checkAttributeTable(invalidEnumAttrTable),
              PLDM_INVALID_BIOS_ATTR_HANDLE);
}

TEST_F(TestBIOSConfig, processBiosAttrChangeNotificationCoverage)
{
    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));
    EXPECT_CALL(dbusHandler, getService(_, _))
        .WillRepeatedly(Throw(std::runtime_error("no bios manager")));

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    const auto attrIndex = findAttrIndex(biosConfig, "str_example1");
    auto attrHandle = biosConfig.findAttrHandle("str_example1");

    auto attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(attrValueTable.has_value());
    auto* originalEntry =
        findAttrValueEntryByHandle(*attrValueTable, attrHandle);
    ASSERT_NE(originalEntry, nullptr);
    EXPECT_EQ(table::attribute_value::decodeStringEntry(originalEntry), "abc");

    biosConfig.processBiosAttrChangeNotification(
        {{"NoSuchProperty", PropertyValue{std::string("new")}}}, attrIndex);
    attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(attrValueTable.has_value());
    EXPECT_EQ(table::attribute_value::decodeStringEntry(
                  findAttrValueEntryByHandle(*attrValueTable, attrHandle)),
              "abc");

    biosConfig.removeTables();
    biosConfig.processBiosAttrChangeNotification(
        {{"Str_example1", PropertyValue{std::string("missing-string-table")}}},
        attrIndex);

    biosConfig.buildTables();
    auto stringTable = biosConfig.getBIOSTable(PLDM_BIOS_STRING_TABLE);
    auto attrTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_TABLE);
    auto attrValueTableSnapshot =
        biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(stringTable.has_value());
    ASSERT_TRUE(attrTable.has_value());
    ASSERT_TRUE(attrValueTableSnapshot.has_value());

    Table invalidAttrTable = *attrTable;
    auto* strAttrEntry = const_cast<pldm_bios_attr_table_entry*>(
        findAttrEntryByName(invalidAttrTable, *stringTable, "str_example1"));
    ASSERT_NE(strAttrEntry, nullptr);
    strAttrEntry->string_handle = htole16(0xFFFF);
    biosConfig.storeTable(tableDir / "attributeTable", invalidAttrTable);
    biosConfig.processBiosAttrChangeNotification(
        {{"Str_example1",
          PropertyValue{std::string("missing-attr-table-entry")}}},
        attrIndex);

    biosConfig.storeTable(tableDir / "attributeTable", *attrTable);
    fs::remove(tableDir / "attributeValueTable");
    biosConfig.processBiosAttrChangeNotification(
        {{"Str_example1",
          PropertyValue{std::string("missing-attr-value-table")}}},
        attrIndex);

    biosConfig.storeTable(tableDir / "attributeValueTable",
                          *attrValueTableSnapshot);
    biosConfig.processBiosAttrChangeNotification(
        {{"Str_example1", PropertyValue{true}}}, attrIndex);

    biosConfig.processBiosAttrChangeNotification(
        {{"Str_example1", PropertyValue{std::string("updated-value")}}},
        attrIndex);
    attrHandle = biosConfig.findAttrHandle("str_example1");
    attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(attrValueTable.has_value());
    auto* updatedEntry =
        findAttrValueEntryByHandle(*attrValueTable, attrHandle);
    ASSERT_NE(updatedEntry, nullptr);
    EXPECT_EQ(table::attribute_value::decodeStringEntry(updatedEntry),
              "updated-value");

    const auto enumIndex = findAttrIndex(biosConfig, "InbandCodeUpdate");
    biosConfig.processBiosAttrChangeNotification(
        {{"Policy", PropertyValue{static_cast<uint8_t>(1)}}}, enumIndex);
    auto enumHandle = biosConfig.findAttrHandle("InbandCodeUpdate");
    attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(attrValueTable.has_value());
    auto* enumEntry = findAttrValueEntryByHandle(*attrValueTable, enumHandle);
    ASSERT_NE(enumEntry, nullptr);
    EXPECT_THAT(table::attribute_value::decodeEnumEntry(enumEntry),
                ElementsAreArray(std::array<uint8_t, 1>{1}));

    const auto integerIndex = findAttrIndex(biosConfig, "VDD_AVSBUS_RAIL");
    biosConfig.processBiosAttrChangeNotification(
        {{"Rail", PropertyValue{static_cast<uint8_t>(4)}}}, integerIndex);
    auto integerHandle = biosConfig.findAttrHandle("VDD_AVSBUS_RAIL");
    attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(attrValueTable.has_value());
    auto* integerEntry =
        findAttrValueEntryByHandle(*attrValueTable, integerHandle);
    ASSERT_NE(integerEntry, nullptr);
    EXPECT_EQ(table::attribute_value::decodeIntegerEntry(integerEntry), 4u);
}

TEST_F(TestBIOSConfig,
       processBiosAttrChangeNotificationMissingValueEntryCoverage)
{
    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));
    EXPECT_CALL(dbusHandler, getService(_, _))
        .WillRepeatedly(Throw(std::runtime_error("no bios manager")));

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    const auto attrIndex = findAttrIndex(biosConfig, "str_example1");
    const auto attrHandle = biosConfig.findAttrHandle("str_example1");

    auto attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(attrValueTable.has_value());

    auto filteredAttrValueTable =
        buildAttrValueTableWithoutHandle(*attrValueTable, attrHandle);
    biosConfig.storeTable(tableDir / "attributeValueTable",
                          filteredAttrValueTable);

    biosConfig.processBiosAttrChangeNotification(
        {{"Str_example1", PropertyValue{std::string("missing-destination")}}},
        attrIndex);

    attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(attrValueTable.has_value());
    EXPECT_EQ(findAttrValueEntryByHandle(*attrValueTable, attrHandle), nullptr);
}

TEST_F(TestBIOSConfig,
       processBiosAttrChangeNotificationMissingAttrTableCoverage)
{
    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));
    EXPECT_CALL(dbusHandler, getService(_, _))
        .WillRepeatedly(Throw(std::runtime_error("no bios manager")));

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    const auto attrIndex = findAttrIndex(biosConfig, "str_example1");
    const auto attrHandle = biosConfig.findAttrHandle("str_example1");

    auto attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(attrValueTable.has_value());
    auto* beforeEntry = findAttrValueEntryByHandle(*attrValueTable, attrHandle);
    ASSERT_NE(beforeEntry, nullptr);
    EXPECT_EQ(table::attribute_value::decodeStringEntry(beforeEntry), "abc");

    fs::remove(tableDir / "attributeTable");
    biosConfig.processBiosAttrChangeNotification(
        {{"Str_example1", PropertyValue{std::string("should-not-apply")}}},
        attrIndex);

    attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(attrValueTable.has_value());
    auto* afterEntry = findAttrValueEntryByHandle(*attrValueTable, attrHandle);
    ASSERT_NE(afterEntry, nullptr);
    EXPECT_EQ(table::attribute_value::decodeStringEntry(afterEntry), "abc");
}

TEST_F(TestBIOSConfig, constructPendingAttributeCoverage)
{
    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));
    EXPECT_CALL(dbusHandler, getService(_, _))
        .WillRepeatedly(Throw(std::runtime_error("no bios manager")));
    EXPECT_CALL(
        dbusHandler,
        setDbusProperty(DBusMapping{"/xyz/abc/def",
                                    "xyz.openbmc_project.str_example1.value",
                                    "Str_example1", "string"},
                        PropertyValue{std::string("wxyz")}))
        .Times(1);
    EXPECT_CALL(dbusHandler,
                setDbusProperty(DBusMapping{"/xyz/openbmc_project/avsbus",
                                            "xyz.openbmc.AvsBus.Manager",
                                            "Rail", "uint8_t"},
                                PropertyValue{static_cast<uint8_t>(3)}))
        .Times(1);
    EXPECT_CALL(dbusHandler,
                setDbusProperty(
                    DBusMapping{"/xyz/abc/def", "xyz.openbmc.InBandCodeUpdate",
                                "Policy", "uint8_t"},
                    PropertyValue{static_cast<uint8_t>(1)}))
        .Times(1);

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    PendingAttributes pendingAttributes{
        {"missing-attribute",
         {"xyz.openbmc_project.BIOSConfig.Manager.AttributeType.String",
          std::string("skip-me")}},
        {"str_example2",
         {"xyz.openbmc_project.BIOSConfig.Manager.AttributeType.Password",
          std::string("skip-unsupported")}},
        {"InbandCodeUpdate",
         {"xyz.openbmc_project.BIOSConfig.Manager.AttributeType.Enumeration",
          std::string("NotAllowed")}},
        {"VDD_AVSBUS_RAIL",
         {"xyz.openbmc_project.BIOSConfig.Manager.AttributeType.Integer",
          static_cast<int64_t>(3)}},
        {"str_example1",
         {"xyz.openbmc_project.BIOSConfig.Manager.AttributeType.String",
          std::string("wxyz")}},
    };

    biosConfig.constructPendingAttribute(pendingAttributes);

    auto attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(attrValueTable.has_value());

    auto codeUpdateHandle = biosConfig.findAttrHandle("InbandCodeUpdate");
    auto* enumEntry =
        findAttrValueEntryByHandle(*attrValueTable, codeUpdateHandle);
    ASSERT_NE(enumEntry, nullptr);
    EXPECT_THAT(table::attribute_value::decodeEnumEntry(enumEntry),
                ElementsAreArray(std::array<uint8_t, 1>{1}));

    auto integerHandle = biosConfig.findAttrHandle("VDD_AVSBUS_RAIL");
    auto* integerEntry =
        findAttrValueEntryByHandle(*attrValueTable, integerHandle);
    ASSERT_NE(integerEntry, nullptr);
    EXPECT_EQ(table::attribute_value::decodeIntegerEntry(integerEntry), 3u);

    auto stringHandle = biosConfig.findAttrHandle("str_example1");
    auto* stringEntry =
        findAttrValueEntryByHandle(*attrValueTable, stringHandle);
    ASSERT_NE(stringEntry, nullptr);
    EXPECT_EQ(table::attribute_value::decodeStringEntry(stringEntry), "wxyz");
}

TEST_F(TestBIOSConfig, buildTablesFromExistingBaseBIOSTableCoverage)
{
    BaseBIOSTable initialTable{
        {"CodeUpdatePolicy",
         {"xyz.openbmc_project.BIOSConfig.Manager.AttributeType.Enumeration",
          true,
          "Display",
          "Description",
          "MenuPath",
          CurrentValue{std::string("Disruptive")},
          DefaultValue{std::string("Concurrent")},
          {}}},
        {"VDD_AVSBUS_RAIL",
         {"xyz.openbmc_project.BIOSConfig.Manager.AttributeType.Integer",
          false,
          "Display",
          "Description",
          "MenuPath",
          CurrentValue{int64_t(5)},
          DefaultValue{int64_t(0)},
          {}}},
        {"str_example1",
         {"xyz.openbmc_project.BIOSConfig.Manager.AttributeType.String",
          false,
          "Display",
          "Description",
          "MenuPath",
          CurrentValue{std::string("from-dbus")},
          DefaultValue{std::string("ab")},
          {}}},
    };
    BIOSConfigDbusFixture dbusFixture(initialTable);

    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));
    EXPECT_CALL(dbusHandler,
                getService(StrEq(BIOSConfigDbusFixture::objectPath),
                           StrEq(BIOSConfigDbusFixture::interfaceName)))
        .WillRepeatedly(
            Return(std::string(BIOSConfigDbusFixture::serviceName)));

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    auto attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(attrValueTable.has_value());

    auto enumHandle = biosConfig.findAttrHandle("CodeUpdatePolicy");
    auto* enumEntry = findAttrValueEntryByHandle(*attrValueTable, enumHandle);
    ASSERT_NE(enumEntry, nullptr);
    EXPECT_THAT(table::attribute_value::decodeEnumEntry(enumEntry),
                ElementsAreArray(std::array<uint8_t, 1>{1}));

    auto integerHandle = biosConfig.findAttrHandle("VDD_AVSBUS_RAIL");
    auto* integerEntry =
        findAttrValueEntryByHandle(*attrValueTable, integerHandle);
    ASSERT_NE(integerEntry, nullptr);
    EXPECT_EQ(table::attribute_value::decodeIntegerEntry(integerEntry), 5u);

    auto stringHandle = biosConfig.findAttrHandle("str_example1");
    auto* stringEntry =
        findAttrValueEntryByHandle(*attrValueTable, stringHandle);
    ASSERT_NE(stringEntry, nullptr);
    EXPECT_EQ(table::attribute_value::decodeStringEntry(stringEntry),
              "from-dbus");

    EXPECT_GE(dbusFixture.setCount, 1u);
    ASSERT_TRUE(dbusFixture.baseTable.contains("str_example1"));
    EXPECT_EQ(
        std::get<std::string>(
            std::get<static_cast<uint8_t>(BIOSConfig::Index::currentValue)>(
                dbusFixture.baseTable.at("str_example1"))),
        "from-dbus");
}

TEST_F(TestBIOSConfig,
       buildTablesFromExistingBaseBIOSTableInvalidEnumValueCoverage)
{
    BaseBIOSTable initialTable{
        {"InbandCodeUpdate",
         {"xyz.openbmc_project.BIOSConfig.Manager.AttributeType.Enumeration",
          false,
          "Display",
          "Description",
          "MenuPath",
          CurrentValue{std::string("InvalidOption")},
          DefaultValue{std::string("Allowed")},
          {}}},
    };
    BIOSConfigDbusFixture dbusFixture(initialTable);

    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));
    EXPECT_CALL(dbusHandler,
                getService(StrEq(BIOSConfigDbusFixture::objectPath),
                           StrEq(BIOSConfigDbusFixture::interfaceName)))
        .WillRepeatedly(
            Return(std::string(BIOSConfigDbusFixture::serviceName)));

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    auto stringTable = biosConfig.getBIOSTable(PLDM_BIOS_STRING_TABLE);
    auto attrTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_TABLE);
    auto attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(stringTable.has_value());
    ASSERT_TRUE(attrTable.has_value());
    ASSERT_TRUE(attrValueTable.has_value());

    BIOSStringTable biosStringTable(*stringTable);
    auto inbandHandle = biosStringTable.findHandle("InbandCodeUpdate");
    auto inbandAttrHandle = biosConfig.findAttrHandle("InbandCodeUpdate");
    auto stringAttrHandle = biosConfig.findAttrHandle("str_example1");
    EXPECT_NE(table::attribute::findByStringHandle(*attrTable, inbandHandle),
              nullptr);
    EXPECT_EQ(findAttrValueEntryByHandle(*attrValueTable, inbandAttrHandle),
              nullptr);
    EXPECT_NE(findAttrValueEntryByHandle(*attrValueTable, stringAttrHandle),
              nullptr);
}

TEST_F(TestBIOSConfig, buildTablesWithEmptyJsonDirCoverage)
{
    auto jsonDir = pldm::test::makeTempDir("BIOSConfigEmptyJson.XXXXXX");
    auto localTableDir =
        pldm::test::makeTempDir("BIOSConfigEmptyTables.XXXXXX");

    MockdBusHandler dbusHandler;
    BIOSConfig biosConfig(jsonDir.c_str(), localTableDir.c_str(), &dbusHandler,
                          0, 0, nullptr, nullptr);

    EXPECT_TRUE(biosConfig.biosAttributes.empty());
    biosConfig.buildTables();
    EXPECT_FALSE(biosConfig.getBIOSTable(PLDM_BIOS_STRING_TABLE).has_value());
    EXPECT_FALSE(biosConfig.getBIOSTable(PLDM_BIOS_ATTR_TABLE).has_value());
    EXPECT_FALSE(biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE).has_value());

    Table stringTable;
    table::string::constructEntry(stringTable, "OnlyString");
    table::appendPadAndChecksum(stringTable);
    biosConfig.buildAndStoreAttrTables(stringTable);
    EXPECT_FALSE(biosConfig.getBIOSTable(PLDM_BIOS_ATTR_TABLE).has_value());
    EXPECT_FALSE(biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE).has_value());

    std::array<uint8_t, sizeof(pldm_bios_attr_val_table_entry)> entry{};
    EXPECT_EQ(biosConfig.setAttrValue(entry.data(), entry.size()),
              PLDM_BIOS_TABLE_UNAVAILABLE);

    fs::remove_all(jsonDir);
    fs::remove_all(localTableDir);
}

TEST_F(TestBIOSConfig, constructAttributeCoverage)
{
    MockdBusHandler dbusHandler;
    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);

    auto stringEntry = findJsonEntry("str_example1");
    auto integerEntry = findJsonEntry("VDD_AVSBUS_RAIL");
    auto enumEntry = findJsonEntry("InbandCodeUpdate");
    ASSERT_TRUE(stringEntry.has_value());
    ASSERT_TRUE(integerEntry.has_value());
    ASSERT_TRUE(enumEntry.has_value());

    auto stringNoDbusEntry = *stringEntry;
    stringNoDbusEntry.erase("dbus");
    auto integerNoDbusEntry = *integerEntry;
    integerNoDbusEntry.erase("dbus");
    auto enumNoDbusEntry = *enumEntry;
    enumNoDbusEntry.erase("dbus");

    biosConfig.biosAttributes.clear();
    biosConfig.biosAttrMatch.clear();

    biosConfig.constructAttribute<BIOSStringAttribute>(stringNoDbusEntry);
    EXPECT_EQ(biosConfig.biosAttributes.size(), 1u);
    EXPECT_TRUE(biosConfig.biosAttrMatch.empty());

    biosConfig.constructAttribute<BIOSIntegerAttribute>(integerNoDbusEntry);
    EXPECT_EQ(biosConfig.biosAttributes.size(), 2u);
    EXPECT_TRUE(biosConfig.biosAttrMatch.empty());

    biosConfig.constructAttribute<BIOSEnumAttribute>(enumNoDbusEntry);
    EXPECT_EQ(biosConfig.biosAttributes.size(), 3u);
    EXPECT_TRUE(biosConfig.biosAttrMatch.empty());

    Json invalidEntry = {{"readOnly", false}};
    biosConfig.constructAttribute<BIOSStringAttribute>(invalidEntry);
    biosConfig.constructAttribute<BIOSIntegerAttribute>(invalidEntry);
    biosConfig.constructAttribute<BIOSEnumAttribute>(invalidEntry);
    EXPECT_EQ(biosConfig.biosAttributes.size(), 3u);
}

TEST_F(TestBIOSConfig, getBIOSTableUnknownTypeAndNoOpUpdateCoverage)
{
    MockdBusHandler dbusHandler;
    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);

    biosConfig.removeTables();
    biosConfig.buildTables();

    EXPECT_FALSE(
        biosConfig.getBIOSTable(static_cast<pldm_bios_table_types>(0xFF))
            .has_value());

    biosConfig.baseBIOSTableMaps.clear();
    EXPECT_NO_THROW(biosConfig.updateBaseBIOSTableProperty());
}

TEST_F(TestBIOSConfig, checkAttributeValueTableWithoutBiosAttributesCoverage)
{
    auto jsonDir = pldm::test::makeTempDir("BIOSConfigNoAttrsJson.XXXXXX");
    auto localTableDir =
        pldm::test::makeTempDir("BIOSConfigNoAttrsTables.XXXXXX");

    MockdBusHandler dbusHandler;
    BIOSConfig donorConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                           nullptr, nullptr);
    donorConfig.removeTables();
    donorConfig.buildTables();

    auto stringTable = donorConfig.getBIOSTable(PLDM_BIOS_STRING_TABLE);
    auto attrTable = donorConfig.getBIOSTable(PLDM_BIOS_ATTR_TABLE);
    auto attrValueTable = donorConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(stringTable.has_value());
    ASSERT_TRUE(attrTable.has_value());
    ASSERT_TRUE(attrValueTable.has_value());

    BIOSConfig emptyConfig(jsonDir.c_str(), localTableDir.c_str(), &dbusHandler,
                           0, 0, nullptr, nullptr);
    emptyConfig.removeTables();
    emptyConfig.biosAttributes.clear();
    emptyConfig.storeTable(localTableDir / "stringTable", *stringTable);
    emptyConfig.storeTable(localTableDir / "attributeTable", *attrTable);

    EXPECT_EQ(emptyConfig.checkAttributeValueTable(*attrValueTable),
              PLDM_SUCCESS);
    EXPECT_FALSE(emptyConfig.baseBIOSTableMaps.empty());

    fs::remove_all(jsonDir);
    fs::remove_all(localTableDir);
}

TEST_F(TestBIOSConfig, buildTablesMissingServiceCoverage)
{
    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));
    EXPECT_CALL(dbusHandler, getService(_, _))
        .WillRepeatedly(
            Return(std::string("xyz.openbmc_project.BIOSConfig.Missing")));

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    auto attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(attrValueTable.has_value());

    auto attrHandle = biosConfig.findAttrHandle("str_example1");
    auto* stringEntry = findAttrValueEntryByHandle(*attrValueTable, attrHandle);
    ASSERT_NE(stringEntry, nullptr);
    EXPECT_EQ(table::attribute_value::decodeStringEntry(stringEntry), "abc");
}

TEST_F(TestBIOSConfig, constructPendingAttributeSetAttrValueFailureCoverage)
{
    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));
    EXPECT_CALL(dbusHandler, getService(_, _))
        .WillRepeatedly(Throw(std::runtime_error("no bios manager")));

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    auto attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(attrValueTable.has_value());
    auto attrHandle = biosConfig.findAttrHandle("str_example1");
    auto* beforeEntry = findAttrValueEntryByHandle(*attrValueTable, attrHandle);
    ASSERT_NE(beforeEntry, nullptr);
    EXPECT_EQ(table::attribute_value::decodeStringEntry(beforeEntry), "abc");

    PendingAttributes pendingAttributes{
        {"str_example1",
         {"xyz.openbmc_project.BIOSConfig.Manager.AttributeType.String",
          std::string("")}},
    };
    biosConfig.constructPendingAttribute(pendingAttributes);

    attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(attrValueTable.has_value());
    auto* afterEntry = findAttrValueEntryByHandle(*attrValueTable, attrHandle);
    ASSERT_NE(afterEntry, nullptr);
    EXPECT_EQ(table::attribute_value::decodeStringEntry(afterEntry), "abc");
}

TEST_F(TestBIOSConfig, biosAttributeSignalCoverage)
{
    BIOSConfigDbusFixture dbusFixture({});

    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));
    EXPECT_CALL(dbusHandler,
                getService(StrEq(BIOSConfigDbusFixture::objectPath),
                           StrEq(BIOSConfigDbusFixture::interfaceName)))
        .WillRepeatedly(
            Return(std::string(BIOSConfigDbusFixture::serviceName)));

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    ASSERT_TRUE(dbusFixture.setStrExample1Property("signal-update"));
    drainDbusSignals();

    auto attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(attrValueTable.has_value());
    auto attrHandle = biosConfig.findAttrHandle("str_example1");
    auto* stringEntry = findAttrValueEntryByHandle(*attrValueTable, attrHandle);
    ASSERT_NE(stringEntry, nullptr);
    EXPECT_EQ(table::attribute_value::decodeStringEntry(stringEntry),
              "signal-update");
}

TEST_F(TestBIOSConfig, biosAttributeIntegerSignalCoverage)
{
    BIOSConfigDbusFixture dbusFixture({});

    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));
    EXPECT_CALL(dbusHandler,
                getService(StrEq(BIOSConfigDbusFixture::objectPath),
                           StrEq(BIOSConfigDbusFixture::interfaceName)))
        .WillRepeatedly(
            Return(std::string(BIOSConfigDbusFixture::serviceName)));

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    ASSERT_TRUE(dbusFixture.setAvsRailProperty(6));
    drainDbusSignals();

    auto attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(attrValueTable.has_value());
    auto attrHandle = biosConfig.findAttrHandle("VDD_AVSBUS_RAIL");
    auto* integerEntry =
        findAttrValueEntryByHandle(*attrValueTable, attrHandle);
    ASSERT_NE(integerEntry, nullptr);
    EXPECT_EQ(table::attribute_value::decodeIntegerEntry(integerEntry), 6u);
}

TEST_F(TestBIOSConfig, biosAttributeEnumSignalCoverage)
{
    BIOSConfigDbusFixture dbusFixture({});

    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));
    EXPECT_CALL(dbusHandler,
                getService(StrEq(BIOSConfigDbusFixture::objectPath),
                           StrEq(BIOSConfigDbusFixture::interfaceName)))
        .WillRepeatedly(
            Return(std::string(BIOSConfigDbusFixture::serviceName)));

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    ASSERT_TRUE(dbusFixture.setInbandCodeUpdatePolicy(1));
    drainDbusSignals();

    auto attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(attrValueTable.has_value());
    auto attrHandle = biosConfig.findAttrHandle("InbandCodeUpdate");
    auto* enumEntry = findAttrValueEntryByHandle(*attrValueTable, attrHandle);
    ASSERT_NE(enumEntry, nullptr);
    EXPECT_THAT(table::attribute_value::decodeEnumEntry(enumEntry),
                ElementsAreArray(std::array<uint8_t, 1>{1}));
}

TEST_F(TestBIOSConfig, pendingAttributesSignalCoverage)
{
    BIOSConfigDbusFixture dbusFixture({});

    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));
    EXPECT_CALL(dbusHandler,
                getService(StrEq(BIOSConfigDbusFixture::objectPath),
                           StrEq(BIOSConfigDbusFixture::interfaceName)))
        .WillRepeatedly(
            Return(std::string(BIOSConfigDbusFixture::serviceName)));
    EXPECT_CALL(
        dbusHandler,
        setDbusProperty(DBusMapping{"/xyz/abc/def",
                                    "xyz.openbmc_project.str_example1.value",
                                    "Str_example1", "string"},
                        PropertyValue{std::string("from-pending-signal")}))
        .Times(1);

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    PendingAttributes pendingAttributes{
        {"str_example1",
         {"xyz.openbmc_project.BIOSConfig.Manager.AttributeType.String",
          std::string("from-pending-signal")}},
    };

    ASSERT_TRUE(dbusFixture.setPendingAttributesProperty(pendingAttributes));
    drainDbusSignals();

    auto attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(attrValueTable.has_value());
    auto attrHandle = biosConfig.findAttrHandle("str_example1");
    auto* stringEntry = findAttrValueEntryByHandle(*attrValueTable, attrHandle);
    ASSERT_NE(stringEntry, nullptr);
    EXPECT_EQ(table::attribute_value::decodeStringEntry(stringEntry),
              "from-pending-signal");
    EXPECT_EQ(dbusFixture.pendingSetCount, 1u);
}

TEST_F(TestBIOSConfig, pendingAttributesSignalIgnoresUnrelatedPropertyCoverage)
{
    BIOSConfigDbusFixture dbusFixture({});

    MockdBusHandler dbusHandler;
    ON_CALL(dbusHandler, getDbusPropertyVariant(_, _, _))
        .WillByDefault(Throw(std::exception()));
    EXPECT_CALL(dbusHandler,
                getService(StrEq(BIOSConfigDbusFixture::objectPath),
                           StrEq(BIOSConfigDbusFixture::interfaceName)))
        .WillRepeatedly(
            Return(std::string(BIOSConfigDbusFixture::serviceName)));

    BIOSConfig biosConfig("./bios_jsons", tableDir.c_str(), &dbusHandler, 0, 0,
                          nullptr, nullptr);
    biosConfig.removeTables();
    biosConfig.buildTables();

    auto attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(attrValueTable.has_value());
    auto attrHandle = biosConfig.findAttrHandle("str_example1");
    auto* beforeEntry = findAttrValueEntryByHandle(*attrValueTable, attrHandle);
    ASSERT_NE(beforeEntry, nullptr);
    auto beforeValue = table::attribute_value::decodeStringEntry(beforeEntry);

    ASSERT_TRUE(dbusFixture.iface->set_property("BaseBIOSTable",
                                                dbusFixture.baseTable));
    drainDbusSignals();

    attrValueTable = biosConfig.getBIOSTable(PLDM_BIOS_ATTR_VAL_TABLE);
    ASSERT_TRUE(attrValueTable.has_value());
    auto* afterEntry = findAttrValueEntryByHandle(*attrValueTable, attrHandle);
    ASSERT_NE(afterEntry, nullptr);
    EXPECT_EQ(table::attribute_value::decodeStringEntry(afterEntry),
              beforeValue);
}
