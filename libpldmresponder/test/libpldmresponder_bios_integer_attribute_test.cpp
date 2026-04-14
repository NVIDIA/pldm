#include "common/test/mocked_utils.hpp"
#include "libpldmresponder/bios_integer_attribute.hpp"
#include "mocked_bios.hpp"

#include <nlohmann/json.hpp>

#include <memory>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pldm::utils;
using namespace pldm::responder::bios;

using ::testing::_;
using ::testing::ElementsAreArray;
using ::testing::Return;
using ::testing::StrEq;
using ::testing::Throw;

class TestBIOSIntegerAttribute : public ::testing::Test
{
  public:
    const auto& getIntegerInfo(const BIOSIntegerAttribute& attribute)
    {
        return attribute.integerInfo;
    }

    uint64_t getAttrValue(BIOSIntegerAttribute& attribute,
                          const PropertyValue& value)
    {
        return attribute.getAttrValue(value);
    }

    uint64_t getAttrValue(BIOSIntegerAttribute& attribute)
    {
        return attribute.getAttrValue();
    }
};

TEST_F(TestBIOSIntegerAttribute, CtorTest)
{
    auto jsonIntegerReadOnly = R"({
         "attribute_name" : "SBE_IMAGE_MINIMUM_VALID_ECS",
         "lower_bound" : 1,
         "upper_bound" : 15,
         "scalar_increment" : 1,
         "default_value" : 2,
         "readOnly" : true,
         "helpText" : "HelpText",
         "displayName" : "DisplayName"
      })"_json;

    BIOSIntegerAttribute integerReadOnly{jsonIntegerReadOnly, nullptr};
    EXPECT_EQ(integerReadOnly.name, "SBE_IMAGE_MINIMUM_VALID_ECS");
    EXPECT_TRUE(integerReadOnly.readOnly);
    auto& integerInfo = getIntegerInfo(integerReadOnly);
    EXPECT_EQ(integerInfo.lowerBound, 1);
    EXPECT_EQ(integerInfo.upperBound, 15);
    EXPECT_EQ(integerInfo.scalarIncrement, 1);
    EXPECT_EQ(integerInfo.defaultValue, 2);

    auto jsonIntegerReadOnlyError = R"({
         "attribute_name" : "SBE_IMAGE_MINIMUM_VALID_ECS",
         "lower_bound" : 1,
         "upper_bound" : 15,
         "scalar_increment" : 1,
         "default_valu" : 2,
         "readOnly" : true,
         "helpText" : "HelpText",
         "displayName" : "DisplayName"
      })"_json; // default_valu -> default_value
    EXPECT_THROW((BIOSIntegerAttribute{jsonIntegerReadOnlyError, nullptr}),
                 Json::exception);

    auto jsonIntegerReadWrite = R"({
         "attribute_name" : "VDD_AVSBUS_RAIL",
         "lower_bound" : 0,
         "upper_bound" : 15,
         "scalar_increment" : 1,
         "default_value" : 0,
         "readOnly" : false,
         "helpText" : "HelpText",
         "displayName" : "DisplayName",
         "dbus":{
            "object_path" : "/xyz/openbmc_project/avsbus",
            "interface" : "xyz.openbmc.AvsBus.Manager",
            "property_type" : "uint8_t",
            "property_name" : "Rail"
         }
      })"_json;

    BIOSIntegerAttribute integerReadWrite{jsonIntegerReadWrite, nullptr};
    EXPECT_EQ(integerReadWrite.name, "VDD_AVSBUS_RAIL");
    EXPECT_TRUE(!integerReadWrite.readOnly);
}

TEST_F(TestBIOSIntegerAttribute, ConstructEntry)
{
    MockBIOSStringTable biosStringTable;
    MockdBusHandler dbusHandler;

    auto jsonIntegerReadOnly = R"({
         "attribute_name" : "VDD_AVSBUS_RAIL",
         "lower_bound" : 1,
         "upper_bound" : 15,
         "scalar_increment" : 1,
         "default_value" : 2,
         "readOnly" : true,
         "helpText" : "HelpText",
         "displayName" : "DisplayName"
      })"_json;

    std::vector<uint8_t> expectedAttrEntry{
        0,    0,                   /* attr handle */
        0x83,                      /* attr type integer read-only*/
        5,    0,                   /* attr name handle */
        1,    0, 0, 0, 0, 0, 0, 0, /* lower bound */
        15,   0, 0, 0, 0, 0, 0, 0, /* upper bound */
        1,    0, 0, 0,             /* scalar increment */
        2,    0, 0, 0, 0, 0, 0, 0, /* defaut value */
    };
    std::vector<uint8_t> expectedAttrValueEntry{
        0,    0,                   /* attr handle */
        0x83,                      /* attr type integer read-only*/
        2,    0, 0, 0, 0, 0, 0, 0, /* current value */
    };

    BIOSIntegerAttribute integerReadOnly{jsonIntegerReadOnly, nullptr};

    ON_CALL(biosStringTable, findHandle(StrEq("VDD_AVSBUS_RAIL")))
        .WillByDefault(Return(5));

    checkConstructEntry(integerReadOnly, biosStringTable, expectedAttrEntry,
                        expectedAttrValueEntry);

    auto jsonIntegerReadWrite = R"({
         "attribute_name" : "VDD_AVSBUS_RAIL",
         "lower_bound" : 1,
         "upper_bound" : 15,
         "scalar_increment" : 1,
         "default_value" : 2,
         "readOnly" : false,
         "helpText" : "HelpText",
         "displayName" : "DisplayName",
         "dbus":{
            "object_path" : "/xyz/openbmc_project/avsbus",
            "interface" : "xyz.openbmc.AvsBus.Manager",
            "property_type" : "uint8_t",
            "property_name" : "Rail"
         }
      })"_json;
    BIOSIntegerAttribute integerReadWrite{jsonIntegerReadWrite, &dbusHandler};

    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(StrEq("/xyz/openbmc_project/avsbus"),
                                       StrEq("Rail"),
                                       StrEq("xyz.openbmc.AvsBus.Manager")))
        .WillOnce(Throw(std::exception()));

    /* Set expected attr type to read-write */
    expectedAttrEntry[2] = PLDM_BIOS_INTEGER;
    expectedAttrValueEntry[2] = PLDM_BIOS_INTEGER;

    checkConstructEntry(integerReadWrite, biosStringTable, expectedAttrEntry,
                        expectedAttrValueEntry);

    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(StrEq("/xyz/openbmc_project/avsbus"),
                                       StrEq("Rail"),
                                       StrEq("xyz.openbmc.AvsBus.Manager")))
        .WillOnce(Return(PropertyValue(uint8_t(7))));

    expectedAttrValueEntry = {
        0, 0,                   /* attr handle */
        3,                      /* attr type integer read-write*/
        7, 0, 0, 0, 0, 0, 0, 0, /* current value */
    };

    checkConstructEntry(integerReadWrite, biosStringTable, expectedAttrEntry,
                        expectedAttrValueEntry);
}

TEST_F(TestBIOSIntegerAttribute, setAttrValueOnDbus)
{
    MockdBusHandler dbusHandler;
    MockBIOSStringTable biosStringTable;

    auto jsonIntegerReadWrite = R"({
         "attribute_name" : "VDD_AVSBUS_RAIL",
         "lower_bound" : 1,
         "upper_bound" : 15,
         "scalar_increment" : 1,
         "default_value" : 2,
         "readOnly" : false,
         "helpText" : "HelpText",
         "displayName" : "DisplayName",
         "dbus":{
            "object_path" : "/xyz/openbmc_project/avsbus",
            "interface" : "xyz.openbmc.AvsBus.Manager",
            "property_type" : "uint8_t",
            "property_name" : "Rail"
         }
      })"_json;
    BIOSIntegerAttribute integerReadWrite{jsonIntegerReadWrite, &dbusHandler};
    DBusMapping dbusMapping{"/xyz/openbmc_project/avsbus",
                            "xyz.openbmc.AvsBus.Manager", "Rail", "uint8_t"};
    std::vector<uint8_t> attrValueEntry = {
        0, 0,                   /* attr handle */
        3,                      /* attr type integer read-write*/
        7, 0, 0, 0, 0, 0, 0, 0, /* current value */
    };

    auto entry = reinterpret_cast<pldm_bios_attr_val_table_entry*>(
        attrValueEntry.data());
    EXPECT_CALL(dbusHandler,
                setDbusProperty(dbusMapping, PropertyValue{uint8_t(7)}))
        .Times(1);
    integerReadWrite.setAttrValueOnDbus(entry, nullptr, biosStringTable);
}

TEST_F(TestBIOSIntegerAttribute, updateAttrValAndGenerateAttributeEntry)
{
    MockdBusHandler dbusHandler;
    auto jsonIntegerReadWrite = R"({
         "attribute_name" : "VDD_AVSBUS_RAIL",
         "lower_bound" : 1,
         "upper_bound" : 15,
         "scalar_increment" : 1,
         "default_value" : 2,
         "readOnly" : false,
         "helpText" : "HelpText",
         "displayName" : "DisplayName",
         "dbus":{
            "object_path" : "/xyz/openbmc_project/avsbus",
            "interface" : "xyz.openbmc.AvsBus.Manager",
            "property_type" : "uint8_t",
            "property_name" : "Rail"
         }
      })"_json;
    BIOSIntegerAttribute integerReadWrite{jsonIntegerReadWrite, &dbusHandler};

    Table updatedValue;
    EXPECT_EQ(integerReadWrite.updateAttrVal(updatedValue, 7, PLDM_BIOS_INTEGER,
                                             PropertyValue{uint8_t(9)}),
              PLDM_SUCCESS);

    auto* updatedEntry =
        reinterpret_cast<const pldm_bios_attr_val_table_entry*>(
            updatedValue.data());
    auto [attrHdl,
          attrType] = table::attribute_value::decodeHeader(updatedEntry);
    EXPECT_EQ(attrHdl, 7);
    EXPECT_EQ(attrType, PLDM_BIOS_INTEGER);
    EXPECT_EQ(table::attribute_value::decodeIntegerEntry(updatedEntry), 9u);

    Table generatedValue;
    integerReadWrite.generateAttributeEntry(
        std::variant<int64_t, std::string>{int64_t(12)}, generatedValue);
    auto* generatedEntry =
        reinterpret_cast<const pldm_bios_attr_val_table_entry*>(
            generatedValue.data());
    EXPECT_EQ(generatedEntry->attr_type, PLDM_BIOS_INTEGER);
    EXPECT_EQ(table::attribute_value::decodeIntegerEntry(generatedEntry), 12u);
}

TEST_F(TestBIOSIntegerAttribute, propertyTypeSwitchCoverage)
{
    MockdBusHandler dbusHandler;
    MockBIOSStringTable biosStringTable;

    const std::vector<std::string> propertyTypes = {
        "uint8_t", "uint16_t", "int16_t", "uint32_t",
        "int32_t", "uint64_t", "int64_t", "double"};

    for (const auto& propertyType : propertyTypes)
    {
        SCOPED_TRACE(propertyType);
        auto jsonIntegerReadWrite = Json{
            {"attribute_name", "VDD_AVSBUS_RAIL"},
            {"lower_bound", 1},
            {"upper_bound", 15},
            {"scalar_increment", 1},
            {"default_value", 2},
            {"readOnly", false},
            {"helpText", "HelpText"},
            {"displayName", "DisplayName"},
            {"dbus",
             {{"object_path", "/xyz/openbmc_project/avsbus"},
              {"interface", "xyz.openbmc.AvsBus.Manager"},
              {"property_type", propertyType},
              {"property_name", "Rail"}}}};
        BIOSIntegerAttribute integerReadWrite{jsonIntegerReadWrite,
                                              &dbusHandler};

        Table attrValueTable;
        table::attribute_value::constructIntegerEntry(attrValueTable, 1,
                                                      PLDM_BIOS_INTEGER, 7);
        auto* entry = reinterpret_cast<const pldm_bios_attr_val_table_entry*>(
            attrValueTable.data());

        DBusMapping dbusMapping{"/xyz/openbmc_project/avsbus",
                                "xyz.openbmc.AvsBus.Manager", "Rail",
                                propertyType};
        EXPECT_CALL(dbusHandler, setDbusProperty(dbusMapping, _))
            .WillOnce([&propertyType](const DBusMapping&,
                                      const PropertyValue& value) {
                if (propertyType == "uint8_t")
                {
                    EXPECT_EQ(std::get<uint8_t>(value), 7);
                }
                else if (propertyType == "uint16_t")
                {
                    EXPECT_EQ(std::get<uint16_t>(value), 7);
                }
                else if (propertyType == "int16_t")
                {
                    EXPECT_EQ(std::get<int16_t>(value), 7);
                }
                else if (propertyType == "uint32_t")
                {
                    EXPECT_EQ(std::get<uint32_t>(value), 7u);
                }
                else if (propertyType == "int32_t")
                {
                    EXPECT_EQ(std::get<int32_t>(value), 7);
                }
                else if (propertyType == "uint64_t")
                {
                    EXPECT_EQ(std::get<uint64_t>(value), 7u);
                }
                else if (propertyType == "int64_t")
                {
                    EXPECT_EQ(std::get<int64_t>(value), 7);
                }
                else if (propertyType == "double")
                {
                    EXPECT_EQ(std::get<double>(value), 7.0);
                }
            });
        integerReadWrite.setAttrValueOnDbus(entry, nullptr, biosStringTable);
    }
}

TEST_F(TestBIOSIntegerAttribute, getAttrValueSwitchAndErrorCoverage)
{
    MockdBusHandler dbusHandler;

    auto makeAttr = [&dbusHandler](const std::string& propertyType) {
        auto jsonIntegerReadWrite = Json{
            {"attribute_name", "VDD_AVSBUS_RAIL"},
            {"lower_bound", 1},
            {"upper_bound", 15},
            {"scalar_increment", 1},
            {"default_value", 2},
            {"readOnly", false},
            {"helpText", "HelpText"},
            {"displayName", "DisplayName"},
            {"dbus",
             {{"object_path", "/xyz/openbmc_project/avsbus"},
              {"interface", "xyz.openbmc.AvsBus.Manager"},
              {"property_type", propertyType},
              {"property_name", "Rail"}}}};
        return BIOSIntegerAttribute{jsonIntegerReadWrite, &dbusHandler};
    };

    {
        auto attr = makeAttr("uint8_t");
        EXPECT_EQ(getAttrValue(attr, PropertyValue{uint8_t(11)}), 11u);
    }
    {
        auto attr = makeAttr("uint16_t");
        EXPECT_EQ(getAttrValue(attr, PropertyValue{uint16_t(11)}), 11u);
    }
    {
        auto attr = makeAttr("int16_t");
        EXPECT_EQ(getAttrValue(attr, PropertyValue{int16_t(11)}), 11u);
    }
    {
        auto attr = makeAttr("uint32_t");
        EXPECT_EQ(getAttrValue(attr, PropertyValue{uint32_t(11)}), 11u);
    }
    {
        auto attr = makeAttr("int32_t");
        EXPECT_EQ(getAttrValue(attr, PropertyValue{int32_t(11)}), 11u);
    }
    {
        auto attr = makeAttr("uint64_t");
        EXPECT_EQ(getAttrValue(attr, PropertyValue{uint64_t(11)}), 11u);
    }
    {
        auto attr = makeAttr("int64_t");
        EXPECT_EQ(getAttrValue(attr, PropertyValue{int64_t(11)}), 11u);
    }
    {
        auto attr = makeAttr("double");
        EXPECT_EQ(getAttrValue(attr, PropertyValue{11.0}), 11u);
    }

    auto unsupportedAttr = makeAttr("unsupported_type");
    EXPECT_THROW(getAttrValue(unsupportedAttr, PropertyValue{uint8_t(1)}),
                 std::invalid_argument);

    MockBIOSStringTable biosStringTable;
    Table attrValueTable;
    table::attribute_value::constructIntegerEntry(attrValueTable, 1,
                                                  PLDM_BIOS_INTEGER, 7);
    auto* entry = reinterpret_cast<const pldm_bios_attr_val_table_entry*>(
        attrValueTable.data());
    EXPECT_THROW(
        unsupportedAttr.setAttrValueOnDbus(entry, nullptr, biosStringTable),
        std::invalid_argument);
}

TEST_F(TestBIOSIntegerAttribute, getAttrValueWrongVariantCoverage)
{
    MockdBusHandler dbusHandler;

    auto makeAttr = [&dbusHandler](const std::string& propertyType) {
        auto jsonIntegerReadWrite = Json{
            {"attribute_name", "VDD_AVSBUS_RAIL"},
            {"lower_bound", 1},
            {"upper_bound", 15},
            {"scalar_increment", 1},
            {"default_value", 2},
            {"readOnly", false},
            {"helpText", "HelpText"},
            {"displayName", "DisplayName"},
            {"dbus",
             {{"object_path", "/xyz/openbmc_project/avsbus"},
              {"interface", "xyz.openbmc.AvsBus.Manager"},
              {"property_type", propertyType},
              {"property_name", "Rail"}}}};
        return BIOSIntegerAttribute{jsonIntegerReadWrite, &dbusHandler};
    };

    auto uint16Attr = makeAttr("uint16_t");
    auto int16Attr = makeAttr("int16_t");
    auto uint32Attr = makeAttr("uint32_t");
    auto int32Attr = makeAttr("int32_t");
    auto uint64Attr = makeAttr("uint64_t");
    auto int64Attr = makeAttr("int64_t");
    auto doubleAttr = makeAttr("double");

    EXPECT_THROW(getAttrValue(uint16Attr, PropertyValue{uint8_t(1)}),
                 std::bad_variant_access);
    EXPECT_THROW(getAttrValue(int16Attr, PropertyValue{uint8_t(1)}),
                 std::bad_variant_access);
    EXPECT_THROW(getAttrValue(uint32Attr, PropertyValue{uint8_t(1)}),
                 std::bad_variant_access);
    EXPECT_THROW(getAttrValue(int32Attr, PropertyValue{uint8_t(1)}),
                 std::bad_variant_access);
    EXPECT_THROW(getAttrValue(uint64Attr, PropertyValue{uint8_t(1)}),
                 std::bad_variant_access);
    EXPECT_THROW(getAttrValue(int64Attr, PropertyValue{uint8_t(1)}),
                 std::bad_variant_access);
    EXPECT_THROW(
        getAttrValue(doubleAttr, PropertyValue{std::string("wrong-variant")}),
        std::bad_variant_access);
}

TEST_F(TestBIOSIntegerAttribute, invalidCtorFieldCoverage)
{
    auto jsonInvalid = R"({
         "attribute_name" : "VDD_AVSBUS_RAIL",
         "lower_bound" : 10,
         "upper_bound" : 5,
         "scalar_increment" : 1,
         "default_value" : 2,
         "readOnly" : true,
         "helpText" : "HelpText",
         "displayName" : "DisplayName"
      })"_json;
    EXPECT_THROW((BIOSIntegerAttribute{jsonInvalid, nullptr}),
                 std::invalid_argument);
}

TEST_F(TestBIOSIntegerAttribute, constructEntryOptionalValueCoverage)
{
    MockdBusHandler dbusHandler;
    MockBIOSStringTable biosStringTable;

    auto jsonIntegerReadWrite = R"({
         "attribute_name" : "VDD_AVSBUS_RAIL",
         "lower_bound" : 1,
         "upper_bound" : 15,
         "scalar_increment" : 1,
         "default_value" : 2,
         "readOnly" : false,
         "helpText" : "HelpText",
         "displayName" : "DisplayName",
         "dbus":{
            "object_path" : "/xyz/openbmc_project/avsbus",
            "interface" : "xyz.openbmc.AvsBus.Manager",
            "property_type" : "uint8_t",
            "property_name" : "Rail"
         }
      })"_json;
    BIOSIntegerAttribute integerReadWrite{jsonIntegerReadWrite, &dbusHandler};
    ON_CALL(biosStringTable, findHandle(StrEq("VDD_AVSBUS_RAIL")))
        .WillByDefault(Return(5));

    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(StrEq("/xyz/openbmc_project/avsbus"),
                                       StrEq("Rail"),
                                       StrEq("xyz.openbmc.AvsBus.Manager")))
        .WillOnce(Return(PropertyValue(uint8_t(6))));
    Table attrTableFromDbus;
    Table attrValueFromDbus;
    integerReadWrite.constructEntry(
        biosStringTable, attrTableFromDbus, attrValueFromDbus,
        std::optional<std::variant<int64_t, std::string>>{
            std::string("ignored")});
    auto* dbusEntry = reinterpret_cast<const pldm_bios_attr_val_table_entry*>(
        attrValueFromDbus.data());
    EXPECT_EQ(table::attribute_value::decodeIntegerEntry(dbusEntry), 6u);

    Table attrTableFromOpt;
    Table attrValueFromOpt;
    integerReadWrite.constructEntry(
        biosStringTable, attrTableFromOpt, attrValueFromOpt,
        std::optional<std::variant<int64_t, std::string>>{int64_t(11)});
    auto* optEntry = reinterpret_cast<const pldm_bios_attr_val_table_entry*>(
        attrValueFromOpt.data());
    EXPECT_EQ(table::attribute_value::decodeIntegerEntry(optEntry), 11u);
}

TEST_F(TestBIOSIntegerAttribute, setAttrValueOnDbusWithoutDbusMapCoverage)
{
    MockBIOSStringTable biosStringTable;

    auto jsonIntegerReadOnly = R"({
         "attribute_name" : "VDD_AVSBUS_RAIL",
         "lower_bound" : 1,
         "upper_bound" : 15,
         "scalar_increment" : 1,
         "default_value" : 2,
         "readOnly" : true,
         "helpText" : "HelpText",
         "displayName" : "DisplayName"
      })"_json;
    BIOSIntegerAttribute integerReadOnly{jsonIntegerReadOnly, nullptr};

    Table attrValueTable;
    table::attribute_value::constructIntegerEntry(attrValueTable, 1,
                                                  PLDM_BIOS_INTEGER, 7);
    auto* entry = reinterpret_cast<const pldm_bios_attr_val_table_entry*>(
        attrValueTable.data());

    EXPECT_NO_THROW(
        integerReadOnly.setAttrValueOnDbus(entry, nullptr, biosStringTable));
}

TEST_F(TestBIOSIntegerAttribute,
       getAttrValueWithoutDbusMapReturnsDefaultCoverage)
{
    auto jsonIntegerReadOnly = R"({
         "attribute_name" : "VDD_AVSBUS_RAIL",
         "lower_bound" : 1,
         "upper_bound" : 15,
         "scalar_increment" : 1,
         "default_value" : 2,
         "readOnly" : true,
         "helpText" : "HelpText",
         "displayName" : "DisplayName"
      })"_json;

    BIOSIntegerAttribute integerReadOnly{jsonIntegerReadOnly, nullptr};
    EXPECT_EQ(getAttrValue(integerReadOnly), 2u);
}

TEST_F(TestBIOSIntegerAttribute,
       constructEntryFallsBackToDefaultOnDbusExceptionCoverage)
{
    MockdBusHandler dbusHandler;
    MockBIOSStringTable biosStringTable;

    auto jsonIntegerReadWrite = R"({
         "attribute_name" : "VDD_AVSBUS_RAIL",
         "lower_bound" : 1,
         "upper_bound" : 15,
         "scalar_increment" : 1,
         "default_value" : 2,
         "readOnly" : false,
         "helpText" : "HelpText",
         "displayName" : "DisplayName",
         "dbus":{
            "object_path" : "/xyz/openbmc_project/avsbus",
            "interface" : "xyz.openbmc.AvsBus.Manager",
            "property_type" : "uint8_t",
            "property_name" : "Rail"
         }
      })"_json;
    BIOSIntegerAttribute integerReadWrite{jsonIntegerReadWrite, &dbusHandler};
    ON_CALL(biosStringTable, findHandle(StrEq("VDD_AVSBUS_RAIL")))
        .WillByDefault(Return(5));

    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(StrEq("/xyz/openbmc_project/avsbus"),
                                       StrEq("Rail"),
                                       StrEq("xyz.openbmc.AvsBus.Manager")))
        .WillOnce(Throw(std::runtime_error("dbus failure")));

    Table attrTable;
    Table attrValueTable;
    integerReadWrite.constructEntry(biosStringTable, attrTable, attrValueTable);
    auto* entry = reinterpret_cast<const pldm_bios_attr_val_table_entry*>(
        attrValueTable.data());
    EXPECT_EQ(table::attribute_value::decodeIntegerEntry(entry), 2u);
}

TEST_F(TestBIOSIntegerAttribute, generateAttributeEntryWrongVariantCoverage)
{
    MockdBusHandler dbusHandler;

    auto jsonIntegerReadWrite = R"({
         "attribute_name" : "VDD_AVSBUS_RAIL",
         "lower_bound" : 1,
         "upper_bound" : 15,
         "scalar_increment" : 1,
         "default_value" : 2,
         "readOnly" : false,
         "helpText" : "HelpText",
         "displayName" : "DisplayName",
         "dbus":{
            "object_path" : "/xyz/openbmc_project/avsbus",
            "interface" : "xyz.openbmc.AvsBus.Manager",
            "property_type" : "uint8_t",
            "property_name" : "Rail"
         }
      })"_json;
    BIOSIntegerAttribute integerReadWrite{jsonIntegerReadWrite, &dbusHandler};

    Table generatedValue;
    EXPECT_THROW(integerReadWrite.generateAttributeEntry(
                     std::variant<int64_t, std::string>{std::string("bad")},
                     generatedValue),
                 std::bad_variant_access);
}
