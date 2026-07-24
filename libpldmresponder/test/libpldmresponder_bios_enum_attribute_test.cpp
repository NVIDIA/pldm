#include "common/test/mocked_utils.hpp"
#include "libpldmresponder/bios_enum_attribute.hpp"
#include "mocked_bios.hpp"

#include <nlohmann/json.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pldm::responder::bios;
using namespace pldm::utils;

using ::testing::_;
using ::testing::ElementsAreArray;
using ::testing::Return;
using ::testing::StrEq;
using ::testing::Throw;

class TestBIOSEnumAttribute : public ::testing::Test
{
  public:
    const auto& getPossibleValues(const BIOSEnumAttribute& attribute)
    {
        return attribute.possibleValues;
    }

    const auto& getDefaultValue(const BIOSEnumAttribute& attribute)
    {
        return attribute.defaultValue;
    }

    uint8_t getValueIndex(BIOSEnumAttribute& attribute,
                          const std::string& value,
                          const std::vector<std::string>& possibleValues)
    {
        return attribute.getValueIndex(value, possibleValues);
    }

    uint8_t getAttrValueIndex(BIOSEnumAttribute& attribute,
                              const PropertyValue& propertyValue)
    {
        return attribute.getAttrValueIndex(propertyValue);
    }

    uint8_t getAttrValueIndex(BIOSEnumAttribute& attribute)
    {
        return attribute.getAttrValueIndex();
    }
};

TEST_F(TestBIOSEnumAttribute, CtorTest)
{
    auto jsonEnumReadOnly = R"({
         "attribute_name" : "CodeUpdatePolicy",
         "possible_values" : [ "Concurrent", "Disruptive" ],
         "default_values" : [ "Concurrent" ],
         "readOnly" : true,
         "helpText" : "HelpText",
         "displayName" : "DisplayName"
      })"_json;

    BIOSEnumAttribute enumReadOnly{jsonEnumReadOnly, nullptr};
    EXPECT_EQ(enumReadOnly.name, "CodeUpdatePolicy");
    EXPECT_TRUE(enumReadOnly.readOnly);
    EXPECT_THAT(getPossibleValues(enumReadOnly),
                ElementsAreArray({"Concurrent", "Disruptive"}));
    EXPECT_EQ(getDefaultValue(enumReadOnly), "Concurrent");

    auto jsonEnumReadOnlyError = R"({
         "attribute_name" : "CodeUpdatePolicy",
         "possible_value" : [ "Concurrent", "Disruptive" ],
         "default_values" : [ "Concurrent" ],
         "readOnly" : true,
         "helpText" : "HelpText",
         "displayName" : "DisplayName"
      })"_json; // possible_value -> possible_values
    EXPECT_THROW((BIOSEnumAttribute{jsonEnumReadOnlyError, nullptr}),
                 Json::exception);

    auto jsonEnumReadWrite = R"({
         "attribute_name" : "FWBootSide",
         "possible_values" : [ "Perm", "Temp" ],
         "default_values" : [ "Perm" ],
         "readOnly" : false,
         "helpText" : "HelpText",
         "displayName" : "DisplayName",
         "dbus":
            {
               "object_path" : "/xyz/abc/def",
               "interface" : "xyz.openbmc.FWBoot.Side",
               "property_name" : "Side",
               "property_type" : "bool",
               "property_values" : [true, false]
            }
      })"_json;

    BIOSEnumAttribute enumReadWrite{jsonEnumReadWrite, nullptr};
    EXPECT_EQ(enumReadWrite.name, "FWBootSide");
    EXPECT_TRUE(!enumReadWrite.readOnly);
}

TEST_F(TestBIOSEnumAttribute, ConstructEntry)
{
    MockBIOSStringTable biosStringTable;
    MockdBusHandler dbusHandler;

    auto jsonEnumReadOnly = R"({
         "attribute_name" : "CodeUpdatePolicy",
         "possible_values" : [ "Concurrent", "Disruptive" ],
         "default_values" : [ "Disruptive" ],
         "readOnly" : true,
         "helpText" : "HelpText",
         "displayName" : "DisplayName"
      })"_json;

    std::vector<uint8_t> expectedAttrEntry{
        0,    0, /* attr handle */
        0x80,    /* attr type enum read-only*/
        4,    0, /* attr name handle */
        2,       /* number of possible value */
        2,    0, /* possible value handle */
        3,    0, /* possible value handle */
        1,       /* number of default value */
        1        /* defaut value string handle index */
    };

    std::vector<uint8_t> expectedAttrValueEntry{
        0, 0, /* attr handle */
        0x80, /* attr type enum read-only*/
        1,    /* number of current value */
        1     /* current value string handle index */
    };

    BIOSEnumAttribute enumReadOnly{jsonEnumReadOnly, nullptr};

    ON_CALL(biosStringTable, findHandle(StrEq("Concurrent")))
        .WillByDefault(Return(2));
    ON_CALL(biosStringTable, findHandle(StrEq("Disruptive")))
        .WillByDefault(Return(3));
    ON_CALL(biosStringTable, findHandle(StrEq("CodeUpdatePolicy")))
        .WillByDefault(Return(4));

    checkConstructEntry(enumReadOnly, biosStringTable, expectedAttrEntry,
                        expectedAttrValueEntry);

    auto jsonEnumReadWrite = R"({
         "attribute_name" : "CodeUpdatePolicy",
         "possible_values" : [ "Concurrent", "Disruptive" ],
         "default_values" : [ "Disruptive" ],
         "readOnly" : false,
         "helpText" : "HelpText",
         "displayName" : "DisplayName",
         "dbus":
            {
               "object_path" : "/xyz/abc/def",
               "interface" : "xyz.openbmc.abc.def",
               "property_name" : "Policy",
               "property_type" : "bool",
               "property_values" : [true, false]
          }
      })"_json;

    BIOSEnumAttribute enumReadWrite{jsonEnumReadWrite, &dbusHandler};

    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(StrEq("/xyz/abc/def"), StrEq("Policy"),
                                       StrEq("xyz.openbmc.abc.def")))
        .WillOnce(Throw(std::exception()));

    /* Set expected attr type to read-write */
    expectedAttrEntry[2] = PLDM_BIOS_ENUMERATION;
    expectedAttrValueEntry[2] = PLDM_BIOS_ENUMERATION;

    checkConstructEntry(enumReadWrite, biosStringTable, expectedAttrEntry,
                        expectedAttrValueEntry);

    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(StrEq("/xyz/abc/def"), StrEq("Policy"),
                                       StrEq("xyz.openbmc.abc.def")))
        .WillOnce(Return(PropertyValue(true)));

    expectedAttrValueEntry = {
        0, 0, /* attr handle */
        0,    /* attr type enum read-write*/
        1,    /* number of current value */
        0     /* current value string handle index */
    };

    checkConstructEntry(enumReadWrite, biosStringTable, expectedAttrEntry,
                        expectedAttrValueEntry);
}

TEST_F(TestBIOSEnumAttribute, setAttrValueOnDbus)
{
    MockBIOSStringTable biosStringTable;
    MockdBusHandler dbusHandler;

    auto jsonEnumReadWrite = R"({
         "attribute_name" : "CodeUpdatePolicy",
         "possible_values" : [ "Concurrent", "Disruptive" ],
         "default_values" : [ "Disruptive" ],
         "readOnly" : false,
         "helpText" : "HelpText",
         "displayName" : "DisplayName",
         "dbus":
            {
               "object_path" : "/xyz/abc/def",
               "interface" : "xyz.openbmc.abc.def",
               "property_name" : "Policy",
               "property_type" : "bool",
               "property_values" : [true, false]
          }
      })"_json;
    DBusMapping dbusMapping{"/xyz/abc/def", "xyz.openbmc.abc.def", "Policy",
                            "bool"};

    BIOSEnumAttribute enumReadWrite{jsonEnumReadWrite, &dbusHandler};

    std::vector<uint8_t> attrEntry{
        0, 0, /* attr handle */
        0,    /* attr type enum read-only*/
        4, 0, /* attr name handle */
        2,    /* number of possible value */
        2, 0, /* possible value handle */
        3, 0, /* possible value handle */
        1,    /* number of default value */
        1     /* defaut value string handle index */
    };

    ON_CALL(biosStringTable, findString(2))
        .WillByDefault(Return(std::string("Concurrent")));
    ON_CALL(biosStringTable, findString(3))
        .WillByDefault(Return(std::string("Disruptive")));

    std::vector<uint8_t> attrValueEntry{
        0, 0, /* attr handle */
        0,    /* attr type enum read-only*/
        1,    /* number of current value */
        0     /* current value string handle index */
    };

    EXPECT_CALL(dbusHandler,
                setDbusProperty(dbusMapping, PropertyValue{bool(true)}))
        .Times(1);
    enumReadWrite.setAttrValueOnDbus(
        reinterpret_cast<pldm_bios_attr_val_table_entry*>(
            attrValueEntry.data()),
        reinterpret_cast<pldm_bios_attr_table_entry*>(attrEntry.data()),
        biosStringTable);
}

TEST_F(TestBIOSEnumAttribute, buildValMapCoversAllSupportedDbusTypes)
{
    struct TestCase
    {
        const char* propertyType;
        Json propertyValues;
        PropertyValue dbusValue;
    };

    const std::array<TestCase, 10> testCases{{
        {"uint8_t", Json::array({0, 1}),
         PropertyValue{static_cast<uint8_t>(1)}},
        {"uint16_t", Json::array({0, 1}),
         PropertyValue{static_cast<uint16_t>(1)}},
        {"uint32_t", Json::array({0, 1}),
         PropertyValue{static_cast<uint32_t>(1)}},
        {"uint64_t", Json::array({0, 1}),
         PropertyValue{static_cast<uint64_t>(1)}},
        {"int16_t", Json::array({-1, 7}),
         PropertyValue{static_cast<int16_t>(7)}},
        {"int32_t", Json::array({-1, 7}),
         PropertyValue{static_cast<int32_t>(7)}},
        {"int64_t", Json::array({-1, 7}),
         PropertyValue{static_cast<int64_t>(7)}},
        {"bool", Json::array({false, true}), PropertyValue{true}},
        {"double", Json::array({0.0, 1.5}), PropertyValue{1.5}},
        {"string", Json::array({"Disabled", "Enabled"}),
         PropertyValue{std::string("Enabled")}},
    }};

    for (const auto& testCase : testCases)
    {
        SCOPED_TRACE(testCase.propertyType);

        auto jsonEnumReadWrite = Json{
            {"attribute_name", "CodeUpdatePolicy"},
            {"possible_values", Json::array({"Concurrent", "Disruptive"})},
            {"default_values", Json::array({"Concurrent"})},
            {"readOnly", false},
            {"helpText", "HelpText"},
            {"displayName", "DisplayName"},
            {"dbus",
             {{"object_path", "/xyz/abc/def"},
              {"interface", "xyz.openbmc.abc.def"},
              {"property_name", "Policy"},
              {"property_type", testCase.propertyType},
              {"property_values", testCase.propertyValues}}}};

        MockdBusHandler dbusHandler;
        EXPECT_CALL(dbusHandler, getDbusPropertyVariant(
                                     StrEq("/xyz/abc/def"), StrEq("Policy"),
                                     StrEq("xyz.openbmc.abc.def")))
            .WillOnce(Return(testCase.dbusValue));

        BIOSEnumAttribute enumReadWrite{jsonEnumReadWrite, &dbusHandler};
        EXPECT_EQ(getAttrValueIndex(enumReadWrite), 1);
    }
}

TEST_F(TestBIOSEnumAttribute, unsupportedDbusTypeThrows)
{
    auto jsonEnumReadWrite = R"({
         "attribute_name" : "CodeUpdatePolicy",
         "possible_values" : [ "Concurrent", "Disruptive" ],
         "default_values" : [ "Concurrent" ],
         "readOnly" : false,
         "helpText" : "HelpText",
         "displayName" : "DisplayName",
         "dbus":
            {
               "object_path" : "/xyz/abc/def",
               "interface" : "xyz.openbmc.abc.def",
               "property_name" : "Policy",
               "property_type" : "array",
               "property_values" : [0, 1]
          }
      })"_json;

    MockdBusHandler dbusHandler;
    EXPECT_THROW((BIOSEnumAttribute{jsonEnumReadWrite, &dbusHandler}),
                 std::invalid_argument);
}

TEST_F(TestBIOSEnumAttribute, emptyPossibleValuesAndDefaultValueCoverage)
{
    auto jsonEnumReadOnly = R"({
         "attribute_name" : "CodeUpdatePolicy",
         "possible_values" : [],
         "default_values" : [ "Concurrent" ],
         "readOnly" : true,
         "helpText" : "HelpText",
         "displayName" : "DisplayName"
      })"_json;

    BIOSEnumAttribute attr{jsonEnumReadOnly, nullptr};
    EXPECT_TRUE(getPossibleValues(attr).empty());
    EXPECT_EQ(getDefaultValue(attr), "Concurrent");

    auto missingDefaultJson = R"({
         "attribute_name" : "CodeUpdatePolicy",
         "possible_values" : [ "Concurrent" ],
         "default_values" : [],
         "readOnly" : true,
         "helpText" : "HelpText",
         "displayName" : "DisplayName"
      })"_json;
    EXPECT_DEATH((BIOSEnumAttribute{missingDefaultJson, nullptr}), "");
}

TEST_F(TestBIOSEnumAttribute, helperCoverage)
{
    auto jsonEnumReadWrite = R"({
         "attribute_name" : "CodeUpdatePolicy",
         "possible_values" : [ "Concurrent", "Disruptive" ],
         "default_values" : [ "Concurrent" ],
         "readOnly" : false,
         "helpText" : "HelpText",
         "displayName" : "DisplayName",
         "dbus":
            {
               "object_path" : "/xyz/abc/def",
               "interface" : "xyz.openbmc.abc.def",
               "property_name" : "Policy",
               "property_type" : "string",
               "property_values" : [ "Enabled", "Disabled" ]
          }
      })"_json;

    MockdBusHandler dbusHandler;
    BIOSEnumAttribute enumReadWrite{jsonEnumReadWrite, &dbusHandler};

    EXPECT_EQ(getAttrValueIndex(enumReadWrite,
                                PropertyValue{std::string("Disruptive")}),
              1);
    EXPECT_EQ(getAttrValueIndex(enumReadWrite,
                                PropertyValue{static_cast<uint8_t>(1)}),
              0);

    Table attrValueEntry;
    EXPECT_EQ(
        enumReadWrite.updateAttrVal(attrValueEntry, 0x21, PLDM_BIOS_ENUMERATION,
                                    PropertyValue{std::string("Disabled")}),
        PLDM_SUCCESS);
    auto* updatedEntry =
        reinterpret_cast<const pldm_bios_attr_val_table_entry*>(
            attrValueEntry.data());
    auto [attrHandle,
          attrType] = table::attribute_value::decodeHeader(updatedEntry);
    EXPECT_EQ(attrHandle, 0x21);
    EXPECT_EQ(attrType, PLDM_BIOS_ENUMERATION);
    EXPECT_THAT(table::attribute_value::decodeEnumEntry(updatedEntry),
                ElementsAreArray(std::array<uint8_t, 1>{1}));

    Table noUpdate;
    EXPECT_EQ(
        enumReadWrite.updateAttrVal(noUpdate, 0x21, PLDM_BIOS_ENUMERATION,
                                    PropertyValue{std::string("Missing")}),
        PLDM_ERROR);

    Table generatedEntry;
    enumReadWrite.generateAttributeEntry(std::string("Disruptive"),
                                         generatedEntry);
    auto* generatedAttrValue =
        reinterpret_cast<const pldm_bios_attr_val_table_entry*>(
            generatedEntry.data());
    EXPECT_EQ(generatedAttrValue->attr_type, 0);
    EXPECT_THAT(table::attribute_value::decodeEnumEntry(generatedAttrValue),
                ElementsAreArray(std::array<uint8_t, 1>{1}));
}

TEST_F(TestBIOSEnumAttribute, setAttrValueOnDbusSkipsUnknownMappedValue)
{
    MockBIOSStringTable biosStringTable;
    MockdBusHandler dbusHandler;

    auto jsonEnumReadWrite = R"({
         "attribute_name" : "CodeUpdatePolicy",
         "possible_values" : [ "Concurrent", "Disruptive" ],
         "default_values" : [ "Concurrent" ],
         "readOnly" : false,
         "helpText" : "HelpText",
         "displayName" : "DisplayName",
         "dbus":
            {
               "object_path" : "/xyz/abc/def",
               "interface" : "xyz.openbmc.abc.def",
               "property_name" : "Policy",
               "property_type" : "string",
               "property_values" : [ "Enabled" ]
          }
      })"_json;

    BIOSEnumAttribute enumReadWrite{jsonEnumReadWrite, &dbusHandler};

    std::vector<uint8_t> attrEntry{
        0, 0, 0, 4, 0, 2, 2, 0, 3, 0, 1, 1,
    };
    std::vector<uint8_t> attrValueEntry{
        0, 0, 0, 1, 1,
    };

    ON_CALL(biosStringTable, findString(2))
        .WillByDefault(Return(std::string("Concurrent")));
    ON_CALL(biosStringTable, findString(3))
        .WillByDefault(Return(std::string("Disruptive")));

    EXPECT_CALL(dbusHandler, setDbusProperty(_, _)).Times(0);
    enumReadWrite.setAttrValueOnDbus(
        reinterpret_cast<pldm_bios_attr_val_table_entry*>(
            attrValueEntry.data()),
        reinterpret_cast<pldm_bios_attr_table_entry*>(attrEntry.data()),
        biosStringTable);
}

TEST_F(TestBIOSEnumAttribute, setAttrValueOnDbusWithoutDbusMapCoverage)
{
    MockBIOSStringTable biosStringTable;

    auto jsonEnumReadOnly = R"({
         "attribute_name" : "CodeUpdatePolicy",
         "possible_values" : [ "Concurrent", "Disruptive" ],
         "default_values" : [ "Concurrent" ],
         "readOnly" : true,
         "helpText" : "HelpText",
         "displayName" : "DisplayName"
      })"_json;

    BIOSEnumAttribute enumReadOnly{jsonEnumReadOnly, nullptr};
    Table attrTable;
    Table attrValueTable;
    ON_CALL(biosStringTable, findHandle(StrEq("Concurrent")))
        .WillByDefault(Return(2));
    ON_CALL(biosStringTable, findHandle(StrEq("Disruptive")))
        .WillByDefault(Return(3));
    ON_CALL(biosStringTable, findHandle(StrEq("CodeUpdatePolicy")))
        .WillByDefault(Return(4));
    enumReadOnly.constructEntry(biosStringTable, attrTable, attrValueTable,
                                std::nullopt);
    EXPECT_NO_THROW(enumReadOnly.setAttrValueOnDbus(
        reinterpret_cast<const pldm_bios_attr_val_table_entry*>(
            attrValueTable.data()),
        reinterpret_cast<const pldm_bios_attr_table_entry*>(attrTable.data()),
        biosStringTable));
}

TEST_F(TestBIOSEnumAttribute, constructEntryOptionalIntegerAndVariantCoverage)
{
    MockBIOSStringTable biosStringTable;
    MockdBusHandler dbusHandler;

    auto jsonEnumReadWrite = R"({
         "attribute_name" : "CodeUpdatePolicy",
         "possible_values" : [ "Concurrent", "Disruptive" ],
         "default_values" : [ "Concurrent" ],
         "readOnly" : false,
         "helpText" : "HelpText",
         "displayName" : "DisplayName",
         "dbus":
            {
               "object_path" : "/xyz/abc/def",
               "interface" : "xyz.openbmc.abc.def",
               "property_name" : "Policy",
               "property_type" : "bool",
               "property_values" : [true, false]
          }
      })"_json;

    BIOSEnumAttribute enumReadWrite{jsonEnumReadWrite, &dbusHandler};
    ON_CALL(biosStringTable, findHandle(StrEq("Concurrent")))
        .WillByDefault(Return(2));
    ON_CALL(biosStringTable, findHandle(StrEq("Disruptive")))
        .WillByDefault(Return(3));
    ON_CALL(biosStringTable, findHandle(StrEq("CodeUpdatePolicy")))
        .WillByDefault(Return(4));

    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(StrEq("/xyz/abc/def"), StrEq("Policy"),
                                       StrEq("xyz.openbmc.abc.def")))
        .WillOnce(Return(PropertyValue(false)));

    Table attrTable;
    Table attrValueTable;
    enumReadWrite.constructEntry(
        biosStringTable, attrTable, attrValueTable,
        std::optional<std::variant<int64_t, std::string>>{int64_t(7)});
    auto* entry = reinterpret_cast<const pldm_bios_attr_val_table_entry*>(
        attrValueTable.data());
    EXPECT_THAT(table::attribute_value::decodeEnumEntry(entry),
                ElementsAreArray(std::array<uint8_t, 1>{1}));

    Table invalidGeneratedEntry;
    EXPECT_THROW(enumReadWrite.generateAttributeEntry(
                     std::variant<int64_t, std::string>{int64_t(1)},
                     invalidGeneratedEntry),
                 std::bad_variant_access);
}

TEST_F(TestBIOSEnumAttribute, getValueIndexMissingAndDbusFallbackCoverage)
{
    auto jsonEnumReadOnly = R"({
         "attribute_name" : "CodeUpdatePolicy",
         "possible_values" : [ "Concurrent", "Disruptive" ],
         "default_values" : [ "Concurrent" ],
         "readOnly" : true,
         "helpText" : "HelpText",
         "displayName" : "DisplayName"
      })"_json;

    BIOSEnumAttribute enumReadOnly{jsonEnumReadOnly, nullptr};
    EXPECT_THROW(
        getValueIndex(enumReadOnly, "Missing", getPossibleValues(enumReadOnly)),
        std::invalid_argument);

    auto jsonEnumReadWrite = R"({
         "attribute_name" : "CodeUpdatePolicy",
         "possible_values" : [ "Concurrent", "Disruptive" ],
         "default_values" : [ "Concurrent" ],
         "readOnly" : false,
         "helpText" : "HelpText",
         "displayName" : "DisplayName",
         "dbus":
            {
               "object_path" : "/xyz/abc/def",
               "interface" : "xyz.openbmc.abc.def",
               "property_name" : "Policy",
               "property_type" : "string",
               "property_values" : [ "Enabled", "Disabled" ]
          }
      })"_json;

    MockdBusHandler dbusHandler;
    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(StrEq("/xyz/abc/def"), StrEq("Policy"),
                                       StrEq("xyz.openbmc.abc.def")))
        .WillOnce(Return(PropertyValue{std::string("Unexpected")}));

    BIOSEnumAttribute enumReadWrite{jsonEnumReadWrite, &dbusHandler};
    EXPECT_EQ(getAttrValueIndex(enumReadWrite), 0);
}

TEST_F(TestBIOSEnumAttribute, ctorMultipleDefaultValuesDeathCoverage)
{
    auto jsonMultiDefault = R"({
         "attribute_name" : "CodeUpdatePolicy",
         "possible_values" : [ "Concurrent", "Disruptive" ],
         "default_values" : [ "Concurrent", "Disruptive" ],
         "readOnly" : true,
         "helpText" : "HelpText",
         "displayName" : "DisplayName"
      })"_json;

    EXPECT_DEATH((BIOSEnumAttribute{jsonMultiDefault, nullptr}), "");
}

TEST_F(TestBIOSEnumAttribute,
       setAttrValueOnDbusMultipleCurrentValuesDeathCoverage)
{
    MockBIOSStringTable biosStringTable;
    MockdBusHandler dbusHandler;

    auto jsonEnumReadWrite = R"({
         "attribute_name" : "CodeUpdatePolicy",
         "possible_values" : [ "Concurrent", "Disruptive" ],
         "default_values" : [ "Concurrent" ],
         "readOnly" : false,
         "helpText" : "HelpText",
         "displayName" : "DisplayName",
         "dbus":
            {
               "object_path" : "/xyz/abc/def",
               "interface" : "xyz.openbmc.abc.def",
               "property_name" : "Policy",
               "property_type" : "string",
               "property_values" : [ "Enabled", "Disabled" ]
          }
      })"_json;

    BIOSEnumAttribute enumReadWrite{jsonEnumReadWrite, &dbusHandler};

    ON_CALL(biosStringTable, findHandle(StrEq("Concurrent")))
        .WillByDefault(Return(2));
    ON_CALL(biosStringTable, findHandle(StrEq("Disruptive")))
        .WillByDefault(Return(3));
    ON_CALL(biosStringTable, findHandle(StrEq("CodeUpdatePolicy")))
        .WillByDefault(Return(4));

    Table attrTable;
    Table attrValueTable;
    enumReadWrite.constructEntry(
        biosStringTable, attrTable, attrValueTable,
        std::optional<std::variant<int64_t, std::string>>{
            std::string("Concurrent")});

    Table invalidAttrValueTable;
    table::attribute_value::constructEnumEntry(
        invalidAttrValueTable, 0x21, PLDM_BIOS_ENUMERATION,
        std::vector<uint8_t>{0, 1});

    EXPECT_DEATH(enumReadWrite.setAttrValueOnDbus(
                     reinterpret_cast<const pldm_bios_attr_val_table_entry*>(
                         invalidAttrValueTable.data()),
                     reinterpret_cast<const pldm_bios_attr_table_entry*>(
                         attrTable.data()),
                     biosStringTable),
                 "");
}

TEST_F(TestBIOSEnumAttribute, updateAttrValWrongVariantCoverage)
{
    MockdBusHandler dbusHandler;

    auto jsonEnumReadWrite = R"({
         "attribute_name" : "CodeUpdatePolicy",
         "possible_values" : [ "Concurrent", "Disruptive" ],
         "default_values" : [ "Concurrent" ],
         "readOnly" : false,
         "helpText" : "HelpText",
         "displayName" : "DisplayName",
         "dbus":
            {
               "object_path" : "/xyz/abc/def",
               "interface" : "xyz.openbmc.abc.def",
               "property_name" : "Policy",
               "property_type" : "string",
               "property_values" : [ "Enabled", "Disabled" ]
          }
      })"_json;

    BIOSEnumAttribute enumReadWrite{jsonEnumReadWrite, &dbusHandler};
    Table newValue;
    EXPECT_THROW(
        enumReadWrite.updateAttrVal(newValue, 0x21, PLDM_BIOS_ENUMERATION,
                                    PropertyValue{static_cast<uint8_t>(7)}),
        std::bad_variant_access);
}
