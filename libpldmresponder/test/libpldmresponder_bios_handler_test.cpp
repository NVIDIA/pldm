#include "libpldmresponder/bios.hpp"
#include "libpldmresponder/bios_table.hpp"

#include <dlfcn.h>
#include <libpldm/bios.h>

#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace fs = std::filesystem;

namespace
{

constexpr auto biosTablesDir = "pldm_bios_handler_tables";
constexpr auto stringTablePath = "pldm_bios_handler_tables/stringTable";
constexpr auto attrValueTablePath =
    "pldm_bios_handler_tables/attributeValueTable";
constexpr auto setDateTimeReqBytes = sizeof(pldm_set_date_time_req);

enum class BiosEncodeHook
{
    none,
    getDateTimeResp,
    getTableResp,
    setTableResp,
    getCurrentValueResp,
};

static BiosEncodeHook biosEncodeHook = BiosEncodeHook::none;

template <typename Fn>
Fn resolveBiosSymbol(const char* symbol)
{
    return reinterpret_cast<Fn>(dlsym(RTLD_NEXT, symbol));
}

struct ScopedBiosEncodeHook
{
    explicit ScopedBiosEncodeHook(BiosEncodeHook hook)
    {
        biosEncodeHook = hook;
    }

    ~ScopedBiosEncodeHook()
    {
        biosEncodeHook = BiosEncodeHook::none;
    }
};

extern "C" int encode_get_date_time_resp(
    uint8_t instance_id, uint8_t completion_code, uint8_t seconds,
    uint8_t minutes, uint8_t hours, uint8_t day, uint8_t month, uint16_t year,
    struct pldm_msg* msg)
{
    if (biosEncodeHook == BiosEncodeHook::getDateTimeResp)
    {
        return PLDM_ERROR_INVALID_DATA;
    }

    using Fn = int (*)(uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t,
                       uint8_t, uint16_t, struct pldm_msg*);
    static auto realFn = resolveBiosSymbol<Fn>("encode_get_date_time_resp");
    return realFn(instance_id, completion_code, seconds, minutes, hours, day,
                  month, year, msg);
}

extern "C" int encode_get_bios_table_resp(
    uint8_t instance_id, uint8_t completion_code, uint32_t next_transfer_handle,
    uint8_t transfer_flag, uint8_t* table_data, size_t payload_length,
    struct pldm_msg* msg)
{
    if (biosEncodeHook == BiosEncodeHook::getTableResp)
    {
        return PLDM_ERROR_INVALID_DATA;
    }

    using Fn = int (*)(uint8_t, uint8_t, uint32_t, uint8_t, uint8_t*, size_t,
                       struct pldm_msg*);
    static auto realFn = resolveBiosSymbol<Fn>("encode_get_bios_table_resp");
    return realFn(instance_id, completion_code, next_transfer_handle,
                  transfer_flag, table_data, payload_length, msg);
}

extern "C" int encode_set_bios_table_resp(
    uint8_t instance_id, uint8_t completion_code, uint32_t next_transfer_handle,
    struct pldm_msg* msg)
{
    if (biosEncodeHook == BiosEncodeHook::setTableResp)
    {
        return PLDM_ERROR_INVALID_DATA;
    }

    using Fn = int (*)(uint8_t, uint8_t, uint32_t, struct pldm_msg*);
    static auto realFn = resolveBiosSymbol<Fn>("encode_set_bios_table_resp");
    return realFn(instance_id, completion_code, next_transfer_handle, msg);
}

extern "C" int encode_get_bios_current_value_by_handle_resp(
    uint8_t instance_id, uint8_t completion_code, uint32_t next_transfer_handle,
    uint8_t transfer_flag, const uint8_t* attribute_data,
    size_t attribute_length, struct pldm_msg* msg)
{
    if (biosEncodeHook == BiosEncodeHook::getCurrentValueResp)
    {
        return PLDM_ERROR_INVALID_DATA;
    }

    using Fn = int (*)(uint8_t, uint8_t, uint32_t, uint8_t, const uint8_t*,
                       size_t, struct pldm_msg*);
    static auto realFn =
        resolveBiosSymbol<Fn>("encode_get_bios_current_value_by_handle_resp");
    return realFn(instance_id, completion_code, next_transfer_handle,
                  transfer_flag, attribute_data, attribute_length, msg);
}

} // namespace

using pldm::responder::Response;
using pldm::responder::bios::BIOSStringTable;
using pldm::responder::bios::Handler;
using pldm::responder::bios::Table;
namespace table = pldm::responder::bios::table;

namespace
{

uint8_t completionCode(const Response& response)
{
    return reinterpret_cast<const pldm_msg*>(response.data())->payload[0];
}

pldm_msg* asMsg(std::vector<uint8_t>& data)
{
    return reinterpret_cast<pldm_msg*>(data.data());
}

std::optional<Table> decodeTableResponse(const Response& response)
{
    auto* msg = reinterpret_cast<const pldm_msg*>(response.data());
    auto payloadLength = response.size() - sizeof(pldm_msg_hdr);

    uint8_t cc = 0;
    uint32_t nextTransferHandle = 0;
    uint8_t transferFlag = 0;
    size_t tableOffset = 0;
    auto rc =
        decode_get_bios_table_resp(msg, payloadLength, &cc, &nextTransferHandle,
                                   &transferFlag, &tableOffset);
    if (rc != PLDM_SUCCESS || cc != PLDM_SUCCESS)
    {
        return std::nullopt;
    }

    return Table(msg->payload + tableOffset, msg->payload + payloadLength);
}

std::optional<Table> decodeCurrentValueResponse(const Response& response)
{
    auto* msg = reinterpret_cast<const pldm_msg*>(response.data());
    auto payloadLength = response.size() - sizeof(pldm_msg_hdr);

    uint8_t cc = 0;
    uint32_t nextTransferHandle = 0;
    uint8_t transferFlag = 0;
    variable_field attributeData{};
    auto rc = decode_get_bios_attribute_current_value_by_handle_resp(
        msg, payloadLength, &cc, &nextTransferHandle, &transferFlag,
        &attributeData);
    if (rc != PLDM_SUCCESS || cc != PLDM_SUCCESS)
    {
        return std::nullopt;
    }

    return Table(attributeData.ptr, attributeData.ptr + attributeData.length);
}

Response getBIOSTableResponse(Handler& handler, uint8_t tableType)
{
    std::vector<uint8_t> request(
        sizeof(pldm_msg_hdr) + PLDM_GET_BIOS_TABLE_REQ_BYTES, 0);
    auto* msg = asMsg(request);
    EXPECT_EQ(
        encode_get_bios_table_req(1, 0, PLDM_GET_FIRSTPART, tableType, msg),
        PLDM_SUCCESS);
    return handler.getBIOSTable(msg, PLDM_GET_BIOS_TABLE_REQ_BYTES);
}

Table getBIOSTable(Handler& handler, uint8_t tableType)
{
    auto response = getBIOSTableResponse(handler, tableType);
    EXPECT_EQ(completionCode(response), PLDM_SUCCESS);

    auto table = decodeTableResponse(response);
    EXPECT_TRUE(table.has_value());
    return table.value_or(Table{});
}

uint16_t findAttrHandle(const Table& attrTable, const Table& stringTable,
                        const std::string& attributeName)
{
    BIOSStringTable biosStringTable(stringTable);
    auto stringHandle = biosStringTable.findHandle(attributeName);
    auto* entry = table::attribute::findByStringHandle(attrTable, stringHandle);
    EXPECT_NE(entry, nullptr);
    if (entry == nullptr)
    {
        return 0;
    }
    return table::attribute::decodeHeader(entry).attrHandle;
}

Table getCurrentValueByHandle(Handler& handler, uint16_t attrHandle)
{
    std::vector<uint8_t> request(
        sizeof(pldm_msg_hdr) + PLDM_GET_BIOS_ATTR_CURR_VAL_BY_HANDLE_REQ_BYTES,
        0);
    auto* msg = asMsg(request);
    EXPECT_EQ(encode_get_bios_attribute_current_value_by_handle_req(
                  4, 0, PLDM_GET_FIRSTPART, attrHandle, msg),
              PLDM_SUCCESS);

    auto response = handler.getBIOSAttributeCurrentValueByHandle(
        msg, PLDM_GET_BIOS_ATTR_CURR_VAL_BY_HANDLE_REQ_BYTES);
    EXPECT_EQ(completionCode(response), PLDM_SUCCESS);

    auto entry = decodeCurrentValueResponse(response);
    EXPECT_TRUE(entry.has_value());
    return entry.value_or(Table{});
}

class TimeDbusFixture
{
  public:
    static constexpr auto mapperService = "xyz.openbmc_project.ObjectMapper";
    static constexpr auto mapperPath = "/xyz/openbmc_project/object_mapper";
    static constexpr auto mapperInterface = "xyz.openbmc_project.ObjectMapper";
    static constexpr auto timeService = "xyz.openbmc_project.Time.Test";
    static constexpr auto timePath = "/xyz/openbmc_project/time/bmc";
    static constexpr auto timeInterface = "xyz.openbmc_project.Time.EpochTime";

    explicit TimeDbusFixture(uint64_t initialElapsedUsec) :
        elapsedUsec(initialElapsedUsec)
    {
        mapperConnection = std::make_shared<sdbusplus::asio::connection>(
            io, sdbusplus::bus::new_bus());
        mapperConnection->request_name(mapperService);
        mapperServer =
            std::make_unique<sdbusplus::asio::object_server>(mapperConnection);
        mapperIface = mapperServer->add_interface(mapperPath, mapperInterface);
        mapperIface->register_method(
            "GetObject",
            [](const std::string& path, const std::vector<std::string>&) {
                std::map<std::string, std::vector<std::string>> response;
                if (path == timePath)
                {
                    response.emplace(timeService,
                                     std::vector<std::string>{timeInterface});
                }
                return response;
            });
        mapperIface->initialize();

        timeConnection = std::make_shared<sdbusplus::asio::connection>(
            io, sdbusplus::bus::new_bus());
        timeConnection->request_name(timeService);
        timeServer =
            std::make_unique<sdbusplus::asio::object_server>(timeConnection);
        timeIface = timeServer->add_interface(timePath, timeInterface);
        timeIface->register_property(
            "Elapsed", elapsedUsec,
            [this](const uint64_t& requested, uint64_t& current) {
                current = requested;
                elapsedUsec = requested;
                ++setCount;
                return true;
            },
            [](const uint64_t& current) { return current; });
        timeIface->initialize();

        ioThread = std::thread([this] { io.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ~TimeDbusFixture()
    {
        io.stop();
        if (ioThread.joinable())
        {
            ioThread.join();
        }
    }

    boost::asio::io_context io;
    std::shared_ptr<sdbusplus::asio::connection> mapperConnection;
    std::shared_ptr<sdbusplus::asio::connection> timeConnection;
    std::unique_ptr<sdbusplus::asio::object_server> mapperServer;
    std::unique_ptr<sdbusplus::asio::object_server> timeServer;
    std::shared_ptr<sdbusplus::asio::dbus_interface> mapperIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> timeIface;
    uint64_t elapsedUsec{};
    size_t setCount = 0;
    std::thread ioThread;
};

class BIOSConfigDbusFixture
{
  public:
    static constexpr auto mapperService = "xyz.openbmc_project.ObjectMapper";
    static constexpr auto mapperPath = "/xyz/openbmc_project/object_mapper";
    static constexpr auto mapperInterface = "xyz.openbmc_project.ObjectMapper";
    static constexpr auto serviceName = "xyz.openbmc_project.BIOSConfig.Test";
    static constexpr auto objectPath =
        "/xyz/openbmc_project/bios_config/manager";
    static constexpr auto interfaceName =
        "xyz.openbmc_project.BIOSConfig.Manager";
    static constexpr auto stringObjectPath = "/xyz/abc/def";
    static constexpr auto stringInterfaceName =
        "xyz.openbmc_project.str_example1.value";

    BIOSConfigDbusFixture()
    {
        mapperConnection = std::make_shared<sdbusplus::asio::connection>(
            io, sdbusplus::bus::new_bus());
        mapperConnection->request_name(mapperService);
        mapperServer =
            std::make_unique<sdbusplus::asio::object_server>(mapperConnection);
        mapperIface = mapperServer->add_interface(mapperPath, mapperInterface);
        mapperIface->register_method(
            "GetObject",
            [](const std::string& path, const std::vector<std::string>&) {
                std::map<std::string, std::vector<std::string>> response;
                if (path == objectPath)
                {
                    response.emplace(serviceName,
                                     std::vector<std::string>{interfaceName});
                }
                else if (path == stringObjectPath)
                {
                    response.emplace(serviceName, std::vector<std::string>{
                                                      stringInterfaceName});
                }
                return response;
            });
        mapperIface->initialize();

        serviceConnection = std::make_shared<sdbusplus::asio::connection>(
            io, sdbusplus::bus::new_bus());
        serviceConnection->request_name(serviceName);
        serviceServer =
            std::make_unique<sdbusplus::asio::object_server>(serviceConnection);

        managerIface = serviceServer->add_interface(objectPath, interfaceName);
        managerIface->register_property(
            "BaseBIOSTable", baseTable,
            [this](const pldm::responder::bios::BaseBIOSTable& requested,
                   pldm::responder::bios::BaseBIOSTable& current) {
                current = requested;
                baseTable = requested;
                ++baseTableSetCount;
                return true;
            },
            [](const pldm::responder::bios::BaseBIOSTable& current) {
                return current;
            });
        managerIface->register_property(
            "PendingAttributes", pendingAttributes,
            [this](const pldm::responder::bios::PendingAttributes& requested,
                   pldm::responder::bios::PendingAttributes& current) {
                current = requested;
                pendingAttributes = requested;
                return true;
            },
            [](const pldm::responder::bios::PendingAttributes& current) {
                return current;
            });
        managerIface->initialize();

        stringIface =
            serviceServer->add_interface(stringObjectPath, stringInterfaceName);
        stringIface->register_property(
            "Str_example1", strExample1,
            [this](const std::string& requested, std::string& current) {
                current = requested;
                strExample1 = requested;
                ++stringSetCount;
                return true;
            },
            [](const std::string& current) { return current; });
        stringIface->initialize();

        ioThread = std::thread([this] { io.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ~BIOSConfigDbusFixture()
    {
        io.stop();
        if (ioThread.joinable())
        {
            ioThread.join();
        }
    }

    boost::asio::io_context io;
    std::shared_ptr<sdbusplus::asio::connection> mapperConnection;
    std::shared_ptr<sdbusplus::asio::connection> serviceConnection;
    std::unique_ptr<sdbusplus::asio::object_server> mapperServer;
    std::unique_ptr<sdbusplus::asio::object_server> serviceServer;
    std::shared_ptr<sdbusplus::asio::dbus_interface> mapperIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> managerIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> stringIface;
    pldm::responder::bios::BaseBIOSTable baseTable{};
    pldm::responder::bios::PendingAttributes pendingAttributes{};
    std::string strExample1 = "abc";
    size_t baseTableSetCount = 0;
    size_t stringSetCount = 0;
    std::thread ioThread;
};

class BIOSHandlerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fs::remove_all(biosTablesDir);
    }

    void TearDown() override
    {
        fs::remove_all(biosTablesDir);
    }
};

} // namespace

TEST_F(BIOSHandlerTest, getDateTimeCoverage)
{
    std::vector<uint8_t> request(sizeof(pldm_msg_hdr), 0);
    auto* msg = asMsg(request);
    ASSERT_EQ(encode_get_date_time_req(7, msg), PLDM_SUCCESS);

    {
        TimeDbusFixture fixture(1555132693000000ULL);
        Handler handler(0, 0, nullptr, nullptr);
        auto response = handler.getDateTime(msg, 0);
        EXPECT_EQ(completionCode(response), PLDM_SUCCESS);

        uint8_t cc = 0;
        uint8_t seconds = 0;
        uint8_t minutes = 0;
        uint8_t hours = 0;
        uint8_t day = 0;
        uint8_t month = 0;
        uint16_t year = 0;
        ASSERT_EQ(decode_get_date_time_resp(
                      reinterpret_cast<const pldm_msg*>(response.data()),
                      response.size() - sizeof(pldm_msg_hdr), &cc, &seconds,
                      &minutes, &hours, &day, &month, &year),
                  PLDM_SUCCESS);
        EXPECT_EQ(cc, PLDM_SUCCESS);
        auto epochTime = std::time_t(1555132693);
        auto* expectedTime = std::localtime(&epochTime);
        ASSERT_NE(expectedTime, nullptr);

        EXPECT_EQ(seconds, pldm::utils::decimalToBcd(expectedTime->tm_sec));
        EXPECT_EQ(minutes, pldm::utils::decimalToBcd(expectedTime->tm_min));
        EXPECT_EQ(hours, pldm::utils::decimalToBcd(expectedTime->tm_hour));
        EXPECT_EQ(day, pldm::utils::decimalToBcd(expectedTime->tm_mday));
        EXPECT_EQ(month, pldm::utils::decimalToBcd(expectedTime->tm_mon + 1));
        EXPECT_EQ(year,
                  pldm::utils::decimalToBcd(expectedTime->tm_year + 1900));
    }

    Handler handler(0, 0, nullptr, nullptr);
    auto response = handler.getDateTime(msg, 0);
    EXPECT_EQ(completionCode(response), PLDM_ERROR);
}

TEST_F(BIOSHandlerTest, getDateTimeResponseEncodeFailureCoverage)
{
    std::vector<uint8_t> request(sizeof(pldm_msg_hdr), 0);
    auto* msg = asMsg(request);
    ASSERT_EQ(encode_get_date_time_req(1, msg), PLDM_SUCCESS);
    TimeDbusFixture fixture(1555132693000000ULL);
    Handler handler(0, 0, nullptr, nullptr);
    ScopedBiosEncodeHook hook(BiosEncodeHook::getDateTimeResp);

    auto response = handler.getDateTime(msg, 0);
    EXPECT_EQ(completionCode(response), PLDM_ERROR_INVALID_DATA);
}

TEST_F(BIOSHandlerTest, setDateTimeCoverage)
{
    std::vector<uint8_t> request(sizeof(pldm_msg_hdr) + setDateTimeReqBytes, 0);
    auto* msg = asMsg(request);
    ASSERT_EQ(encode_set_date_time_req(3, 13, 18, 5, 13, 4, 2019, msg,
                                       setDateTimeReqBytes),
              PLDM_SUCCESS);

    {
        TimeDbusFixture fixture(0);
        Handler handler(0, 0, nullptr, nullptr);
        auto response = handler.setDateTime(msg, setDateTimeReqBytes);
        EXPECT_EQ(completionCode(response), PLDM_SUCCESS);
        EXPECT_EQ(fixture.elapsedUsec, 1555132693000000ULL);
        EXPECT_EQ(fixture.setCount, 1u);
    }

    {
        Handler handler(0, 0, nullptr, nullptr);
        auto response = handler.setDateTime(msg, 0);
        EXPECT_EQ(completionCode(response), PLDM_ERROR_INVALID_LENGTH);
    }

    Handler handler(0, 0, nullptr, nullptr);
    auto response = handler.setDateTime(msg, setDateTimeReqBytes);
    EXPECT_EQ(completionCode(response), PLDM_ERROR);
}

TEST_F(BIOSHandlerTest, getBIOSTableCoverage)
{
    Handler handler(0, 0, nullptr, nullptr);

    auto response = getBIOSTableResponse(handler, PLDM_BIOS_STRING_TABLE);
    EXPECT_EQ(completionCode(response), PLDM_SUCCESS);
    auto table = decodeTableResponse(response);
    ASSERT_TRUE(table.has_value());
    EXPECT_FALSE(table->empty());

    fs::remove(stringTablePath);
    auto unavailable = getBIOSTableResponse(handler, PLDM_BIOS_STRING_TABLE);
    EXPECT_EQ(completionCode(unavailable), PLDM_BIOS_TABLE_UNAVAILABLE);

    std::vector<uint8_t> request(
        sizeof(pldm_msg_hdr) + PLDM_GET_BIOS_TABLE_REQ_BYTES, 0);
    auto* msg = asMsg(request);
    ASSERT_EQ(encode_get_bios_table_req(1, 0, PLDM_GET_FIRSTPART,
                                        PLDM_BIOS_STRING_TABLE, msg),
              PLDM_SUCCESS);

    auto shortResponse = handler.getBIOSTable(msg, 0);
    EXPECT_EQ(completionCode(shortResponse), PLDM_ERROR_INVALID_LENGTH);
}

TEST_F(BIOSHandlerTest, getBIOSTableInvalidInstanceIdDiesOnEncodeFailure)
{
    std::vector<uint8_t> request(
        sizeof(pldm_msg_hdr) + PLDM_GET_BIOS_TABLE_REQ_BYTES, 0);
    auto* msg = asMsg(request);
    ASSERT_EQ(encode_get_bios_table_req(1, 0, PLDM_GET_FIRSTPART,
                                        PLDM_BIOS_STRING_TABLE, msg),
              PLDM_SUCCESS);
    Handler handler(0, 0, nullptr, nullptr);
    ScopedBiosEncodeHook hook(BiosEncodeHook::getTableResp);

    auto response = handler.getBIOSTable(msg, PLDM_GET_BIOS_TABLE_REQ_BYTES);
    EXPECT_EQ(completionCode(response), PLDM_ERROR_INVALID_DATA);
}

TEST_F(BIOSHandlerTest, setBIOSTableCoverage)
{
    Handler handler(0, 0, nullptr, nullptr);
    auto stringTable = getBIOSTable(handler, PLDM_BIOS_STRING_TABLE);

    std::vector<uint8_t> request(
        sizeof(pldm_msg_hdr) + PLDM_SET_BIOS_TABLE_MIN_REQ_BYTES +
            stringTable.size(),
        0);
    auto* msg = asMsg(request);
    ASSERT_EQ(encode_set_bios_table_req(
                  2, 0, PLDM_START_AND_END, PLDM_BIOS_STRING_TABLE,
                  stringTable.data(), stringTable.size(), msg,
                  request.size() - sizeof(pldm_msg_hdr)),
              PLDM_SUCCESS);
    auto response =
        handler.setBIOSTable(msg, request.size() - sizeof(pldm_msg_hdr));
    EXPECT_EQ(completionCode(response), PLDM_SUCCESS);

    auto reloadedStringTable = getBIOSTable(handler, PLDM_BIOS_STRING_TABLE);
    EXPECT_EQ(reloadedStringTable, stringTable);

    Table badChecksum = stringTable;
    badChecksum.back() ^= 0xFF;
    std::vector<uint8_t> badRequest(
        sizeof(pldm_msg_hdr) + PLDM_SET_BIOS_TABLE_MIN_REQ_BYTES +
            badChecksum.size(),
        0);
    auto* badMsg = asMsg(badRequest);
    ASSERT_EQ(encode_set_bios_table_req(
                  2, 0, PLDM_START_AND_END, PLDM_BIOS_STRING_TABLE,
                  badChecksum.data(), badChecksum.size(), badMsg,
                  badRequest.size() - sizeof(pldm_msg_hdr)),
              PLDM_SUCCESS);
    auto badResponse =
        handler.setBIOSTable(badMsg, badRequest.size() - sizeof(pldm_msg_hdr));
    EXPECT_EQ(completionCode(badResponse),
              PLDM_INVALID_BIOS_TABLE_DATA_INTEGRITY_CHECK);

    auto attrTable = getBIOSTable(handler, PLDM_BIOS_ATTR_TABLE);
    fs::remove(stringTablePath);
    std::vector<uint8_t> invalidTypeRequest(
        sizeof(pldm_msg_hdr) + PLDM_SET_BIOS_TABLE_MIN_REQ_BYTES +
            attrTable.size(),
        0);
    auto* invalidTypeMsg = asMsg(invalidTypeRequest);
    ASSERT_EQ(encode_set_bios_table_req(
                  2, 0, PLDM_START_AND_END, PLDM_BIOS_ATTR_TABLE,
                  attrTable.data(), attrTable.size(), invalidTypeMsg,
                  invalidTypeRequest.size() - sizeof(pldm_msg_hdr)),
              PLDM_SUCCESS);
    auto invalidTypeResponse = handler.setBIOSTable(
        invalidTypeMsg, invalidTypeRequest.size() - sizeof(pldm_msg_hdr));
    EXPECT_EQ(completionCode(invalidTypeResponse),
              PLDM_INVALID_BIOS_TABLE_TYPE);

    auto shortResponse = handler.setBIOSTable(msg, 0);
    EXPECT_EQ(completionCode(shortResponse), PLDM_ERROR_INVALID_LENGTH);
}

TEST_F(BIOSHandlerTest, setBIOSTableInvalidInstanceIdDiesOnEncodeFailure)
{
    Handler handler(0, 0, nullptr, nullptr);
    auto stringTable = getBIOSTable(handler, PLDM_BIOS_STRING_TABLE);

    std::vector<uint8_t> request(
        sizeof(pldm_msg_hdr) + PLDM_SET_BIOS_TABLE_MIN_REQ_BYTES +
            stringTable.size(),
        0);
    auto* msg = asMsg(request);
    ASSERT_EQ(encode_set_bios_table_req(
                  1, 0, PLDM_START_AND_END, PLDM_BIOS_STRING_TABLE,
                  stringTable.data(), stringTable.size(), msg,
                  request.size() - sizeof(pldm_msg_hdr)),
              PLDM_SUCCESS);
    ScopedBiosEncodeHook hook(BiosEncodeHook::setTableResp);

    auto response =
        handler.setBIOSTable(msg, request.size() - sizeof(pldm_msg_hdr));
    EXPECT_EQ(completionCode(response), PLDM_ERROR_INVALID_DATA);
}

TEST_F(BIOSHandlerTest, getBIOSAttributeCurrentValueByHandleCoverage)
{
    Handler handler(0, 0, nullptr, nullptr);
    auto stringTable = getBIOSTable(handler, PLDM_BIOS_STRING_TABLE);
    auto attrTable = getBIOSTable(handler, PLDM_BIOS_ATTR_TABLE);
    auto attrHandle = findAttrHandle(attrTable, stringTable, "str_example1");

    auto response = getCurrentValueByHandle(handler, attrHandle);
    auto* entry = reinterpret_cast<const pldm_bios_attr_val_table_entry*>(
        response.data());
    EXPECT_EQ(table::attribute_value::decodeStringEntry(entry), "abc");

    std::vector<uint8_t> request(
        sizeof(pldm_msg_hdr) + PLDM_GET_BIOS_ATTR_CURR_VAL_BY_HANDLE_REQ_BYTES,
        0);
    auto* msg = asMsg(request);
    ASSERT_EQ(encode_get_bios_attribute_current_value_by_handle_req(
                  4, 0, PLDM_GET_FIRSTPART, 0xFFFF, msg),
              PLDM_SUCCESS);
    auto invalidHandleResponse = handler.getBIOSAttributeCurrentValueByHandle(
        msg, PLDM_GET_BIOS_ATTR_CURR_VAL_BY_HANDLE_REQ_BYTES);
    EXPECT_EQ(completionCode(invalidHandleResponse),
              PLDM_INVALID_BIOS_ATTR_HANDLE);

    fs::remove(attrValueTablePath);
    auto unavailableResponse = handler.getBIOSAttributeCurrentValueByHandle(
        msg, PLDM_GET_BIOS_ATTR_CURR_VAL_BY_HANDLE_REQ_BYTES);
    EXPECT_EQ(completionCode(unavailableResponse), PLDM_BIOS_TABLE_UNAVAILABLE);

    auto shortResponse = handler.getBIOSAttributeCurrentValueByHandle(msg, 0);
    EXPECT_EQ(completionCode(shortResponse), PLDM_ERROR_INVALID_LENGTH);
}

TEST_F(BIOSHandlerTest,
       getBIOSAttributeCurrentValueByHandleInvalidInstanceIdDiesOnEncodeFailure)
{
    Handler handler(0, 0, nullptr, nullptr);
    auto stringTable = getBIOSTable(handler, PLDM_BIOS_STRING_TABLE);
    auto attrTable = getBIOSTable(handler, PLDM_BIOS_ATTR_TABLE);
    auto attrHandle = findAttrHandle(attrTable, stringTable, "str_example1");

    std::vector<uint8_t> request(
        sizeof(pldm_msg_hdr) + PLDM_GET_BIOS_ATTR_CURR_VAL_BY_HANDLE_REQ_BYTES,
        0);
    auto* msg = asMsg(request);
    ASSERT_EQ(encode_get_bios_attribute_current_value_by_handle_req(
                  1, 0, PLDM_GET_FIRSTPART, attrHandle, msg),
              PLDM_SUCCESS);
    ScopedBiosEncodeHook hook(BiosEncodeHook::getCurrentValueResp);

    auto response = handler.getBIOSAttributeCurrentValueByHandle(
        msg, PLDM_GET_BIOS_ATTR_CURR_VAL_BY_HANDLE_REQ_BYTES);
    EXPECT_EQ(completionCode(response), PLDM_ERROR_INVALID_DATA);
}

TEST_F(BIOSHandlerTest, setBIOSAttributeCurrentValueCoverage)
{
    BIOSConfigDbusFixture dbusFixture;
    Handler handler(0, 0, nullptr, nullptr);
    auto stringTable = getBIOSTable(handler, PLDM_BIOS_STRING_TABLE);
    auto attrTable = getBIOSTable(handler, PLDM_BIOS_ATTR_TABLE);
    auto attrHandle = findAttrHandle(attrTable, stringTable, "str_example1");

    Table attrValueEntry;
    table::attribute_value::constructStringEntry(attrValueEntry, attrHandle,
                                                 PLDM_BIOS_STRING, "wxyz");

    std::vector<uint8_t> request(
        sizeof(pldm_msg_hdr) + PLDM_SET_BIOS_ATTR_CURR_VAL_MIN_REQ_BYTES +
            attrValueEntry.size(),
        0);
    auto* msg = asMsg(request);
    ASSERT_EQ(encode_set_bios_attribute_current_value_req(
                  5, 0, PLDM_START_AND_END, attrValueEntry.data(),
                  attrValueEntry.size(), msg,
                  request.size() - sizeof(pldm_msg_hdr)),
              PLDM_SUCCESS);

    auto response = handler.setBIOSAttributeCurrentValue(
        msg, request.size() - sizeof(pldm_msg_hdr));
    EXPECT_EQ(completionCode(response), PLDM_SUCCESS);
    EXPECT_EQ(dbusFixture.strExample1, "wxyz");
    EXPECT_GT(dbusFixture.baseTableSetCount, 0u);
    EXPECT_EQ(dbusFixture.stringSetCount, 1u);

    auto updatedEntry = getCurrentValueByHandle(handler, attrHandle);
    auto* updatedAttrValue =
        reinterpret_cast<const pldm_bios_attr_val_table_entry*>(
            updatedEntry.data());
    EXPECT_EQ(table::attribute_value::decodeStringEntry(updatedAttrValue),
              "wxyz");

    Table invalidLengthEntry;
    table::attribute_value::constructStringEntry(
        invalidLengthEntry, attrHandle, PLDM_BIOS_STRING,
        std::string(101, 'x'));
    std::vector<uint8_t> invalidLengthRequest(
        sizeof(pldm_msg_hdr) + PLDM_SET_BIOS_ATTR_CURR_VAL_MIN_REQ_BYTES +
            invalidLengthEntry.size(),
        0);
    auto* invalidLengthMsg = asMsg(invalidLengthRequest);
    ASSERT_EQ(encode_set_bios_attribute_current_value_req(
                  5, 0, PLDM_START_AND_END, invalidLengthEntry.data(),
                  invalidLengthEntry.size(), invalidLengthMsg,
                  invalidLengthRequest.size() - sizeof(pldm_msg_hdr)),
              PLDM_SUCCESS);
    auto invalidLengthResponse = handler.setBIOSAttributeCurrentValue(
        invalidLengthMsg, invalidLengthRequest.size() - sizeof(pldm_msg_hdr));
    EXPECT_EQ(completionCode(invalidLengthResponse), PLDM_ERROR_INVALID_LENGTH);

    Table unknownHandleEntry;
    table::attribute_value::constructStringEntry(unknownHandleEntry, 0xFFFF,
                                                 PLDM_BIOS_STRING, "abcd");
    std::vector<uint8_t> unknownHandleRequest(
        sizeof(pldm_msg_hdr) + PLDM_SET_BIOS_ATTR_CURR_VAL_MIN_REQ_BYTES +
            unknownHandleEntry.size(),
        0);
    auto* unknownHandleMsg = asMsg(unknownHandleRequest);
    ASSERT_EQ(encode_set_bios_attribute_current_value_req(
                  5, 0, PLDM_START_AND_END, unknownHandleEntry.data(),
                  unknownHandleEntry.size(), unknownHandleMsg,
                  unknownHandleRequest.size() - sizeof(pldm_msg_hdr)),
              PLDM_SUCCESS);
    auto unknownHandleResponse = handler.setBIOSAttributeCurrentValue(
        unknownHandleMsg, unknownHandleRequest.size() - sizeof(pldm_msg_hdr));
    EXPECT_EQ(completionCode(unknownHandleResponse), PLDM_ERROR);

    fs::remove(attrValueTablePath);
    auto unavailableResponse = handler.setBIOSAttributeCurrentValue(
        msg, request.size() - sizeof(pldm_msg_hdr));
    EXPECT_EQ(completionCode(unavailableResponse), PLDM_BIOS_TABLE_UNAVAILABLE);

    auto shortResponse = handler.setBIOSAttributeCurrentValue(msg, 0);
    EXPECT_EQ(completionCode(shortResponse), PLDM_ERROR_INVALID_LENGTH);
}
