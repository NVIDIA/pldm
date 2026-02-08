// readHostEID_test.cpp - Unit tests for readHostEID()
//
// readHostEID() reads from HOST_EID_PATH, which is a compile-time constant
// defined in config.h (force-included via -include). To test it with a
// controlled file path, we #undef HOST_EID_PATH after config.h has been
// processed and redefine it to a temporary test path, then #include the
// implementation file to get a version of readHostEID() that uses our path.

#ifdef HOST_EID_PATH
#undef HOST_EID_PATH
#endif
#define HOST_EID_PATH "/tmp/claude/test_host_eid"

// Include the implementation so readHostEID() is compiled with our
// overridden HOST_EID_PATH.
#include <fstream>
#include <string>

#include "../utils.cpp" // NOLINT(bugprone-suspicious-include)

#include <gtest/gtest.h>

using namespace pldm::utils;

class ReadHostEIDTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Ensure the directory exists
        std::filesystem::create_directories("/tmp/claude");
        // Remove any leftover file from a previous test
        std::filesystem::remove(HOST_EID_PATH);
    }

    void TearDown() override
    {
        std::filesystem::remove(HOST_EID_PATH);
    }

    void writeEIDFile(const std::string& content)
    {
        std::ofstream ofs(HOST_EID_PATH);
        ASSERT_TRUE(ofs.good()) << "Failed to create test EID file";
        ofs << content;
        ofs.close();
    }
};

// Valid EID value: file contains a simple numeric string
TEST_F(ReadHostEIDTest, ValidEID)
{
    writeEIDFile("42");
    uint8_t eid = readHostEID();
    EXPECT_EQ(eid, 42);
}

// EID value of zero
TEST_F(ReadHostEIDTest, ZeroEID)
{
    writeEIDFile("0");
    uint8_t eid = readHostEID();
    EXPECT_EQ(eid, 0);
}

// Maximum valid uint8_t EID
TEST_F(ReadHostEIDTest, MaxUint8EID)
{
    writeEIDFile("255");
    uint8_t eid = readHostEID();
    EXPECT_EQ(eid, 255);
}

// File does not exist: should return 0 (default EID)
TEST_F(ReadHostEIDTest, FileDoesNotExist)
{
    // SetUp already removes the file; do not create one
    uint8_t eid = readHostEID();
    EXPECT_EQ(eid, 0);
}

// File is empty: should return 0
TEST_F(ReadHostEIDTest, EmptyFile)
{
    writeEIDFile("");
    uint8_t eid = readHostEID();
    EXPECT_EQ(eid, 0);
}

// File contains only whitespace: eidFile >> eidStr produces empty string
TEST_F(ReadHostEIDTest, WhitespaceOnlyFile)
{
    writeEIDFile("   \n\t  ");
    uint8_t eid = readHostEID();
    EXPECT_EQ(eid, 0);
}

// EID with trailing whitespace
TEST_F(ReadHostEIDTest, EIDWithTrailingWhitespace)
{
    writeEIDFile("13  \n");
    uint8_t eid = readHostEID();
    EXPECT_EQ(eid, 13);
}

// EID with leading whitespace (>> skips leading whitespace)
TEST_F(ReadHostEIDTest, EIDWithLeadingWhitespace)
{
    writeEIDFile("   7");
    uint8_t eid = readHostEID();
    EXPECT_EQ(eid, 7);
}

// Non-numeric content: atoi returns 0 for non-numeric strings
TEST_F(ReadHostEIDTest, NonNumericContent)
{
    writeEIDFile("abc");
    uint8_t eid = readHostEID();
    EXPECT_EQ(eid, 0);
}

// Mixed content starting with number: atoi parses leading digits
TEST_F(ReadHostEIDTest, MixedContentStartingWithNumber)
{
    writeEIDFile("12abc");
    uint8_t eid = readHostEID();
    EXPECT_EQ(eid, 12);
}

// EID value that overflows uint8_t: atoi returns int, then truncated
TEST_F(ReadHostEIDTest, OverflowValue)
{
    writeEIDFile("256");
    uint8_t eid = readHostEID();
    // 256 as int is 0x100; truncated to uint8_t is 0
    EXPECT_EQ(eid, static_cast<uint8_t>(256));
}

// Negative value: atoi handles negative, truncated to uint8_t
TEST_F(ReadHostEIDTest, NegativeValue)
{
    writeEIDFile("-1");
    uint8_t eid = readHostEID();
    // atoi("-1") == -1; cast to uint8_t wraps to 255
    EXPECT_EQ(eid, static_cast<uint8_t>(-1));
}

// Multiple values in file: >> reads only the first token
TEST_F(ReadHostEIDTest, MultipleValuesReadsFirst)
{
    writeEIDFile("10 20 30");
    uint8_t eid = readHostEID();
    EXPECT_EQ(eid, 10);
}
