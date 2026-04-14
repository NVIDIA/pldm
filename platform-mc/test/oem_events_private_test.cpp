#include "platform-mc/oem_events.hpp"
#include "platform-mc/smbios_mdr.hpp"

#include <endian.h>
#include <sys/resource.h>
#include <unistd.h>

#include <sdbusplus/test/sdbus_mock.hpp>

#include <array>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace pldm::oem_events
{
constexpr const char* testPldmEventDir = "pldm_oem_events_private_cov/events";
constexpr const char* testInventoryDir =
    "pldm_oem_events_private_cov/inventory";
} // namespace pldm::oem_events

namespace mdr
{
constexpr const char* testDefaultFile =
    "pldm_oem_events_private_cov/smbios/smbios2";
} // namespace mdr

#define PLDM_EVENT_DIR testPldmEventDir
#define inventoryDir testInventoryDir
#define defaultFile testDefaultFile
#include "../oem_events.cpp" // NOLINT(bugprone-suspicious-include)
#include "../smbios_mdr.cpp" // NOLINT(bugprone-suspicious-include)
#undef defaultFile
#undef inventoryDir
#undef PLDM_EVENT_DIR

namespace
{

namespace fs = std::filesystem;

using namespace pldm::oem_events;

fs::path eventRoot()
{
    return testPldmEventDir;
}

fs::path inventoryRoot()
{
    return testInventoryDir;
}

fs::path smbiosRoot()
{
    return fs::path(mdr::testDefaultFile).parent_path();
}

fs::path smbiosFile()
{
    return mdr::testDefaultFile;
}

std::vector<uint8_t> makeOemEventPayload(uint8_t formatVersion,
                                         uint8_t formatType,
                                         const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> eventData{
        formatVersion,
        formatType,
        static_cast<uint8_t>(payload.size() & 0xFF),
        static_cast<uint8_t>((payload.size() >> 8) & 0xFF),
    };
    eventData.insert(eventData.end(), payload.begin(), payload.end());
    return eventData;
}

std::vector<uint8_t> makeOemEventPayload(
    uint8_t formatVersion, uint8_t formatType, const std::string& payload)
{
    return makeOemEventPayload(
        formatVersion, formatType,
        std::vector<uint8_t>(payload.begin(), payload.end()));
}

std::vector<uint8_t> readBytes(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    EXPECT_TRUE(file.is_open()) << path;
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

class ScopedFileSizeLimit
{
  public:
    explicit ScopedFileSizeLimit(rlim_t limit)
    {
        if (getrlimit(RLIMIT_FSIZE, &oldLimit) != 0)
        {
            return;
        }
        oldLimitValid = true;

        struct sigaction newAction{};
        newAction.sa_handler = SIG_IGN;
        sigemptyset(&newAction.sa_mask);
        if (sigaction(SIGXFSZ, &newAction, &oldAction) != 0)
        {
            return;
        }
        oldActionValid = true;

        struct rlimit newLimit{limit, oldLimit.rlim_max};
        if (setrlimit(RLIMIT_FSIZE, &newLimit) != 0)
        {
            return;
        }
        limitSet = true;
    }

    ~ScopedFileSizeLimit()
    {
        if (limitSet && oldLimitValid)
        {
            setrlimit(RLIMIT_FSIZE, &oldLimit);
        }
        if (oldActionValid)
        {
            sigaction(SIGXFSZ, &oldAction, nullptr);
        }
    }

    bool ready() const
    {
        return limitSet;
    }

  private:
    struct rlimit oldLimit{};
    struct sigaction oldAction{};
    bool oldLimitValid = false;
    bool oldActionValid = false;
    bool limitSet = false;
};

class OemEventsPrivateTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        cleanupPaths();
    }

    void TearDown() override
    {
        cleanupPaths();
    }

  private:
    static void cleanupPaths()
    {
        fs::remove_all(eventRoot());
        fs::remove_all(inventoryRoot());
        fs::remove_all(smbiosRoot());
    }
};

class OemEventsDbusMockTest : public OemEventsPrivateTest
{
  protected:
    void SetUp() override
    {
        OemEventsPrivateTest::SetUp();

        auto& busRef = pldm::utils::DBusHandler::getBus();
        auto mockBus = sdbusplus::get_mocked_new(&mock);
        savedBus.emplace(std::move(busRef));
        busRef = std::move(mockBus);
        busSwapped = true;
    }

    void TearDown() override
    {
        if (busSwapped)
        {
            pldm::utils::DBusHandler::getBus() = std::move(*savedBus);
            savedBus.reset();
        }
        OemEventsPrivateTest::TearDown();
    }

    void expectNewMethodCall(const char* service, const char* path,
                             const char* interface, const char* method)
    {
        EXPECT_CALL(mock, sd_bus_message_new_method_call(
                              testing::_, testing::_, testing::StrEq(service),
                              testing::StrEq(path), testing::StrEq(interface),
                              testing::StrEq(method)))
            .WillOnce(testing::Return(0));
    }

    void expectAppendString(const char* value)
    {
        EXPECT_CALL(mock, sd_bus_message_append_basic(
                              nullptr, SD_BUS_TYPE_STRING,
                              testing::MatcherCast<const void*>(
                                  testing::SafeMatcherCast<const char*>(
                                      testing::StrEq(value)))))
            .WillOnce(testing::Return(0));
    }

    void expectAppendInt32(int32_t value)
    {
        EXPECT_CALL(mock, sd_bus_message_append_basic(
                              nullptr, SD_BUS_TYPE_INT32, testing::_))
            .WillOnce([value](sd_bus_message*, char, const void* input) {
                EXPECT_EQ(value, *static_cast<const int32_t*>(input));
                return 0;
            });
    }

    void expectBusCallNoReply()
    {
        EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, 0, testing::_, nullptr))
            .WillOnce(testing::Return(0));
    }

    void expectBusCallWithReply()
    {
        EXPECT_CALL(mock,
                    sd_bus_call(nullptr, nullptr, 0, testing::_, testing::_))
            .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                         sd_bus_message** reply) {
                *reply = nullptr;
                return 0;
            });
    }

    void expectReadBool(bool value)
    {
        EXPECT_CALL(mock, sd_bus_message_read_basic(
                              nullptr, SD_BUS_TYPE_BOOLEAN, testing::_))
            .WillOnce([value](sd_bus_message*, char, void* output) {
                *static_cast<int*>(output) = value ? 1 : 0;
                return 0;
            });
    }

    testing::StrictMock<sdbusplus::SdBusMock> mock;
    std::optional<sdbusplus::bus_t> savedBus;
    bool busSwapped = false;
};

TEST_F(OemEventsPrivateTest, sanitizeTerminusNameCoverage)
{
    EXPECT_EQ("ProcessorModule_0", sanitizeTerminusName("ProcessorModule_0"));
    EXPECT_EQ("_unsafe_name", sanitizeTerminusName("../unsafe\\name"));
    EXPECT_EQ("unknown_terminus", sanitizeTerminusName(".."));
    EXPECT_EQ((eventRoot() / "_unsafe_name").string(),
              getEventDir("../unsafe\\name"));
}

TEST_F(OemEventsPrivateTest,
       sanitizeTerminusNameStripsMultipleTraversalSegmentsCoverage)
{
    EXPECT_EQ("_module__subdir___payload",
              sanitizeTerminusName("../module/..\\subdir/../../payload"));
    EXPECT_EQ("___", sanitizeTerminusName("../..\\../.."));
}

TEST_F(OemEventsPrivateTest, cperEventWriteAndOverwriteCoverage)
{
    const auto warningPayload =
        makeOemEventPayload(0x02, 0x01, std::vector<uint8_t>{'A', 'B', 'C'});
    const auto filePath =
        eventRoot() / "_unsafe_module" / CPER_ERROR_COUNT_FILE;

    EXPECT_TRUE(handleCperErrorCountEvent(
        "../unsafe\\module", warningPayload.data(), warningPayload.size()));
    EXPECT_EQ(std::vector<uint8_t>({'A', 'B', 'C'}), readBytes(filePath));
    EXPECT_FALSE(fs::exists(filePath.string() + ".tmp"));

    const auto overwritePayload =
        makeOemEventPayload(0x01, 0x00, std::vector<uint8_t>{'Z'});
    EXPECT_TRUE(handleCperErrorCountEvent(
        "../unsafe\\module", overwritePayload.data(), overwritePayload.size()));
    EXPECT_EQ(std::vector<uint8_t>({'Z'}), readBytes(filePath));
}

TEST_F(OemEventsPrivateTest, pcieEventWriteCoverage)
{
    const auto ltssmPayload =
        makeOemEventPayload(0x01, 0x00, std::vector<uint8_t>{0x10, 0x20});
    const auto telemetryPayload =
        makeOemEventPayload(0x01, 0x00, std::vector<uint8_t>{0xAB});
    const auto eventDir = eventRoot() / "Processor_Module_0";

    EXPECT_TRUE(handlePcieLtssmEvent("Processor/Module\\0", ltssmPayload.data(),
                                     ltssmPayload.size()));
    EXPECT_TRUE(
        handlePcieTelemetryEvent("Processor/Module\\0", telemetryPayload.data(),
                                 telemetryPayload.size()));

    EXPECT_EQ(std::vector<uint8_t>({0x10, 0x20}),
              readBytes(eventDir / PCIE_LTSSM_FILE));
    EXPECT_EQ(std::vector<uint8_t>({0xAB}),
              readBytes(eventDir / PCIE_TELEMETRY_FILE));
}

TEST_F(OemEventsPrivateTest, saveEventDataFilesystemFailureCoverage)
{
    std::ofstream rootFile(eventRoot());
    ASSERT_TRUE(rootFile.is_open());
    rootFile << "not a directory";
    rootFile.close();

    const auto payload =
        makeOemEventPayload(0x01, 0x00, std::vector<uint8_t>{0x01});
    EXPECT_FALSE(
        handleCperErrorCountEvent("conflict", payload.data(), payload.size()));
}

TEST_F(OemEventsPrivateTest, saveEventDataRejectsTooSmallBufferCoverage)
{
    const std::array<uint8_t, OEM_EVENT_HEADER_SIZE - 1> shortPayload{
        0x01, 0x00, 0x00};

    EXPECT_FALSE(handleCperErrorCountEvent("short", shortPayload.data(),
                                           shortPayload.size()));
    EXPECT_FALSE(fs::exists(eventRoot() / "short" / CPER_ERROR_COUNT_FILE));
}

TEST_F(OemEventsPrivateTest, saveEventDataRejectsTruncatedPayloadCoverage)
{
    const std::vector<uint8_t> truncatedPayload{0x01, 0x00, 0x04,
                                                0x00, 0xAA, 0xBB};

    EXPECT_FALSE(
        handlePcieTelemetryEvent("telemetry_truncated", truncatedPayload.data(),
                                 truncatedPayload.size()));
    EXPECT_FALSE(
        fs::exists(eventRoot() / "telemetry_truncated" / PCIE_TELEMETRY_FILE));
}

TEST_F(OemEventsPrivateTest, saveEventDataOpenAndRenameFailureCoverage)
{
    const auto payload =
        makeOemEventPayload(0x01, 0x00, std::vector<uint8_t>{0x11, 0x22});
    const auto eventDir = fs::path(getEventDir("existing_file"));
    fs::create_directories(eventRoot());

    {
        std::ofstream notDirectory(eventDir);
        ASSERT_TRUE(notDirectory.is_open());
        notDirectory << "file";
    }
    EXPECT_FALSE(saveEventData("existing_file", CPER_ERROR_COUNT_FILE,
                               payload.data(), payload.size()));

    fs::remove(eventDir);
    fs::create_directories(eventDir);
    fs::create_directory(eventDir / CPER_ERROR_COUNT_FILE);

    EXPECT_FALSE(saveEventData("existing_file", CPER_ERROR_COUNT_FILE,
                               payload.data(), payload.size()));
    EXPECT_FALSE(
        fs::exists(eventDir / (std::string(CPER_ERROR_COUNT_FILE) + ".tmp")));
}

TEST_F(OemEventsPrivateTest, saveEventDataCloseFailureCoverage)
{
    const auto payload =
        makeOemEventPayload(0x01, 0x00, std::vector<uint8_t>{0x11, 0x22, 0x33});
    const auto eventDir = eventRoot() / "close_failure";
    ScopedFileSizeLimit fileSizeLimit(1);
    ASSERT_TRUE(fileSizeLimit.ready());

    EXPECT_FALSE(saveEventData("close_failure", CPER_ERROR_COUNT_FILE,
                               payload.data(), payload.size()));
    EXPECT_FALSE(
        fs::exists(eventDir / (std::string(CPER_ERROR_COUNT_FILE) + ".tmp")));
}

TEST_F(OemEventsPrivateTest, inventorySuccessCoverage)
{
    const std::string jsonPayload = R"({"name":"GPU0","serial":"123"})";
    const auto inventoryEvent = makeOemEventPayload(0x02, 0x01, jsonPayload);

    EXPECT_FALSE(handleInventoryEvent(
        "../unsafe\\module", inventoryEvent.data(), inventoryEvent.size()));

    const std::string updatedJsonPayload = R"({"name":"GPU0","serial":"456"})";
    const auto updatedEvent =
        makeOemEventPayload(0x01, 0x00, updatedJsonPayload);
    EXPECT_FALSE(handleInventoryEvent("../unsafe\\module", updatedEvent.data(),
                                      updatedEvent.size()));
}

TEST_F(OemEventsPrivateTest, inventoryOpenAndRenameFailureCoverage)
{
    const std::string jsonPayload = R"({"status":"ok"})";
    const auto inventoryEvent = makeOemEventPayload(0x01, 0x00, jsonPayload);
    const auto filePath = inventoryRoot() / "Terminus_Inventory.json";
    const auto tmpPath = filePath.string() + ".tmp";

    fs::create_directories(inventoryRoot());
    fs::create_directory(tmpPath);
    EXPECT_FALSE(handleInventoryEvent("Terminus", inventoryEvent.data(),
                                      inventoryEvent.size()));
    fs::remove_all(tmpPath);

    fs::create_directory(filePath);
    EXPECT_FALSE(handleInventoryEvent("Terminus", inventoryEvent.data(),
                                      inventoryEvent.size()));
    EXPECT_FALSE(fs::exists(tmpPath));
}

TEST_F(OemEventsPrivateTest, inventoryCloseFailureCoverage)
{
    const std::string jsonPayload = R"({"status":"close_failure"})";
    const auto inventoryEvent = makeOemEventPayload(0x01, 0x00, jsonPayload);
    const auto filePath = inventoryRoot() / "GPU1_Inventory.json";
    const auto tmpPath = filePath.string() + ".tmp";
    ScopedFileSizeLimit fileSizeLimit(1);
    ASSERT_TRUE(fileSizeLimit.ready());

    EXPECT_FALSE(handleInventoryEvent("GPU1", inventoryEvent.data(),
                                      inventoryEvent.size()));
    EXPECT_FALSE(fs::exists(filePath));
    EXPECT_FALSE(fs::exists(tmpPath));
}

TEST_F(OemEventsPrivateTest, inventoryDirectoryCreationFailureCoverage)
{
    std::ofstream rootFile(inventoryRoot());
    ASSERT_TRUE(rootFile.is_open());
    rootFile << "not a directory";
    rootFile.close();

    const auto inventoryEvent =
        makeOemEventPayload(0x01, 0x00, std::string(R"({"id":7})"));
    EXPECT_FALSE(handleInventoryEvent("Terminus", inventoryEvent.data(),
                                      inventoryEvent.size()));
}

TEST_F(OemEventsPrivateTest, inventoryRejectsTooSmallAndZeroPayloadCoverage)
{
    const std::array<uint8_t, OEM_EVENT_HEADER_SIZE - 1> shortEvent{
        0x01, 0x00, 0x00};
    EXPECT_FALSE(handleInventoryEvent("GPU_short", shortEvent.data(),
                                      shortEvent.size()));

    const std::array<uint8_t, OEM_EVENT_HEADER_SIZE> zeroPayloadEvent{
        0x01, 0x00, 0x00, 0x00};
    EXPECT_FALSE(handleInventoryEvent("GPU_zero", zeroPayloadEvent.data(),
                                      zeroPayloadEvent.size()));

    EXPECT_FALSE(fs::exists(inventoryRoot() / "GPU_short_Inventory.json"));
    EXPECT_FALSE(fs::exists(inventoryRoot() / "GPU_zero_Inventory.json"));
}

TEST_F(OemEventsPrivateTest, inventoryRejectsTruncatedPayloadCoverage)
{
    const std::vector<uint8_t> truncatedEvent{0x01, 0x00, 0x05, 0x00, '{', '}'};

    EXPECT_FALSE(handleInventoryEvent("GPU_truncated", truncatedEvent.data(),
                                      truncatedEvent.size()));
    EXPECT_FALSE(fs::exists(inventoryRoot() / "GPU_truncated_Inventory.json"));
}

TEST_F(OemEventsPrivateTest, saveSmbiosDataSuccessCoverage)
{
    std::vector<uint8_t> smbiosData{0x10, 0x20, 0x30};
    ASSERT_TRUE(mdr::saveSmbiosData(smbiosData.size(), smbiosData.data()));

    auto fileData = readBytes(smbiosFile());
    ASSERT_GE(fileData.size(), sizeof(mdr::MDRSMBIOSHeader));

    mdr::MDRSMBIOSHeader header{};
    memcpy(&header, fileData.data(), sizeof(header));
    EXPECT_EQ(mdr::dirVersion, header.dirVer);
    EXPECT_EQ(mdr::typeII, header.mdrType);
    EXPECT_EQ(smbiosData.size(), header.dataSize);
    EXPECT_EQ(std::vector<uint8_t>(smbiosData.begin(), smbiosData.end()),
              std::vector<uint8_t>(fileData.begin() + sizeof(header),
                                   fileData.end()));

    std::vector<uint8_t> updatedData{0x44};
    ASSERT_TRUE(mdr::saveSmbiosData(updatedData.size(), updatedData.data()));
    fileData = readBytes(smbiosFile());
    EXPECT_EQ(std::vector<uint8_t>(updatedData.begin(), updatedData.end()),
              std::vector<uint8_t>(fileData.begin() + sizeof(header),
                                   fileData.end()));
}

TEST_F(OemEventsPrivateTest, saveSmbiosDataFailureCoverage)
{
    std::ofstream rootFile(smbiosRoot());
    ASSERT_TRUE(rootFile.is_open());
    rootFile << "not a directory";
    rootFile.close();

    std::vector<uint8_t> smbiosData{0xAA};
    EXPECT_FALSE(mdr::saveSmbiosData(smbiosData.size(), smbiosData.data()));

    fs::remove_all(smbiosRoot());
    fs::create_directories(smbiosRoot());
    fs::create_directory(smbiosFile());
    EXPECT_FALSE(mdr::saveSmbiosData(smbiosData.size(), smbiosData.data()));
}

TEST_F(OemEventsPrivateTest, saveSmbiosDataWriteExceptionCoverage)
{
    std::vector<uint8_t> smbiosData{0x11, 0x22, 0x33};
    ScopedFileSizeLimit fileSizeLimit(1);
    ASSERT_TRUE(fileSizeLimit.ready());

    EXPECT_FALSE(mdr::saveSmbiosData(smbiosData.size(), smbiosData.data()));
}

TEST_F(OemEventsPrivateTest, handleSmbiosEventSaveAndSyncCoverage)
{
    const std::array<uint8_t, 5> validEvent{{0x01, 0x02, 0x00, 0x10, 0x20}};
    EXPECT_FALSE(handleSmbiosEvent(validEvent.data(), validEvent.size()));

    auto fileData = readBytes(smbiosFile());
    ASSERT_EQ(sizeof(mdr::MDRSMBIOSHeader) + 2, fileData.size());
    EXPECT_EQ(
        std::vector<uint8_t>({0x10, 0x20}),
        std::vector<uint8_t>(fileData.begin() + sizeof(mdr::MDRSMBIOSHeader),
                             fileData.end()));
}

TEST_F(OemEventsPrivateTest, handleSmbiosEventSaveFailureCoverage)
{
    std::ofstream rootFile(smbiosRoot());
    ASSERT_TRUE(rootFile.is_open());
    rootFile << "not a directory";
    rootFile.close();

    const std::array<uint8_t, 5> validEvent{{0x01, 0x02, 0x00, 0x33, 0x44}};
    EXPECT_FALSE(handleSmbiosEvent(validEvent.data(), validEvent.size()));
}

TEST_F(OemEventsDbusMockTest, inventoryNotifySuccessCoverage)
{
    const std::string jsonPayload = R"({"name":"GPU0","serial":"789"})";
    const auto inventoryEvent = makeOemEventPayload(0x01, 0x00, jsonPayload);

    testing::InSequence seq;
    expectNewMethodCall(nvidiaInfoService, nvidiaInfoObjectPath,
                        nvidiaInfoInterface, "CreateInfo");
    expectAppendInt32(0);
    expectAppendString(jsonPayload.c_str());
    expectBusCallNoReply();

    EXPECT_TRUE(handleInventoryEvent("ProcessorModule_0", inventoryEvent.data(),
                                     inventoryEvent.size()));
}

TEST_F(OemEventsDbusMockTest, inventoryNotifyFailureCoverage)
{
    const std::string jsonPayload = R"({"name":"GPU2","serial":"999"})";
    const auto inventoryEvent = makeOemEventPayload(0x01, 0x00, jsonPayload);

    testing::InSequence seq;
    expectNewMethodCall(nvidiaInfoService, nvidiaInfoObjectPath,
                        nvidiaInfoInterface, "CreateInfo");
    expectAppendInt32(1);
    expectAppendString(jsonPayload.c_str());
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, 0, testing::_, nullptr))
        .WillOnce(testing::Return(-1));

    EXPECT_FALSE(handleInventoryEvent(
        "ProcessorModule_1", inventoryEvent.data(), inventoryEvent.size()));
}

TEST_F(OemEventsDbusMockTest, syncSmbiosDataStatusCoverage)
{
    {
        testing::InSequence seq;
        expectNewMethodCall(mdr::service, mdr::objectPath, mdr::interface,
                            "AgentSynchronizeData");
        expectBusCallWithReply();
        expectReadBool(true);
        EXPECT_TRUE(mdr::syncSmbiosData());
    }

    {
        testing::InSequence seq;
        expectNewMethodCall(mdr::service, mdr::objectPath, mdr::interface,
                            "AgentSynchronizeData");
        expectBusCallWithReply();
        expectReadBool(false);
        EXPECT_FALSE(mdr::syncSmbiosData());
    }
}

TEST_F(OemEventsDbusMockTest, syncSmbiosDataExceptionCoverage)
{
    testing::InSequence seq;
    expectNewMethodCall(mdr::service, mdr::objectPath, mdr::interface,
                        "AgentSynchronizeData");
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, 0, testing::_, testing::_))
        .WillOnce(testing::Return(-1));

    EXPECT_FALSE(mdr::syncSmbiosData());
}

TEST_F(OemEventsDbusMockTest, handleSmbiosEventSuccessCoverage)
{
    const std::array<uint8_t, 5> validEvent{{0x01, 0x02, 0x00, 0xAA, 0x55}};

    testing::InSequence seq;
    expectNewMethodCall(mdr::service, mdr::objectPath, mdr::interface,
                        "AgentSynchronizeData");
    expectBusCallWithReply();
    expectReadBool(true);

    EXPECT_TRUE(handleSmbiosEvent(validEvent.data(), validEvent.size()));

    auto fileData = readBytes(smbiosFile());
    ASSERT_EQ(sizeof(mdr::MDRSMBIOSHeader) + 2, fileData.size());
    EXPECT_EQ(
        std::vector<uint8_t>({0xAA, 0x55}),
        std::vector<uint8_t>(fileData.begin() + sizeof(mdr::MDRSMBIOSHeader),
                             fileData.end()));
}

} // namespace
