#include "libpldmresponder/bios_table.hpp"

#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <vector>

#include <gtest/gtest.h>

using namespace pldm::responder::bios;

class TestBIOSTable : public testing::Test
{
  public:
    void SetUp() override
    {
        auto tmpdir = (fs::current_path() / "pldm_bios_table.XXXXXX").string();
        std::vector<char> tmpdirTemplate(tmpdir.begin(), tmpdir.end());
        tmpdirTemplate.push_back('\0');
        auto* createdDir = mkdtemp(tmpdirTemplate.data());
        ASSERT_NE(createdDir, nullptr);
        dir = fs::path(createdDir);
    }

    void TearDown() override
    {
        fs::remove_all(dir);
    }

    fs::path dir;
};

TEST_F(TestBIOSTable, testStoreLoad)
{
    std::vector<uint8_t> table{10, 34, 56, 100, 44, 55, 69, 21, 48, 2, 7, 82};
    fs::path file(dir / "t1");
    BIOSTable t(file.string().c_str());
    std::vector<uint8_t> out{};

    ASSERT_THROW(t.load(out), fs::filesystem_error);

    ASSERT_EQ(true, t.isEmpty());

    t.store(table);
    t.load(out);
    ASSERT_EQ(true, std::equal(table.begin(), table.end(), out.begin()));
}

TEST_F(TestBIOSTable, testLoadOntoExisting)
{
    std::vector<uint8_t> table{10, 34, 56, 100, 44, 55, 69, 21, 48, 2, 7, 82};
    fs::path file(dir / "t1");
    BIOSTable t(file.string().c_str());
    std::vector<uint8_t> out{99, 99};

    ASSERT_THROW(t.load(out), fs::filesystem_error);

    ASSERT_EQ(true, t.isEmpty());

    t.store(table);
    t.load(out);
    ASSERT_EQ(true, std::equal(table.begin(), table.end(), out.begin() + 2));
    ASSERT_EQ(out[0], 99);
    ASSERT_EQ(out[1], 99);
}

TEST_F(TestBIOSTable, emptyFileCoverage)
{
    fs::path file(dir / "empty");
    std::ofstream(file).close();

    BIOSTable table(file.string().c_str());
    std::vector<uint8_t> out{42, 99};

    EXPECT_TRUE(table.isEmpty());
    table.load(out);
    EXPECT_EQ(out, (std::vector<uint8_t>{42, 99}));
}

TEST_F(TestBIOSTable, appendPadAndChecksumEmptyTableCoverage)
{
    Table table;

    table::appendPadAndChecksum(table);

    EXPECT_FALSE(table.empty());
    EXPECT_EQ(table.size(), pldm_bios_table_pad_checksum_size(0));
}

TEST_F(TestBIOSTable, appendPadAndChecksumNonEmptyTableCoverage)
{
    Table table{0x11, 0x22, 0x33};
    const auto originalSize = table.size();

    table::appendPadAndChecksum(table);

    EXPECT_GT(table.size(), originalSize);
    EXPECT_EQ(table[0], 0x11);
    EXPECT_EQ(table[1], 0x22);
    EXPECT_EQ(table[2], 0x33);
}

TEST_F(TestBIOSTable, stringTableLookupAndUpdateCoverage)
{
    Table stringTable;
    table::string::constructEntry(stringTable, "alpha");
    table::string::constructEntry(stringTable, "beta");
    table::appendPadAndChecksum(stringTable);

    EXPECT_TRUE(
        pldm_bios_table_checksum(stringTable.data(), stringTable.size()));

    BIOSStringTable biosStringTable(stringTable);
    auto alphaHandle = biosStringTable.findHandle("alpha");
    auto betaHandle = biosStringTable.findHandle("beta");
    EXPECT_EQ(biosStringTable.findString(alphaHandle), "alpha");
    EXPECT_EQ(biosStringTable.findString(betaHandle), "beta");
    EXPECT_THROW(biosStringTable.findString(0xFFFF), std::invalid_argument);
    EXPECT_THROW(biosStringTable.findHandle("missing"), std::invalid_argument);

    Table attrValueTable;
    table::attribute_value::constructStringEntry(attrValueTable, 1,
                                                 PLDM_BIOS_STRING, "old");
    table::appendPadAndChecksum(attrValueTable);

    Table updatedEntry;
    table::attribute_value::constructStringEntry(updatedEntry, 1,
                                                 PLDM_BIOS_STRING, "new");
    auto updatedTable = table::attribute_value::updateTable(
        attrValueTable, updatedEntry.data(), updatedEntry.size());
    ASSERT_TRUE(updatedTable.has_value());

    auto* updatedAttrEntry =
        reinterpret_cast<const pldm_bios_attr_val_table_entry*>(
            updatedTable->data());
    EXPECT_EQ(table::attribute_value::decodeStringEntry(updatedAttrEntry),
              "new");
}

TEST_F(TestBIOSTable, stringTableConstructFromBIOSTableCoverage)
{
    Table stringTable;
    auto* alphaEntry = table::string::constructEntry(stringTable, "alpha");
    auto alphaHandle = table::string::decodeHandle(alphaEntry);
    auto* betaEntry = table::string::constructEntry(stringTable, "beta");
    auto betaHandle = table::string::decodeHandle(betaEntry);
    table::appendPadAndChecksum(stringTable);

    auto tmpfile = (dir / "BIOSStringTable.XXXXXX").string();
    std::vector<char> tmpfileTemplate(tmpfile.begin(), tmpfile.end());
    tmpfileTemplate.push_back('\0');
    int fd = mkstemp(tmpfileTemplate.data());
    ASSERT_GE(fd, 0);
    close(fd);

    BIOSTable biosTable(tmpfileTemplate.data());
    biosTable.store(stringTable);
    BIOSStringTable fromFile(biosTable);
    EXPECT_EQ(fromFile.findString(alphaHandle), "alpha");
    EXPECT_EQ(fromFile.findString(betaHandle), "beta");

    fs::remove(tmpfileTemplate.data());
}

TEST_F(TestBIOSTable, stringTableConstructFromMissingBIOSTableThrows)
{
    BIOSTable biosTable((dir / "missing_table").c_str());
    EXPECT_THROW([[maybe_unused]] BIOSStringTable fromFile(biosTable),
                 fs::filesystem_error);
}

TEST_F(TestBIOSTable, updateTableMissingHandleCoverage)
{
    Table attrValueTable;
    table::attribute_value::constructStringEntry(attrValueTable, 1,
                                                 PLDM_BIOS_STRING, "old");
    table::appendPadAndChecksum(attrValueTable);

    Table updatedEntry;
    table::attribute_value::constructStringEntry(updatedEntry, 1,
                                                 PLDM_BIOS_STRING, "new");
    auto* invalidEntry =
        reinterpret_cast<pldm_bios_attr_val_table_entry*>(updatedEntry.data());
    invalidEntry->attr_type = PLDM_BIOS_INTEGER;
    EXPECT_FALSE(table::attribute_value::updateTable(
                     attrValueTable, updatedEntry.data(), updatedEntry.size())
                     .has_value());
}

TEST_F(TestBIOSTable, constructEntryFailureCoverage)
{
    Table attrTable;

    pldm_bios_table_attr_entry_string_info badStringInfo = {
        1, false, 1, 5, 4, 4, "abcd"};
    EXPECT_THROW(
        table::attribute::constructStringEntry(attrTable, &badStringInfo),
        std::runtime_error);

    pldm_bios_table_attr_entry_integer_info badIntegerInfo = {
        2, false, 10, 5, 1, 7};
    EXPECT_THROW(
        table::attribute::constructIntegerEntry(attrTable, &badIntegerInfo),
        std::runtime_error);

    Table attrValueTable;
    EXPECT_THROW(table::attribute_value::constructIntegerEntry(
                     attrValueTable, 1, PLDM_BIOS_STRING, 9),
                 std::runtime_error);
    EXPECT_THROW(table::attribute_value::constructEnumEntry(
                     attrValueTable, 1, PLDM_BIOS_STRING, {0}),
                 std::runtime_error);
}

TEST_F(TestBIOSTable, findInEmptyAttributeTableCoverage)
{
    Table attrTable;

    EXPECT_EQ(table::attribute::findByHandle(attrTable, 1), nullptr);
    EXPECT_EQ(table::attribute::findByStringHandle(attrTable, 1), nullptr);
}

TEST_F(TestBIOSTable, decodeFailureCoverage)
{
    Table attrTable;
    pldm_bios_table_attr_entry_integer_info integerInfo = {
        3, false, 0, 10, 1, 4};
    auto* integerEntry =
        table::attribute::constructIntegerEntry(attrTable, &integerInfo);
    EXPECT_THROW(
        static_cast<void>(table::attribute::decodeStringEntry(integerEntry)),
        std::runtime_error);
    EXPECT_THROW(
        static_cast<void>(table::attribute::decodeEnumEntry(integerEntry)),
        std::runtime_error);
}

TEST_F(TestBIOSTable, attributeStringRoundTripCoverage)
{
    Table stringTable;
    auto* nameEntry = table::string::constructEntry(stringTable, "StringAttr");
    auto stringHandle = table::string::decodeHandle(nameEntry);

    Table attrTable;
    pldm_bios_table_attr_entry_string_info stringInfo = {
        stringHandle, false, PLDM_BIOS_STRING, 1, 8, 4, "abcd"};
    auto* stringEntry =
        table::attribute::constructStringEntry(attrTable, &stringInfo);

    auto header = table::attribute::decodeHeader(stringEntry);
    auto field = table::attribute::decodeStringEntry(stringEntry);

    EXPECT_EQ(header.stringHandle, stringHandle);
    EXPECT_EQ(header.attrType, PLDM_BIOS_STRING);
    EXPECT_EQ(field.stringType, PLDM_BIOS_STRING);
    EXPECT_EQ(field.minLength, 1);
    EXPECT_EQ(field.maxLength, 8);
    EXPECT_EQ(field.defLength, 4);
    EXPECT_EQ(field.defString, "abcd");
    EXPECT_EQ(table::attribute::findByHandle(attrTable, header.attrHandle),
              stringEntry);
    EXPECT_EQ(table::attribute::findByStringHandle(attrTable, stringHandle),
              stringEntry);
}

TEST_F(TestBIOSTable, attributeIntegerRoundTripCoverage)
{
    Table stringTable;
    auto* nameEntry = table::string::constructEntry(stringTable, "IntegerAttr");
    auto stringHandle = table::string::decodeHandle(nameEntry);

    Table attrTable;
    pldm_bios_table_attr_entry_integer_info integerInfo = {
        stringHandle, false, 1, 20, 2, 9};
    auto* integerEntry =
        table::attribute::constructIntegerEntry(attrTable, &integerInfo);

    auto header = table::attribute::decodeHeader(integerEntry);
    auto field = table::attribute::decodeIntegerEntry(integerEntry);

    EXPECT_EQ(header.stringHandle, stringHandle);
    EXPECT_EQ(header.attrType, PLDM_BIOS_INTEGER);
    EXPECT_EQ(field.lowerBound, 1u);
    EXPECT_EQ(field.upperBound, 20u);
    EXPECT_EQ(field.scalarIncrement, 2u);
    EXPECT_EQ(field.defaultValue, 9u);
    EXPECT_EQ(table::attribute::findByHandle(attrTable, header.attrHandle),
              integerEntry);
    EXPECT_EQ(table::attribute::findByStringHandle(attrTable, stringHandle),
              integerEntry);
}

TEST_F(TestBIOSTable, attributeReadonlyStringRoundTripCoverage)
{
    Table stringTable;
    auto* nameEntry =
        table::string::constructEntry(stringTable, "ReadonlyStringAttr");
    auto stringHandle = table::string::decodeHandle(nameEntry);

    Table attrTable;
    pldm_bios_table_attr_entry_string_info stringInfo = {
        stringHandle, true, PLDM_BIOS_STRING, 1, 8, 4, "abcd"};
    auto* stringEntry =
        table::attribute::constructStringEntry(attrTable, &stringInfo);

    auto header = table::attribute::decodeHeader(stringEntry);
    EXPECT_EQ(header.attrType, PLDM_BIOS_STRING_READ_ONLY);
}

TEST_F(TestBIOSTable, attributeReadonlyIntegerRoundTripCoverage)
{
    Table stringTable;
    auto* nameEntry =
        table::string::constructEntry(stringTable, "ReadonlyIntegerAttr");
    auto stringHandle = table::string::decodeHandle(nameEntry);

    Table attrTable;
    pldm_bios_table_attr_entry_integer_info integerInfo = {
        stringHandle, true, 1, 20, 2, 9};
    auto* integerEntry =
        table::attribute::constructIntegerEntry(attrTable, &integerInfo);

    auto header = table::attribute::decodeHeader(integerEntry);
    EXPECT_EQ(header.attrType, PLDM_BIOS_INTEGER_READ_ONLY);
}

TEST_F(TestBIOSTable, attributeEnumRoundTripCoverage)
{
    Table stringTable;
    auto stringHandle = table::string::decodeHandle(
        table::string::constructEntry(stringTable, "EnumAttr"));
    std::vector<uint16_t> possibleValues{
        table::string::decodeHandle(
            table::string::constructEntry(stringTable, "Enabled")),
        table::string::decodeHandle(
            table::string::constructEntry(stringTable, "Disabled"))};
    std::vector<uint8_t> defaultIndices{1};

    Table attrTable;
    pldm_bios_table_attr_entry_enum_info enumInfo = {
        stringHandle,
        false,
        static_cast<uint8_t>(possibleValues.size()),
        possibleValues.data(),
        static_cast<uint8_t>(defaultIndices.size()),
        defaultIndices.data(),
    };
    auto* enumEntry =
        table::attribute::constructEnumEntry(attrTable, &enumInfo);

    auto header = table::attribute::decodeHeader(enumEntry);
    auto field = table::attribute::decodeEnumEntry(enumEntry);

    EXPECT_EQ(header.stringHandle, stringHandle);
    EXPECT_EQ(header.attrType, PLDM_BIOS_ENUMERATION);
    EXPECT_EQ(field.possibleValueStringHandle, possibleValues);
    EXPECT_EQ(field.defaultValueIndex, defaultIndices);
    EXPECT_EQ(table::attribute::findByHandle(attrTable, header.attrHandle),
              enumEntry);
    EXPECT_EQ(table::attribute::findByStringHandle(attrTable, stringHandle),
              enumEntry);
}

TEST_F(TestBIOSTable, attributeReadonlyEnumRoundTripCoverage)
{
    Table stringTable;
    auto stringHandle = table::string::decodeHandle(
        table::string::constructEntry(stringTable, "ReadonlyEnumAttr"));
    std::vector<uint16_t> possibleValues{
        table::string::decodeHandle(
            table::string::constructEntry(stringTable, "Enabled")),
        table::string::decodeHandle(
            table::string::constructEntry(stringTable, "Disabled"))};
    std::vector<uint8_t> defaultIndices{0};

    Table attrTable;
    pldm_bios_table_attr_entry_enum_info enumInfo = {
        stringHandle,
        true,
        static_cast<uint8_t>(possibleValues.size()),
        possibleValues.data(),
        static_cast<uint8_t>(defaultIndices.size()),
        defaultIndices.data(),
    };
    auto* enumEntry =
        table::attribute::constructEnumEntry(attrTable, &enumInfo);

    auto header = table::attribute::decodeHeader(enumEntry);
    EXPECT_EQ(header.attrType, PLDM_BIOS_ENUMERATION_READ_ONLY);
}

TEST_F(TestBIOSTable, attributeStringEmptyDefaultCoverage)
{
    Table stringTable;
    auto* nameEntry =
        table::string::constructEntry(stringTable, "EmptyStringAttr");
    auto stringHandle = table::string::decodeHandle(nameEntry);

    Table attrTable;
    pldm_bios_table_attr_entry_string_info stringInfo = {
        stringHandle, false, PLDM_BIOS_STRING, 0, 8, 0, ""};
    auto* stringEntry =
        table::attribute::constructStringEntry(attrTable, &stringInfo);

    auto field = table::attribute::decodeStringEntry(stringEntry);
    EXPECT_EQ(field.defLength, 0);
    EXPECT_TRUE(field.defString.empty());
}

TEST_F(TestBIOSTable, attributeEnumEmptyDefaultCoverage)
{
    Table stringTable;
    auto stringHandle = table::string::decodeHandle(
        table::string::constructEntry(stringTable, "EnumNoDefaultAttr"));
    std::vector<uint16_t> possibleValues{
        table::string::decodeHandle(
            table::string::constructEntry(stringTable, "Auto")),
        table::string::decodeHandle(
            table::string::constructEntry(stringTable, "Manual"))};
    std::vector<uint8_t> defaultIndices{};

    Table attrTable;
    pldm_bios_table_attr_entry_enum_info enumInfo = {
        stringHandle,
        false,
        static_cast<uint8_t>(possibleValues.size()),
        possibleValues.data(),
        static_cast<uint8_t>(defaultIndices.size()),
        defaultIndices.data(),
    };
    auto* enumEntry =
        table::attribute::constructEnumEntry(attrTable, &enumInfo);

    auto field = table::attribute::decodeEnumEntry(enumEntry);
    EXPECT_EQ(field.possibleValueStringHandle, possibleValues);
    EXPECT_TRUE(field.defaultValueIndex.empty());
}

TEST_F(TestBIOSTable, attributeEnumWithoutPossibleValuesCoverage)
{
    Table stringTable;
    auto* nameEntry =
        table::string::constructEntry(stringTable, "EnumEmptyValuesAttr");
    auto stringHandle = table::string::decodeHandle(nameEntry);
    std::vector<uint16_t> possibleValues{};
    std::vector<uint8_t> defaultIndices{};

    Table attrTable;
    pldm_bios_table_attr_entry_enum_info enumInfo = {
        stringHandle,
        false,
        static_cast<uint8_t>(possibleValues.size()),
        possibleValues.data(),
        static_cast<uint8_t>(defaultIndices.size()),
        defaultIndices.data(),
    };
    auto* enumEntry =
        table::attribute::constructEnumEntry(attrTable, &enumInfo);

    auto field = table::attribute::decodeEnumEntry(enumEntry);
    EXPECT_TRUE(field.possibleValueStringHandle.empty());
    EXPECT_TRUE(field.defaultValueIndex.empty());
}

TEST_F(TestBIOSTable, findMissingAttributeEntriesCoverage)
{
    Table stringTable;
    auto* nameEntry = table::string::constructEntry(stringTable, "MissingAttr");
    auto stringHandle = table::string::decodeHandle(nameEntry);

    Table attrTable;
    pldm_bios_table_attr_entry_string_info stringInfo = {
        stringHandle, false, PLDM_BIOS_STRING, 1, 8, 4, "abcd"};
    auto* stringEntry =
        table::attribute::constructStringEntry(attrTable, &stringInfo);
    auto handle = table::attribute::decodeHeader(stringEntry).attrHandle;

    EXPECT_EQ(table::attribute::findByHandle(attrTable, handle + 1), nullptr);
    EXPECT_EQ(table::attribute::findByStringHandle(attrTable, 0xFFFF), nullptr);
}

TEST_F(TestBIOSTable, attributeValueStringRoundTripCoverage)
{
    Table attrValueTable;
    auto* stringEntry = table::attribute_value::constructStringEntry(
        attrValueTable, 0x20, PLDM_BIOS_STRING, "runtime-value");

    auto header = table::attribute_value::decodeHeader(stringEntry);
    EXPECT_EQ(header.attrHandle, 0x20);
    EXPECT_EQ(header.attrType, PLDM_BIOS_STRING);
    EXPECT_EQ(table::attribute_value::decodeStringEntry(stringEntry),
              "runtime-value");
}

TEST_F(TestBIOSTable, attributeValueStringInvalidTypeCoverage)
{
    Table attrValueTable;

    EXPECT_THROW(table::attribute_value::constructStringEntry(
                     attrValueTable, 0x20, PLDM_BIOS_INTEGER, "runtime-value"),
                 std::runtime_error);
}

TEST_F(TestBIOSTable, attributeValueIntegerRoundTripCoverage)
{
    Table attrValueTable;
    auto* integerEntry = table::attribute_value::constructIntegerEntry(
        attrValueTable, 0x21, PLDM_BIOS_INTEGER, 42);

    auto header = table::attribute_value::decodeHeader(integerEntry);
    EXPECT_EQ(header.attrHandle, 0x21);
    EXPECT_EQ(header.attrType, PLDM_BIOS_INTEGER);
    EXPECT_EQ(table::attribute_value::decodeIntegerEntry(integerEntry), 42u);
}

TEST_F(TestBIOSTable, attributeValueEnumRoundTripCoverage)
{
    Table attrValueTable;
    std::vector<uint8_t> handleIndices{0, 2};
    auto* enumEntry = table::attribute_value::constructEnumEntry(
        attrValueTable, 0x22, PLDM_BIOS_ENUMERATION, handleIndices);

    auto header = table::attribute_value::decodeHeader(enumEntry);
    EXPECT_EQ(header.attrHandle, 0x22);
    EXPECT_EQ(header.attrType, PLDM_BIOS_ENUMERATION);
    EXPECT_EQ(table::attribute_value::decodeEnumEntry(enumEntry),
              handleIndices);
}

TEST_F(TestBIOSTable, attributeValueEmptyStringCoverage)
{
    Table attrValueTable;
    auto* stringEntry = table::attribute_value::constructStringEntry(
        attrValueTable, 0x23, PLDM_BIOS_STRING, "");

    EXPECT_TRUE(table::attribute_value::decodeStringEntry(stringEntry).empty());
}

TEST_F(TestBIOSTable, attributeValueEnumSingleIndexCoverage)
{
    Table attrValueTable;
    auto* enumEntry = table::attribute_value::constructEnumEntry(
        attrValueTable, 0x24, PLDM_BIOS_ENUMERATION, {1});

    EXPECT_EQ(table::attribute_value::decodeEnumEntry(enumEntry),
              std::vector<uint8_t>({1}));
}

TEST_F(TestBIOSTable, attributeValueEnumEmptyIndicesCoverage)
{
    Table attrValueTable;
    EXPECT_THROW(table::attribute_value::constructEnumEntry(
                     attrValueTable, 0x25, PLDM_BIOS_ENUMERATION, {}),
                 std::runtime_error);
}

TEST_F(TestBIOSTable, updateTableEmptyAndTruncatedEntryCoverage)
{
    Table updatedEntry;
    table::attribute_value::constructStringEntry(updatedEntry, 0x30,
                                                 PLDM_BIOS_STRING, "value");

    EXPECT_TRUE(table::attribute_value::updateTable(
                    Table{}, updatedEntry.data(), updatedEntry.size())
                    .has_value());

    Table attrValueTable;
    table::attribute_value::constructStringEntry(attrValueTable, 0x31,
                                                 PLDM_BIOS_STRING, "old");
    table::appendPadAndChecksum(attrValueTable);

    EXPECT_TRUE(table::attribute_value::updateTable(attrValueTable,
                                                    updatedEntry.data(), 0)
                    .has_value());
}

TEST_F(TestBIOSTable, attributeEnumMultipleDefaultIndicesCoverage)
{
    Table stringTable;
    const auto handle0 = table::string::decodeHandle(
        table::string::constructEntry(stringTable, "Enum0"));
    const auto handle1 = table::string::decodeHandle(
        table::string::constructEntry(stringTable, "Enum1"));
    const auto handle2 = table::string::decodeHandle(
        table::string::constructEntry(stringTable, "Enum2"));
    const auto nameHandle = table::string::decodeHandle(
        table::string::constructEntry(stringTable, "EnumAttr"));

    Table attrTable;
    std::array<uint16_t, 3> possibleValueHandles{handle0, handle1, handle2};
    std::array<uint8_t, 2> defaultIndices{0, 2};
    pldm_bios_table_attr_entry_enum_info enumInfo = {
        nameHandle,
        false,
        static_cast<uint8_t>(possibleValueHandles.size()),
        possibleValueHandles.data(),
        static_cast<uint8_t>(defaultIndices.size()),
        defaultIndices.data()};
    auto* enumEntry =
        table::attribute::constructEnumEntry(attrTable, &enumInfo);

    auto field = table::attribute::decodeEnumEntry(enumEntry);
    EXPECT_EQ(field.possibleValueStringHandle,
              std::vector<uint16_t>(possibleValueHandles.begin(),
                                    possibleValueHandles.end()));
    EXPECT_EQ(
        field.defaultValueIndex,
        std::vector<uint8_t>(defaultIndices.begin(), defaultIndices.end()));
}

TEST_F(TestBIOSTable, stringTableLongAndEmptyEntryCoverage)
{
    Table stringTable;
    auto* emptyEntry = table::string::constructEntry(stringTable, "");
    auto emptyHandle = table::string::decodeHandle(emptyEntry);
    std::string longName(96, 'L');
    auto* longEntry = table::string::constructEntry(stringTable, longName);
    auto longHandle = table::string::decodeHandle(longEntry);
    table::appendPadAndChecksum(stringTable);

    BIOSStringTable biosStringTable(stringTable);
    EXPECT_EQ(biosStringTable.findString(emptyHandle), "");
    EXPECT_EQ(biosStringTable.findString(longHandle), longName);
    EXPECT_EQ(biosStringTable.findHandle(""), emptyHandle);
    EXPECT_EQ(biosStringTable.findHandle(longName), longHandle);
}

TEST_F(TestBIOSTable, attributeStringLongDefaultCoverage)
{
    Table stringTable;
    auto* nameEntry =
        table::string::constructEntry(stringTable, "LongStringAttributeName");
    auto stringHandle = table::string::decodeHandle(nameEntry);

    std::string longDefault(96, 'd');
    Table attrTable;
    pldm_bios_table_attr_entry_string_info stringInfo = {
        stringHandle,
        false,
        PLDM_BIOS_STRING,
        0,
        static_cast<uint16_t>(longDefault.size()),
        static_cast<uint16_t>(longDefault.size()),
        longDefault.c_str()};
    auto* stringEntry =
        table::attribute::constructStringEntry(attrTable, &stringInfo);

    auto field = table::attribute::decodeStringEntry(stringEntry);
    EXPECT_EQ(field.stringType, PLDM_BIOS_STRING);
    EXPECT_EQ(field.defLength, longDefault.size());
    EXPECT_EQ(field.defString, longDefault);
}

TEST_F(TestBIOSTable, attributeEnumLongPossibleValueCoverage)
{
    Table stringTable;
    auto nameHandle = table::string::decodeHandle(
        table::string::constructEntry(stringTable, "LongEnumAttribute"));
    std::vector<uint16_t> possibleValueHandles;
    constexpr size_t possibleValueCount = 6;
    possibleValueHandles.reserve(possibleValueCount);
    for (size_t index = 0; index < possibleValueCount; ++index)
    {
        possibleValueHandles.emplace_back(
            table::string::decodeHandle(table::string::constructEntry(
                stringTable,
                "Value_" + std::to_string(index) + std::string(24, 'x'))));
    }

    Table attrTable;
    std::array<uint8_t, 3> defaultIndices{1, 3, 5};
    pldm_bios_table_attr_entry_enum_info enumInfo = {
        nameHandle,
        false,
        static_cast<uint8_t>(possibleValueHandles.size()),
        possibleValueHandles.data(),
        static_cast<uint8_t>(defaultIndices.size()),
        defaultIndices.data()};
    auto* enumEntry =
        table::attribute::constructEnumEntry(attrTable, &enumInfo);

    auto field = table::attribute::decodeEnumEntry(enumEntry);
    EXPECT_EQ(field.possibleValueStringHandle, possibleValueHandles);
    EXPECT_EQ(
        field.defaultValueIndex,
        std::vector<uint8_t>(defaultIndices.begin(), defaultIndices.end()));
}

TEST_F(TestBIOSTable, attributeValueLongStringUpdateCoverage)
{
    Table attrValueTable;
    table::attribute_value::constructStringEntry(attrValueTable, 0x40,
                                                 PLDM_BIOS_STRING, "short");
    table::appendPadAndChecksum(attrValueTable);

    Table updatedEntry;
    std::string replacement(120, 'r');
    auto* replacementEntry = table::attribute_value::constructStringEntry(
        updatedEntry, 0x40, PLDM_BIOS_STRING, replacement);

    auto updatedTable = table::attribute_value::updateTable(
        attrValueTable, updatedEntry.data(), updatedEntry.size());
    ASSERT_TRUE(updatedTable.has_value());
    auto* updatedAttrEntry =
        reinterpret_cast<const pldm_bios_attr_val_table_entry*>(
            updatedTable->data());
    EXPECT_EQ(table::attribute_value::decodeStringEntry(updatedAttrEntry),
              replacement);
    EXPECT_EQ(table::attribute_value::decodeStringEntry(replacementEntry),
              replacement);
}
