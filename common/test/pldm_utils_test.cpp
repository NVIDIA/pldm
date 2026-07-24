// Override FLIGHT_RECORDER_MAX_ENTRIES before including flight_recorder.hpp
// config.h (force-included) sets it to 0; we need a non-zero value for testing
#ifdef FLIGHT_RECORDER_MAX_ENTRIES
#undef FLIGHT_RECORDER_MAX_ENTRIES
#endif
#define FLIGHT_RECORDER_MAX_ENTRIES 5

#include "common/bios_utils.hpp"
#include "common/flight_recorder.hpp"
#include "common/instance_id.hpp"
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "common/log_rate_limit.hpp"
#undef private
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "common/mctp_error_handling.hpp"
#include "common/mmap_stream.hpp"
#include "common/utils.hpp"
#include "mocked_utils.hpp"
#include "test/test_tmp_utils.hpp"

#include <fcntl.h>
#include <libpldm/base.h>
#include <libpldm/bios.h>
#include <libpldm/firmware_update.h>
#include <libpldm/fru.h>
#include <libpldm/platform.h>
#include <linux/mctp.h>
#include <systemd/sd-bus-protocol.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>
#include <xyz/openbmc_project/Logging/Create/client.hpp>

#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

#include <gtest/gtest.h>

using namespace pldm::utils;

namespace
{

class StubDbusHandlerInterface : public pldm::utils::DBusHandlerInterface
{
  public:
    std::string getService(const char* /*path*/,
                           const char* /*interface*/) const override
    {
        return "stub.service";
    }

    GetSubTreeResponse getSubtree(
        const std::string& /*path*/, int /*depth*/,
        const std::vector<std::string>& /*ifaceList*/) const override
    {
        return {};
    }

    GetSubTreePathsResponse getSubTreePaths(
        const std::string& /*objectPath*/, int /*depth*/,
        const std::vector<std::string>& /*ifaceList*/) const override
    {
        return {};
    }

    GetAncestorsResponse getAncestors(
        const std::string& /*path*/,
        const std::vector<std::string>& /*ifaceList*/) const override
    {
        return {};
    }

    void setDbusProperty(const DBusMapping& /*dBusMap*/,
                         const PropertyValue& /*value*/) const override
    {}

    PropertyValue getDbusPropertyVariant(
        const char* /*objPath*/, const char* /*dbusProp*/,
        const char* /*dbusInterface*/) const override
    {
        return static_cast<uint32_t>(123);
    }

    PropertyMap getDbusPropertiesVariant(
        const char* /*serviceName*/, const char* /*objPath*/,
        const char* /*dbusInterface*/) const override
    {
        return {};
    }

    GetAssociatedSubTreeResponse getAssociatedSubTree(
        const sdbusplus::object_path& /*objectPath*/,
        const sdbusplus::object_path& /*subtree*/, int /*depth*/,
        const std::vector<std::string>& /*ifaceList*/) const override
    {
        return {};
    }
};

class MockBusDBusHandler : public pldm::utils::DBusHandler
{
  public:
    static void setTestBus(sdbusplus::bus_t* bus)
    {
        testBus = bus;
    }

    static auto& getBus()
    {
        return *testBus;
    }

    std::string getService(const char* path,
                           const char* interface) const override
    {
        using DbusInterfaceList = std::vector<std::string>;
        DbusInterfaceList interfaces =
            interface ? DbusInterfaceList{interface} : DbusInterfaceList{};
        auto mapperResponse =
            mapperCall<std::map<std::string, std::vector<std::string>>>(
                "GetObject", path, interfaces);
        return mapperResponse.begin()->first;
    }

    GetSubTreeResponse getSubtree(
        const std::string& searchPath, int depth,
        const std::vector<std::string>& ifaceList) const override
    {
        return mapperCall<GetSubTreeResponse>("GetSubTree", searchPath, depth,
                                              ifaceList);
    }

    GetSubTreePathsResponse getSubTreePaths(
        const std::string& objectPath, int depth,
        const std::vector<std::string>& ifaceList) const override
    {
        return mapperCall<GetSubTreePathsResponse>(
            "GetSubTreePaths", objectPath, depth, ifaceList);
    }

    GetAncestorsResponse getAncestors(
        const std::string& path,
        const std::vector<std::string>& ifaceList) const override
    {
        return mapperCall<GetAncestorsResponse>("GetAncestors", path,
                                                ifaceList);
    }

    void setDbusProperty(const DBusMapping& dBusMap,
                         const PropertyValue& value) const override
    {
        auto doSet = [&](const auto& typedValue) {
            auto service = getService(dBusMap.objectPath.c_str(),
                                      dBusMap.interface.c_str());
            methodCallNoReply(service.c_str(), dBusMap.objectPath.c_str(),
                              dbusProperties, "Set", dbusTimeout,
                              dBusMap.interface.c_str(),
                              dBusMap.propertyName.c_str(), typedValue);
        };

        if (dBusMap.propertyType == "uint8_t")
        {
            doSet(std::variant<uint8_t>(std::get<uint8_t>(value)));
        }
        else if (dBusMap.propertyType == "bool")
        {
            doSet(std::variant<bool>(std::get<bool>(value)));
        }
        else if (dBusMap.propertyType == "int16_t")
        {
            doSet(std::variant<int16_t>(std::get<int16_t>(value)));
        }
        else if (dBusMap.propertyType == "uint16_t")
        {
            doSet(std::variant<uint16_t>(std::get<uint16_t>(value)));
        }
        else if (dBusMap.propertyType == "int32_t")
        {
            doSet(std::variant<int32_t>(std::get<int32_t>(value)));
        }
        else if (dBusMap.propertyType == "uint32_t")
        {
            doSet(std::variant<uint32_t>(std::get<uint32_t>(value)));
        }
        else if (dBusMap.propertyType == "int64_t")
        {
            doSet(std::variant<int64_t>(std::get<int64_t>(value)));
        }
        else if (dBusMap.propertyType == "uint64_t")
        {
            doSet(std::variant<uint64_t>(std::get<uint64_t>(value)));
        }
        else if (dBusMap.propertyType == "double")
        {
            doSet(std::variant<double>(std::get<double>(value)));
        }
        else if (dBusMap.propertyType == "string")
        {
            doSet(std::variant<std::string>(std::get<std::string>(value)));
        }
        else if (dBusMap.propertyType == "array[string]")
        {
            doSet(std::variant<std::vector<std::string>>(
                std::get<std::vector<std::string>>(value)));
        }
        else if (dBusMap.propertyType == "array[object_path]")
        {
            doSet(std::variant<std::vector<sdbusplus::object_path>>(
                std::get<std::vector<sdbusplus::object_path>>(value)));
        }
        else
        {
            throw std::invalid_argument("Unsupported Dbus Type");
        }
    }

    GetAssociatedSubTreeResponse getAssociatedSubTree(
        const sdbusplus::object_path& objectPath,
        const sdbusplus::object_path& subtree, int depth,
        const std::vector<std::string>& ifaceList) const override
    {
        return mapperCall<GetAssociatedSubTreeResponse>(
            "GetAssociatedSubTree", objectPath, subtree, depth, ifaceList);
    }

    PropertyMap getDbusPropertiesVariant(
        const char* serviceName, const char* objPath,
        const char* dbusInterface) const override
    {
        return methodCall<PropertyMap>(serviceName, objPath, dbusProperties,
                                       "GetAll", dbusTimeout, dbusInterface);
    }

    bool checkDbusPropertyVariant(const char* objPath, const char* dbusProp,
                                  const char* dbusInterface) const
    {
        auto service = getService(objPath, dbusInterface);
        try
        {
            auto getAll = methodCall<std::unordered_map<
                std::string,
                std::variant<std::string, std::vector<std::string>>>>(
                service.c_str(), objPath, dbusProperties, "GetAll", 0,
                dbusInterface);
            return getAll.contains(dbusProp);
        }
        catch (const sdbusplus::exception_t&)
        {
            return false;
        }
    }

  private:
    template <typename Response, typename... Args>
    Response mapperCall(const char* method, Args&&... args) const
    {
        return methodCall<Response>(
            ObjectMapper::default_service, ObjectMapper::instance_path,
            ObjectMapper::interface, method, dbusTimeout,
            std::forward<Args>(args)...);
    }

    template <typename Response, typename... Args>
    Response methodCall(const char* service, const char* path,
                        const char* interface, const char* method,
                        uint64_t timeout, Args&&... args) const
    {
        auto& bus = getBus();
        auto request = bus.new_method_call(service, path, interface, method);
        request.append(std::forward<Args>(args)...);
        auto reply = bus.call(request, timeout);
        return reply.unpack<Response>();
    }

    template <typename... Args>
    void methodCallNoReply(const char* service, const char* path,
                           const char* interface, const char* method,
                           uint64_t timeout, Args&&... args) const
    {
        auto& bus = getBus();
        auto request = bus.new_method_call(service, path, interface, method);
        request.append(std::forward<Args>(args)...);
        bus.call_noreply(request, timeout);
    }

    static sdbusplus::bus_t* testBus;
};

sdbusplus::bus_t* MockBusDBusHandler::testBus = nullptr;

class DBusMockTestHelpers : public testing::Test
{
  protected:
    testing::StrictMock<sdbusplus::SdBusMock> mock;

    void expectNewMethodCall(const char* service, const char* path,
                             const char* interface, const char* method)
    {
        EXPECT_CALL(mock, sd_bus_message_new_method_call(
                              testing::_, testing::_, testing::StrEq(service),
                              testing::StrEq(path), testing::StrEq(interface),
                              testing::StrEq(method)))
            .WillOnce(testing::Return(0));
    }

    template <typename T>
    void expectAppendBasic(char type, T value)
    {
        EXPECT_CALL(mock, sd_bus_message_append_basic(
                              nullptr, type,
                              testing::MatcherCast<const void*>(
                                  testing::SafeMatcherCast<const T*>(
                                      testing::Pointee(testing::Eq(value))))))
            .WillOnce(testing::Return(0));
    }

    void expectAppendString(char type, const char* value)
    {
        EXPECT_CALL(mock, sd_bus_message_append_basic(
                              nullptr, type,
                              testing::MatcherCast<const void*>(
                                  testing::SafeMatcherCast<const char*>(
                                      testing::StrEq(value)))))
            .WillOnce(testing::Return(0));
    }

    void expectOpenContainer(char type, const char* contents)
    {
        EXPECT_CALL(mock, sd_bus_message_open_container(
                              nullptr, type, testing::StrEq(contents)))
            .WillOnce(testing::Return(0));
    }

    void expectCloseContainer()
    {
        EXPECT_CALL(mock, sd_bus_message_close_container(nullptr))
            .WillOnce(testing::Return(0));
    }

    void expectBusCallWithReply(uint64_t timeout = dbusTimeout)
    {
        EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, timeout, testing::_,
                                      testing::_))
            .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                         sd_bus_message** reply) {
                *reply = nullptr;
                return 0;
            });
    }

    void expectBusCallNoReply()
    {
        EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, dbusTimeout, testing::_,
                                      nullptr))
            .WillOnce(testing::Return(0));
    }

    void expectReadString(const char* value)
    {
        EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, SD_BUS_TYPE_STRING,
                                                    testing::_))
            .WillOnce([value](sd_bus_message*, char, void* output) {
                *static_cast<const char**>(output) = value;
                return 0;
            });
    }

    void expectReadOneServiceMapEntry(const char* service,
                                      const char* interface)
    {
        EXPECT_CALL(mock, sd_bus_message_enter_container(
                              nullptr, SD_BUS_TYPE_ARRAY, testing::_))
            .WillOnce(testing::Return(0));
        EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, false))
            .WillOnce(testing::Return(0));
        EXPECT_CALL(mock, sd_bus_message_enter_container(
                              nullptr, SD_BUS_TYPE_DICT_ENTRY, testing::_))
            .WillOnce(testing::Return(0));
        expectReadString(service);
        EXPECT_CALL(mock, sd_bus_message_enter_container(
                              nullptr, SD_BUS_TYPE_ARRAY, testing::_))
            .WillOnce(testing::Return(0));
        EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, false))
            .WillOnce(testing::Return(0));
        expectReadString(interface);
        EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, false))
            .WillOnce(testing::Return(1));
        EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
            .WillOnce(testing::Return(0));
        EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
            .WillOnce(testing::Return(0));
        EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, false))
            .WillOnce(testing::Return(1));
        EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
            .WillOnce(testing::Return(0));
    }

    void expectReadEmptyMapLikeContainer()
    {
        EXPECT_CALL(mock, sd_bus_message_enter_container(
                              nullptr, SD_BUS_TYPE_ARRAY, testing::_))
            .WillOnce(testing::Return(0));
        EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, false))
            .WillOnce(testing::Return(1));
        EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
            .WillOnce(testing::Return(0));
    }

    void expectReadOnePathVectorEntry(const char* path)
    {
        EXPECT_CALL(mock, sd_bus_message_enter_container(
                              nullptr, SD_BUS_TYPE_ARRAY, testing::_))
            .WillOnce(testing::Return(0));
        EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, false))
            .WillOnce(testing::Return(0));
        expectReadString(path);
        EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, false))
            .WillOnce(testing::Return(1));
        EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
            .WillOnce(testing::Return(0));
    }

    void expectGetObjectCall(const char* objectPath, const char* interface,
                             const char* service)
    {
        expectNewMethodCall(ObjectMapper::default_service,
                            ObjectMapper::instance_path,
                            ObjectMapper::interface, "GetObject");
        expectAppendString(SD_BUS_TYPE_STRING, objectPath);
        expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
        expectAppendString(SD_BUS_TYPE_STRING, interface);
        expectCloseContainer();
        expectBusCallWithReply();
        expectReadOneServiceMapEntry(service, interface);
    }

    void expectSetPropertyCall(const char* service, const char* objectPath,
                               const char* interface, const char* propertyName)
    {
        expectNewMethodCall(service, objectPath, dbusProperties, "Set");
        expectAppendString(SD_BUS_TYPE_STRING, interface);
        expectAppendString(SD_BUS_TYPE_STRING, propertyName);
    }
};

class DBusHandlerBusMockTest : public DBusMockTestHelpers
{
  protected:
    sdbusplus::bus_t bus = sdbusplus::get_mocked_new(&mock);
    MockBusDBusHandler handler;

    void SetUp() override
    {
        MockBusDBusHandler::setTestBus(&bus);
    }

    void TearDown() override
    {
        MockBusDBusHandler::setTestBus(nullptr);
    }
};

} // namespace

TEST(GetNumPadBytesTest, NoPaddingNeeded)
{
    EXPECT_EQ(getNumPadBytes(0), 0);
    EXPECT_EQ(getNumPadBytes(4), 0);
    EXPECT_EQ(getNumPadBytes(8), 0);
    EXPECT_EQ(getNumPadBytes(12), 0);
}

TEST(GetNumPadBytesTest, OneBytePadding)
{
    EXPECT_EQ(getNumPadBytes(3), 1);
    EXPECT_EQ(getNumPadBytes(7), 1);
    EXPECT_EQ(getNumPadBytes(11), 1);
}

TEST(GetNumPadBytesTest, TwoBytesPadding)
{
    EXPECT_EQ(getNumPadBytes(2), 2);
    EXPECT_EQ(getNumPadBytes(6), 2);
    EXPECT_EQ(getNumPadBytes(10), 2);
}

TEST(GetNumPadBytesTest, ThreeBytesPadding)
{
    EXPECT_EQ(getNumPadBytes(1), 3);
    EXPECT_EQ(getNumPadBytes(5), 3);
    EXPECT_EQ(getNumPadBytes(9), 3);
}

TEST(GetNumPadBytesTest, LargeValues)
{
    EXPECT_EQ(getNumPadBytes(1001), 3);
    EXPECT_EQ(getNumPadBytes(1024), 0);
    EXPECT_EQ(getNumPadBytes(65535), 1);
}

TEST(GetInventoryObjects, testForEmptyObject)
{
    ObjectValueTree result =
        DBusHandler::getInventoryObjects<GetManagedEmptyObject>();
    EXPECT_TRUE(result.empty());
}

TEST(GetInventoryObjects, emptyObjectReferenceIsReused)
{
    auto& first = DBusHandler::getInventoryObjects<GetManagedEmptyObject>();
    auto* firstPtr = &first;
    auto& second = DBusHandler::getInventoryObjects<GetManagedEmptyObject>();

    EXPECT_EQ(firstPtr, &second);
    EXPECT_TRUE(second.empty());
}

TEST(GetInventoryObjects, testForObject)
{
    std::string path = "/foo/bar";
    std::string service = "foo.bar";
    auto result = DBusHandler::getInventoryObjects<GetManagedObject>();
    EXPECT_EQ(result[path].begin()->first, service);
    auto function =
        std::get<bool>(result[path][service][std::string("Functional")]);
    auto model =
        std::get<std::string>(result[path][service][std::string("Model")]);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(function);
    EXPECT_EQ(model, std::string("1234 - 00Z"));
}

TEST(decodeDate, testGooduintToDate)
{
    uint64_t data = 20191212115959;
    uint16_t year = 2019;
    uint8_t month = 12;
    uint8_t day = 12;
    uint8_t hours = 11;
    uint8_t minutes = 59;
    uint8_t seconds = 59;

    uint16_t retyear = 0;
    uint8_t retmonth = 0;
    uint8_t retday = 0;
    uint8_t rethours = 0;
    uint8_t retminutes = 0;
    uint8_t retseconds = 0;

    auto ret = uintToDate(data, &retyear, &retmonth, &retday, &rethours,
                          &retminutes, &retseconds);

    EXPECT_EQ(ret, true);
    EXPECT_EQ(year, retyear);
    EXPECT_EQ(month, retmonth);
    EXPECT_EQ(day, retday);
    EXPECT_EQ(hours, rethours);
    EXPECT_EQ(minutes, retminutes);
    EXPECT_EQ(seconds, retseconds);
}

TEST(decodeDate, testBaduintToDate)
{
    uint64_t data = 10191212115959;

    uint16_t retyear = 0;
    uint8_t retmonth = 0;
    uint8_t retday = 0;
    uint8_t rethours = 0;
    uint8_t retminutes = 0;
    uint8_t retseconds = 0;

    auto ret = uintToDate(data, &retyear, &retmonth, &retday, &rethours,
                          &retminutes, &retseconds);

    EXPECT_EQ(ret, false);
}

TEST(parseEffecterData, testGoodDecodeEffecterData)
{
    std::vector<uint8_t> effecterData = {1, 1, 0, 1};
    uint8_t effecterCount = 2;
    set_effecter_state_field stateField0 = {1, 1};
    set_effecter_state_field stateField1 = {0, 1};

    auto effecterField = parseEffecterData(effecterData, effecterCount);
    EXPECT_NE(effecterField, std::nullopt);
    EXPECT_EQ(effecterCount, effecterField->size());

    std::vector<set_effecter_state_field> stateField = effecterField.value();
    EXPECT_EQ(stateField[0].set_request, stateField0.set_request);
    EXPECT_EQ(stateField[0].effecter_state, stateField0.effecter_state);
    EXPECT_EQ(stateField[1].set_request, stateField1.set_request);
    EXPECT_EQ(stateField[1].effecter_state, stateField1.effecter_state);
}

TEST(parseEffecterData, testBadDecodeEffecterData)
{
    std::vector<uint8_t> effecterData = {0, 1, 0, 1, 0, 1};
    uint8_t effecterCount = 2;

    auto effecterField = parseEffecterData(effecterData, effecterCount);

    EXPECT_EQ(effecterField, std::nullopt);
}

TEST(parseEffecterData, testZeroCountWithPayloadReturnsNullopt)
{
    std::vector<uint8_t> effecterData = {0, 1};

    auto effecterField = parseEffecterData(effecterData, 0);

    EXPECT_EQ(effecterField, std::nullopt);
}

TEST(FindStateEffecterPDR, testOneMatch)
{
    auto repo = pldm_pdr_init();
    uint8_t tid = 1;
    uint16_t entityID = 33;
    uint16_t stateSetId = 196;

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));

    auto rec = new (pdr.data()) pldm_state_effecter_pdr;

    auto state = new (rec->possible_states) state_effecter_possible_states;

    rec->hdr.type = 11;
    rec->hdr.record_handle = 1;
    rec->entity_type = 33;
    rec->container_id = 0;
    rec->composite_effecter_count = 1;
    state->state_set_id = 196;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    auto record = findStateEffecterPDR(tid, entityID, stateSetId, repo);

    EXPECT_EQ(pdr, record[0]);

    pldm_pdr_destroy(repo);
}

TEST(FindStateEffecterPDR, testNoMatch)
{
    auto repo = pldm_pdr_init();
    uint8_t tid = 1;
    uint16_t entityID = 44;
    uint16_t stateSetId = 196;

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));

    auto rec = new (pdr.data()) pldm_state_effecter_pdr;

    auto state = new (rec->possible_states) state_effecter_possible_states;

    rec->hdr.type = 11;
    rec->hdr.record_handle = 1;
    rec->entity_type = 33;
    rec->container_id = 0;
    rec->composite_effecter_count = 1;
    state->state_set_id = 196;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    auto record = findStateEffecterPDR(tid, entityID, stateSetId, repo);

    EXPECT_EQ(record.empty(), true);

    pldm_pdr_destroy(repo);
}

TEST(FindStateEffecterPDR, testEmptyRepo)
{
    auto repo = pldm_pdr_init();
    uint8_t tid = 1;
    uint16_t entityID = 33;
    uint16_t stateSetId = 196;

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));

    auto record = findStateEffecterPDR(tid, entityID, stateSetId, repo);

    EXPECT_EQ(record.empty(), true);

    pldm_pdr_destroy(repo);
}

TEST(FindStateEffecterPDR, testMoreMatch)
{
    auto repo = pldm_pdr_init();
    uint8_t tid = 1;

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));

    auto rec = new (pdr.data()) pldm_state_effecter_pdr;

    auto state = new (rec->possible_states) state_effecter_possible_states;

    rec->hdr.type = 11;
    rec->hdr.record_handle = 1;
    rec->entity_type = 31;
    rec->container_id = 0;
    rec->composite_effecter_count = 1;
    state->state_set_id = 129;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    std::vector<uint8_t> pdr_second(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));

    auto rec_second = new (pdr_second.data()) pldm_state_effecter_pdr;

    auto state_second = new (rec_second->possible_states)
        state_effecter_possible_states;

    rec_second->hdr.type = 11;
    rec_second->hdr.record_handle = 2;
    rec_second->entity_type = 31;
    rec_second->container_id = 0;
    rec_second->composite_effecter_count = 1;
    state_second->state_set_id = 129;
    state_second->possible_states_size = 1;

    handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr_second.data(), pdr_second.size(), false, 1,
                           &handle),
              0);

    uint16_t entityID_ = 31;
    uint16_t stateSetId_ = 129;

    auto record = findStateEffecterPDR(tid, entityID_, stateSetId_, repo);

    EXPECT_EQ(pdr, record[0]);
    EXPECT_EQ(pdr_second, record[1]);

    pldm_pdr_destroy(repo);
}

TEST(FindStateEffecterPDR, testManyNoMatch)
{
    auto repo = pldm_pdr_init();
    uint8_t tid = 1;
    uint16_t entityID = 33;
    uint16_t stateSetId = 196;

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));

    auto rec = new (pdr.data()) pldm_state_effecter_pdr;

    auto state = new (rec->possible_states) state_effecter_possible_states;

    rec->hdr.type = 11;
    rec->hdr.record_handle = 1;
    rec->entity_type = 34;
    rec->container_id = 0;
    rec->composite_effecter_count = 1;
    state->state_set_id = 198;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    std::vector<uint8_t> pdr_second(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));

    auto rec_second = new (pdr_second.data()) pldm_state_effecter_pdr;

    auto state_second = new (rec_second->possible_states)
        state_effecter_possible_states;

    rec_second->hdr.type = 11;
    rec_second->hdr.record_handle = 2;
    rec_second->entity_type = 39;
    rec_second->container_id = 0;
    rec_second->composite_effecter_count = 1;
    state_second->state_set_id = 169;
    state_second->possible_states_size = 1;

    handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr_second.data(), pdr_second.size(), false, 1,
                           &handle),
              0);

    auto record = findStateEffecterPDR(tid, entityID, stateSetId, repo);

    EXPECT_EQ(record.empty(), true);

    pldm_pdr_destroy(repo);
}

TEST(FindStateEffecterPDR, testOneMatchOneNoMatch)
{
    auto repo = pldm_pdr_init();
    uint8_t tid = 1;
    uint16_t entityID = 67;
    uint16_t stateSetId = 192;

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));

    auto rec = new (pdr.data()) pldm_state_effecter_pdr;

    auto state = new (rec->possible_states) state_effecter_possible_states;

    rec->hdr.type = 11;
    rec->hdr.record_handle = 1;
    rec->entity_type = 32;
    rec->container_id = 0;
    rec->composite_effecter_count = 1;
    state->state_set_id = 198;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    std::vector<uint8_t> pdr_second(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));

    auto rec_second = new (pdr_second.data()) pldm_state_effecter_pdr;

    auto state_second = new (rec_second->possible_states)
        state_effecter_possible_states;

    rec_second->hdr.type = 11;
    rec_second->hdr.record_handle = 2;
    rec_second->entity_type = 67;
    rec_second->container_id = 0;
    rec_second->composite_effecter_count = 1;
    state_second->state_set_id = 192;
    state_second->possible_states_size = 1;

    handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr_second.data(), pdr_second.size(), false, 1,
                           &handle),
              0);

    auto record = findStateEffecterPDR(tid, entityID, stateSetId, repo);

    EXPECT_EQ(pdr_second, record[0]);
    EXPECT_EQ(record.size(), 1);

    pldm_pdr_destroy(repo);
}

TEST(FindStateEffecterPDR, testOneMatchManyNoMatch)
{
    auto repo = pldm_pdr_init();
    uint8_t tid = 1;
    uint16_t entityID = 67;
    uint16_t stateSetId = 192;

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));

    auto rec = new (pdr.data()) pldm_state_effecter_pdr;

    auto state = new (rec->possible_states) state_effecter_possible_states;

    rec->hdr.type = 11;
    rec->hdr.record_handle = 1;
    rec->entity_type = 32;
    rec->container_id = 0;
    rec->composite_effecter_count = 1;
    state->state_set_id = 198;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    std::vector<uint8_t> pdr_second(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));

    auto rec_second = new (pdr_second.data()) pldm_state_effecter_pdr;

    auto state_second = new (rec_second->possible_states)
        state_effecter_possible_states;

    rec_second->hdr.type = 11;
    rec_second->hdr.record_handle = 2;
    rec_second->entity_type = 67;
    rec_second->container_id = 0;
    rec_second->composite_effecter_count = 1;
    state_second->state_set_id = 192;
    state_second->possible_states_size = 1;

    handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr_second.data(), pdr_second.size(), false, 1,
                           &handle),
              0);

    std::vector<uint8_t> pdr_third(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));

    auto rec_third = new (pdr_third.data()) pldm_state_effecter_pdr;

    auto state_third = new (rec_third->possible_states)
        state_effecter_possible_states;

    rec_third->hdr.type = 11;
    rec_third->hdr.record_handle = 3;
    rec_third->entity_type = 69;
    rec_third->container_id = 0;
    rec_third->composite_effecter_count = 1;
    state_third->state_set_id = 199;
    state_third->possible_states_size = 1;

    auto record = findStateEffecterPDR(tid, entityID, stateSetId, repo);

    EXPECT_EQ(pdr_second, record[0]);
    EXPECT_EQ(record.size(), 1);

    pldm_pdr_destroy(repo);
}

TEST(FindStateEffecterPDR, testCompositeEffecter)
{
    auto repo = pldm_pdr_init();
    uint8_t tid = 1;
    uint16_t entityID = 67;
    uint16_t stateSetId = 192;

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states) * 3);

    auto rec = new (pdr.data()) pldm_state_effecter_pdr;
    auto state_start = rec->possible_states;

    auto state = new (state_start) state_effecter_possible_states;

    rec->hdr.type = 11;
    rec->hdr.record_handle = 1;
    rec->entity_type = 67;
    rec->container_id = 0;
    rec->composite_effecter_count = 3;
    state->state_set_id = 198;
    state->possible_states_size = 1;

    state_start += state->possible_states_size + sizeof(state->state_set_id) +
                   sizeof(state->possible_states_size);
    state = new (state_start) state_effecter_possible_states;
    state->state_set_id = 193;
    state->possible_states_size = 1;

    state_start += state->possible_states_size + sizeof(state->state_set_id) +
                   sizeof(state->possible_states_size);
    state = new (state_start) state_effecter_possible_states;
    state->state_set_id = 192;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    auto record = findStateEffecterPDR(tid, entityID, stateSetId, repo);

    EXPECT_EQ(pdr, record[0]);

    pldm_pdr_destroy(repo);
}

TEST(FindStateEffecterPDR, testNoMatchCompositeEffecter)
{
    auto repo = pldm_pdr_init();
    uint8_t tid = 1;
    uint16_t entityID = 67;
    uint16_t stateSetId = 192;

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states) * 3);

    auto rec = new (pdr.data()) pldm_state_effecter_pdr;
    auto state_start = rec->possible_states;

    auto state = new (state_start) state_effecter_possible_states;

    rec->hdr.type = 11;
    rec->hdr.record_handle = 1;
    rec->entity_type = 34;
    rec->container_id = 0;
    rec->composite_effecter_count = 3;
    state->state_set_id = 198;
    state->possible_states_size = 1;

    state_start += state->possible_states_size + sizeof(state->state_set_id) +
                   sizeof(state->possible_states_size);
    state = new (state_start) state_effecter_possible_states;
    state->state_set_id = 193;
    state->possible_states_size = 1;

    state_start += state->possible_states_size + sizeof(state->state_set_id) +
                   sizeof(state->possible_states_size);
    state = new (state_start) state_effecter_possible_states;
    state->state_set_id = 123;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    auto record = findStateEffecterPDR(tid, entityID, stateSetId, repo);

    EXPECT_EQ(record.empty(), true);

    pldm_pdr_destroy(repo);
}

TEST(FindStateSensorPDR, testOneMatch)
{
    auto repo = pldm_pdr_init();
    uint8_t tid = 1;
    uint16_t entityID = 5;
    uint16_t stateSetId = 1;

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states));

    auto rec = new (pdr.data()) pldm_state_sensor_pdr;

    auto state = new (rec->possible_states) state_sensor_possible_states;

    rec->hdr.type = 4;
    rec->hdr.record_handle = 1;
    rec->entity_type = 5;
    rec->container_id = 0;
    rec->composite_sensor_count = 1;
    state->state_set_id = 1;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    auto record = findStateSensorPDR(tid, entityID, stateSetId, repo);

    EXPECT_EQ(pdr, record[0]);

    pldm_pdr_destroy(repo);
}

TEST(FindStateSensorPDR, testNoMatch)
{
    auto repo = pldm_pdr_init();
    uint8_t tid = 1;
    uint16_t entityID = 5;
    uint16_t stateSetId = 1;

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states));

    auto rec = new (pdr.data()) pldm_state_sensor_pdr;

    auto state = new (rec->possible_states) state_sensor_possible_states;

    rec->hdr.type = 4;
    rec->hdr.record_handle = 1;
    rec->entity_type = 55;
    rec->container_id = 0;
    rec->composite_sensor_count = 1;
    state->state_set_id = 1;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    auto record = findStateSensorPDR(tid, entityID, stateSetId, repo);

    EXPECT_EQ(record.empty(), true);

    pldm_pdr_destroy(repo);
}

TEST(FindStateSensorPDR, testEmptyRepo)
{
    auto repo = pldm_pdr_init();
    uint8_t tid = 1;
    uint16_t entityID = 5;
    uint16_t stateSetId = 1;

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states));

    auto record = findStateSensorPDR(tid, entityID, stateSetId, repo);

    EXPECT_EQ(record.empty(), true);

    pldm_pdr_destroy(repo);
}

TEST(FindStateSensorPDR, testMoreMatch)
{
    auto repo = pldm_pdr_init();
    uint8_t tid = 1;

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states));

    auto rec = new (pdr.data()) pldm_state_sensor_pdr;

    auto state = new (rec->possible_states) state_sensor_possible_states;

    rec->hdr.type = 4;
    rec->hdr.record_handle = 1;
    rec->entity_type = 5;
    rec->container_id = 0;
    rec->composite_sensor_count = 1;
    state->state_set_id = 1;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    std::vector<uint8_t> pdr_second(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states));

    auto rec_second = new (pdr_second.data()) pldm_state_sensor_pdr;

    auto state_second = new (rec_second->possible_states)
        state_sensor_possible_states;

    rec_second->hdr.type = 4;
    rec_second->hdr.record_handle = 2;
    rec_second->entity_type = 5;
    rec_second->container_id = 0;
    rec_second->composite_sensor_count = 1;
    state_second->state_set_id = 1;
    state_second->possible_states_size = 1;

    handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr_second.data(), pdr_second.size(), false, 1,
                           &handle),
              0);

    uint16_t entityID_ = 5;
    uint16_t stateSetId_ = 1;

    auto record = findStateSensorPDR(tid, entityID_, stateSetId_, repo);

    EXPECT_EQ(pdr, record[0]);
    EXPECT_EQ(pdr_second, record[1]);

    pldm_pdr_destroy(repo);
}

TEST(FindStateSensorPDR, testManyNoMatch)
{
    auto repo = pldm_pdr_init();
    uint8_t tid = 1;
    uint16_t entityID = 5;
    uint16_t stateSetId = 1;

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states));

    auto rec = new (pdr.data()) pldm_state_sensor_pdr;

    auto state = new (rec->possible_states) state_sensor_possible_states;

    rec->hdr.type = 4;
    rec->hdr.record_handle = 1;
    rec->entity_type = 56;
    rec->container_id = 0;
    rec->composite_sensor_count = 1;
    state->state_set_id = 2;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    std::vector<uint8_t> pdr_second(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states));

    auto rec_second = new (pdr_second.data()) pldm_state_sensor_pdr;

    auto state_second = new (rec_second->possible_states)
        state_sensor_possible_states;

    rec_second->hdr.type = 4;
    rec_second->hdr.record_handle = 2;
    rec_second->entity_type = 66;
    rec_second->container_id = 0;
    rec_second->composite_sensor_count = 1;
    state_second->state_set_id = 3;
    state_second->possible_states_size = 1;

    handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr_second.data(), pdr_second.size(), false, 1,
                           &handle),
              0);

    auto record = findStateSensorPDR(tid, entityID, stateSetId, repo);

    EXPECT_EQ(record.empty(), true);

    pldm_pdr_destroy(repo);
}

TEST(FindStateSensorPDR, testOneMatchOneNoMatch)
{
    auto repo = pldm_pdr_init();
    uint8_t tid = 1;
    uint16_t entityID = 5;
    uint16_t stateSetId = 1;

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states));

    auto rec = new (pdr.data()) pldm_state_sensor_pdr;

    auto state = new (rec->possible_states) state_sensor_possible_states;

    rec->hdr.type = 4;
    rec->hdr.record_handle = 1;
    rec->entity_type = 10;
    rec->container_id = 0;
    rec->composite_sensor_count = 1;
    state->state_set_id = 20;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    std::vector<uint8_t> pdr_second(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states));

    auto rec_second = new (pdr_second.data()) pldm_state_sensor_pdr;

    auto state_second = new (rec_second->possible_states)
        state_sensor_possible_states;

    rec_second->hdr.type = 4;
    rec_second->hdr.record_handle = 2;
    rec_second->entity_type = 5;
    rec_second->container_id = 0;
    rec_second->composite_sensor_count = 1;
    state_second->state_set_id = 1;
    state_second->possible_states_size = 1;

    handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr_second.data(), pdr_second.size(), false, 1,
                           &handle),
              0);

    auto record = findStateSensorPDR(tid, entityID, stateSetId, repo);

    EXPECT_EQ(pdr_second, record[0]);
    EXPECT_EQ(record.size(), 1);

    pldm_pdr_destroy(repo);
}

TEST(FindStateSensorPDR, testOneMatchManyNoMatch)
{
    auto repo = pldm_pdr_init();
    uint8_t tid = 1;
    uint16_t entityID = 5;
    uint16_t stateSetId = 1;

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states));

    auto rec = new (pdr.data()) pldm_state_sensor_pdr;

    auto state = new (rec->possible_states) state_sensor_possible_states;

    rec->hdr.type = 4;
    rec->hdr.record_handle = 1;
    rec->entity_type = 6;
    rec->container_id = 0;
    rec->composite_sensor_count = 1;
    state->state_set_id = 9;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    std::vector<uint8_t> pdr_second(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states));

    auto rec_second = new (pdr_second.data()) pldm_state_sensor_pdr;

    auto state_second = new (rec_second->possible_states)
        state_sensor_possible_states;

    rec_second->hdr.type = 4;
    rec_second->hdr.record_handle = 2;
    rec_second->entity_type = 5;
    rec_second->container_id = 0;
    rec_second->composite_sensor_count = 1;
    state_second->state_set_id = 1;
    state_second->possible_states_size = 1;

    handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr_second.data(), pdr_second.size(), false, 1,
                           &handle),
              0);

    std::vector<uint8_t> pdr_third(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states));

    auto rec_third = new (pdr_third.data()) pldm_state_sensor_pdr;

    auto state_third = new (rec_third->possible_states)
        state_sensor_possible_states;

    rec_third->hdr.type = 4;
    rec_third->hdr.record_handle = 3;
    rec_third->entity_type = 7;
    rec_third->container_id = 0;
    rec_third->composite_sensor_count = 1;
    state_third->state_set_id = 12;
    state_third->possible_states_size = 1;

    auto record = findStateSensorPDR(tid, entityID, stateSetId, repo);

    EXPECT_EQ(pdr_second, record[0]);
    EXPECT_EQ(record.size(), 1);

    pldm_pdr_destroy(repo);
}

TEST(FindStateSensorPDR, testCompositeSensor)
{
    auto repo = pldm_pdr_init();
    uint8_t tid = 1;
    uint16_t entityID = 5;
    uint16_t stateSetId = 1;

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states) * 3);

    auto rec = new (pdr.data()) pldm_state_sensor_pdr;
    auto state_start = rec->possible_states;

    auto state = new (state_start) state_sensor_possible_states;

    rec->hdr.type = 4;
    rec->hdr.record_handle = 1;
    rec->entity_type = 5;
    rec->container_id = 0;
    rec->composite_sensor_count = 3;
    state->state_set_id = 2;
    state->possible_states_size = 1;

    state_start += state->possible_states_size + sizeof(state->state_set_id) +
                   sizeof(state->possible_states_size);
    state = new (state_start) state_sensor_possible_states;

    state->state_set_id = 7;
    state->possible_states_size = 1;

    state_start += state->possible_states_size + sizeof(state->state_set_id) +
                   sizeof(state->possible_states_size);
    state = new (state_start) state_sensor_possible_states;

    state->state_set_id = 1;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    auto record = findStateSensorPDR(tid, entityID, stateSetId, repo);

    EXPECT_EQ(pdr, record[0]);

    pldm_pdr_destroy(repo);
}

TEST(FindStateSensorPDR, testNoMatchCompositeSensor)
{
    auto repo = pldm_pdr_init();
    uint8_t tid = 1;
    uint16_t entityID = 5;
    uint16_t stateSetId = 1;

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states) * 3);

    auto rec = new (pdr.data()) pldm_state_sensor_pdr;
    auto state_start = rec->possible_states;

    auto state = new (state_start) state_sensor_possible_states;

    rec->hdr.type = 4;
    rec->hdr.record_handle = 1;
    rec->entity_type = 21;
    rec->container_id = 0;
    rec->composite_sensor_count = 3;
    state->state_set_id = 15;
    state->possible_states_size = 1;

    state_start += state->possible_states_size + sizeof(state->state_set_id) +
                   sizeof(state->possible_states_size);
    state = new (state_start) state_sensor_possible_states;
    state->state_set_id = 19;
    state->possible_states_size = 1;

    state_start += state->possible_states_size + sizeof(state->state_set_id) +
                   sizeof(state->possible_states_size);
    state = new (state_start) state_sensor_possible_states;
    state->state_set_id = 39;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    auto record = findStateSensorPDR(tid, entityID, stateSetId, repo);

    EXPECT_EQ(record.empty(), true);

    pldm_pdr_destroy(repo);
}

TEST(toString, allTestCases)
{
    variable_field buffer{};
    constexpr std::string_view str1{};
    auto returnStr1 = toString(buffer);
    EXPECT_EQ(returnStr1, str1);

    constexpr std::string_view str2{"0penBmc"};
    constexpr std::array<uint8_t, 7> bufferArr1{0x30, 0x70, 0x65, 0x6e,
                                                0x42, 0x6d, 0x63};
    buffer.ptr = bufferArr1.data();
    buffer.length = bufferArr1.size();
    auto returnStr2 = toString(buffer);
    EXPECT_EQ(returnStr2, str2);

    constexpr std::string_view str3{"0pen mc"};
    // \xa0 - the non-breaking space in ISO-8859-1
    constexpr std::array<uint8_t, 7> bufferArr2{0x30, 0x70, 0x65, 0x6e,
                                                0xa0, 0x6d, 0x63};
    buffer.ptr = bufferArr2.data();
    buffer.length = bufferArr2.size();
    auto returnStr3 = toString(buffer);
    EXPECT_EQ(returnStr3, str3);
}

TEST(toString, nullPtrWithLength)
{
    // ptr is null but length > 0 → should still return ""
    variable_field buffer{};
    buffer.ptr = nullptr;
    buffer.length = 5;
    EXPECT_EQ(toString(buffer), "");
}

TEST(toString, nonNullPtrWithZeroLength)
{
    // ptr is non-null but length is 0 → should return ""
    constexpr std::array<uint8_t, 3> arr{0x41, 0x42, 0x43};
    variable_field buffer{};
    buffer.ptr = arr.data();
    buffer.length = 0;
    EXPECT_EQ(toString(buffer), "");
}

TEST(Split, allTestCases)
{
    std::string s1 = "aa,bb,cc,dd";
    auto results1 = split(s1, ",");
    EXPECT_EQ(results1[0], "aa");
    EXPECT_EQ(results1[1], "bb");
    EXPECT_EQ(results1[2], "cc");
    EXPECT_EQ(results1[3], "dd");

    std::string s2 = "aa||bb||cc||dd";
    auto results2 = split(s2, "||");
    EXPECT_EQ(results2[0], "aa");
    EXPECT_EQ(results2[1], "bb");
    EXPECT_EQ(results2[2], "cc");
    EXPECT_EQ(results2[3], "dd");

    std::string s3 = " aa || bb||cc|| dd";
    auto results3 = split(s3, "||", " ");
    EXPECT_EQ(results3[0], "aa");
    EXPECT_EQ(results3[1], "bb");
    EXPECT_EQ(results3[2], "cc");
    EXPECT_EQ(results3[3], "dd");

    std::string s4 = "aa\\\\bb\\cc\\dd";
    auto results4 = split(s4, "\\");
    EXPECT_EQ(results4[0], "aa");
    EXPECT_EQ(results4[1], "bb");
    EXPECT_EQ(results4[2], "cc");
    EXPECT_EQ(results4[3], "dd");

    std::string s5 = "aa\\";
    auto results5 = split(s5, "\\");
    EXPECT_EQ(results5[0], "aa");
}

TEST(ValidEID, allTestCases)
{
    auto rc = isValidEID(MCTP_ADDR_NULL);
    EXPECT_EQ(rc, false);
    rc = isValidEID(MCTP_ADDR_ANY);
    EXPECT_EQ(rc, false);
    rc = isValidEID(1);
    EXPECT_EQ(rc, false);
    rc = isValidEID(2);
    EXPECT_EQ(rc, false);
    rc = isValidEID(3);
    EXPECT_EQ(rc, false);
    rc = isValidEID(4);
    EXPECT_EQ(rc, false);
    rc = isValidEID(5);
    EXPECT_EQ(rc, false);
    rc = isValidEID(6);
    EXPECT_EQ(rc, false);
    rc = isValidEID(7);
    EXPECT_EQ(rc, false);
    rc = isValidEID(pldm::MCTP_START_VALID_EID);
    EXPECT_EQ(rc, true);
    rc = isValidEID(254);
    EXPECT_EQ(rc, true);
}

TEST(ReadOptionalEidProperty, absentKeyReturnsNullopt)
{
    pldm::utils::PropertyMap props;
    EXPECT_EQ(readOptionalEidProperty(props, "StaticEID"), std::nullopt);
}

TEST(ReadOptionalEidProperty, acceptsInRangeIntegralAlternatives)
{
    pldm::utils::PropertyMap props{
        {"u8", uint8_t{12}},   {"u16", uint16_t{34}}, {"u32", uint32_t{56}},
        {"u64", uint64_t{78}}, {"i16", int16_t{90}},  {"i32", int32_t{123}},
        {"i64", int64_t{254}}, {"zero", uint8_t{0}},  {"max", uint16_t{255}},
    };
    EXPECT_EQ(readOptionalEidProperty(props, "u8"), uint8_t{12});
    EXPECT_EQ(readOptionalEidProperty(props, "u16"), uint8_t{34});
    EXPECT_EQ(readOptionalEidProperty(props, "u32"), uint8_t{56});
    EXPECT_EQ(readOptionalEidProperty(props, "u64"), uint8_t{78});
    EXPECT_EQ(readOptionalEidProperty(props, "i16"), uint8_t{90});
    EXPECT_EQ(readOptionalEidProperty(props, "i32"), uint8_t{123});
    EXPECT_EQ(readOptionalEidProperty(props, "i64"), uint8_t{254});
    EXPECT_EQ(readOptionalEidProperty(props, "zero"), uint8_t{0});
    EXPECT_EQ(readOptionalEidProperty(props, "max"), uint8_t{255});
}

TEST(ReadOptionalEidProperty, rejectsOutOfRangeInsteadOfNarrowing)
{
    // 300 would silently narrow to 44, -1 to 255, huge values to arbitrary
    // EIDs; all must be rejected.
    pldm::utils::PropertyMap props{
        {"tooBig", uint16_t{300}},
        {"negative", int32_t{-1}},
        {"huge", uint64_t{0xFFFFFFFFFFFFFF2CULL}},
        {"aboveMax", int64_t{256}},
    };
    EXPECT_EQ(readOptionalEidProperty(props, "tooBig"), std::nullopt);
    EXPECT_EQ(readOptionalEidProperty(props, "negative"), std::nullopt);
    EXPECT_EQ(readOptionalEidProperty(props, "huge"), std::nullopt);
    EXPECT_EQ(readOptionalEidProperty(props, "aboveMax"), std::nullopt);
}

TEST(ReadOptionalEidProperty, parsesDecimalStringsStrictly)
{
    pldm::utils::PropertyMap props{
        {"good", std::string{"42"}},     {"fractional", std::string{"12.9"}},
        {"negative", std::string{"-1"}}, {"tooBig", std::string{"300"}},
        {"junk", std::string{"abc"}},    {"trailing", std::string{"42abc"}},
        {"empty", std::string{""}},
    };
    EXPECT_EQ(readOptionalEidProperty(props, "good"), uint8_t{42});
    EXPECT_EQ(readOptionalEidProperty(props, "fractional"), std::nullopt);
    EXPECT_EQ(readOptionalEidProperty(props, "negative"), std::nullopt);
    EXPECT_EQ(readOptionalEidProperty(props, "tooBig"), std::nullopt);
    EXPECT_EQ(readOptionalEidProperty(props, "junk"), std::nullopt);
    EXPECT_EQ(readOptionalEidProperty(props, "trailing"), std::nullopt);
    EXPECT_EQ(readOptionalEidProperty(props, "empty"), std::nullopt);
}

TEST(ReadOptionalEidProperty, rejectsNonNumericVariantAlternatives)
{
    pldm::utils::PropertyMap props{
        {"boolean", true},
        {"floating", double{12.0}},
        {"bytes", std::vector<uint8_t>{12}},
    };
    EXPECT_EQ(readOptionalEidProperty(props, "boolean"), std::nullopt);
    EXPECT_EQ(readOptionalEidProperty(props, "floating"), std::nullopt);
    EXPECT_EQ(readOptionalEidProperty(props, "bytes"), std::nullopt);
}

TEST(TrimNameForDbus, goodTest)
{
    std::string name = "Name with  space";
    std::string_view expectedName = "Name_with__space";
    std::string_view result = trimNameForDbus(name);
    EXPECT_EQ(expectedName, result);
    name = "Name 1\0"; // NOLINT(bugprone-string-literal-with-embedded-nul)
    expectedName = "Name_1";
    result = trimNameForDbus(name);
    EXPECT_EQ(expectedName, result);
}

TEST(TrimNameForDbus, embeddedNullStripsSuffix)
{
    std::string name{'N', 'a', 'm', 'e', ' ', '1', '\0', 'x', 'y', 'z'};
    std::string_view result = trimNameForDbus(name);

    EXPECT_EQ(result, "Name_1");
    EXPECT_EQ(name.size(), 6u);
}

TEST(DBusMapping, fieldAssignment)
{
    DBusMapping mapping{"/xyz/openbmc_project/example",
                        "xyz.openbmc_project.Example", "Present", "bool"};

    EXPECT_EQ(mapping.objectPath, "/xyz/openbmc_project/example");
    EXPECT_EQ(mapping.interface, "xyz.openbmc_project.Example");
    EXPECT_EQ(mapping.propertyName, "Present");
    EXPECT_EQ(mapping.propertyType, "bool");
}

TEST(DBusHandlerInterface, virtualDestructorViaBasePointer)
{
    pldm::utils::DBusHandlerInterface* interfacePtr =
        new StubDbusHandlerInterface();
    delete interfacePtr;
}

TEST(DBusHandlerTemplate, getDbusPropertySuccess)
{
    MockdBusHandler handler;
    EXPECT_CALL(handler,
                getDbusPropertyVariant("/xyz/openbmc_project/example", "Value",
                                       "xyz.openbmc_project.Example"))
        .WillOnce(::testing::Return(pldm::utils::PropertyValue{uint32_t{42}}));

    auto value = handler.getDbusProperty<uint32_t>(
        "/xyz/openbmc_project/example", "Value", "xyz.openbmc_project.Example");
    EXPECT_EQ(value, 42u);
}

TEST(DBusHandlerTemplate, getDbusPropertyCoversAdditionalScalarTypes)
{
    MockdBusHandler handler;

    EXPECT_CALL(handler, getDbusPropertyVariant("/xyz/openbmc_project/example",
                                                "Present",
                                                "xyz.openbmc_project.Example"))
        .WillOnce(::testing::Return(pldm::utils::PropertyValue{true}));
    EXPECT_TRUE(
        handler.getDbusProperty<bool>("/xyz/openbmc_project/example", "Present",
                                      "xyz.openbmc_project.Example"));

    EXPECT_CALL(handler,
                getDbusPropertyVariant("/xyz/openbmc_project/example", "Byte",
                                       "xyz.openbmc_project.Example"))
        .WillOnce(::testing::Return(pldm::utils::PropertyValue{uint8_t{7}}));
    EXPECT_EQ(
        handler.getDbusProperty<uint8_t>("/xyz/openbmc_project/example", "Byte",
                                         "xyz.openbmc_project.Example"),
        uint8_t{7});
}

TEST(DBusHandlerTemplate, getDbusPropertyCoversAdditionalCollectionTypes)
{
    using Associations =
        std::vector<std::tuple<std::string, std::string, std::string>>;
    using ObjectPaths = std::vector<sdbusplus::object_path>;
    MockdBusHandler handler;

    const std::vector<uint64_t> counters{9, 99};
    EXPECT_CALL(handler, getDbusPropertyVariant("/xyz/openbmc_project/example",
                                                "Counters",
                                                "xyz.openbmc_project.Example"))
        .WillOnce(::testing::Return(pldm::utils::PropertyValue{counters}));
    EXPECT_EQ(handler.getDbusProperty<std::vector<uint64_t>>(
                  "/xyz/openbmc_project/example", "Counters",
                  "xyz.openbmc_project.Example"),
              counters);

    const ObjectPaths objectPaths{
        sdbusplus::object_path("/xyz/openbmc_project/object0"),
        sdbusplus::object_path("/xyz/openbmc_project/object1")};
    EXPECT_CALL(handler, getDbusPropertyVariant("/xyz/openbmc_project/example",
                                                "ObjectPaths",
                                                "xyz.openbmc_project.Example"))
        .WillOnce(::testing::Return(pldm::utils::PropertyValue{objectPaths}));
    EXPECT_EQ(handler.getDbusProperty<ObjectPaths>(
                  "/xyz/openbmc_project/example", "ObjectPaths",
                  "xyz.openbmc_project.Example"),
              objectPaths);

    const Associations associations{
        {"chassis", "all_states",
         "/xyz/openbmc_project/inventory/system/chassis/chassis0"},
        {"pcie_slot", "all_states",
         "/xyz/openbmc_project/inventory/system/chassis/motherboard/slot0"}};
    EXPECT_CALL(handler, getDbusPropertyVariant("/xyz/openbmc_project/example",
                                                "Associations",
                                                "xyz.openbmc_project.Example"))
        .WillOnce(::testing::Return(pldm::utils::PropertyValue{associations}));
    EXPECT_EQ(handler.getDbusProperty<Associations>(
                  "/xyz/openbmc_project/example", "Associations",
                  "xyz.openbmc_project.Example"),
              associations);
}

TEST(DBusHandlerTemplate, getDbusPropertyCoversRemainingTemplateTypes)
{
    MockdBusHandler handler;

    EXPECT_CALL(handler,
                getDbusPropertyVariant("/xyz/openbmc_project/example", "I16",
                                       "xyz.openbmc_project.Example"))
        .WillOnce(::testing::Return(pldm::utils::PropertyValue{int16_t{-12}}));
    EXPECT_EQ(
        handler.getDbusProperty<int16_t>("/xyz/openbmc_project/example", "I16",
                                         "xyz.openbmc_project.Example"),
        int16_t{-12});

    EXPECT_CALL(handler,
                getDbusPropertyVariant("/xyz/openbmc_project/example", "U16",
                                       "xyz.openbmc_project.Example"))
        .WillOnce(::testing::Return(pldm::utils::PropertyValue{uint16_t{34}}));
    EXPECT_EQ(
        handler.getDbusProperty<uint16_t>("/xyz/openbmc_project/example", "U16",
                                          "xyz.openbmc_project.Example"),
        uint16_t{34});

    EXPECT_CALL(handler,
                getDbusPropertyVariant("/xyz/openbmc_project/example", "I32",
                                       "xyz.openbmc_project.Example"))
        .WillOnce(
            ::testing::Return(pldm::utils::PropertyValue{int32_t{-5678}}));
    EXPECT_EQ(
        handler.getDbusProperty<int32_t>("/xyz/openbmc_project/example", "I32",
                                         "xyz.openbmc_project.Example"),
        int32_t{-5678});

    EXPECT_CALL(handler,
                getDbusPropertyVariant("/xyz/openbmc_project/example", "I64",
                                       "xyz.openbmc_project.Example"))
        .WillOnce(
            ::testing::Return(pldm::utils::PropertyValue{int64_t{-987654321}}));
    EXPECT_EQ(
        handler.getDbusProperty<int64_t>("/xyz/openbmc_project/example", "I64",
                                         "xyz.openbmc_project.Example"),
        int64_t{-987654321});

    EXPECT_CALL(handler,
                getDbusPropertyVariant("/xyz/openbmc_project/example", "Double",
                                       "xyz.openbmc_project.Example"))
        .WillOnce(::testing::Return(pldm::utils::PropertyValue{42.25}));
    EXPECT_DOUBLE_EQ(handler.getDbusProperty<double>(
                         "/xyz/openbmc_project/example", "Double",
                         "xyz.openbmc_project.Example"),
                     42.25);

    const std::vector<std::string> parents{
        "/xyz/openbmc_project/inventory/system/chassis/chassis0",
        "/xyz/openbmc_project/inventory/system/chassis/chassis1"};
    EXPECT_CALL(handler, getDbusPropertyVariant("/xyz/openbmc_project/example",
                                                "Parents",
                                                "xyz.openbmc_project.Example"))
        .WillOnce(::testing::Return(pldm::utils::PropertyValue{parents}));
    EXPECT_EQ(handler.getDbusProperty<std::vector<std::string>>(
                  "/xyz/openbmc_project/example", "Parents",
                  "xyz.openbmc_project.Example"),
              parents);

    const std::vector<uint8_t> rawData{0x10, 0x20, 0x30, 0x40};
    EXPECT_CALL(handler, getDbusPropertyVariant("/xyz/openbmc_project/example",
                                                "RawData",
                                                "xyz.openbmc_project.Example"))
        .WillOnce(::testing::Return(pldm::utils::PropertyValue{rawData}));
    EXPECT_EQ(handler.getDbusProperty<std::vector<uint8_t>>(
                  "/xyz/openbmc_project/example", "RawData",
                  "xyz.openbmc_project.Example"),
              rawData);
}

TEST(DBusHandlerTemplate, getDbusPropertyThrowsOnVariantMismatch)
{
    MockdBusHandler handler;
    EXPECT_CALL(handler,
                getDbusPropertyVariant("/xyz/openbmc_project/example", "Value",
                                       "xyz.openbmc_project.Example"))
        .WillOnce(::testing::Return(
            pldm::utils::PropertyValue{std::string("wrong type")}));

    EXPECT_THROW((handler.getDbusProperty<uint64_t>(
                     "/xyz/openbmc_project/example", "Value",
                     "xyz.openbmc_project.Example")),
                 std::bad_variant_access);
}

TEST(DBusHandlerTemplate,
     getDbusPropertyThrowsOnRemainingScalarAndAssociationMismatches)
{
    using Associations =
        std::vector<std::tuple<std::string, std::string, std::string>>;
    MockdBusHandler handler;

    EXPECT_CALL(handler,
                getDbusPropertyVariant("/xyz/openbmc_project/example", "I16",
                                       "xyz.openbmc_project.Example"))
        .WillOnce(
            ::testing::Return(pldm::utils::PropertyValue{std::string("bad")}));
    EXPECT_THROW(
        (handler.getDbusProperty<int16_t>("/xyz/openbmc_project/example", "I16",
                                          "xyz.openbmc_project.Example")),
        std::bad_variant_access);

    EXPECT_CALL(handler,
                getDbusPropertyVariant("/xyz/openbmc_project/example", "U16",
                                       "xyz.openbmc_project.Example"))
        .WillOnce(
            ::testing::Return(pldm::utils::PropertyValue{std::string("bad")}));
    EXPECT_THROW((handler.getDbusProperty<uint16_t>(
                     "/xyz/openbmc_project/example", "U16",
                     "xyz.openbmc_project.Example")),
                 std::bad_variant_access);

    EXPECT_CALL(handler,
                getDbusPropertyVariant("/xyz/openbmc_project/example", "I32",
                                       "xyz.openbmc_project.Example"))
        .WillOnce(
            ::testing::Return(pldm::utils::PropertyValue{std::string("bad")}));
    EXPECT_THROW(
        (handler.getDbusProperty<int32_t>("/xyz/openbmc_project/example", "I32",
                                          "xyz.openbmc_project.Example")),
        std::bad_variant_access);

    EXPECT_CALL(handler,
                getDbusPropertyVariant("/xyz/openbmc_project/example", "U32",
                                       "xyz.openbmc_project.Example"))
        .WillOnce(
            ::testing::Return(pldm::utils::PropertyValue{std::string("bad")}));
    EXPECT_THROW((handler.getDbusProperty<uint32_t>(
                     "/xyz/openbmc_project/example", "U32",
                     "xyz.openbmc_project.Example")),
                 std::bad_variant_access);

    EXPECT_CALL(handler,
                getDbusPropertyVariant("/xyz/openbmc_project/example", "I64",
                                       "xyz.openbmc_project.Example"))
        .WillOnce(
            ::testing::Return(pldm::utils::PropertyValue{std::string("bad")}));
    EXPECT_THROW(
        (handler.getDbusProperty<int64_t>("/xyz/openbmc_project/example", "I64",
                                          "xyz.openbmc_project.Example")),
        std::bad_variant_access);

    EXPECT_CALL(handler,
                getDbusPropertyVariant("/xyz/openbmc_project/example", "U64",
                                       "xyz.openbmc_project.Example"))
        .WillOnce(
            ::testing::Return(pldm::utils::PropertyValue{std::string("bad")}));
    EXPECT_THROW((handler.getDbusProperty<uint64_t>(
                     "/xyz/openbmc_project/example", "U64",
                     "xyz.openbmc_project.Example")),
                 std::bad_variant_access);

    EXPECT_CALL(handler,
                getDbusPropertyVariant("/xyz/openbmc_project/example", "Double",
                                       "xyz.openbmc_project.Example"))
        .WillOnce(
            ::testing::Return(pldm::utils::PropertyValue{std::string("bad")}));
    EXPECT_THROW((handler.getDbusProperty<double>(
                     "/xyz/openbmc_project/example", "Double",
                     "xyz.openbmc_project.Example")),
                 std::bad_variant_access);

    EXPECT_CALL(handler, getDbusPropertyVariant("/xyz/openbmc_project/example",
                                                "Associations",
                                                "xyz.openbmc_project.Example"))
        .WillOnce(
            ::testing::Return(pldm::utils::PropertyValue{std::string("bad")}));
    EXPECT_THROW((handler.getDbusProperty<Associations>(
                     "/xyz/openbmc_project/example", "Associations",
                     "xyz.openbmc_project.Example")),
                 std::bad_variant_access);
}

TEST(DBusHandlerStatic, getAsioConnectionReturnsPointer)
{
    auto& conn = pldm::utils::DBusHandler::getAsioConnection();
    EXPECT_NE(conn.get(), nullptr);
    auto* first = conn.get();
    auto& second = pldm::utils::DBusHandler::getAsioConnection();
    EXPECT_EQ(first, second.get());
}

TEST(DBusHandlerStatic, getBusReturnsStableReference)
{
    auto& first = pldm::utils::DBusHandler::getBus();
    auto* firstPtr = &first;
    auto& second = pldm::utils::DBusHandler::getBus();
    EXPECT_EQ(firstPtr, &second);
}

TEST_F(DBusHandlerBusMockTest, getServiceParsesMapperReply)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* interface = "xyz.openbmc_project.Example";
    constexpr auto* service = "xyz.openbmc_project.ExampleService";

    testing::InSequence seq;
    expectNewMethodCall(ObjectMapper::default_service,
                        ObjectMapper::instance_path, ObjectMapper::interface,
                        "GetObject");
    expectAppendString(SD_BUS_TYPE_STRING, objectPath);
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
    expectAppendString(SD_BUS_TYPE_STRING, interface);
    expectCloseContainer();
    expectBusCallWithReply();
    expectReadOneServiceMapEntry(service, interface);

    auto resolvedService = handler.getService(objectPath, interface);
    EXPECT_EQ(resolvedService, service);
}

TEST_F(DBusHandlerBusMockTest, getSubtreeParsesEmptyReply)
{
    constexpr auto* searchPath = "/xyz/openbmc_project";
    constexpr int depth = 0;

    testing::InSequence seq;
    expectNewMethodCall(ObjectMapper::default_service,
                        ObjectMapper::instance_path, ObjectMapper::interface,
                        "GetSubTree");
    expectAppendString(SD_BUS_TYPE_STRING, searchPath);
    expectAppendBasic<int>(SD_BUS_TYPE_INT32, depth);
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
    expectCloseContainer();
    expectBusCallWithReply();
    expectReadEmptyMapLikeContainer();

    auto response = handler.getSubtree(searchPath, depth, {});
    EXPECT_TRUE(response.empty());
}

TEST_F(DBusHandlerBusMockTest, getSubTreePathsParsesReply)
{
    constexpr auto* objectPath = "/xyz/openbmc_project";
    constexpr auto* subtreePath = "/xyz/openbmc_project/example0";
    constexpr int depth = 0;

    testing::InSequence seq;
    expectNewMethodCall(ObjectMapper::default_service,
                        ObjectMapper::instance_path, ObjectMapper::interface,
                        "GetSubTreePaths");
    expectAppendString(SD_BUS_TYPE_STRING, objectPath);
    expectAppendBasic<int>(SD_BUS_TYPE_INT32, depth);
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
    expectCloseContainer();
    expectBusCallWithReply();
    expectReadOnePathVectorEntry(subtreePath);

    auto paths = handler.getSubTreePaths(objectPath, depth, {});
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(paths.front(), subtreePath);
}

TEST_F(DBusHandlerBusMockTest, getAncestorsParsesEmptyReply)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";

    testing::InSequence seq;
    expectNewMethodCall(ObjectMapper::default_service,
                        ObjectMapper::instance_path, ObjectMapper::interface,
                        "GetAncestors");
    expectAppendString(SD_BUS_TYPE_STRING, objectPath);
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
    expectCloseContainer();
    expectBusCallWithReply();
    expectReadEmptyMapLikeContainer();

    auto response = handler.getAncestors(objectPath, {});
    EXPECT_TRUE(response.empty());
}

TEST_F(DBusHandlerBusMockTest, getAssociatedSubTreeParsesEmptyReply)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* subtreePath = "/xyz/openbmc_project/inventory";
    constexpr int depth = 0;

    testing::InSequence seq;
    expectNewMethodCall(ObjectMapper::default_service,
                        ObjectMapper::instance_path, ObjectMapper::interface,
                        "GetAssociatedSubTree");
    expectAppendString(SD_BUS_TYPE_OBJECT_PATH, objectPath);
    expectAppendString(SD_BUS_TYPE_OBJECT_PATH, subtreePath);
    expectAppendBasic<int>(SD_BUS_TYPE_INT32, depth);
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
    expectCloseContainer();
    expectBusCallWithReply();
    expectReadEmptyMapLikeContainer();

    auto response = handler.getAssociatedSubTree(
        sdbusplus::object_path(objectPath), sdbusplus::object_path(subtreePath),
        depth, {});
    EXPECT_TRUE(response.empty());
}

TEST_F(DBusHandlerBusMockTest, getDbusPropertiesVariantParsesEmptyReply)
{
    constexpr auto* service = "xyz.openbmc_project.Example";
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* interface = "xyz.openbmc_project.Example";

    testing::InSequence seq;
    expectNewMethodCall(service, objectPath, dbusProperties, "GetAll");
    expectAppendString(SD_BUS_TYPE_STRING, interface);
    expectBusCallWithReply();
    expectReadEmptyMapLikeContainer();

    auto response =
        handler.getDbusPropertiesVariant(service, objectPath, interface);
    EXPECT_TRUE(response.empty());
}

TEST_F(DBusHandlerBusMockTest,
       checkDbusPropertyVariantReturnsFalseWhenPropertyMissing)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* interface = "xyz.openbmc_project.Example";
    constexpr auto* service = "xyz.openbmc_project.ExampleService";

    testing::InSequence seq;
    expectNewMethodCall(ObjectMapper::default_service,
                        ObjectMapper::instance_path, ObjectMapper::interface,
                        "GetObject");
    expectAppendString(SD_BUS_TYPE_STRING, objectPath);
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
    expectAppendString(SD_BUS_TYPE_STRING, interface);
    expectCloseContainer();
    expectBusCallWithReply();
    expectReadOneServiceMapEntry(service, interface);

    expectNewMethodCall(service, objectPath, dbusProperties, "GetAll");
    expectAppendString(SD_BUS_TYPE_STRING, interface);
    expectBusCallWithReply(0);
    expectReadEmptyMapLikeContainer();

    auto present = handler.checkDbusPropertyVariant(
        objectPath, "MissingProperty", interface);
    EXPECT_FALSE(present);
}

TEST_F(DBusHandlerBusMockTest, setDbusPropertyBoolBuildsExpectedMessage)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* interface = "xyz.openbmc_project.Example";
    constexpr auto* service = "xyz.openbmc_project.ExampleService";
    constexpr auto* propertyName = "Enabled";
    constexpr int depth = 1;

    testing::InSequence seq;
    expectNewMethodCall(ObjectMapper::default_service,
                        ObjectMapper::instance_path, ObjectMapper::interface,
                        "GetObject");
    expectAppendString(SD_BUS_TYPE_STRING, objectPath);
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
    expectAppendString(SD_BUS_TYPE_STRING, interface);
    expectCloseContainer();
    expectBusCallWithReply();
    expectReadOneServiceMapEntry(service, interface);

    expectNewMethodCall(service, objectPath, dbusProperties, "Set");
    expectAppendString(SD_BUS_TYPE_STRING, interface);
    expectAppendString(SD_BUS_TYPE_STRING, propertyName);
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "b");
    expectAppendBasic<int>(SD_BUS_TYPE_BOOLEAN, depth);
    expectCloseContainer();
    expectBusCallNoReply();

    DBusMapping mapping{objectPath, interface, propertyName, "bool"};
    handler.setDbusProperty(mapping, PropertyValue{true});
}

TEST_F(DBusHandlerBusMockTest, setDbusPropertyUint8BuildsExpectedMessage)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* interface = "xyz.openbmc_project.Example";
    constexpr auto* service = "xyz.openbmc_project.ExampleService";
    constexpr auto* propertyName = "ByteValue";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectSetPropertyCall(service, objectPath, interface, propertyName);
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "y");
    expectAppendBasic<uint8_t>(SD_BUS_TYPE_BYTE, uint8_t(42));
    expectCloseContainer();
    expectBusCallNoReply();

    DBusMapping mapping{objectPath, interface, propertyName, "uint8_t"};
    handler.setDbusProperty(mapping, PropertyValue{uint8_t(42)});
}

TEST_F(DBusHandlerBusMockTest, setDbusPropertyInt16BuildsExpectedMessage)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* interface = "xyz.openbmc_project.Example";
    constexpr auto* service = "xyz.openbmc_project.ExampleService";
    constexpr auto* propertyName = "Int16Value";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectSetPropertyCall(service, objectPath, interface, propertyName);
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "n");
    expectAppendBasic<int16_t>(SD_BUS_TYPE_INT16, int16_t(-100));
    expectCloseContainer();
    expectBusCallNoReply();

    DBusMapping mapping{objectPath, interface, propertyName, "int16_t"};
    handler.setDbusProperty(mapping, PropertyValue{int16_t(-100)});
}

TEST_F(DBusHandlerBusMockTest, setDbusPropertyUint16BuildsExpectedMessage)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* interface = "xyz.openbmc_project.Example";
    constexpr auto* service = "xyz.openbmc_project.ExampleService";
    constexpr auto* propertyName = "Uint16Value";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectSetPropertyCall(service, objectPath, interface, propertyName);
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "q");
    expectAppendBasic<uint16_t>(SD_BUS_TYPE_UINT16, uint16_t(1000));
    expectCloseContainer();
    expectBusCallNoReply();

    DBusMapping mapping{objectPath, interface, propertyName, "uint16_t"};
    handler.setDbusProperty(mapping, PropertyValue{uint16_t(1000)});
}

TEST_F(DBusHandlerBusMockTest, setDbusPropertyInt32BuildsExpectedMessage)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* interface = "xyz.openbmc_project.Example";
    constexpr auto* service = "xyz.openbmc_project.ExampleService";
    constexpr auto* propertyName = "Int32Value";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectSetPropertyCall(service, objectPath, interface, propertyName);
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "i");
    expectAppendBasic<int32_t>(SD_BUS_TYPE_INT32, int32_t(-100000));
    expectCloseContainer();
    expectBusCallNoReply();

    DBusMapping mapping{objectPath, interface, propertyName, "int32_t"};
    handler.setDbusProperty(mapping, PropertyValue{int32_t(-100000)});
}

TEST_F(DBusHandlerBusMockTest, setDbusPropertyUint32BuildsExpectedMessage)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* interface = "xyz.openbmc_project.Example";
    constexpr auto* service = "xyz.openbmc_project.ExampleService";
    constexpr auto* propertyName = "Uint32Value";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectSetPropertyCall(service, objectPath, interface, propertyName);
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "u");
    expectAppendBasic<uint32_t>(SD_BUS_TYPE_UINT32, uint32_t(100000));
    expectCloseContainer();
    expectBusCallNoReply();

    DBusMapping mapping{objectPath, interface, propertyName, "uint32_t"};
    handler.setDbusProperty(mapping, PropertyValue{uint32_t(100000)});
}

TEST_F(DBusHandlerBusMockTest, setDbusPropertyInt64BuildsExpectedMessage)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* interface = "xyz.openbmc_project.Example";
    constexpr auto* service = "xyz.openbmc_project.ExampleService";
    constexpr auto* propertyName = "Int64Value";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectSetPropertyCall(service, objectPath, interface, propertyName);
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "x");
    expectAppendBasic<int64_t>(SD_BUS_TYPE_INT64, int64_t(-1000000000));
    expectCloseContainer();
    expectBusCallNoReply();

    DBusMapping mapping{objectPath, interface, propertyName, "int64_t"};
    handler.setDbusProperty(mapping, PropertyValue{int64_t(-1000000000)});
}

TEST_F(DBusHandlerBusMockTest, setDbusPropertyUint64BuildsExpectedMessage)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* interface = "xyz.openbmc_project.Example";
    constexpr auto* service = "xyz.openbmc_project.ExampleService";
    constexpr auto* propertyName = "Uint64Value";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectSetPropertyCall(service, objectPath, interface, propertyName);
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "t");
    expectAppendBasic<uint64_t>(SD_BUS_TYPE_UINT64, uint64_t(1000000000));
    expectCloseContainer();
    expectBusCallNoReply();

    DBusMapping mapping{objectPath, interface, propertyName, "uint64_t"};
    handler.setDbusProperty(mapping, PropertyValue{uint64_t(1000000000)});
}

TEST_F(DBusHandlerBusMockTest, setDbusPropertyDoubleBuildsExpectedMessage)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* interface = "xyz.openbmc_project.Example";
    constexpr auto* service = "xyz.openbmc_project.ExampleService";
    constexpr auto* propertyName = "DoubleValue";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectSetPropertyCall(service, objectPath, interface, propertyName);
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "d");
    expectAppendBasic<double>(SD_BUS_TYPE_DOUBLE, 42.5);
    expectCloseContainer();
    expectBusCallNoReply();

    DBusMapping mapping{objectPath, interface, propertyName, "double"};
    handler.setDbusProperty(mapping, PropertyValue{double(42.5)});
}

TEST_F(DBusHandlerBusMockTest, setDbusPropertyStringBuildsExpectedMessage)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* interface = "xyz.openbmc_project.Example";
    constexpr auto* service = "xyz.openbmc_project.ExampleService";
    constexpr auto* propertyName = "StringValue";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectSetPropertyCall(service, objectPath, interface, propertyName);
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "s");
    expectAppendString(SD_BUS_TYPE_STRING, "hello");
    expectCloseContainer();
    expectBusCallNoReply();

    DBusMapping mapping{objectPath, interface, propertyName, "string"};
    handler.setDbusProperty(mapping, PropertyValue{std::string("hello")});
}

TEST_F(DBusHandlerBusMockTest, setDbusPropertyArrayStringBuildsExpectedMessage)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* interface = "xyz.openbmc_project.Example";
    constexpr auto* service = "xyz.openbmc_project.ExampleService";
    constexpr auto* propertyName = "StringArray";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectSetPropertyCall(service, objectPath, interface, propertyName);
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "as");
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
    expectAppendString(SD_BUS_TYPE_STRING, "hello");
    expectAppendString(SD_BUS_TYPE_STRING, "world");
    expectCloseContainer();
    expectCloseContainer();
    expectBusCallNoReply();

    DBusMapping mapping{objectPath, interface, propertyName, "array[string]"};
    handler.setDbusProperty(
        mapping, PropertyValue{std::vector<std::string>{"hello", "world"}});
}

TEST_F(DBusHandlerBusMockTest,
       setDbusPropertyArrayObjectPathBuildsExpectedMessage)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* interface = "xyz.openbmc_project.Example";
    constexpr auto* service = "xyz.openbmc_project.ExampleService";
    constexpr auto* propertyName = "PathArray";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectSetPropertyCall(service, objectPath, interface, propertyName);
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "ao");
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "o");
    expectAppendString(SD_BUS_TYPE_OBJECT_PATH, "/test/a");
    expectAppendString(SD_BUS_TYPE_OBJECT_PATH, "/test/b");
    expectCloseContainer();
    expectCloseContainer();
    expectBusCallNoReply();

    DBusMapping mapping{objectPath, interface, propertyName,
                        "array[object_path]"};
    handler.setDbusProperty(mapping,
                            PropertyValue{std::vector<sdbusplus::object_path>{
                                sdbusplus::object_path("/test/a"),
                                sdbusplus::object_path("/test/b")}});
}

TEST_F(DBusHandlerBusMockTest, setDbusPropertyUnsupportedTypeThrows)
{
    DBusMapping mapping{"/path", "iface", "prop", "unknown"};
    PropertyValue value{uint8_t(0)};
    EXPECT_THROW(handler.setDbusProperty(mapping, value),
                 std::invalid_argument);
}

// ---------------------------------------------------------------------------
// DBusHandlerDirectTest: exercises REAL DBusHandler functions in
// common/utils.cpp by swapping a mock bus into the static returned by
// DBusHandler::getBus().
// ---------------------------------------------------------------------------

class DBusHandlerDirectTest : public DBusMockTestHelpers
{
  protected:
    DBusHandler directHandler;
    std::optional<sdbusplus::bus_t> savedBus;
    bool busSwapped = false;

    void SetUp() override
    {
        try
        {
            auto& busRef = DBusHandler::getBus();
            auto mockBus = sdbusplus::get_mocked_new(&mock);
            savedBus.emplace(std::move(busRef));
            busRef = std::move(mockBus);
            busSwapped = true;
        }
        catch (...)
        {
            GTEST_SKIP() << "System bus not available for direct testing";
        }
    }

    void TearDown() override
    {
        if (busSwapped)
        {
            DBusHandler::getBus() = std::move(*savedBus);
            savedBus.reset();
        }
    }
};

TEST_F(DBusHandlerDirectTest, uint8)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/test";
    constexpr auto* interface = "xyz.openbmc_project.Test";
    constexpr auto* service = "xyz.openbmc_project.TestService";
    constexpr auto* propertyName = "ByteVal";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectSetPropertyCall(service, objectPath, interface, propertyName);
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "y");
    expectAppendBasic<uint8_t>('y', uint8_t(42));
    expectCloseContainer();
    expectBusCallNoReply();

    DBusMapping mapping{objectPath, interface, propertyName, "uint8_t"};
    directHandler.setDbusProperty(mapping, PropertyValue{uint8_t(42)});
}

TEST_F(DBusHandlerDirectTest, boolTrue)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/test";
    constexpr auto* interface = "xyz.openbmc_project.Test";
    constexpr auto* service = "xyz.openbmc_project.TestService";
    constexpr auto* propertyName = "Active";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectSetPropertyCall(service, objectPath, interface, propertyName);
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "b");
    expectAppendBasic<int>(SD_BUS_TYPE_BOOLEAN, true);
    expectCloseContainer();
    expectBusCallNoReply();

    DBusMapping mapping{objectPath, interface, propertyName, "bool"};
    directHandler.setDbusProperty(mapping, PropertyValue{true});
}

TEST_F(DBusHandlerDirectTest, int16)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/test";
    constexpr auto* interface = "xyz.openbmc_project.Test";
    constexpr auto* service = "xyz.openbmc_project.TestService";
    constexpr auto* propertyName = "SmallSigned";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectSetPropertyCall(service, objectPath, interface, propertyName);
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "n");
    expectAppendBasic<int16_t>('n', int16_t(-100));
    expectCloseContainer();
    expectBusCallNoReply();

    DBusMapping mapping{objectPath, interface, propertyName, "int16_t"};
    directHandler.setDbusProperty(mapping, PropertyValue{int16_t(-100)});
}

TEST_F(DBusHandlerDirectTest, uint16)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/test";
    constexpr auto* interface = "xyz.openbmc_project.Test";
    constexpr auto* service = "xyz.openbmc_project.TestService";
    constexpr auto* propertyName = "Port";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectSetPropertyCall(service, objectPath, interface, propertyName);
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "q");
    expectAppendBasic<uint16_t>('q', uint16_t(8080));
    expectCloseContainer();
    expectBusCallNoReply();

    DBusMapping mapping{objectPath, interface, propertyName, "uint16_t"};
    directHandler.setDbusProperty(mapping, PropertyValue{uint16_t(8080)});
}

TEST_F(DBusHandlerDirectTest, int32)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/test";
    constexpr auto* interface = "xyz.openbmc_project.Test";
    constexpr auto* service = "xyz.openbmc_project.TestService";
    constexpr auto* propertyName = "SignedVal";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectSetPropertyCall(service, objectPath, interface, propertyName);
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "i");
    expectAppendBasic<int32_t>('i', int32_t(-50000));
    expectCloseContainer();
    expectBusCallNoReply();

    DBusMapping mapping{objectPath, interface, propertyName, "int32_t"};
    directHandler.setDbusProperty(mapping, PropertyValue{int32_t(-50000)});
}

TEST_F(DBusHandlerDirectTest, uint32)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/test";
    constexpr auto* interface = "xyz.openbmc_project.Test";
    constexpr auto* service = "xyz.openbmc_project.TestService";
    constexpr auto* propertyName = "UnsignedVal";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectSetPropertyCall(service, objectPath, interface, propertyName);
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "u");
    expectAppendBasic<uint32_t>('u', uint32_t(100000));
    expectCloseContainer();
    expectBusCallNoReply();

    DBusMapping mapping{objectPath, interface, propertyName, "uint32_t"};
    directHandler.setDbusProperty(mapping, PropertyValue{uint32_t(100000)});
}

TEST_F(DBusHandlerDirectTest, int64)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/test";
    constexpr auto* interface = "xyz.openbmc_project.Test";
    constexpr auto* service = "xyz.openbmc_project.TestService";
    constexpr auto* propertyName = "BigSigned";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectSetPropertyCall(service, objectPath, interface, propertyName);
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "x");
    expectAppendBasic<int64_t>('x', int64_t(-1000000));
    expectCloseContainer();
    expectBusCallNoReply();

    DBusMapping mapping{objectPath, interface, propertyName, "int64_t"};
    directHandler.setDbusProperty(mapping, PropertyValue{int64_t(-1000000)});
}

TEST_F(DBusHandlerDirectTest, uint64)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/test";
    constexpr auto* interface = "xyz.openbmc_project.Test";
    constexpr auto* service = "xyz.openbmc_project.TestService";
    constexpr auto* propertyName = "BigUnsigned";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectSetPropertyCall(service, objectPath, interface, propertyName);
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "t");
    expectAppendBasic<uint64_t>('t', uint64_t(1000000));
    expectCloseContainer();
    expectBusCallNoReply();

    DBusMapping mapping{objectPath, interface, propertyName, "uint64_t"};
    directHandler.setDbusProperty(mapping, PropertyValue{uint64_t(1000000)});
}

TEST_F(DBusHandlerDirectTest, doubleVal)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/test";
    constexpr auto* interface = "xyz.openbmc_project.Test";
    constexpr auto* service = "xyz.openbmc_project.TestService";
    constexpr auto* propertyName = "Temperature";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectSetPropertyCall(service, objectPath, interface, propertyName);
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "d");
    expectAppendBasic<double>('d', 36.6);
    expectCloseContainer();
    expectBusCallNoReply();

    DBusMapping mapping{objectPath, interface, propertyName, "double"};
    directHandler.setDbusProperty(mapping, PropertyValue{36.6});
}

TEST_F(DBusHandlerDirectTest, stringVal)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/test";
    constexpr auto* interface = "xyz.openbmc_project.Test";
    constexpr auto* service = "xyz.openbmc_project.TestService";
    constexpr auto* propertyName = "Name";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectSetPropertyCall(service, objectPath, interface, propertyName);
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "s");
    expectAppendString(SD_BUS_TYPE_STRING, "hello");
    expectCloseContainer();
    expectBusCallNoReply();

    DBusMapping mapping{objectPath, interface, propertyName, "string"};
    directHandler.setDbusProperty(mapping, PropertyValue{std::string("hello")});
}

TEST_F(DBusHandlerDirectTest, arrayString)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/test";
    constexpr auto* interface = "xyz.openbmc_project.Test";
    constexpr auto* service = "xyz.openbmc_project.TestService";
    constexpr auto* propertyName = "Tags";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectSetPropertyCall(service, objectPath, interface, propertyName);
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "as");
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
    expectAppendString(SD_BUS_TYPE_STRING, "tag1");
    expectAppendString(SD_BUS_TYPE_STRING, "tag2");
    expectCloseContainer();
    expectCloseContainer();
    expectBusCallNoReply();

    DBusMapping mapping{objectPath, interface, propertyName, "array[string]"};
    directHandler.setDbusProperty(
        mapping, PropertyValue{std::vector<std::string>{"tag1", "tag2"}});
}

TEST_F(DBusHandlerDirectTest, arrayObjectPath)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/test";
    constexpr auto* interface = "xyz.openbmc_project.Test";
    constexpr auto* service = "xyz.openbmc_project.TestService";
    constexpr auto* propertyName = "Paths";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectSetPropertyCall(service, objectPath, interface, propertyName);
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "ao");
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "o");
    expectAppendString(SD_BUS_TYPE_OBJECT_PATH, "/obj/a");
    expectAppendString(SD_BUS_TYPE_OBJECT_PATH, "/obj/b");
    expectCloseContainer();
    expectCloseContainer();
    expectBusCallNoReply();

    DBusMapping mapping{objectPath, interface, propertyName,
                        "array[object_path]"};
    directHandler.setDbusProperty(
        mapping, PropertyValue{std::vector<sdbusplus::object_path>{
                     sdbusplus::object_path("/obj/a"),
                     sdbusplus::object_path("/obj/b")}});
}

TEST_F(DBusHandlerDirectTest, unsupportedTypeThrows)
{
    DBusMapping mapping{"/path", "iface", "prop", "unknown"};
    PropertyValue value{uint8_t(0)};
    EXPECT_THROW(directHandler.setDbusProperty(mapping, value),
                 std::invalid_argument);
}

TEST_F(DBusHandlerDirectTest, setDbusPropertyVariantTypeMismatchThrows)
{
    DBusMapping mapping{"/path", "iface", "prop", "int16_t"};
    PropertyValue wrongTypeValue{uint16_t(7)};
    EXPECT_THROW(directHandler.setDbusProperty(mapping, wrongTypeValue),
                 std::bad_variant_access);
}

TEST_F(DBusHandlerDirectTest, setDbusPropertyVariantTypeMismatchMatrixThrows)
{
    EXPECT_THROW(
        directHandler.setDbusProperty({"/path", "iface", "prop", "uint8_t"},
                                      PropertyValue{uint16_t(7)}),
        std::bad_variant_access);
    EXPECT_THROW(
        directHandler.setDbusProperty({"/path", "iface", "prop", "bool"},
                                      PropertyValue{uint8_t(1)}),
        std::bad_variant_access);
    EXPECT_THROW(
        directHandler.setDbusProperty({"/path", "iface", "prop", "uint16_t"},
                                      PropertyValue{int16_t(-1)}),
        std::bad_variant_access);
    EXPECT_THROW(
        directHandler.setDbusProperty({"/path", "iface", "prop", "int32_t"},
                                      PropertyValue{uint32_t(1)}),
        std::bad_variant_access);
    EXPECT_THROW(
        directHandler.setDbusProperty({"/path", "iface", "prop", "uint32_t"},
                                      PropertyValue{int32_t(-1)}),
        std::bad_variant_access);
    EXPECT_THROW(
        directHandler.setDbusProperty({"/path", "iface", "prop", "int64_t"},
                                      PropertyValue{uint64_t(1)}),
        std::bad_variant_access);
    EXPECT_THROW(
        directHandler.setDbusProperty({"/path", "iface", "prop", "uint64_t"},
                                      PropertyValue{int64_t(-1)}),
        std::bad_variant_access);
    EXPECT_THROW(
        directHandler.setDbusProperty({"/path", "iface", "prop", "double"},
                                      PropertyValue{std::string("wrong")}),
        std::bad_variant_access);
    EXPECT_THROW(
        directHandler.setDbusProperty({"/path", "iface", "prop", "string"},
                                      PropertyValue{uint32_t(7)}),
        std::bad_variant_access);
    EXPECT_THROW(directHandler.setDbusProperty(
                     {"/path", "iface", "prop", "array[string]"},
                     PropertyValue{std::string("wrong")}),
                 std::bad_variant_access);
    EXPECT_THROW(directHandler.setDbusProperty(
                     {"/path", "iface", "prop", "array[object_path]"},
                     PropertyValue{std::vector<std::string>{"wrong"}}),
                 std::bad_variant_access);
}

TEST_F(DBusHandlerDirectTest, getSubTreePathsReturnsOnePath)
{
    constexpr auto* objectPath = "/xyz/openbmc_project";
    constexpr auto* subtreePath = "/xyz/openbmc_project/example0";
    constexpr int depth = 0;

    testing::InSequence seq;
    expectNewMethodCall(ObjectMapper::default_service,
                        ObjectMapper::instance_path, ObjectMapper::interface,
                        "GetSubTreePaths");
    expectAppendString(SD_BUS_TYPE_STRING, objectPath);
    expectAppendBasic<int>(SD_BUS_TYPE_INT32, depth);
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
    expectCloseContainer();
    expectBusCallWithReply();
    expectReadOnePathVectorEntry(subtreePath);

    auto paths = directHandler.getSubTreePaths(objectPath, depth, {});
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(paths.front(), subtreePath);
}

TEST_F(DBusHandlerDirectTest, getSubtreeThrowsOnMapperCallFailure)
{
    constexpr auto* searchPath = "/xyz/openbmc_project";
    constexpr int depth = 0;

    testing::InSequence seq;
    expectNewMethodCall(ObjectMapper::default_service,
                        ObjectMapper::instance_path, ObjectMapper::interface,
                        "GetSubTree");
    expectAppendString(SD_BUS_TYPE_STRING, searchPath);
    expectAppendBasic<int>(SD_BUS_TYPE_INT32, depth);
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
    expectCloseContainer();
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, dbusTimeout, testing::_,
                                  testing::_))
        .WillOnce(testing::Return(-EINVAL));

    EXPECT_THROW(directHandler.getSubtree(searchPath, depth, {}),
                 sdbusplus::exception::SdBusError);
}

TEST_F(DBusHandlerDirectTest, getSubTreePathsThrowsOnMapperCallFailure)
{
    constexpr auto* objectPath = "/xyz/openbmc_project";
    constexpr int depth = 0;

    testing::InSequence seq;
    expectNewMethodCall(ObjectMapper::default_service,
                        ObjectMapper::instance_path, ObjectMapper::interface,
                        "GetSubTreePaths");
    expectAppendString(SD_BUS_TYPE_STRING, objectPath);
    expectAppendBasic<int>(SD_BUS_TYPE_INT32, depth);
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
    expectCloseContainer();
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, dbusTimeout, testing::_,
                                  testing::_))
        .WillOnce(testing::Return(-EINVAL));

    EXPECT_THROW(directHandler.getSubTreePaths(objectPath, depth, {}),
                 sdbusplus::exception::SdBusError);
}

TEST_F(DBusHandlerDirectTest, getAncestorsReturnsEmpty)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";

    testing::InSequence seq;
    expectNewMethodCall(ObjectMapper::default_service,
                        ObjectMapper::instance_path, ObjectMapper::interface,
                        "GetAncestors");
    expectAppendString(SD_BUS_TYPE_STRING, objectPath);
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
    expectCloseContainer();
    expectBusCallWithReply();
    expectReadEmptyMapLikeContainer();

    auto response = directHandler.getAncestors(objectPath, {});
    EXPECT_TRUE(response.empty());
}

TEST_F(DBusHandlerDirectTest, getAncestorsThrowsOnMapperCallFailure)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";

    testing::InSequence seq;
    expectNewMethodCall(ObjectMapper::default_service,
                        ObjectMapper::instance_path, ObjectMapper::interface,
                        "GetAncestors");
    expectAppendString(SD_BUS_TYPE_STRING, objectPath);
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
    expectCloseContainer();
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, dbusTimeout, testing::_,
                                  testing::_))
        .WillOnce(testing::Return(-EINVAL));

    EXPECT_THROW(directHandler.getAncestors(objectPath, {}),
                 sdbusplus::exception::SdBusError);
}

TEST_F(DBusHandlerDirectTest, getAssociatedSubTreeReturnsEmpty)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* subtreePath = "/xyz/openbmc_project/inventory";
    constexpr int depth = 0;

    testing::InSequence seq;
    expectNewMethodCall(ObjectMapper::default_service,
                        ObjectMapper::instance_path, ObjectMapper::interface,
                        "GetAssociatedSubTree");
    expectAppendString(SD_BUS_TYPE_OBJECT_PATH, objectPath);
    expectAppendString(SD_BUS_TYPE_OBJECT_PATH, subtreePath);
    expectAppendBasic<int>(SD_BUS_TYPE_INT32, depth);
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
    expectCloseContainer();
    expectBusCallWithReply();
    expectReadEmptyMapLikeContainer();

    auto response = directHandler.getAssociatedSubTree(
        sdbusplus::object_path(objectPath), sdbusplus::object_path(subtreePath),
        depth, {});
    EXPECT_TRUE(response.empty());
}

TEST_F(DBusHandlerDirectTest, getAssociatedSubTreeThrowsOnMapperCallFailure)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* subtreePath = "/xyz/openbmc_project/inventory";
    constexpr int depth = 0;

    testing::InSequence seq;
    expectNewMethodCall(ObjectMapper::default_service,
                        ObjectMapper::instance_path, ObjectMapper::interface,
                        "GetAssociatedSubTree");
    expectAppendString(SD_BUS_TYPE_OBJECT_PATH, objectPath);
    expectAppendString(SD_BUS_TYPE_OBJECT_PATH, subtreePath);
    expectAppendBasic<int>(SD_BUS_TYPE_INT32, depth);
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
    expectCloseContainer();
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, dbusTimeout, testing::_,
                                  testing::_))
        .WillOnce(testing::Return(-EINVAL));

    EXPECT_THROW(directHandler.getAssociatedSubTree(
                     sdbusplus::object_path(objectPath),
                     sdbusplus::object_path(subtreePath), depth, {}),
                 sdbusplus::exception::SdBusError);
}

TEST_F(DBusHandlerDirectTest, getDbusPropertiesVariantReturnsEmpty)
{
    constexpr auto* service = "xyz.openbmc_project.Example";
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* interface = "xyz.openbmc_project.Example";

    testing::InSequence seq;
    expectNewMethodCall(service, objectPath, dbusProperties, "GetAll");
    expectAppendString(SD_BUS_TYPE_STRING, interface);
    expectBusCallWithReply();
    expectReadEmptyMapLikeContainer();

    auto response =
        directHandler.getDbusPropertiesVariant(service, objectPath, interface);
    EXPECT_TRUE(response.empty());
}

TEST_F(DBusHandlerDirectTest, getDbusPropertiesVariantThrowsOnGetAllFailure)
{
    constexpr auto* service = "xyz.openbmc_project.Example";
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* interface = "xyz.openbmc_project.Example";

    testing::InSequence seq;
    expectNewMethodCall(service, objectPath, dbusProperties, "GetAll");
    expectAppendString(SD_BUS_TYPE_STRING, interface);
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, dbusTimeout, testing::_,
                                  testing::_))
        .WillOnce(testing::Return(-EINVAL));

    EXPECT_THROW(
        directHandler.getDbusPropertiesVariant(service, objectPath, interface),
        sdbusplus::exception::SdBusError);
}

TEST_F(DBusHandlerDirectTest, getDbusPropertyVariantThrowsOnGetFailure)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* interface = "xyz.openbmc_project.Example";
    constexpr auto* property = "Present";
    constexpr auto* service = "xyz.openbmc_project.ExampleService";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectNewMethodCall(service, objectPath, dbusProperties, "Get");
    expectAppendString(SD_BUS_TYPE_STRING, interface);
    expectAppendString(SD_BUS_TYPE_STRING, property);
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, dbusTimeout, testing::_,
                                  testing::_))
        .WillOnce(testing::Return(-EINVAL));

    EXPECT_THROW(
        directHandler.getDbusPropertyVariant(objectPath, property, interface),
        sdbusplus::exception::SdBusError);
}

TEST_F(DBusHandlerDirectTest, checkDbusPropertyVariantReturnsFalse)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* interface = "xyz.openbmc_project.Example";
    constexpr auto* service = "xyz.openbmc_project.ExampleService";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectNewMethodCall(service, objectPath, dbusProperties, "GetAll");
    expectAppendString(SD_BUS_TYPE_STRING, interface);
    expectBusCallWithReply(0);
    expectReadEmptyMapLikeContainer();

    auto present = directHandler.checkDbusPropertyVariant(
        objectPath, "MissingProp", interface);
    EXPECT_FALSE(present);
}

TEST_F(DBusHandlerDirectTest, checkDbusPropertyVariantReturnsFalseOnGetAllError)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* interface = "xyz.openbmc_project.Example";
    constexpr auto* service = "xyz.openbmc_project.ExampleService";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectNewMethodCall(service, objectPath, dbusProperties, "GetAll");
    expectAppendString(SD_BUS_TYPE_STRING, interface);
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, 0, testing::_, testing::_))
        .WillOnce(testing::Return(-EINVAL));

    auto present = directHandler.checkDbusPropertyVariant(
        objectPath, "Present", interface);
    EXPECT_FALSE(present);
}

TEST_F(DBusHandlerDirectTest, getManagedObjReturnsEmpty)
{
    constexpr auto* service = "xyz.openbmc_project.Example";
    constexpr auto* rootPath = "/xyz/openbmc_project";

    testing::InSequence seq;
    expectNewMethodCall(service, rootPath, "org.freedesktop.DBus.ObjectManager",
                        "GetManagedObjects");
    expectBusCallWithReply(0);
    expectReadEmptyMapLikeContainer();

    auto response = DBusHandler::getManagedObj(service, rootPath);
    EXPECT_TRUE(response.empty());
}

TEST_F(DBusHandlerDirectTest, getManagedObjThrowsOnCallFailure)
{
    constexpr auto* service = "xyz.openbmc_project.Example";
    constexpr auto* rootPath = "/xyz/openbmc_project";

    testing::InSequence seq;
    expectNewMethodCall(service, rootPath, "org.freedesktop.DBus.ObjectManager",
                        "GetManagedObjects");
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, 0, testing::_, testing::_))
        .WillOnce(testing::Return(-EINVAL));

    EXPECT_THROW(DBusHandler::getManagedObj(service, rootPath),
                 sdbusplus::exception::SdBusError);
}

TEST_F(DBusHandlerDirectTest, reportErrorCallsLoggingCreate)
{
    testing::InSequence seq;
    expectNewMethodCall("xyz.openbmc_project.Logging",
                        "/xyz/openbmc_project/logging",
                        "xyz.openbmc_project.Logging.Create", "Create");
    expectAppendString(SD_BUS_TYPE_STRING, "test error occurred");
    expectAppendString(SD_BUS_TYPE_STRING,
                       "xyz.openbmc_project.Logging.Entry.Level.Error");
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "{ss}");
    expectCloseContainer();
    expectBusCallNoReply();

    reportError("test error occurred");
}

TEST_F(DBusHandlerDirectTest, reportErrorSwallowsCreateFailure)
{
    testing::InSequence seq;
    expectNewMethodCall("xyz.openbmc_project.Logging",
                        "/xyz/openbmc_project/logging",
                        "xyz.openbmc_project.Logging.Create", "Create");
    expectAppendString(SD_BUS_TYPE_STRING, "test error failed");
    expectAppendString(SD_BUS_TYPE_STRING,
                       "xyz.openbmc_project.Logging.Entry.Level.Error");
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "{ss}");
    expectCloseContainer();
    EXPECT_CALL(mock,
                sd_bus_call(nullptr, nullptr, dbusTimeout, testing::_, nullptr))
        .WillOnce(testing::Return(-EINVAL));

    EXPECT_NO_THROW({ reportError("test error failed"); });
}

TEST_F(DBusHandlerDirectTest, recoverMctpEndpointCallsRecover)
{
    constexpr auto* endpointPath = "/xyz/openbmc_project/mctp/1/9";
    constexpr auto* mctpIface = "au.com.codeconstruct.MCTP.Endpoint1";
    constexpr auto* service = "au.com.codeconstruct.MCTP1";

    testing::InSequence seq;
    expectGetObjectCall(endpointPath, mctpIface, service);
    expectNewMethodCall(service, endpointPath, mctpIface, "Recover");
    expectBusCallNoReply();

    recoverMctpEndpoint(endpointPath);
}

TEST_F(DBusHandlerDirectTest, recoverMctpEndpointSwallowsLookupFailure)
{
    constexpr auto* endpointPath = "/xyz/openbmc_project/mctp/1/19";
    constexpr auto* mctpIface = "au.com.codeconstruct.MCTP.Endpoint1";

    expectNewMethodCall(ObjectMapper::default_service,
                        ObjectMapper::instance_path, ObjectMapper::interface,
                        "GetObject");
    expectAppendString(SD_BUS_TYPE_STRING, endpointPath);
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
    expectAppendString(SD_BUS_TYPE_STRING, mctpIface);
    expectCloseContainer();
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, dbusTimeout, testing::_,
                                  testing::_))
        .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                     sd_bus_message** reply) {
            if (reply != nullptr)
            {
                *reply = nullptr;
            }
            return -EINVAL;
        });

    EXPECT_NO_THROW({ recoverMctpEndpoint(endpointPath); });
}

TEST_F(DBusHandlerDirectTest, recoverMctpEndpointSwallowsRecoverCallFailure)
{
    constexpr auto* endpointPath = "/xyz/openbmc_project/mctp/1/29";
    constexpr auto* mctpIface = "au.com.codeconstruct.MCTP.Endpoint1";
    constexpr auto* service = "au.com.codeconstruct.MCTP1";

    testing::InSequence seq;
    expectGetObjectCall(endpointPath, mctpIface, service);
    expectNewMethodCall(service, endpointPath, mctpIface, "Recover");
    EXPECT_CALL(mock,
                sd_bus_call(nullptr, nullptr, dbusTimeout, testing::_, nullptr))
        .WillOnce(testing::Return(-EINVAL));

    EXPECT_NO_THROW({ recoverMctpEndpoint(endpointPath); });
}

TEST_F(DBusHandlerDirectTest, getServiceNullInterface)
{
    // Exercises the else branch at line 247: interface == nullptr
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* service = "xyz.openbmc_project.ExampleService";
    constexpr auto* iface = "xyz.openbmc_project.Example";

    testing::InSequence seq;
    expectNewMethodCall(ObjectMapper::default_service,
                        ObjectMapper::instance_path, ObjectMapper::interface,
                        "GetObject");
    expectAppendString(SD_BUS_TYPE_STRING, objectPath);
    // Empty interface list (no interface string appended)
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
    expectCloseContainer();
    expectBusCallWithReply();
    expectReadOneServiceMapEntry(service, iface);

    auto result = directHandler.getService(objectPath, nullptr);
    EXPECT_EQ(result, service);
}

TEST_F(DBusHandlerDirectTest,
       getServiceNullInterfaceThrowsWhenMapperReplyIsEmpty)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";

    testing::InSequence seq;
    expectNewMethodCall(ObjectMapper::default_service,
                        ObjectMapper::instance_path, ObjectMapper::interface,
                        "GetObject");
    expectAppendString(SD_BUS_TYPE_STRING, objectPath);
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
    expectCloseContainer();
    expectBusCallWithReply();
    expectReadEmptyMapLikeContainer();

    EXPECT_THROW(directHandler.getService(objectPath, nullptr),
                 sdbusplus::exception::SdBusError);
}

TEST_F(DBusHandlerDirectTest, getServiceThrowsWhenMapperReplyIsEmpty)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* interface = "xyz.openbmc_project.Example";

    testing::InSequence seq;
    expectNewMethodCall(ObjectMapper::default_service,
                        ObjectMapper::instance_path, ObjectMapper::interface,
                        "GetObject");
    expectAppendString(SD_BUS_TYPE_STRING, objectPath);
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
    expectAppendString(SD_BUS_TYPE_STRING, interface);
    expectCloseContainer();
    expectBusCallWithReply();
    expectReadEmptyMapLikeContainer();

    EXPECT_THROW(directHandler.getService(objectPath, interface),
                 sdbusplus::exception::SdBusError);
}

TEST_F(DBusHandlerDirectTest, getServiceThrowsWhenMapperCallFails)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* interface = "xyz.openbmc_project.Example";

    testing::InSequence seq;
    expectNewMethodCall(ObjectMapper::default_service,
                        ObjectMapper::instance_path, ObjectMapper::interface,
                        "GetObject");
    expectAppendString(SD_BUS_TYPE_STRING, objectPath);
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
    expectAppendString(SD_BUS_TYPE_STRING, interface);
    expectCloseContainer();
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, dbusTimeout, testing::_,
                                  testing::_))
        .WillOnce(testing::Return(-EINVAL));

    EXPECT_THROW(directHandler.getService(objectPath, interface),
                 sdbusplus::exception::SdBusError);
}

TEST_F(DBusHandlerDirectTest, checkDbusPropertyVariantReturnsTrue)
{
    // Exercises the "property found → return true" branch at line 484
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* interface = "xyz.openbmc_project.Example";
    constexpr auto* service = "xyz.openbmc_project.ExampleService";

    testing::InSequence seq;
    // getService (GetObject)
    expectGetObjectCall(objectPath, interface, service);
    // GetAll call
    expectNewMethodCall(service, objectPath, dbusProperties, "GetAll");
    expectAppendString(SD_BUS_TYPE_STRING, interface);
    expectBusCallWithReply(0);
    // Read a map with one entry: {"Present": variant<string>("true")}
    // Enter outer array container (array of dict entries)
    EXPECT_CALL(mock, sd_bus_message_enter_container(
                          testing::_, SD_BUS_TYPE_ARRAY, testing::_))
        .WillOnce(testing::Return(1));
    // First dict entry
    EXPECT_CALL(mock, sd_bus_message_at_end(testing::_, 0))
        .WillOnce(testing::Return(0));
    EXPECT_CALL(mock, sd_bus_message_enter_container(
                          testing::_, SD_BUS_TYPE_DICT_ENTRY, testing::_))
        .WillOnce(testing::Return(1));
    // Read key: "Present"
    expectReadString("Present");
    // Variant deserialization: sdbusplus calls verify_type to find matching
    // alternative.  For variant<string, vector<string>>, it tries "s" first.
    EXPECT_CALL(mock,
                sd_bus_message_verify_type(testing::_, SD_BUS_TYPE_VARIANT,
                                           testing::_))
        .WillOnce(testing::Return(1)); // matches "s" (string)
    // Enter variant container
    EXPECT_CALL(mock, sd_bus_message_enter_container(
                          testing::_, SD_BUS_TYPE_VARIANT, testing::_))
        .WillOnce(testing::Return(1));
    // Read string value
    expectReadString("true");
    EXPECT_CALL(mock, sd_bus_message_exit_container(testing::_))
        .WillOnce(testing::Return(1));
    // Exit dict entry
    EXPECT_CALL(mock, sd_bus_message_exit_container(testing::_))
        .WillOnce(testing::Return(1));
    // No more entries
    EXPECT_CALL(mock, sd_bus_message_at_end(testing::_, 0))
        .WillOnce(testing::Return(1));
    // Exit outer array
    EXPECT_CALL(mock, sd_bus_message_exit_container(testing::_))
        .WillOnce(testing::Return(1));

    auto present = directHandler.checkDbusPropertyVariant(
        objectPath, "Present", interface);
    EXPECT_TRUE(present);
}

TEST_F(DBusHandlerDirectTest,
       checkDbusPropertyVariantReturnsFalseWhenDifferentPropertyExists)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* interface = "xyz.openbmc_project.Example";
    constexpr auto* service = "xyz.openbmc_project.ExampleService";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectNewMethodCall(service, objectPath, dbusProperties, "GetAll");
    expectAppendString(SD_BUS_TYPE_STRING, interface);
    expectBusCallWithReply(0);
    EXPECT_CALL(mock, sd_bus_message_enter_container(
                          testing::_, SD_BUS_TYPE_ARRAY, testing::_))
        .WillOnce(testing::Return(1));
    EXPECT_CALL(mock, sd_bus_message_at_end(testing::_, 0))
        .WillOnce(testing::Return(0));
    EXPECT_CALL(mock, sd_bus_message_enter_container(
                          testing::_, SD_BUS_TYPE_DICT_ENTRY, testing::_))
        .WillOnce(testing::Return(1));
    expectReadString("Other");
    EXPECT_CALL(mock, sd_bus_message_verify_type(
                          testing::_, SD_BUS_TYPE_VARIANT, testing::_))
        .WillOnce(testing::Return(1));
    EXPECT_CALL(mock, sd_bus_message_enter_container(
                          testing::_, SD_BUS_TYPE_VARIANT, testing::_))
        .WillOnce(testing::Return(1));
    expectReadString("value");
    EXPECT_CALL(mock, sd_bus_message_exit_container(testing::_))
        .WillOnce(testing::Return(1));
    EXPECT_CALL(mock, sd_bus_message_exit_container(testing::_))
        .WillOnce(testing::Return(1));
    EXPECT_CALL(mock, sd_bus_message_at_end(testing::_, 0))
        .WillOnce(testing::Return(1));
    EXPECT_CALL(mock, sd_bus_message_exit_container(testing::_))
        .WillOnce(testing::Return(1));

    auto present = directHandler.checkDbusPropertyVariant(
        objectPath, "Present", interface);
    EXPECT_FALSE(present);
}

TEST_F(DBusHandlerDirectTest, checkDbusPropertyVariantReturnsFalseNotFound)
{
    // Exercises the "property not found → return false" branch at line 484
    constexpr auto* objectPath = "/xyz/openbmc_project/example";
    constexpr auto* interface = "xyz.openbmc_project.Example";
    constexpr auto* service = "xyz.openbmc_project.ExampleService";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectNewMethodCall(service, objectPath, dbusProperties, "GetAll");
    expectAppendString(SD_BUS_TYPE_STRING, interface);
    expectBusCallWithReply(0);
    // Read an empty map (no entries)
    EXPECT_CALL(mock, sd_bus_message_enter_container(
                          testing::_, SD_BUS_TYPE_ARRAY, testing::_))
        .WillOnce(testing::Return(1));
    // Immediately at end — no entries
    EXPECT_CALL(mock, sd_bus_message_at_end(testing::_, 0))
        .WillOnce(testing::Return(1));
    EXPECT_CALL(mock, sd_bus_message_exit_container(testing::_))
        .WillOnce(testing::Return(1));

    auto found = directHandler.checkDbusPropertyVariant(
        objectPath, "NonExistent", interface);
    EXPECT_FALSE(found);
}

TEST_F(DBusHandlerDirectTest, setFruPresenceSetsTrue)
{
    constexpr auto* fruPath = "/xyz/openbmc_project/inventory/fru0";
    constexpr auto* fruIface = "xyz.openbmc_project.Inventory.Item";
    constexpr auto* service = "xyz.openbmc_project.InventoryService";

    testing::InSequence seq;
    expectGetObjectCall(fruPath, fruIface, service);
    expectSetPropertyCall(service, fruPath, fruIface, "Present");
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "b");
    expectAppendBasic<int>(SD_BUS_TYPE_BOOLEAN, true);
    expectCloseContainer();
    expectBusCallNoReply();

    setFruPresence(fruPath, true);
}

TEST_F(DBusHandlerDirectTest, setFruPresenceSwallowsPropertyFailure)
{
    constexpr auto* fruPath = "/xyz/openbmc_project/inventory/fru1";
    constexpr auto* fruIface = "xyz.openbmc_project.Inventory.Item";
    constexpr auto* service = "xyz.openbmc_project.InventoryService";

    testing::InSequence seq;
    expectGetObjectCall(fruPath, fruIface, service);
    expectSetPropertyCall(service, fruPath, fruIface, "Present");
    expectOpenContainer(SD_BUS_TYPE_VARIANT, "b");
    expectAppendBasic<int>(SD_BUS_TYPE_BOOLEAN, false);
    expectCloseContainer();
    EXPECT_CALL(mock,
                sd_bus_call(nullptr, nullptr, dbusTimeout, testing::_, nullptr))
        .WillOnce(testing::Return(-EINVAL));

    EXPECT_NO_THROW({ setFruPresence(fruPath, false); });
}

// Helper to exercise the non-virtual getDbusProperty<T> template in the header
// by overriding only the virtual getDbusPropertyVariant that it calls.
class GetDbusPropertyTestHandler : public DBusHandler
{
  public:
    PropertyValue returnValue;
    PropertyValue getDbusPropertyVariant(
        const char* /*objPath*/, const char* /*dbusProp*/,
        const char* /*dbusInterface*/) const override
    {
        return returnValue;
    }
};

TEST(GetDbusPropertyDirect, uint8)
{
    GetDbusPropertyTestHandler handler;
    handler.returnValue = uint8_t(42);
    auto result = handler.getDbusProperty<uint8_t>("/path", "prop", "iface");
    EXPECT_EQ(result, uint8_t(42));
}

TEST(GetDbusPropertyDirect, boolValue)
{
    GetDbusPropertyTestHandler handler;
    handler.returnValue = true;
    EXPECT_TRUE(handler.getDbusProperty<bool>("/path", "prop", "iface"));
}

TEST(GetDbusPropertyDirect, string)
{
    GetDbusPropertyTestHandler handler;
    handler.returnValue = std::string("hello");
    auto result =
        handler.getDbusProperty<std::string>("/path", "prop", "iface");
    EXPECT_EQ(result, "hello");
}

TEST(GetDbusPropertyDirect, vectorUint64)
{
    GetDbusPropertyTestHandler handler;
    const std::vector<uint64_t> counters{9, 99, 999};
    handler.returnValue = counters;
    auto result = handler.getDbusProperty<std::vector<uint64_t>>(
        "/path", "prop", "iface");
    EXPECT_EQ(result, counters);
}

TEST(GetDbusPropertyDirect, objectPaths)
{
    using ObjectPaths = std::vector<sdbusplus::object_path>;

    GetDbusPropertyTestHandler handler;
    const ObjectPaths expected{
        sdbusplus::object_path("/xyz/openbmc_project/object0"),
        sdbusplus::object_path("/xyz/openbmc_project/object1")};
    handler.returnValue = expected;
    auto result =
        handler.getDbusProperty<ObjectPaths>("/path", "prop", "iface");
    EXPECT_EQ(result, expected);
}

TEST(GetDbusPropertyDirect, associations)
{
    using Associations =
        std::vector<std::tuple<std::string, std::string, std::string>>;

    GetDbusPropertyTestHandler handler;
    const Associations expected{
        {"chassis", "all_states",
         "/xyz/openbmc_project/inventory/system/chassis/chassis0"},
        {"processor", "all_states",
         "/xyz/openbmc_project/inventory/system/chassis/motherboard/cpu0"}};
    handler.returnValue = expected;
    auto result =
        handler.getDbusProperty<Associations>("/path", "prop", "iface");
    EXPECT_EQ(result, expected);
}

TEST(GetDbusPropertyDirect, throwsOnVariantMismatchAcrossAdditionalTypes)
{
    using Associations =
        std::vector<std::tuple<std::string, std::string, std::string>>;
    using ObjectPaths = std::vector<sdbusplus::object_path>;

    GetDbusPropertyTestHandler handler;

    handler.returnValue = std::string("wrong");
    EXPECT_THROW(handler.getDbusProperty<bool>("/path", "prop", "iface"),
                 std::bad_variant_access);

    handler.returnValue = std::string("wrong");
    EXPECT_THROW(handler.getDbusProperty<uint8_t>("/path", "prop", "iface"),
                 std::bad_variant_access);

    handler.returnValue = uint64_t(77);
    EXPECT_THROW(handler.getDbusProperty<std::string>("/path", "prop", "iface"),
                 std::bad_variant_access);

    handler.returnValue = std::vector<uint8_t>{0x11, 0x22};
    EXPECT_THROW(handler.getDbusProperty<std::vector<uint64_t>>("/path", "prop",
                                                                "iface"),
                 std::bad_variant_access);

    handler.returnValue = std::vector<std::string>{"not", "paths"};
    EXPECT_THROW(handler.getDbusProperty<ObjectPaths>("/path", "prop", "iface"),
                 std::bad_variant_access);

    handler.returnValue = std::string("wrong");
    EXPECT_THROW(handler.getDbusProperty<int16_t>("/path", "prop", "iface"),
                 std::bad_variant_access);

    handler.returnValue = std::string("wrong");
    EXPECT_THROW(handler.getDbusProperty<uint16_t>("/path", "prop", "iface"),
                 std::bad_variant_access);

    handler.returnValue = std::string("wrong");
    EXPECT_THROW(handler.getDbusProperty<int32_t>("/path", "prop", "iface"),
                 std::bad_variant_access);

    handler.returnValue = std::string("wrong");
    EXPECT_THROW(handler.getDbusProperty<uint32_t>("/path", "prop", "iface"),
                 std::bad_variant_access);

    handler.returnValue = std::string("wrong");
    EXPECT_THROW(handler.getDbusProperty<int64_t>("/path", "prop", "iface"),
                 std::bad_variant_access);

    handler.returnValue = std::string("wrong");
    EXPECT_THROW(handler.getDbusProperty<uint64_t>("/path", "prop", "iface"),
                 std::bad_variant_access);

    handler.returnValue = std::string("wrong");
    EXPECT_THROW(handler.getDbusProperty<double>("/path", "prop", "iface"),
                 std::bad_variant_access);

    handler.returnValue = std::string("wrong");
    EXPECT_THROW(
        handler.getDbusProperty<Associations>("/path", "prop", "iface"),
        std::bad_variant_access);
}

TEST(EmitStateSensorEventSignal, returnsStatus)
{
    auto rc = emitStateSensorEventSignal(1, 2, 0, 1, 0);
    EXPECT_TRUE(rc == PLDM_SUCCESS || rc == PLDM_ERROR);
}

TEST_F(DBusHandlerDirectTest, emitStateSensorEventSignalReturnsErrorOnFailure)
{
    EXPECT_CALL(mock, sd_bus_message_new_signal(
                          testing::_, testing::_,
                          testing::StrEq("/xyz/openbmc_project/pldm"),
                          testing::StrEq("xyz.openbmc_project.PLDM.Event"),
                          testing::StrEq("StateSensorEvent")))
        .WillOnce(testing::Return(-EINVAL));

    auto rc = emitStateSensorEventSignal(1, 2, 0, 1, 0);
    EXPECT_EQ(rc, PLDM_ERROR);
}

TEST_F(DBusHandlerDirectTest, checkForFruPresenceReturnsFalseWhenMissing)
{
    constexpr auto* objectPath = "/xyz/openbmc_project/does_not_exist";
    constexpr auto* interface = "xyz.openbmc_project.Inventory.Item";

    expectNewMethodCall(ObjectMapper::default_service,
                        ObjectMapper::instance_path, ObjectMapper::interface,
                        "GetObject");
    expectAppendString(SD_BUS_TYPE_STRING, objectPath);
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
    expectAppendString(SD_BUS_TYPE_STRING, interface);
    expectCloseContainer();
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, dbusTimeout, testing::_,
                                  testing::_))
        .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                     sd_bus_message** reply) {
            if (reply != nullptr)
            {
                *reply = nullptr;
            }
            return -EINVAL;
        });

    auto isPresent = checkForFruPresence(objectPath);
    EXPECT_FALSE(isPresent);
}

TEST_F(DBusHandlerDirectTest, checkForFruPresenceReturnsTrueWhenPresent)
{
    constexpr auto* objectPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis0";
    constexpr auto* interface = "xyz.openbmc_project.Inventory.Item";
    constexpr auto* property = "Present";
    constexpr auto* service = "xyz.openbmc_project.InventoryService";

    testing::InSequence seq;
    expectGetObjectCall(objectPath, interface, service);
    expectNewMethodCall(service, objectPath, dbusProperties, "Get");
    expectAppendString(SD_BUS_TYPE_STRING, interface);
    expectAppendString(SD_BUS_TYPE_STRING, property);
    expectBusCallWithReply();
    EXPECT_CALL(mock, sd_bus_message_verify_type(
                          testing::_, SD_BUS_TYPE_VARIANT, testing::_))
        .WillOnce(testing::Return(1));
    EXPECT_CALL(mock, sd_bus_message_enter_container(
                          testing::_, SD_BUS_TYPE_VARIANT, testing::_))
        .WillOnce(testing::Return(1));
    EXPECT_CALL(mock, sd_bus_message_read_basic(testing::_, SD_BUS_TYPE_BOOLEAN,
                                                testing::_))
        .WillOnce([](sd_bus_message*, char, void* output) {
            *static_cast<int*>(output) = 1;
            return 0;
        });
    EXPECT_CALL(mock, sd_bus_message_exit_container(testing::_))
        .WillOnce(testing::Return(1));

    EXPECT_TRUE(checkForFruPresence(objectPath));
}

TEST(dbusPropValuesToDouble, goodTest)
{
    double value = 0;
    bool ret =
        dbusPropValuesToDouble("uint8_t", static_cast<uint8_t>(0x12), &value);
    EXPECT_EQ(true, ret);
    EXPECT_EQ(0x12, value);
    ret =
        dbusPropValuesToDouble("int16_t", static_cast<int16_t>(0x1234), &value);
    EXPECT_EQ(true, ret);
    EXPECT_EQ(0x1234, value);
    ret = dbusPropValuesToDouble("uint16_t", static_cast<uint16_t>(0x8234),
                                 &value);
    EXPECT_EQ(true, ret);
    EXPECT_EQ(0x8234, value);
    ret = dbusPropValuesToDouble("int32_t", static_cast<int32_t>(0x12345678),
                                 &value);
    EXPECT_EQ(true, ret);
    EXPECT_EQ(0x12345678, value);
    ret = dbusPropValuesToDouble("uint32_t", static_cast<uint32_t>(0x82345678),
                                 &value);
    EXPECT_EQ(true, ret);
    EXPECT_EQ(0x82345678, value);
    ret = dbusPropValuesToDouble(
        "int64_t", static_cast<int64_t>(0x1234567898765432), &value);
    EXPECT_EQ(true, ret);
    EXPECT_EQ(0x1234567898765432, value);
    ret = dbusPropValuesToDouble(
        "uint64_t", static_cast<uint64_t>(0x8234567898765432), &value);
    EXPECT_EQ(true, ret);
    EXPECT_EQ(0x8234567898765432, value);
    ret = dbusPropValuesToDouble("double", static_cast<double>(1234.5678),
                                 &value);
    EXPECT_EQ(true, ret);
    EXPECT_EQ(1234.5678, value);
}

TEST(dbusPropValuesToDouble, badTest)
{
    double value = std::numeric_limits<double>::quiet_NaN();
    /* Type and Data variant are different */
    bool ret =
        dbusPropValuesToDouble("uint8_t", static_cast<uint16_t>(0x12), &value);
    EXPECT_EQ(false, ret);
    ret =
        dbusPropValuesToDouble("int16_t", static_cast<uint16_t>(0x12), &value);
    EXPECT_EQ(false, ret);
    ret =
        dbusPropValuesToDouble("uint16_t", static_cast<int16_t>(0x12), &value);
    EXPECT_EQ(false, ret);
    ret =
        dbusPropValuesToDouble("int32_t", static_cast<uint32_t>(0x12), &value);
    EXPECT_EQ(false, ret);
    ret =
        dbusPropValuesToDouble("uint32_t", static_cast<int32_t>(0x12), &value);
    EXPECT_EQ(false, ret);
    ret =
        dbusPropValuesToDouble("int64_t", static_cast<uint64_t>(0x12), &value);
    EXPECT_EQ(false, ret);
    ret =
        dbusPropValuesToDouble("uint64_t", static_cast<int64_t>(0x12), &value);
    EXPECT_EQ(false, ret);
    /* Unsupported Types */
    ret = dbusPropValuesToDouble("string", static_cast<std::string>("hello"),
                                 &value);
    EXPECT_EQ(false, ret);
    ret = dbusPropValuesToDouble("bool", static_cast<bool>(true), &value);
    EXPECT_EQ(false, ret);
    ret = dbusPropValuesToDouble("vector<uint8_t>",
                                 static_cast<std::string>("hello"), &value);
    EXPECT_EQ(false, ret);
    ret = dbusPropValuesToDouble("vector<string>",
                                 static_cast<std::string>("hello"), &value);
    EXPECT_EQ(false, ret);
    /* Support Type but Data Type is unsupported */
    ret = dbusPropValuesToDouble("double", static_cast<std::string>("hello"),
                                 &value);
    EXPECT_EQ(false, ret);
    /* Null pointer */
    ret = dbusPropValuesToDouble("double", static_cast<std::string>("hello"),
                                 nullptr);
    EXPECT_EQ(false, ret);
}

TEST(dbusPropValuesToDouble, nullOutputPointerCoverage)
{
    EXPECT_FALSE(
        dbusPropValuesToDouble("uint8_t", static_cast<uint8_t>(0x12), nullptr));
    EXPECT_FALSE(
        dbusPropValuesToDouble("int32_t", static_cast<int32_t>(-42), nullptr));
    EXPECT_FALSE(
        dbusPropValuesToDouble("double", static_cast<double>(3.1415), nullptr));
}

TEST(FruFieldValuestring, goodTest)
{
    std::vector<uint8_t> data = {0x41, 0x6d, 0x70, 0x65, 0x72, 0x65};
    std::string expectedString = "Ampere";
    auto result = fruFieldValuestring(data.data(), data.size());
    EXPECT_EQ(expectedString, result);
}

TEST(FruFieldValuestring, BadTest)
{
    std::vector<uint8_t> data = {0x41, 0x6d, 0x70, 0x65, 0x72, 0x65};
    auto result = fruFieldValuestring(data.data(), 0);
    EXPECT_EQ(std::nullopt, result);
    result = fruFieldValuestring(nullptr, data.size());
    EXPECT_EQ(std::nullopt, result);
}

TEST(fruFieldParserU32, goodTest)
{
    std::vector<uint8_t> data = {0x10, 0x12, 0x14, 0x25};
    uint32_t expectedU32 = 0x25141210;
    auto result = fruFieldParserU32(data.data(), data.size());
    EXPECT_EQ(expectedU32, result.value());
}

TEST(fruFieldParserU32, BadTest)
{
    std::vector<uint8_t> data = {0x10, 0x12, 0x14, 0x25};
    auto result = fruFieldParserU32(data.data(), data.size() - 1);
    EXPECT_EQ(std::nullopt, result);
    result = fruFieldParserU32(nullptr, data.size());
    EXPECT_EQ(std::nullopt, result);
}

// ===== Wave 0: Pure utility function tests =====

TEST(CheckIfLogicalBitSet, testCases)
{
    EXPECT_EQ(checkIfLogicalBitSet(0x0000), true);
    EXPECT_EQ(checkIfLogicalBitSet(0x0001), true);
    EXPECT_EQ(checkIfLogicalBitSet(0x1234), true);
    EXPECT_EQ(checkIfLogicalBitSet(0x7FFF), true);
    EXPECT_EQ(checkIfLogicalBitSet(0x8000), false);
    EXPECT_EQ(checkIfLogicalBitSet(0x8001), false);
    EXPECT_EQ(checkIfLogicalBitSet(0xABCD), false);
    EXPECT_EQ(checkIfLogicalBitSet(0xFFFF), false);
}

TEST(JsonEntryToDbusVal, testAllTypes)
{
    auto result = jsonEntryToDbusVal("uint8_t", nlohmann::json(10));
    EXPECT_EQ(std::get<uint8_t>(result), 10);

    result = jsonEntryToDbusVal("uint16_t", nlohmann::json(1000));
    EXPECT_EQ(std::get<uint16_t>(result), 1000);

    result = jsonEntryToDbusVal("uint32_t", nlohmann::json(100000));
    EXPECT_EQ(std::get<uint32_t>(result), 100000u);

    result = jsonEntryToDbusVal("uint64_t", nlohmann::json(1000000));
    EXPECT_EQ(std::get<uint64_t>(result), 1000000u);

    result = jsonEntryToDbusVal("int16_t", nlohmann::json(-100));
    EXPECT_EQ(std::get<int16_t>(result), -100);

    result = jsonEntryToDbusVal("int32_t", nlohmann::json(-100000));
    EXPECT_EQ(std::get<int32_t>(result), -100000);

    result = jsonEntryToDbusVal("int64_t", nlohmann::json(-1000000));
    EXPECT_EQ(std::get<int64_t>(result), -1000000);

    result = jsonEntryToDbusVal("bool", nlohmann::json(true));
    EXPECT_EQ(std::get<bool>(result), true);

    result = jsonEntryToDbusVal("double", nlohmann::json(3.14));
    EXPECT_NEAR(std::get<double>(result), 3.14, 0.001);

    result = jsonEntryToDbusVal("string", nlohmann::json("hello"));
    EXPECT_EQ(std::get<std::string>(result), "hello");
}

TEST(JsonEntryToDbusVal, testUnknownType)
{
    auto result = jsonEntryToDbusVal("unknown_type", nlohmann::json(0));
    (void)result;
}

TEST(GetPldmTypeName, testAllTypes)
{
    EXPECT_STREQ(getPldmTypeName(PLDM_BASE), "Base/Control");
    EXPECT_STREQ(getPldmTypeName(PLDM_PLATFORM), "Platform(Type2)");
    EXPECT_STREQ(getPldmTypeName(PLDM_BIOS), "BIOS");
    EXPECT_STREQ(getPldmTypeName(PLDM_FRU), "FRU");
    EXPECT_STREQ(getPldmTypeName(PLDM_FWUP), "FW_Update(Type5)");
    EXPECT_STREQ(getPldmTypeName(PLDM_RDE), "RDE");
    EXPECT_STREQ(getPldmTypeName(PLDM_OEM), "OEM");
    EXPECT_STREQ(getPldmTypeName(99), "Unknown");
}

TEST(GetPldmCommandName, testBaseCommands)
{
    EXPECT_EQ(getPldmCommandName(PLDM_BASE, PLDM_GET_TID), "GetTID");
    EXPECT_EQ(getPldmCommandName(PLDM_BASE, PLDM_GET_PLDM_VERSION),
              "GetPLDMVersion");
    EXPECT_EQ(getPldmCommandName(PLDM_BASE, PLDM_GET_PLDM_TYPES),
              "GetPLDMTypes");
    EXPECT_EQ(getPldmCommandName(PLDM_BASE, PLDM_GET_PLDM_COMMANDS),
              "GetPLDMCommands");
    EXPECT_EQ(getPldmCommandName(PLDM_BASE, 0xFE), "Cmd_0xfe");
}

TEST(GetPldmCommandName, testPlatformCommands)
{
    EXPECT_EQ(getPldmCommandName(PLDM_PLATFORM, PLDM_SET_STATE_EFFECTER_STATES),
              "SetStateEffecterStates");
    EXPECT_EQ(getPldmCommandName(PLDM_PLATFORM, PLDM_GET_STATE_SENSOR_READINGS),
              "GetStateSensorReadings");
    EXPECT_EQ(getPldmCommandName(PLDM_PLATFORM, PLDM_GET_SENSOR_READING),
              "GetSensorReading");
    EXPECT_EQ(getPldmCommandName(PLDM_PLATFORM, PLDM_GET_PDR), "GetPDR");
    EXPECT_EQ(getPldmCommandName(PLDM_PLATFORM, PLDM_PLATFORM_EVENT_MESSAGE),
              "PlatformEventMessage");
    EXPECT_EQ(
        getPldmCommandName(PLDM_PLATFORM, PLDM_POLL_FOR_PLATFORM_EVENT_MESSAGE),
        "PollForPlatformEventMessage");
    // default: unknown platform command
    EXPECT_EQ(getPldmCommandName(PLDM_PLATFORM, 0xFE), "Cmd_0xfe");
}

TEST(GetPldmCommandName, testFwUpdateCommands)
{
    EXPECT_EQ(getPldmCommandName(PLDM_FWUP, PLDM_QUERY_DEVICE_IDENTIFIERS),
              "QueryDeviceIdentifiers");
    EXPECT_EQ(getPldmCommandName(PLDM_FWUP, PLDM_GET_FIRMWARE_PARAMETERS),
              "GetFirmwareParameters");
    EXPECT_EQ(getPldmCommandName(PLDM_FWUP, PLDM_REQUEST_UPDATE),
              "RequestUpdate");
    EXPECT_EQ(getPldmCommandName(PLDM_FWUP, PLDM_PASS_COMPONENT_TABLE),
              "PassComponentTable");
    EXPECT_EQ(getPldmCommandName(PLDM_FWUP, PLDM_UPDATE_COMPONENT),
              "UpdateComponent");
    EXPECT_EQ(getPldmCommandName(PLDM_FWUP, PLDM_REQUEST_FIRMWARE_DATA),
              "RequestFirmwareData");
    EXPECT_EQ(getPldmCommandName(PLDM_FWUP, PLDM_TRANSFER_COMPLETE),
              "TransferComplete");
    EXPECT_EQ(getPldmCommandName(PLDM_FWUP, PLDM_VERIFY_COMPLETE),
              "VerifyComplete");
    EXPECT_EQ(getPldmCommandName(PLDM_FWUP, PLDM_APPLY_COMPLETE),
              "ApplyComplete");
    EXPECT_EQ(getPldmCommandName(PLDM_FWUP, PLDM_ACTIVATE_FIRMWARE),
              "ActivateFirmware");
    EXPECT_EQ(getPldmCommandName(PLDM_FWUP, PLDM_GET_STATUS), "GetStatus");
    EXPECT_EQ(getPldmCommandName(PLDM_FWUP, PLDM_CANCEL_UPDATE_COMPONENT),
              "CancelUpdateComponent");
    EXPECT_EQ(getPldmCommandName(PLDM_FWUP, PLDM_CANCEL_UPDATE),
              "CancelUpdate");
    // default: unknown fw-update command
    EXPECT_EQ(getPldmCommandName(PLDM_FWUP, 0xFE), "Cmd_0xfe");
}

TEST(GetPldmCommandName, testBiosCommands)
{
    EXPECT_EQ(getPldmCommandName(PLDM_BIOS, PLDM_GET_BIOS_TABLE),
              "GetBIOSTable");
    EXPECT_EQ(getPldmCommandName(PLDM_BIOS, PLDM_SET_BIOS_TABLE),
              "SetBIOSTable");
    EXPECT_EQ(
        getPldmCommandName(PLDM_BIOS, PLDM_SET_BIOS_ATTRIBUTE_CURRENT_VALUE),
        "SetBIOSAttributeCurrentValue");
    EXPECT_EQ(getPldmCommandName(
                  PLDM_BIOS, PLDM_GET_BIOS_ATTRIBUTE_CURRENT_VALUE_BY_HANDLE),
              "GetBIOSAttributeCurrentValueByHandle");
    EXPECT_EQ(getPldmCommandName(PLDM_BIOS, PLDM_GET_DATE_TIME), "GetDateTime");
    EXPECT_EQ(getPldmCommandName(PLDM_BIOS, PLDM_SET_DATE_TIME), "SetDateTime");
    // default: unknown BIOS command
    EXPECT_EQ(getPldmCommandName(PLDM_BIOS, 0xFE), "Cmd_0xfe");
}

TEST(GetPldmCommandName, testFruCommands)
{
    EXPECT_EQ(getPldmCommandName(PLDM_FRU, PLDM_GET_FRU_RECORD_TABLE_METADATA),
              "GetFRURecordTableMetadata");
    EXPECT_EQ(getPldmCommandName(PLDM_FRU, PLDM_GET_FRU_RECORD_TABLE),
              "GetFRURecordTable");
    // default: unknown FRU command
    EXPECT_EQ(getPldmCommandName(PLDM_FRU, 0xFE), "Cmd_0xfe");
}

TEST(GetPldmCommandName, testUnknownTypeCommand)
{
    EXPECT_EQ(getPldmCommandName(0xFF, 0x01), "Cmd_0x01");
}

TEST(GenerateSwId, returnsValueInRange)
{
    auto id = generateSwId();
    EXPECT_GE(id, 0);
    EXPECT_LE(id, 9999);
}

TEST(DecimalToBcd, testCases)
{
    EXPECT_EQ(decimalToBcd(0u), 0u);
    EXPECT_EQ(decimalToBcd(5u), 0x05u);
    EXPECT_EQ(decimalToBcd(9u), 0x09u);
    EXPECT_EQ(decimalToBcd(10u), 0x10u);
    EXPECT_EQ(decimalToBcd(12u), 0x12u);
    EXPECT_EQ(decimalToBcd(25u), 0x25u);
    EXPECT_EQ(decimalToBcd(99u), 0x99u);
    EXPECT_EQ(decimalToBcd(100u), 0x100u);
    EXPECT_EQ(decimalToBcd(123u), 0x123u);
    EXPECT_EQ(decimalToBcd(255u), 0x255u);
    EXPECT_EQ(decimalToBcd(456u), 0x456u);
    EXPECT_EQ(decimalToBcd(9999u), 0x9999u);
    EXPECT_EQ(decimalToBcd<uint8_t>(42), 0x42u);
    EXPECT_EQ(decimalToBcd<uint16_t>(1234), 0x1234u);
    EXPECT_EQ(decimalToBcd<uint32_t>(56789), 0x56789u);
}

TEST(FindParent, testCases)
{
    EXPECT_EQ(findParent("/a/b/c"), "/a/b");
    EXPECT_EQ(findParent("/a"), "/");
    EXPECT_EQ(findParent("/"), "/");
}

TEST(GetCurrentSystemTime, testNonEmpty)
{
    auto result = getCurrentSystemTime();
    EXPECT_FALSE(result.empty());
    EXPECT_GT(result.size(), 10u);
}

TEST(GenerateSwId, testRange)
{
    for (int i = 0; i < 10; i++)
    {
        auto id = generateSwId();
        EXPECT_GE(id, 0);
        EXPECT_LE(id, 9999);
    }
}

TEST(GenerateSwId, testRandomness)
{
    for (int i = 0; i < 100; ++i)
    {
        auto id = generateSwId();
        EXPECT_GE(id, 0);
        EXPECT_LT(id, 10000);
    }

    std::set<long int> ids;
    for (int i = 0; i < 50; ++i)
    {
        ids.insert(generateSwId());
    }
    EXPECT_GT(ids.size(), 1);
}

TEST(PrintBuffer, testVectorOverload)
{
    std::vector<uint8_t> emptyBuf;
    printBuffer(true, emptyBuf);

    std::vector<uint8_t> buf1 = {0x01, 0x02, 0x03};
    printBuffer(true, buf1);

    std::vector<uint8_t> buf2 = {0x04, 0x05};
    printBuffer(false, buf2);
}

TEST(PrintBuffer, testPldmMsgOverload)
{
    std::vector<uint8_t> buf(sizeof(pldm_msg_hdr) + 2, 0);
    auto msg = reinterpret_cast<const pldm_msg*>(buf.data());
    printBuffer(true, msg, 2);
    printBuffer(false, msg, 2);
}

// ===== Wave 1: PDR function tests =====

TEST(FindStateEffecterId, testMatch)
{
    auto repo = pldm_pdr_init();

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));

    auto rec = new (pdr.data()) pldm_state_effecter_pdr;
    auto state = new (rec->possible_states) state_effecter_possible_states;

    rec->hdr.type = 11;
    rec->hdr.record_handle = 1;
    rec->entity_type = 33;
    rec->entity_instance = 1;
    rec->container_id = 0;
    rec->composite_effecter_count = 1;
    rec->effecter_id = 7;
    state->state_set_id = 196;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    auto result = findStateEffecterId(repo, 33, 1, 0, 196, true);
    EXPECT_EQ(result, 7);

    pldm_pdr_destroy(repo);
}

TEST(FindStateEffecterId, testNoMatch)
{
    auto repo = pldm_pdr_init();

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));

    auto rec = new (pdr.data()) pldm_state_effecter_pdr;
    auto state = new (rec->possible_states) state_effecter_possible_states;

    rec->hdr.type = 11;
    rec->hdr.record_handle = 1;
    rec->entity_type = 33;
    rec->entity_instance = 1;
    rec->container_id = 0;
    rec->composite_effecter_count = 1;
    rec->effecter_id = 7;
    state->state_set_id = 196;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    auto result = findStateEffecterId(repo, 44, 1, 0, 196, false);
    EXPECT_EQ(result, PLDM_INVALID_EFFECTER_ID);

    pldm_pdr_destroy(repo);
}

TEST(FindStateEffecterId, testEmptyRepo)
{
    auto repo = pldm_pdr_init();

    auto result = findStateEffecterId(repo, 33, 1, 0, 196, false);
    EXPECT_EQ(result, PLDM_INVALID_EFFECTER_ID);

    pldm_pdr_destroy(repo);
}

TEST(FindStateEffecterId, testCompositeEffecter)
{
    auto repo = pldm_pdr_init();

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states) * 2);

    auto rec = new (pdr.data()) pldm_state_effecter_pdr;
    auto state_start = rec->possible_states;

    auto state = new (state_start) state_effecter_possible_states;

    rec->hdr.type = 11;
    rec->hdr.record_handle = 1;
    rec->entity_type = 33;
    rec->entity_instance = 1;
    rec->container_id = 0;
    rec->composite_effecter_count = 2;
    rec->effecter_id = 10;
    state->state_set_id = 196;
    state->possible_states_size = 1;

    state_start += state->possible_states_size + sizeof(state->state_set_id) +
                   sizeof(state->possible_states_size);
    state = new (state_start) state_effecter_possible_states;
    state->state_set_id = 197;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    auto result = findStateEffecterId(repo, 33, 1, 0, 197, true);
    EXPECT_EQ(result, 10);

    pldm_pdr_destroy(repo);
}

TEST(FindStateEffecterId, testEntityInstanceMismatch)
{
    auto repo = pldm_pdr_init();

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));

    auto rec = new (pdr.data()) pldm_state_effecter_pdr;
    auto state = new (rec->possible_states) state_effecter_possible_states;

    rec->hdr.type = 11;
    rec->hdr.record_handle = 1;
    rec->entity_type = 33;
    rec->entity_instance = 1;
    rec->container_id = 0;
    rec->composite_effecter_count = 1;
    rec->effecter_id = 7;
    state->state_set_id = 196;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    // entityType matches but entityInstance doesn't
    auto result = findStateEffecterId(repo, 33, 99, 0, 196, true);
    EXPECT_EQ(result, PLDM_INVALID_EFFECTER_ID);

    pldm_pdr_destroy(repo);
}

TEST(FindStateEffecterId, testContainerIdMismatch)
{
    auto repo = pldm_pdr_init();

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));

    auto rec = new (pdr.data()) pldm_state_effecter_pdr;
    auto state = new (rec->possible_states) state_effecter_possible_states;

    rec->hdr.type = 11;
    rec->hdr.record_handle = 1;
    rec->entity_type = 33;
    rec->entity_instance = 1;
    rec->container_id = 0;
    rec->composite_effecter_count = 1;
    rec->effecter_id = 7;
    state->state_set_id = 196;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    // entityType + entityInstance match but containerId doesn't
    auto result = findStateEffecterId(repo, 33, 1, 99, 196, true);
    EXPECT_EQ(result, PLDM_INVALID_EFFECTER_ID);

    pldm_pdr_destroy(repo);
}

TEST(GetStateSensorPDRsByType, testMatch)
{
    auto repo = pldm_pdr_init();

    std::vector<uint8_t> pdr1(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states));

    auto rec1 = new (pdr1.data()) pldm_state_sensor_pdr;
    auto state1 = new (rec1->possible_states) state_sensor_possible_states;

    rec1->hdr.type = 4;
    rec1->hdr.record_handle = 1;
    rec1->entity_type = 5;
    rec1->entity_instance = 1;
    rec1->container_id = 0;
    rec1->composite_sensor_count = 1;
    rec1->sensor_id = 100;
    state1->state_set_id = 1;
    state1->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr1.data(), pdr1.size(), false, 1, &handle),
              0);

    std::vector<uint8_t> pdr2(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states));

    auto rec2 = new (pdr2.data()) pldm_state_sensor_pdr;
    auto state2 = new (rec2->possible_states) state_sensor_possible_states;

    rec2->hdr.type = 4;
    rec2->hdr.record_handle = 2;
    rec2->entity_type = 10;
    rec2->entity_instance = 1;
    rec2->container_id = 0;
    rec2->composite_sensor_count = 1;
    rec2->sensor_id = 200;
    state2->state_set_id = 1;
    state2->possible_states_size = 1;

    handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr2.data(), pdr2.size(), false, 1, &handle),
              0);

    auto result = getStateSensorPDRsByType(5, repo);
    EXPECT_EQ(result.size(), 1);

    pldm_pdr_destroy(repo);
}

TEST(GetStateSensorPDRsByType, testMultipleMatches)
{
    auto repo = pldm_pdr_init();

    std::vector<uint8_t> pdr1(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states));

    auto rec1 = new (pdr1.data()) pldm_state_sensor_pdr;
    auto state1 = new (rec1->possible_states) state_sensor_possible_states;

    rec1->hdr.type = 4;
    rec1->hdr.record_handle = 1;
    rec1->entity_type = 5;
    rec1->entity_instance = 1;
    rec1->container_id = 0;
    rec1->composite_sensor_count = 1;
    rec1->sensor_id = 100;
    state1->state_set_id = 1;
    state1->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr1.data(), pdr1.size(), false, 1, &handle),
              0);

    std::vector<uint8_t> pdr2(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states));

    auto rec2 = new (pdr2.data()) pldm_state_sensor_pdr;
    auto state2 = new (rec2->possible_states) state_sensor_possible_states;

    rec2->hdr.type = 4;
    rec2->hdr.record_handle = 2;
    rec2->entity_type = 5;
    rec2->entity_instance = 1;
    rec2->container_id = 0;
    rec2->composite_sensor_count = 1;
    rec2->sensor_id = 200;
    state2->state_set_id = 2;
    state2->possible_states_size = 1;

    handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr2.data(), pdr2.size(), false, 1, &handle),
              0);

    auto result = getStateSensorPDRsByType(5, repo);
    EXPECT_EQ(result.size(), 2);

    pldm_pdr_destroy(repo);
}

TEST(GetStateSensorPDRsByType, testNoMatch)
{
    auto repo = pldm_pdr_init();
    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states));

    auto rec = new (pdr.data()) pldm_state_sensor_pdr;
    auto state = new (rec->possible_states) state_sensor_possible_states;

    rec->hdr.type = 4;
    rec->hdr.record_handle = 1;
    rec->entity_type = 10;
    rec->entity_instance = 1;
    rec->container_id = 0;
    rec->composite_sensor_count = 1;
    rec->sensor_id = 100;
    state->state_set_id = 1;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    auto result = getStateSensorPDRsByType(5, repo);
    EXPECT_EQ(result.size(), 0);

    pldm_pdr_destroy(repo);
}

TEST(GetStateSensorPDRsByType, testNullRepo)
{
    auto result = getStateSensorPDRsByType(5, nullptr);
    EXPECT_EQ(result.size(), 0);
}

TEST(GetStateSensorPDRsByType, testEmptyRepo)
{
    auto repo = pldm_pdr_init();
    auto result = getStateSensorPDRsByType(5, repo);
    EXPECT_TRUE(result.empty());
    pldm_pdr_destroy(repo);
}

TEST(GetStateEffecterPDRsByType, testMatch)
{
    auto repo = pldm_pdr_init();

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));

    auto rec = new (pdr.data()) pldm_state_effecter_pdr;
    auto state = new (rec->possible_states) state_effecter_possible_states;

    rec->hdr.type = 11;
    rec->hdr.record_handle = 1;
    rec->entity_type = 33;
    rec->entity_instance = 1;
    rec->container_id = 0;
    rec->composite_effecter_count = 1;
    rec->effecter_id = 50;
    state->state_set_id = 196;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    auto result = getStateEffecterPDRsByType(33, repo);
    EXPECT_EQ(result.size(), 1);

    pldm_pdr_destroy(repo);
}

TEST(GetStateEffecterPDRsByType, testMultipleMatches)
{
    auto repo = pldm_pdr_init();

    std::vector<uint8_t> pdr1(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));

    auto rec1 = new (pdr1.data()) pldm_state_effecter_pdr;
    auto state1 = new (rec1->possible_states) state_effecter_possible_states;

    rec1->hdr.type = 11;
    rec1->hdr.record_handle = 1;
    rec1->entity_type = 33;
    rec1->entity_instance = 1;
    rec1->container_id = 0;
    rec1->composite_effecter_count = 1;
    rec1->effecter_id = 50;
    state1->state_set_id = 196;
    state1->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr1.data(), pdr1.size(), false, 1, &handle),
              0);

    std::vector<uint8_t> pdr2(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));

    auto rec2 = new (pdr2.data()) pldm_state_effecter_pdr;
    auto state2 = new (rec2->possible_states) state_effecter_possible_states;

    rec2->hdr.type = 11;
    rec2->hdr.record_handle = 2;
    rec2->entity_type = 33;
    rec2->entity_instance = 1;
    rec2->container_id = 0;
    rec2->composite_effecter_count = 1;
    rec2->effecter_id = 60;
    state2->state_set_id = 197;
    state2->possible_states_size = 1;

    handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr2.data(), pdr2.size(), false, 1, &handle),
              0);

    auto result = getStateEffecterPDRsByType(33, repo);
    EXPECT_EQ(result.size(), 2);

    pldm_pdr_destroy(repo);
}

TEST(GetStateEffecterPDRsByType, testNullRepo)
{
    auto result = getStateEffecterPDRsByType(33, nullptr);
    EXPECT_EQ(result.size(), 0);
}

TEST(GetStateEffecterPDRsByType, testEmptyRepo)
{
    auto repo = pldm_pdr_init();
    auto result = getStateEffecterPDRsByType(33, repo);
    EXPECT_TRUE(result.empty());
    pldm_pdr_destroy(repo);
}

TEST(GetStateEffecterPDRsByType, testNoMatch)
{
    auto repo = pldm_pdr_init();
    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));

    auto rec = new (pdr.data()) pldm_state_effecter_pdr;
    auto state = new (rec->possible_states) state_effecter_possible_states;

    rec->hdr.type = 11;
    rec->hdr.record_handle = 1;
    rec->entity_type = 44;
    rec->entity_instance = 1;
    rec->container_id = 0;
    rec->composite_effecter_count = 1;
    rec->effecter_id = 77;
    state->state_set_id = 196;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    auto result = getStateEffecterPDRsByType(33, repo);
    EXPECT_TRUE(result.empty());

    pldm_pdr_destroy(repo);
}

TEST(FindSensorIds, testNoMatch)
{
    auto repo = pldm_pdr_init();

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states));

    auto rec = new (pdr.data()) pldm_state_sensor_pdr;
    auto state = new (rec->possible_states) state_sensor_possible_states;

    rec->hdr.type = 4;
    rec->hdr.record_handle = 1;
    rec->entity_type = 5;
    rec->entity_instance = 1;
    rec->container_id = 0;
    rec->composite_sensor_count = 1;
    rec->sensor_id = 100;
    state->state_set_id = 1;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    auto result = findSensorIds(repo, 5, 2, 0);
    EXPECT_TRUE(result.empty());

    pldm_pdr_destroy(repo);
}

TEST(FindSensorIds, testNullRepo)
{
    auto result = findSensorIds(nullptr, 5, 1, 0);
    EXPECT_TRUE(result.empty());
}

TEST(FindSensorIds, testContainerIdMismatch)
{
    auto repo = pldm_pdr_init();

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states));

    auto rec = new (pdr.data()) pldm_state_sensor_pdr;
    auto state = new (rec->possible_states) state_sensor_possible_states;

    rec->hdr.type = 4;
    rec->hdr.record_handle = 1;
    rec->entity_type = 5;
    rec->entity_instance = 1;
    rec->container_id = 0;
    rec->composite_sensor_count = 1;
    rec->sensor_id = 100;
    state->state_set_id = 1;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    // entityType + entityInstance match but containerId doesn't
    auto result = findSensorIds(repo, 5, 1, 99);
    EXPECT_TRUE(result.empty());

    pldm_pdr_destroy(repo);
}

TEST(FindSensorIds, testEntityTypeMismatchInPDRFilter)
{
    // Adds 2 sensor PDRs: one with matching entity_type=5 and one with
    // entity_type=99.  getStateSensorPDRsByType iterates both, exercising the
    // entity_type mismatch branch in the PDR filter function.
    auto repo = pldm_pdr_init();

    // PDR with non-matching entity_type
    std::vector<uint8_t> pdr1(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states));
    auto rec1 = new (pdr1.data()) pldm_state_sensor_pdr;
    auto state1 = new (rec1->possible_states) state_sensor_possible_states;
    rec1->hdr.type = 4;
    rec1->hdr.record_handle = 1;
    rec1->entity_type = 99; // different entity type
    rec1->entity_instance = 1;
    rec1->container_id = 0;
    rec1->composite_sensor_count = 1;
    rec1->sensor_id = 100;
    state1->state_set_id = 1;
    state1->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr1.data(), pdr1.size(), false, 1, &handle),
              0);

    // PDR with matching entity_type
    std::vector<uint8_t> pdr2(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states));
    auto rec2 = new (pdr2.data()) pldm_state_sensor_pdr;
    auto state2 = new (rec2->possible_states) state_sensor_possible_states;
    rec2->hdr.type = 4;
    rec2->hdr.record_handle = 2;
    rec2->entity_type = 5; // matching entity type
    rec2->entity_instance = 1;
    rec2->container_id = 0;
    rec2->composite_sensor_count = 1;
    rec2->sensor_id = 200;
    state2->state_set_id = 1;
    state2->possible_states_size = 1;

    handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr2.data(), pdr2.size(), false, 1, &handle),
              0);

    // Only the matching PDR should be returned
    auto result = findSensorIds(repo, 5, 1, 0);
    EXPECT_EQ(result.size(), 1);
    EXPECT_EQ(result[0], 200);

    pldm_pdr_destroy(repo);
}

TEST(FindEffecterIds, testEntityTypeMismatchInPDRFilter)
{
    // Same pattern for effecter PDRs — non-matching entity_type PDR exercises
    // the false branch in getStateEffecterPDRsByType entity_type check.
    auto repo = pldm_pdr_init();

    // PDR with non-matching entity_type
    std::vector<uint8_t> pdr1(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));
    auto rec1 = new (pdr1.data()) pldm_state_effecter_pdr;
    auto state1 = new (rec1->possible_states) state_effecter_possible_states;
    rec1->hdr.type = 11;
    rec1->hdr.record_handle = 1;
    rec1->entity_type = 99; // different entity type
    rec1->entity_instance = 1;
    rec1->container_id = 0;
    rec1->composite_effecter_count = 1;
    rec1->effecter_id = 50;
    state1->state_set_id = 196;
    state1->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr1.data(), pdr1.size(), false, 1, &handle),
              0);

    // PDR with matching entity_type
    std::vector<uint8_t> pdr2(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));
    auto rec2 = new (pdr2.data()) pldm_state_effecter_pdr;
    auto state2 = new (rec2->possible_states) state_effecter_possible_states;
    rec2->hdr.type = 11;
    rec2->hdr.record_handle = 2;
    rec2->entity_type = 33; // matching entity type
    rec2->entity_instance = 1;
    rec2->container_id = 0;
    rec2->composite_effecter_count = 1;
    rec2->effecter_id = 60;
    state2->state_set_id = 196;
    state2->possible_states_size = 1;

    handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr2.data(), pdr2.size(), false, 1, &handle),
              0);

    auto result = findEffecterIds(repo, 33, 1, 0);
    EXPECT_EQ(result.size(), 1);
    EXPECT_EQ(result[0], 60);

    pldm_pdr_destroy(repo);
}

TEST(FindEffecterIds, testMatch)
{
    auto repo = pldm_pdr_init();

    std::vector<uint8_t> pdr1(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));

    auto rec1 = new (pdr1.data()) pldm_state_effecter_pdr;
    auto state1 = new (rec1->possible_states) state_effecter_possible_states;

    rec1->hdr.type = 11;
    rec1->hdr.record_handle = 1;
    rec1->entity_type = 33;
    rec1->entity_instance = 1;
    rec1->container_id = 0;
    rec1->composite_effecter_count = 1;
    rec1->effecter_id = 50;
    state1->state_set_id = 196;
    state1->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr1.data(), pdr1.size(), false, 1, &handle),
              0);

    std::vector<uint8_t> pdr2(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));

    auto rec2 = new (pdr2.data()) pldm_state_effecter_pdr;
    auto state2 = new (rec2->possible_states) state_effecter_possible_states;

    rec2->hdr.type = 11;
    rec2->hdr.record_handle = 2;
    rec2->entity_type = 33;
    rec2->entity_instance = 1;
    rec2->container_id = 0;
    rec2->composite_effecter_count = 1;
    rec2->effecter_id = 60;
    state2->state_set_id = 197;
    state2->possible_states_size = 1;

    handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr2.data(), pdr2.size(), false, 1, &handle),
              0);

    auto result = findEffecterIds(repo, 33, 1, 0);
    EXPECT_EQ(result.size(), 2);

    pldm_pdr_destroy(repo);
}

TEST(FindEffecterIds, testNoMatch)
{
    auto repo = pldm_pdr_init();

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));

    auto rec = new (pdr.data()) pldm_state_effecter_pdr;
    auto state = new (rec->possible_states) state_effecter_possible_states;

    rec->hdr.type = 11;
    rec->hdr.record_handle = 1;
    rec->entity_type = 33;
    rec->entity_instance = 1;
    rec->container_id = 0;
    rec->composite_effecter_count = 1;
    rec->effecter_id = 50;
    state->state_set_id = 196;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    auto result = findEffecterIds(repo, 33, 2, 0);
    EXPECT_TRUE(result.empty());

    pldm_pdr_destroy(repo);
}

TEST(FindEffecterIds, testNullRepo)
{
    auto result = findEffecterIds(nullptr, 33, 1, 0);
    EXPECT_TRUE(result.empty());
}

TEST(FindEffecterIds, testContainerIdMismatch)
{
    auto repo = pldm_pdr_init();

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
        sizeof(struct state_effecter_possible_states));

    auto rec = new (pdr.data()) pldm_state_effecter_pdr;
    auto state = new (rec->possible_states) state_effecter_possible_states;

    rec->hdr.type = 11;
    rec->hdr.record_handle = 1;
    rec->entity_type = 33;
    rec->entity_instance = 1;
    rec->container_id = 0;
    rec->composite_effecter_count = 1;
    rec->effecter_id = 50;
    state->state_set_id = 196;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    // entityType + entityInstance match but containerId doesn't
    auto result = findEffecterIds(repo, 33, 1, 99);
    EXPECT_TRUE(result.empty());

    pldm_pdr_destroy(repo);
}

TEST(FindStateSensorId, testMatch)
{
    auto repo = pldm_pdr_init();

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states));

    auto rec = new (pdr.data()) pldm_state_sensor_pdr;
    auto state = new (rec->possible_states) state_sensor_possible_states;

    rec->hdr.type = 4;
    rec->hdr.record_handle = 1;
    rec->entity_type = 5;
    rec->entity_instance = 1;
    rec->container_id = 0;
    rec->composite_sensor_count = 1;
    rec->sensor_id = 42;
    state->state_set_id = 1;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    auto result = findStateSensorId(repo, 1, 5, 1, 0, 1);
    EXPECT_EQ(result, 42);

    pldm_pdr_destroy(repo);
}

TEST(FindStateSensorId, testNoMatch)
{
    auto repo = pldm_pdr_init();

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states));

    auto rec = new (pdr.data()) pldm_state_sensor_pdr;
    auto state = new (rec->possible_states) state_sensor_possible_states;

    rec->hdr.type = 4;
    rec->hdr.record_handle = 1;
    rec->entity_type = 5;
    rec->entity_instance = 1;
    rec->container_id = 0;
    rec->composite_sensor_count = 1;
    rec->sensor_id = 42;
    state->state_set_id = 1;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    auto result = findStateSensorId(repo, 1, 99, 1, 0, 1);
    EXPECT_EQ(result, PLDM_INVALID_EFFECTER_ID);

    pldm_pdr_destroy(repo);
}

TEST(FindStateSensorId, testEntityInstanceMismatch)
{
    auto repo = pldm_pdr_init();

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states) * 2);

    auto rec = new (pdr.data()) pldm_state_sensor_pdr;
    auto stateStart = rec->possible_states;
    auto state = new (stateStart) state_sensor_possible_states;

    rec->hdr.type = 4;
    rec->hdr.record_handle = 1;
    rec->entity_type = 5;
    rec->entity_instance = 1;
    rec->container_id = 0;
    rec->composite_sensor_count = 2;
    rec->sensor_id = 42;
    state->state_set_id = 1;
    state->possible_states_size = 1;

    stateStart += state->possible_states_size + sizeof(state->state_set_id) +
                  sizeof(state->possible_states_size);
    state = new (stateStart) state_sensor_possible_states;
    state->state_set_id = 2;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    auto result = findStateSensorId(repo, 1, 5, 2, 0, 1);
    EXPECT_EQ(result, PLDM_INVALID_EFFECTER_ID);

    pldm_pdr_destroy(repo);
}

TEST(FindStateSensorId, testContainerIdMismatch)
{
    auto repo = pldm_pdr_init();

    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_sensor_pdr) - sizeof(uint8_t) +
        sizeof(struct state_sensor_possible_states));

    auto rec = new (pdr.data()) pldm_state_sensor_pdr;
    auto state = new (rec->possible_states) state_sensor_possible_states;

    rec->hdr.type = 4;
    rec->hdr.record_handle = 1;
    rec->entity_type = 5;
    rec->entity_instance = 1;
    rec->container_id = 0;
    rec->composite_sensor_count = 1;
    rec->sensor_id = 42;
    state->state_set_id = 1;
    state->possible_states_size = 1;

    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo, pdr.data(), pdr.size(), false, 1, &handle), 0);

    // entityType + entityInstance + stateSetId match but containerId doesn't
    auto result = findStateSensorId(repo, 1, 5, 1, 99, 1);
    EXPECT_EQ(result, PLDM_INVALID_EFFECTER_ID);

    pldm_pdr_destroy(repo);
}

// ===== Wave 2: MmapStream tests =====

TEST(MmapStreamBuf, seekoffFromBeginning)
{
    char buf[] = "0123456789";
    pldm::MmapStreamBuf sb(buf, 10);

    auto pos1 = sb.pubseekoff(5, std::ios_base::beg);
    EXPECT_EQ(pos1, 5);

    auto pos2 = sb.pubseekoff(0, std::ios_base::beg);
    EXPECT_EQ(pos2, 0);

    auto pos3 = sb.pubseekoff(10, std::ios_base::beg);
    EXPECT_EQ(pos3, 10);
}

TEST(MmapStreamBuf, seekoffFromCurrent)
{
    char buf[] = "0123456789";
    pldm::MmapStreamBuf sb(buf, 10);

    sb.pubseekoff(3, std::ios_base::beg);

    auto pos1 = sb.pubseekoff(2, std::ios_base::cur);
    EXPECT_EQ(pos1, 5);

    auto pos2 = sb.pubseekoff(-1, std::ios_base::cur);
    EXPECT_EQ(pos2, 4);
}

TEST(MmapStreamBuf, seekoffFromEnd)
{
    char buf[] = "0123456789";
    pldm::MmapStreamBuf sb(buf, 10);

    auto pos1 = sb.pubseekoff(-3, std::ios_base::end);
    EXPECT_EQ(pos1, 7);

    auto pos2 = sb.pubseekoff(0, std::ios_base::end);
    EXPECT_EQ(pos2, 10);
}

TEST(MmapStreamBuf, seekoffOutOfRange)
{
    char buf[] = "0123456789";
    pldm::MmapStreamBuf sb(buf, 10);

    auto pos1 = sb.pubseekoff(-1, std::ios_base::beg);
    EXPECT_EQ(pos1, -1);

    auto pos2 = sb.pubseekoff(11, std::ios_base::beg);
    EXPECT_EQ(pos2, -1);

    auto pos3 = sb.pubseekoff(1, std::ios_base::end);
    EXPECT_EQ(pos3, -1);
}

TEST(MmapStreamBuf, seekoffInvalidDirection)
{
    char buf[] = "0123456789";
    pldm::MmapStreamBuf sb(buf, 10);

    auto invalidDir = static_cast<std::ios_base::seekdir>(-1);
    auto pos = sb.pubseekoff(0, invalidDir);
    EXPECT_EQ(pos, -1);
}

TEST(MmapStreamBuf, seekpos)
{
    char buf[] = "0123456789";
    pldm::MmapStreamBuf sb(buf, 10);

    auto pos1 = sb.pubseekpos(5);
    EXPECT_EQ(pos1, 5);

    auto pos2 = sb.pubseekpos(0);
    EXPECT_EQ(pos2, 0);

    auto pos3 = sb.pubseekpos(10);
    EXPECT_EQ(pos3, 10);

    auto pos4 = sb.pubseekpos(-1);
    EXPECT_EQ(pos4, -1);

    auto pos5 = sb.pubseekpos(11);
    EXPECT_EQ(pos5, -1);
}

TEST(MmapStream, constructAndQuery)
{
    char buf[] = "Hello";
    pldm::MmapStream stream(buf, 5);

    EXPECT_EQ(stream.size(), 5u);
    EXPECT_EQ(stream.data(), reinterpret_cast<const uint8_t*>(buf));
}

TEST(MmapStream, readData)
{
    char buf[] = "ABCDE";
    pldm::MmapStream stream(buf, 5);

    EXPECT_EQ(stream.get(), 'A');
    EXPECT_EQ(stream.get(), 'B');
    EXPECT_EQ(stream.get(), 'C');
    EXPECT_EQ(stream.get(), 'D');
    EXPECT_EQ(stream.get(), 'E');
}

TEST(MmapStream, seekAndRead)
{
    char buf[] = "0123456789";
    pldm::MmapStream stream(buf, 10);

    stream.seekg(5);
    EXPECT_EQ(stream.get(), '5');
}

TEST(MmapFile, defaultConstruct)
{
    pldm::MmapFile mmapFile;

    EXPECT_EQ(mmapFile.data(), nullptr);
    EXPECT_EQ(mmapFile.size(), 0u);
}

TEST(MmapFile, mapValidFile)
{
    auto tmpPath = pldm::test::makeTempFile("test_mmap_XXXXXX");
    int fd = open(tmpPath.c_str(), O_RDWR);
    ASSERT_NE(fd, -1);

    const char* testData = "test data for mmap";
    size_t testLen = std::strlen(testData);
    ssize_t written = write(fd, testData, testLen);
    ASSERT_EQ(static_cast<size_t>(written), testLen);

    pldm::MmapFile mmapFile;
    bool result = mmapFile.map(fd, false);
    EXPECT_TRUE(result);
    EXPECT_NE(mmapFile.data(), nullptr);
    EXPECT_EQ(mmapFile.size(), testLen);

    EXPECT_EQ(std::memcmp(mmapFile.data(), testData, testLen), 0);

    mmapFile.unmap();
    EXPECT_EQ(mmapFile.data(), nullptr);
    EXPECT_EQ(mmapFile.size(), 0u);

    close(fd);
    unlink(tmpPath.c_str());
}

TEST(MmapFile, mapWithOwnedFd)
{
    auto tmpPath = pldm::test::makeTempFile("test_mmap_owned_XXXXXX");
    int fd = open(tmpPath.c_str(), O_RDWR);
    ASSERT_NE(fd, -1);

    const char* testData = "owned fd data";
    size_t testLen = std::strlen(testData);
    ssize_t written = write(fd, testData, testLen);
    ASSERT_EQ(static_cast<size_t>(written), testLen);

    {
        pldm::MmapFile mmapFile;
        bool result = mmapFile.map(fd, true);
        EXPECT_TRUE(result);
        EXPECT_NE(mmapFile.data(), nullptr);
        EXPECT_EQ(mmapFile.size(), testLen);
        EXPECT_EQ(std::memcmp(mmapFile.data(), testData, testLen), 0);
    }

    unlink(tmpPath.c_str());
}

TEST(MmapFile, mapInvalidFd)
{
    pldm::MmapFile mmapFile;
    bool result = mmapFile.map(-1, false);
    EXPECT_FALSE(result);
    EXPECT_EQ(mmapFile.data(), nullptr);
    EXPECT_EQ(mmapFile.size(), 0u);
}

TEST(MmapFile, mapWriteOnlyFdWithOwnedFdFailsAndClosesFd)
{
    auto tmpPath = pldm::test::makeTempFile("test_mmap_writeonly_XXXXXX");
    int initFd = open(tmpPath.c_str(), O_RDWR);
    ASSERT_NE(initFd, -1);

    const char* testData = "mmap failure with O_WRONLY";
    size_t testLen = std::strlen(testData);
    ssize_t written = write(initFd, testData, testLen);
    ASSERT_EQ(static_cast<size_t>(written), testLen);
    close(initFd);

    int fd = open(tmpPath.c_str(), O_WRONLY);
    ASSERT_NE(fd, -1);

    pldm::MmapFile mmapFile;
    bool result = mmapFile.map(fd, true);
    EXPECT_FALSE(result);
    EXPECT_EQ(mmapFile.data(), nullptr);
    EXPECT_EQ(mmapFile.size(), 0u);

    errno = 0;
    EXPECT_EQ(fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);

    unlink(tmpPath.c_str());
}

TEST(MmapFile, mapWriteOnlyFdWithoutOwnershipLeavesFdOpen)
{
    auto tmpPath = pldm::test::makeTempFile("test_mmap_writeonly_noown_XXXXXX");
    int initFd = open(tmpPath.c_str(), O_RDWR);
    ASSERT_NE(initFd, -1);

    const char* testData = "mmap failure with O_WRONLY no ownership";
    size_t testLen = std::strlen(testData);
    ssize_t written = write(initFd, testData, testLen);
    ASSERT_EQ(static_cast<size_t>(written), testLen);
    close(initFd);

    int fd = open(tmpPath.c_str(), O_WRONLY);
    ASSERT_NE(fd, -1);

    pldm::MmapFile mmapFile;
    bool result = mmapFile.map(fd, false);
    EXPECT_FALSE(result);
    EXPECT_EQ(mmapFile.data(), nullptr);
    EXPECT_EQ(mmapFile.size(), 0u);

    errno = 0;
    EXPECT_NE(fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(errno, 0);
    close(fd);
    unlink(tmpPath.c_str());
}

TEST(MmapFile, unmapWithoutMap)
{
    pldm::MmapFile mmapFile;

    mmapFile.unmap();

    EXPECT_EQ(mmapFile.data(), nullptr);
    EXPECT_EQ(mmapFile.size(), 0u);
}

TEST(MmapFile, heapConstructAndDestroy)
{
    auto* mmapFile = new pldm::MmapFile();
    EXPECT_EQ(mmapFile->data(), nullptr);
    EXPECT_EQ(mmapFile->size(), 0u);
    delete mmapFile;
}

// ===== Wave 4: InstanceIdDb tests =====

#include <filesystem>

static constexpr size_t pldmMaxInstanceIds = 32;

class InstanceIdDbTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        dbPath = pldm::test::makeTempFile("test_iid_XXXXXX");
        int fd = ::open(dbPath.c_str(), O_RDWR);
        ASSERT_NE(fd, -1);
        ::close(fd);
        // The instance DB file must be pre-sized to PLDM_MAX_TIDS * 32 bytes
        std::filesystem::resize_file(dbPath, 256u * pldmMaxInstanceIds);
    }

    void TearDown() override
    {
        std::filesystem::remove(dbPath);
    }

    std::filesystem::path dbPath;
};

// Test-only mirror of libpldm requester/instance-id.c internals.
// Used to drive specific error paths in InstanceIdDb wrapper.
struct TestPldmTidState
{
    uint8_t prev;
    uint32_t allocations;
};

struct TestPldmInstanceDb
{
    TestPldmTidState state[256];
    int lockDbFd;
};

static TestPldmInstanceDb* getRawInstanceDb(pldm::InstanceIdDb& db)
{
    auto* rawDbPtr = reinterpret_cast<void**>(&db);
    return reinterpret_cast<TestPldmInstanceDb*>(*rawDbPtr);
}

TEST_F(InstanceIdDbTest, constructWithTempPath)
{
    pldm::InstanceIdDb db(dbPath.string());
}

TEST_F(InstanceIdDbTest, nextAndFree)
{
    pldm::InstanceIdDb db(dbPath.string());
    uint8_t tid = 1;
    auto id = db.next(tid);
    ASSERT_TRUE(id.has_value());
    EXPECT_LT(id.value(), 32);
    db.free(tid, id.value());
}

TEST_F(InstanceIdDbTest, freeUnallocated)
{
    pldm::InstanceIdDb db(dbPath.string());
    EXPECT_THROW(db.free(1, 5), std::runtime_error);
}

TEST_F(InstanceIdDbTest, defaultConstructor)
{
    try
    {
        pldm::InstanceIdDb db;
    }
    catch (const std::error_condition&)
    {
        GTEST_SKIP() << "Default instance ID DB is not available in this env";
    }
}

TEST_F(InstanceIdDbTest, nextThrowsWhenNoFreeInstanceIds)
{
    pldm::InstanceIdDb db(dbPath.string());
    constexpr uint8_t tid = 4;

    std::vector<uint8_t> allocated;
    allocated.reserve(32);
    for (int i = 0; i < 32; ++i)
    {
        auto id = db.next(tid);
        ASSERT_TRUE(id.has_value());
        allocated.emplace_back(id.value());
    }

    auto result = db.next(tid);
    EXPECT_FALSE(result.has_value());
}

TEST_F(InstanceIdDbTest, nextThrowsSystemErrorOnProtocolState)
{
    pldm::InstanceIdDb db(dbPath.string());
    constexpr uint8_t tid = 7;

    auto* rawDb = getRawInstanceDb(db);
    ASSERT_NE(rawDb, nullptr);
    rawDb->state[tid].prev = 32;

    auto result = db.next(tid);
    EXPECT_FALSE(result.has_value());
}

TEST_F(InstanceIdDbTest, freeThrowsSystemErrorOnFcntlFailure)
{
    pldm::InstanceIdDb db(dbPath.string());
    constexpr uint8_t tid = 9;
    auto id = db.next(tid);
    ASSERT_TRUE(id.has_value());

    auto* rawDb = getRawInstanceDb(db);
    ASSERT_NE(rawDb, nullptr);
    rawDb->lockDbFd = -1;

    EXPECT_THROW(db.free(tid, id.value()), std::error_condition);
}

TEST(InstanceIdDb, constructInvalidPath)
{
    EXPECT_THROW(pldm::InstanceIdDb db("/nonexistent/path/iid_db"),
                 std::error_condition);
}

// ===== Wave 5: FlightRecorder tests =====

TEST(FlightRecorder, getInstance)
{
    auto& inst1 = pldm::flightrecorder::FlightRecorder::GetInstance();
    auto& inst2 = pldm::flightrecorder::FlightRecorder::GetInstance();
    EXPECT_EQ(&inst1, &inst2);
}

TEST(FlightRecorder, saveRecord)
{
    auto& recorder = pldm::flightrecorder::FlightRecorder::GetInstance();
    std::vector<uint8_t> data = {0x01, 0x02, 0x03};
    recorder.saveRecord(data, true);
    recorder.saveRecord(data, false);
}

TEST(FlightRecorder, playRecorder)
{
    auto& recorder = pldm::flightrecorder::FlightRecorder::GetInstance();
    std::vector<uint8_t> data = {0x10, 0x20, 0x30};
    recorder.saveRecord(data, true);
    recorder.playRecorder();

    // Verify dump file was created (path: /tmp/pldm_flight_recorder)
    std::ifstream dumpFile(pldm::flightrecorder::flightRecorderDumpPath);
    if (dumpFile.good())
    {
        std::string line;
        std::getline(dumpFile, line);
        EXPECT_FALSE(line.empty());
        dumpFile.close();
        unlink(pldm::flightrecorder::flightRecorderDumpPath);
    }
}

TEST(FlightRecorder, wrapAround)
{
    auto& recorder = pldm::flightrecorder::FlightRecorder::GetInstance();
    // Save more than FLIGHT_RECORDER_MAX_ENTRIES to exercise wrap-around
    for (int i = 0; i < FLIGHT_RECORDER_MAX_ENTRIES + 2; i++)
    {
        std::vector<uint8_t> data = {static_cast<uint8_t>(i)};
        recorder.saveRecord(data, (i % 2 == 0));
    }
    recorder.playRecorder();

    std::ifstream dumpFile(pldm::flightrecorder::flightRecorderDumpPath);
    if (dumpFile.good())
    {
        dumpFile.close();
        unlink(pldm::flightrecorder::flightRecorderDumpPath);
    }
}

TEST(CustomFDTest, constructWithValidFd)
{
    auto tmpPath = pldm::test::makeTempFile("customfd_valid_XXXXXX");
    int rawFd = open(tmpPath.c_str(), O_RDWR);
    ASSERT_GE(rawFd, 0) << "mkstemp failed";
    unlink(tmpPath.c_str());

    CustomFD cfd(rawFd);
    EXPECT_EQ(cfd(), rawFd);
    EXPECT_NE(fcntl(cfd(), F_GETFD), -1)
        << "fd should be valid while CustomFD is alive";
}

TEST(CustomFDTest, constructWithInvalidFd)
{
    CustomFD cfd(-1);
    EXPECT_EQ(cfd(), -1);
}

TEST(CustomFDTest, destructorClosesFd)
{
    auto tmpPath = pldm::test::makeTempFile("customfd_close_XXXXXX");
    int rawFd = open(tmpPath.c_str(), O_RDWR);
    ASSERT_GE(rawFd, 0) << "mkstemp failed";
    unlink(tmpPath.c_str());

    {
        CustomFD cfd(rawFd);
        EXPECT_NE(fcntl(rawFd, F_GETFD), -1)
            << "fd should be valid inside scope";
    }
    // After CustomFD goes out of scope, the fd must be closed
    EXPECT_EQ(fcntl(rawFd, F_GETFD), -1)
        << "fd should be closed after CustomFD destruction";
}

TEST(CustomFDTest, destructorSafeWithNegativeFd)
{
    // Constructing and destroying a CustomFD with -1 must not crash
    {
        CustomFD cfd(-1);
        EXPECT_EQ(cfd(), -1);
    }
}

TEST(LogRateLimiterTest, suppressesRepeatedLogsWithinInterval)
{
    LogRateLimiter<int> limiter(std::chrono::seconds(60));

    EXPECT_TRUE(limiter.shouldLog(7));

    limiter.recordLog(7);

    ASSERT_TRUE(limiter.lastLogTime.contains(7));
    EXPECT_FALSE(limiter.shouldLog(7));

    limiter.clear(99);
    EXPECT_TRUE(limiter.lastLogTime.contains(7));

    limiter.clear(7);
    EXPECT_TRUE(limiter.lastLogTime.contains(7));
}

TEST(LogRateLimiterTest, zeroIntervalAllowsImmediateRelogAndClear)
{
    LogRateLimiter<int> limiter(std::chrono::seconds::zero());

    limiter.recordLog(11);

    ASSERT_TRUE(limiter.lastLogTime.contains(11));
    EXPECT_TRUE(limiter.shouldLog(11));

    limiter.clear(11);
    EXPECT_FALSE(limiter.lastLogTime.contains(11));
    EXPECT_TRUE(limiter.shouldLog(11));
}

TEST(LogRateLimiterTest, keyVariantsCoverClearBranches)
{
    auto exerciseClearBranches = []<typename Key>(Key key) {
        LogRateLimiter<Key> intervalLimiter(std::chrono::seconds(60));
        intervalLimiter.clear(key);
        EXPECT_FALSE(intervalLimiter.lastLogTime.contains(key));

        intervalLimiter.recordLog(key);
        intervalLimiter.clear(key);
        EXPECT_TRUE(intervalLimiter.lastLogTime.contains(key));

        LogRateLimiter<Key> zeroLimiter(std::chrono::seconds::zero());
        zeroLimiter.recordLog(key);
        ASSERT_TRUE(zeroLimiter.lastLogTime.contains(key));
        zeroLimiter.clear(key);
        EXPECT_FALSE(zeroLimiter.lastLogTime.contains(key));
        zeroLimiter.clear(key);
        EXPECT_FALSE(zeroLimiter.lastLogTime.contains(key));
    };

    exerciseClearBranches(uint8_t{7});
    exerciseClearBranches(uint64_t{42});
}

// Helper: encode a BIOS string table entry into a byte vector.
// Returns the encoded entry bytes.
static std::vector<uint8_t> encodeBiosStringEntry(const std::string& str)
{
    uint16_t strLen = static_cast<uint16_t>(str.length());
    size_t entryLen = pldm_bios_table_string_entry_encode_length(strLen);
    std::vector<uint8_t> entry(entryLen, 0);
    int rc = pldm_bios_table_string_entry_encode(entry.data(), entry.size(),
                                                 str.c_str(), strLen);
    EXPECT_EQ(rc, PLDM_SUCCESS);
    return entry;
}

// Helper: build a multi-entry BIOS string table by concatenating encoded
// entries.
static std::vector<uint8_t> buildBiosStringTable(
    const std::vector<std::string>& strings)
{
    std::vector<uint8_t> table;
    for (const auto& s : strings)
    {
        auto entry = encodeBiosStringEntry(s);
        table.insert(table.end(), entry.begin(), entry.end());
    }
    return table;
}

using BIOSStringTableIter =
    pldm::bios::utils::BIOSTableIter<PLDM_BIOS_STRING_TABLE>;
using BIOSAttrTableIter =
    pldm::bios::utils::BIOSTableIter<PLDM_BIOS_ATTR_TABLE>;
using BIOSAttrValueTableIter =
    pldm::bios::utils::BIOSTableIter<PLDM_BIOS_ATTR_VAL_TABLE>;

TEST(BIOSTableIterTest, iterateStringTable)
{
    // Build a table with 3 string entries
    auto table = buildBiosStringTable({"Alpha", "Bravo", "Charlie"});

    BIOSStringTableIter biosIter(table.data(), table.size());
    int count = 0;
    for (auto it = biosIter.begin(); it != biosIter.end(); ++it)
    {
        // Dereference should return a non-null entry pointer
        const auto* entry = *it;
        ASSERT_NE(entry, nullptr);
        count++;
    }
    EXPECT_EQ(count, 3);
}

TEST(BIOSTableIterTest, emptyTable)
{
    // A zero-length table should have begin() == end() immediately
    std::vector<uint8_t> emptyData;
    BIOSStringTableIter biosIter(emptyData.data(), 0);
    auto it = biosIter.begin();
    EXPECT_TRUE(it == biosIter.end());
}

TEST(BIOSTableIterTest, singleEntry)
{
    auto table = buildBiosStringTable({"OnlyOne"});

    BIOSStringTableIter biosIter(table.data(), table.size());
    int count = 0;
    for (auto it = biosIter.begin(); it != biosIter.end(); ++it)
    {
        const auto* entry = *it;
        ASSERT_NE(entry, nullptr);
        count++;
    }
    EXPECT_EQ(count, 1);
}

TEST(BIOSTableIterTest, dereferenceReturnsValidEntry)
{
    const std::string expected = "HelloBIOS";
    auto table = buildBiosStringTable({expected});

    BIOSStringTableIter biosIter(table.data(), table.size());
    auto it = biosIter.begin();
    ASSERT_TRUE(it != biosIter.end());

    const auto* entry = *it;
    ASSERT_NE(entry, nullptr);

    // Decode the string length and verify it matches
    uint16_t decodedLen =
        pldm_bios_table_string_entry_decode_string_length(entry);
    EXPECT_EQ(decodedLen, expected.length());

    // Decode the string content and verify it matches
    std::vector<char> buffer(decodedLen + 1, '\0');
    int rc = pldm_bios_table_string_entry_decode_string(entry, buffer.data(),
                                                        buffer.size());
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_STREQ(buffer.data(), expected.c_str());
}

TEST(BIOSTableIterTest, emptyAttrTable)
{
    std::vector<uint8_t> emptyData;
    BIOSAttrTableIter biosIter(emptyData.data(), 0);
    auto it = biosIter.begin();
    EXPECT_TRUE(it == biosIter.end());
}

TEST(BIOSTableIterTest, emptyAttrValueTable)
{
    std::vector<uint8_t> emptyData;
    BIOSAttrValueTableIter biosIter(emptyData.data(), 0);
    auto it = biosIter.begin();
    EXPECT_TRUE(it == biosIter.end());
}

// ===== extractPldmType tests =====

using pldm::transport::extractPldmType;
using pldm::transport::MctpError;

TEST(ExtractPldmType, validPldmType)
{
    MctpError error;
    memset(&error, 0, sizeof(error));
    error.msg_type = MCTP_MSG_TYPE_PLDM;
    error.payload_len = 3;
    error.payload[1] = 0x05; // PLDM_FWUP
    EXPECT_EQ(extractPldmType(error), 0x05);
}

TEST(ExtractPldmType, nonPldmMsgType)
{
    MctpError error;
    memset(&error, 0, sizeof(error));
    error.msg_type = 0x00;
    EXPECT_EQ(extractPldmType(error), 0xFF);
}

TEST(ExtractPldmType, payloadTooShort)
{
    MctpError error;
    memset(&error, 0, sizeof(error));
    error.msg_type = MCTP_MSG_TYPE_PLDM;
    error.payload_len = 1;
    EXPECT_EQ(extractPldmType(error), 0xFF);
}

TEST(ExtractPldmType, payloadLenZero)
{
    MctpError error;
    memset(&error, 0, sizeof(error));
    error.msg_type = MCTP_MSG_TYPE_PLDM;
    error.payload_len = 0;
    EXPECT_EQ(extractPldmType(error), 0xFF);
}

TEST(ExtractPldmType, typeMaskedCorrectly)
{
    MctpError error;
    memset(&error, 0, sizeof(error));
    error.msg_type = MCTP_MSG_TYPE_PLDM;
    error.payload_len = 2;
    error.payload[1] = 0xFF; // upper bits set, mask 0x3F should yield 0x3F
    EXPECT_EQ(extractPldmType(error), 0x3F);
}

TEST(ExtractPldmType, pldmBase)
{
    MctpError error;
    memset(&error, 0, sizeof(error));
    error.msg_type = MCTP_MSG_TYPE_PLDM;
    error.payload_len = 2;
    error.payload[1] = 0x00; // PLDM_BASE
    EXPECT_EQ(extractPldmType(error), 0x00);
}

TEST(ExtractPldmType, pldmPlatform)
{
    MctpError error;
    memset(&error, 0, sizeof(error));
    error.msg_type = MCTP_MSG_TYPE_PLDM;
    error.payload_len = 2;
    error.payload[1] = 0x02; // PLDM_PLATFORM
    EXPECT_EQ(extractPldmType(error), 0x02);
}

/*****************************************************************************
 * MatchEntryInfo tests
 *
 * Use the concrete FirmwareInventoryInfo alias
 *   = MatchEntryInfo<MatchFirmwareInfo, FirmwareInfo>
 * where
 *   MatchFirmwareInfo = std::vector<std::tuple<DBusIntfMatch, FirmwareInfo>>
 *   DBusIntfMatch     = std::pair<dbus::Interface, dbus::PropertyMap>
 *   FirmwareInfo      = std::tuple<CreateComponentIdNameMap,
 *                                  UpdateComponentIdNameMap>
 ****************************************************************************/

using namespace pldm::fw_update;

// Namespace alias to avoid ambiguity with
// pldm::utils::{PropertyMap,InterfaceMap}
namespace dbus = pldm::dbus;

static FirmwareInfo makeFirmwareInfo();

// ---------------------------------------------------------------------------
// compareValues tests
// ---------------------------------------------------------------------------

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("O0")
#endif

TEST(MatchEntryInfoTest, compareValuesSameTypeArithmetic)
{
    // Same type, same value
    EXPECT_TRUE(
        (FirmwareInventoryInfo::compareValues(uint32_t(42), uint32_t(42))));

    // Same type, different value
    EXPECT_FALSE(
        (FirmwareInventoryInfo::compareValues(uint32_t(1), uint32_t(2))));
}

TEST(MatchEntryInfoTest, compareValuesCrossTypeArithmetic)
{
    // Different arithmetic types, same numeric value
    EXPECT_TRUE(
        (FirmwareInventoryInfo::compareValues(uint32_t(42), int64_t(42))));

    EXPECT_TRUE(
        (FirmwareInventoryInfo::compareValues(uint16_t(5), uint32_t(5))));
}

TEST(MatchEntryInfoTest, compareValuesSameTypeString)
{
    using std::string;

    EXPECT_TRUE((FirmwareInventoryInfo::compareValues(string("hello"),
                                                      string("hello"))));

    EXPECT_FALSE(
        (FirmwareInventoryInfo::compareValues(string("a"), string("b"))));
}

TEST(MatchEntryInfoTest, compareValuesIncompatibleTypes)
{
    // string vs uint32_t -- incompatible, must return false
    EXPECT_FALSE((
        FirmwareInventoryInfo::compareValues(std::string("42"), uint32_t(42))));
}

TEST(MatchEntryInfoTest, DefaultCtorAndDtorCoveredForAllAliases)
{
    FirmwareInventoryInfo deviceInventoryInfo{};
    FirmwareInventoryInfo firmwareInventoryInfo{};
    ComponentNameMapInfo componentNameMapInfo{};

    EXPECT_TRUE(deviceInventoryInfo.infos.empty());
    EXPECT_TRUE(firmwareInventoryInfo.infos.empty());
    EXPECT_TRUE(componentNameMapInfo.infos.empty());
}

static const std::array<dbus::Value, 12>& allDbusValues()
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

template <typename InventoryInfoType>
__attribute__((noinline)) static bool invokeIsPropertyMatchNoInline(
    const InventoryInfoType& inv, const dbus::InterfaceMap& ifaceMap,
    const std::pair<dbus::Property, dbus::Value>& cfgProp,
    const dbus::Interface& interface)
{
    auto fn = &InventoryInfoType::isPropertyMatch;
    return (inv.*fn)(ifaceMap, cfgProp, interface);
}

template <typename InventoryInfoType, typename EntryType>
__attribute__((noinline)) static bool invokeMatchInventoryEntryNoInline(
    const InventoryInfoType& inv, const dbus::InterfaceMap& ifaceMap,
    EntryType& entry)
{
    auto fn = &InventoryInfoType::matchInventoryEntry;
    return (inv.*fn)(ifaceMap, entry);
}

static bool expectedCompareResult(const dbus::Value& expected,
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
__attribute__((noinline)) static bool invokeCompareValuesNoInline(
    const dbus::Value& expected, const dbus::Value& actual)
{
    return std::visit(
        [](const auto& lhs, const auto& rhs) {
            return InventoryInfoType::compareValues(lhs, rhs);
        },
        expected, actual);
}

TEST(MatchEntryInfoTest, compareValuesAllValueTypePairsForAllAliases)
{
    for (size_t expectedIdx = 0; expectedIdx < allDbusValues().size();
         ++expectedIdx)
    {
        for (size_t actualIdx = 0; actualIdx < allDbusValues().size();
             ++actualIdx)
        {
            const auto expectedVal = allDbusValues()[expectedIdx];
            const auto actualVal = allDbusValues()[actualIdx];
            const bool expectedResult =
                expectedCompareResult(expectedVal, actualVal);

            EXPECT_EQ(invokeCompareValuesNoInline<FirmwareInventoryInfo>(
                          expectedVal, actualVal),
                      expectedResult)
                << "FirmwareInventoryInfo pair mismatch (" << expectedIdx
                << ", " << actualIdx << ")";
            EXPECT_EQ(invokeCompareValuesNoInline<ComponentNameMapInfo>(
                          expectedVal, actualVal),
                      expectedResult)
                << "ComponentNameMapInfo pair mismatch (" << expectedIdx << ", "
                << actualIdx << ")";
        }
    }
}

TEST(MatchEntryInfoTest, isPropertyMatchAllValueTypePairs)
{
    constexpr const char* interface = "com.example.AllPairs";
    constexpr const char* property = "Value";

    for (size_t expectedIdx = 0; expectedIdx < allDbusValues().size();
         ++expectedIdx)
    {
        for (size_t actualIdx = 0; actualIdx < allDbusValues().size();
             ++actualIdx)
        {
            const auto expectedVal = allDbusValues()[expectedIdx];
            const auto actualVal = allDbusValues()[actualIdx];
            const bool expectedResult =
                expectedCompareResult(expectedVal, actualVal);

            dbus::InterfaceMap ifaceMap = {
                {interface, dbus::PropertyMap{{property, actualVal}}}};
            std::pair<dbus::Property, dbus::Value> cfgProp{property,
                                                           expectedVal};
            dbus::PropertyMap cfgPropMap = {cfgProp};

            {
                MatchFirmwareInfo matchInfos;
                matchInfos.push_back(
                    {DBusIntfMatch{interface, cfgPropMap}, makeFirmwareInfo()});
                FirmwareInventoryInfo inv(matchInfos);
                EXPECT_EQ(inv.isPropertyMatch(ifaceMap, cfgProp, interface),
                          expectedResult)
                    << "FirmwareInventoryInfo mismatch for pair ("
                    << expectedIdx << ", " << actualIdx << ")";
            }

            {
                MatchComponentNameMapInfo matchInfos;
                matchInfos.push_back({DBusIntfMatch{interface, cfgPropMap},
                                      ComponentIdNameMap{{1, "comp1"}}});
                ComponentNameMapInfo inv(matchInfos);
                EXPECT_EQ(inv.isPropertyMatch(ifaceMap, cfgProp, interface),
                          expectedResult)
                    << "ComponentNameMapInfo mismatch for pair (" << expectedIdx
                    << ", " << actualIdx << ")";
            }
        }
    }
}

TEST(MatchEntryInfoTest, isPropertyMatchAllValueTypesNoInlineDispatch)
{
    constexpr const char* interface = "com.example.NoInline";
    constexpr const char* property = "Value";

    for (const auto& value : allDbusValues())
    {
        dbus::PropertyMap cfgProps = {{property, value}};
        dbus::InterfaceMap ifaceMap = {{interface, cfgProps}};
        std::pair<dbus::Property, dbus::Value> cfgProp{property, value};

        {
            MatchFirmwareInfo matchInfos;
            matchInfos.push_back(
                {DBusIntfMatch{interface, cfgProps}, makeFirmwareInfo()});
            FirmwareInventoryInfo inv(matchInfos);
            EXPECT_TRUE(invokeIsPropertyMatchNoInline(inv, ifaceMap, cfgProp,
                                                      interface));
        }

        {
            MatchComponentNameMapInfo matchInfos;
            matchInfos.push_back({DBusIntfMatch{interface, cfgProps},
                                  ComponentIdNameMap{{1, "comp1"}}});
            ComponentNameMapInfo inv(matchInfos);
            EXPECT_TRUE(invokeIsPropertyMatchNoInline(inv, ifaceMap, cfgProp,
                                                      interface));
        }
    }
}

TEST(MatchEntryInfoTest, matchInventoryEntryPropertyPathNoInlineDispatch)
{
    {
        dbus::PropertyMap cfgProps = {
            {"Name", dbus::Value{std::string("GPU0")}}};
        dbus::PropertyMap dbusProps = {
            {"Name", dbus::Value{std::string("GPU0")}},
            {"Extra", dbus::Value{uint32_t(99)}}};
        dbus::InterfaceMap ifaceMap = {{"com.example.Device", dbusProps}};
        FirmwareInfo expected = makeFirmwareInfo();

        MatchFirmwareInfo matchInfos;
        matchInfos.push_back(
            {DBusIntfMatch{"com.example.Device", cfgProps}, expected});
        FirmwareInventoryInfo inv(matchInfos);
        FirmwareInfo result{};

        EXPECT_TRUE(invokeMatchInventoryEntryNoInline(inv, ifaceMap, result));
        EXPECT_EQ(result, expected);
    }

    {
        dbus::PropertyMap cfgProps = {
            {"Model", dbus::Value{std::string("FW1")}}};
        dbus::PropertyMap dbusProps = {
            {"Model", dbus::Value{std::string("FW1")}},
            {"Extra", dbus::Value{uint32_t(99)}}};
        dbus::InterfaceMap ifaceMap = {{"com.example.Firmware", dbusProps}};
        FirmwareInfo expected = makeFirmwareInfo();

        MatchFirmwareInfo matchInfos;
        matchInfos.push_back(
            {DBusIntfMatch{"com.example.Firmware", cfgProps}, expected});
        FirmwareInventoryInfo inv(matchInfos);
        FirmwareInfo result{};

        EXPECT_TRUE(invokeMatchInventoryEntryNoInline(inv, ifaceMap, result));
        EXPECT_EQ(result, expected);
    }

    {
        dbus::PropertyMap cfgProps = {
            {"Name", dbus::Value{std::string("GPU0")}}};
        dbus::PropertyMap dbusProps = {
            {"Name", dbus::Value{std::string("GPU0")}},
            {"Extra", dbus::Value{uint32_t(42)}}};
        dbus::InterfaceMap ifaceMap = {{"com.example.Component", dbusProps}};
        ComponentIdNameMap expected = {{10, "compA"}};

        MatchComponentNameMapInfo matchInfos;
        matchInfos.push_back(
            {DBusIntfMatch{"com.example.Component", cfgProps}, expected});
        ComponentNameMapInfo inv(matchInfos);
        ComponentIdNameMap result;

        EXPECT_TRUE(invokeMatchInventoryEntryNoInline(inv, ifaceMap, result));
        EXPECT_EQ(result, expected);
    }
}

// ---------------------------------------------------------------------------
// isDirectMatch tests
// ---------------------------------------------------------------------------

TEST(MatchEntryInfoTest, isDirectMatchSuccess)
{
    // Build an InterfaceMap that contains an interface with one property.
    dbus::PropertyMap cfgProps = {{"Name", dbus::Value{std::string("GPU0")}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Device", cfgProps}};

    // Construct a FirmwareInventoryInfo with one entry.
    MatchFirmwareInfo matchInfos;
    DBusIntfMatch match =
        std::make_pair(std::string("com.example.Device"), cfgProps);
    matchInfos.push_back(std::make_tuple(match, makeFirmwareInfo()));

    FirmwareInventoryInfo inv(matchInfos);

    EXPECT_TRUE(inv.isDirectMatch(ifaceMap, "com.example.Device", cfgProps));
}

TEST(MatchEntryInfoTest, isDirectMatchFail)
{
    // Interface exists but property value differs.
    dbus::PropertyMap dbusProps = {{"Name", dbus::Value{std::string("GPU1")}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Device", dbusProps}};

    dbus::PropertyMap cfgProps = {{"Name", dbus::Value{std::string("GPU0")}}};
    MatchFirmwareInfo matchInfos;
    DBusIntfMatch match =
        std::make_pair(std::string("com.example.Device"), cfgProps);
    matchInfos.push_back(std::make_tuple(match, makeFirmwareInfo()));

    FirmwareInventoryInfo inv(matchInfos);

    EXPECT_FALSE(inv.isDirectMatch(ifaceMap, "com.example.Device", cfgProps));
}

TEST(MatchEntryInfoTest, isDirectMatchMissingInterface)
{
    // The queried interface does not exist in the InterfaceMap at all.
    dbus::PropertyMap dbusProps = {{"Name", dbus::Value{std::string("GPU0")}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Other", dbusProps}};

    dbus::PropertyMap cfgProps = {{"Name", dbus::Value{std::string("GPU0")}}};
    MatchFirmwareInfo matchInfos;
    DBusIntfMatch match =
        std::make_pair(std::string("com.example.Device"), cfgProps);
    matchInfos.push_back(std::make_tuple(match, makeFirmwareInfo()));

    FirmwareInventoryInfo inv(matchInfos);

    EXPECT_FALSE(inv.isDirectMatch(ifaceMap, "com.example.Device", cfgProps));
}

// ---------------------------------------------------------------------------
// isPropertyMatch tests
// ---------------------------------------------------------------------------

TEST(MatchEntryInfoTest, isPropertyMatchSuccess)
{
    dbus::PropertyMap dbusProps = {{"Name", dbus::Value{std::string("GPU0")}},
                                   {"Id", dbus::Value{uint32_t(7)}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Device", dbusProps}};

    // Match just one property.
    std::pair<dbus::Property, dbus::Value> cfgProp{"Id",
                                                   dbus::Value{uint32_t(7)}};

    dbus::PropertyMap cfgPropMap = {cfgProp};
    MatchFirmwareInfo matchInfos;
    DBusIntfMatch match =
        std::make_pair(std::string("com.example.Device"), cfgPropMap);
    matchInfos.push_back(std::make_tuple(match, makeFirmwareInfo()));

    FirmwareInventoryInfo inv(matchInfos);

    EXPECT_TRUE(inv.isPropertyMatch(ifaceMap, cfgProp, "com.example.Device"));
}

TEST(MatchEntryInfoTest, isPropertyMatchMissingProperty)
{
    dbus::PropertyMap dbusProps = {{"Name", dbus::Value{std::string("GPU0")}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Device", dbusProps}};

    // Ask for a property that doesn't exist in the InterfaceMap.
    std::pair<dbus::Property, dbus::Value> cfgProp{"Missing",
                                                   dbus::Value{uint32_t(1)}};

    dbus::PropertyMap cfgPropMap = {cfgProp};
    MatchFirmwareInfo matchInfos;
    DBusIntfMatch match =
        std::make_pair(std::string("com.example.Device"), cfgPropMap);
    matchInfos.push_back(std::make_tuple(match, makeFirmwareInfo()));

    FirmwareInventoryInfo inv(matchInfos);

    EXPECT_FALSE(inv.isPropertyMatch(ifaceMap, cfgProp, "com.example.Device"));
}

TEST(MatchEntryInfoTest, isPropertyMatchAllValueTypes)
{
    // Exercise the visitor lambda (line 327) for every dbus::Value alternative
    // across both FirmwareInventoryInfo and FirmwareInventoryInfo
    // instantiations.
    auto testWithDevice = [](const char* name, dbus::Value val) {
        dbus::PropertyMap dbusProps = {{name, val}};
        dbus::InterfaceMap ifaceMap = {{"com.example.Dev", dbusProps}};
        std::pair<dbus::Property, dbus::Value> cfgProp{name, val};
        dbus::PropertyMap cfgPropMap = {cfgProp};
        MatchFirmwareInfo mi;
        mi.push_back(
            {DBusIntfMatch{"com.example.Dev", cfgPropMap}, makeFirmwareInfo()});
        FirmwareInventoryInfo inv(mi);
        EXPECT_TRUE(inv.isPropertyMatch(ifaceMap, cfgProp, "com.example.Dev"))
            << "FirmwareInventoryInfo failed for: " << name;
    };

    auto testWithFirmware = [](const char* name, dbus::Value val) {
        dbus::PropertyMap dbusProps = {{name, val}};
        dbus::InterfaceMap ifaceMap = {{"com.example.Dev", dbusProps}};
        std::pair<dbus::Property, dbus::Value> cfgProp{name, val};
        dbus::PropertyMap cfgPropMap = {cfgProp};
        MatchFirmwareInfo mi;
        FirmwareInfo fi{CreateComponentIdNameMap{}, UpdateComponentIdNameMap{}};
        mi.push_back({DBusIntfMatch{"com.example.Dev", cfgPropMap}, fi});
        FirmwareInventoryInfo inv(mi);
        EXPECT_TRUE(inv.isPropertyMatch(ifaceMap, cfgProp, "com.example.Dev"))
            << "FirmwareInventoryInfo failed for: " << name;
    };

    // All 12 dbus::Value alternatives
    const std::vector<std::pair<const char*, dbus::Value>> cases = {
        {"b", dbus::Value{true}},
        {"y", dbus::Value{uint8_t(1)}},
        {"n", dbus::Value{int16_t(-1)}},
        {"q", dbus::Value{uint16_t(2)}},
        {"i", dbus::Value{int32_t(-3)}},
        {"u", dbus::Value{uint32_t(4)}},
        {"x", dbus::Value{int64_t(-5)}},
        {"t", dbus::Value{uint64_t(6)}},
        {"d", dbus::Value{double(7.7)}},
        {"s", dbus::Value{std::string("hello")}},
        {"ay", dbus::Value{std::vector<uint8_t>{1, 2}}},
        {"at", dbus::Value{std::vector<uint64_t>{3, 4}}},
    };

    for (const auto& [name, val] : cases)
    {
        testWithDevice(name, val);
        testWithFirmware(name, val);
    }
}

// ---------------------------------------------------------------------------
// matchInventoryEntry tests
// ---------------------------------------------------------------------------

TEST(MatchEntryInfoTest, matchInventoryEntryDirectMatch)
{
    // When the full PropertyMap matches exactly, matchInventoryEntry should
    // return true and populate the output entry.
    dbus::PropertyMap cfgProps = {{"Name", dbus::Value{std::string("GPU0")}},
                                  {"Id", dbus::Value{uint32_t(7)}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Device", cfgProps}};

    FirmwareInfo expected = makeFirmwareInfo();

    MatchFirmwareInfo matchInfos;
    DBusIntfMatch match =
        std::make_pair(std::string("com.example.Device"), cfgProps);
    matchInfos.push_back(std::make_tuple(match, expected));

    FirmwareInventoryInfo inv(matchInfos);
    FirmwareInfo result{};

    EXPECT_TRUE(inv.matchInventoryEntry(ifaceMap, result));
    EXPECT_EQ(result, expected);
}

TEST(MatchEntryInfoTest, matchInventoryEntryNoMatch)
{
    // No interface in the InterfaceMap matches any configured entry.
    dbus::PropertyMap dbusProps = {{"Name", dbus::Value{std::string("NIC0")}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.NIC", dbusProps}};

    dbus::PropertyMap cfgProps = {{"Name", dbus::Value{std::string("GPU0")}}};
    MatchFirmwareInfo matchInfos;
    DBusIntfMatch match =
        std::make_pair(std::string("com.example.Device"), cfgProps);
    matchInfos.push_back(std::make_tuple(match, makeFirmwareInfo()));

    FirmwareInventoryInfo inv(matchInfos);
    FirmwareInfo result{};

    EXPECT_FALSE(inv.matchInventoryEntry(ifaceMap, result));
}

// ---------------------------------------------------------------------------
// Helper to build a minimal FirmwareInfo value for match-output tests.
// ---------------------------------------------------------------------------
static FirmwareInfo makeFirmwareInfo()
{
    return FirmwareInfo{CreateComponentIdNameMap{}, UpdateComponentIdNameMap{}};
}

// ---------------------------------------------------------------------------
// isPropertyMatch – additional paths for block coverage
// ---------------------------------------------------------------------------

TEST(MatchEntryInfoTest, isPropertyMatchMissingInterface)
{
    // Interface not present in InterfaceMap → early return false (line 314)
    dbus::PropertyMap dbusProps = {{"Name", dbus::Value{std::string("GPU0")}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Other", dbusProps}};

    std::pair<dbus::Property, dbus::Value> cfgProp{
        "Name", dbus::Value{std::string("GPU0")}};
    dbus::PropertyMap cfgPropMap = {cfgProp};
    MatchFirmwareInfo matchInfos;
    matchInfos.push_back(
        {DBusIntfMatch{"com.example.Device", cfgPropMap}, makeFirmwareInfo()});

    FirmwareInventoryInfo inv(matchInfos);

    EXPECT_FALSE(inv.isPropertyMatch(ifaceMap, cfgProp, "com.example.Device"));
}

TEST(MatchEntryInfoTest, isPropertyMatchValueMismatch)
{
    // Property found but values differ → compareValues returns false
    dbus::PropertyMap dbusProps = {{"Id", dbus::Value{uint32_t(99)}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Device", dbusProps}};

    std::pair<dbus::Property, dbus::Value> cfgProp{"Id",
                                                   dbus::Value{uint32_t(7)}};
    dbus::PropertyMap cfgPropMap = {cfgProp};
    MatchFirmwareInfo matchInfos;
    matchInfos.push_back(
        {DBusIntfMatch{"com.example.Device", cfgPropMap}, makeFirmwareInfo()});

    FirmwareInventoryInfo inv(matchInfos);

    EXPECT_FALSE(inv.isPropertyMatch(ifaceMap, cfgProp, "com.example.Device"));
}

// ---------------------------------------------------------------------------
// FirmwareInventoryInfo – isDirectMatch (all three paths)
// ---------------------------------------------------------------------------

TEST(MatchEntryInfoTest, isDirectMatchFirmwareSuccess)
{
    dbus::PropertyMap cfgProps = {{"Model", dbus::Value{std::string("FW1")}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Firmware", cfgProps}};

    MatchFirmwareInfo matchInfos;
    matchInfos.push_back(
        {DBusIntfMatch{"com.example.Firmware", cfgProps}, makeFirmwareInfo()});

    FirmwareInventoryInfo inv(matchInfos);

    EXPECT_TRUE(inv.isDirectMatch(ifaceMap, "com.example.Firmware", cfgProps));
}

TEST(MatchEntryInfoTest, isDirectMatchFirmwareFail)
{
    dbus::PropertyMap dbusProps = {{"Model", dbus::Value{std::string("FW2")}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Firmware", dbusProps}};

    dbus::PropertyMap cfgProps = {{"Model", dbus::Value{std::string("FW1")}}};
    MatchFirmwareInfo matchInfos;
    matchInfos.push_back(
        {DBusIntfMatch{"com.example.Firmware", cfgProps}, makeFirmwareInfo()});

    FirmwareInventoryInfo inv(matchInfos);

    EXPECT_FALSE(inv.isDirectMatch(ifaceMap, "com.example.Firmware", cfgProps));
}

TEST(MatchEntryInfoTest, isDirectMatchFirmwareMissingInterface)
{
    dbus::PropertyMap dbusProps = {{"Model", dbus::Value{std::string("FW1")}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Other", dbusProps}};

    dbus::PropertyMap cfgProps = {{"Model", dbus::Value{std::string("FW1")}}};
    MatchFirmwareInfo matchInfos;
    matchInfos.push_back(
        {DBusIntfMatch{"com.example.Firmware", cfgProps}, makeFirmwareInfo()});

    FirmwareInventoryInfo inv(matchInfos);

    EXPECT_FALSE(inv.isDirectMatch(ifaceMap, "com.example.Firmware", cfgProps));
}

// ---------------------------------------------------------------------------
// FirmwareInventoryInfo – isPropertyMatch (all four paths)
// ---------------------------------------------------------------------------

TEST(MatchEntryInfoTest, isPropertyMatchFirmwareMissingInterface)
{
    dbus::PropertyMap dbusProps = {{"Model", dbus::Value{std::string("FW1")}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Other", dbusProps}};

    std::pair<dbus::Property, dbus::Value> cfgProp{
        "Model", dbus::Value{std::string("FW1")}};
    dbus::PropertyMap cfgPropMap = {cfgProp};
    MatchFirmwareInfo matchInfos;
    matchInfos.push_back({DBusIntfMatch{"com.example.Firmware", cfgPropMap},
                          makeFirmwareInfo()});

    FirmwareInventoryInfo inv(matchInfos);

    EXPECT_FALSE(
        inv.isPropertyMatch(ifaceMap, cfgProp, "com.example.Firmware"));
}

TEST(MatchEntryInfoTest, isPropertyMatchFirmwareMissingProperty)
{
    dbus::PropertyMap dbusProps = {{"Model", dbus::Value{std::string("FW1")}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Firmware", dbusProps}};

    std::pair<dbus::Property, dbus::Value> cfgProp{"Missing",
                                                   dbus::Value{uint32_t(1)}};
    dbus::PropertyMap cfgPropMap = {cfgProp};
    MatchFirmwareInfo matchInfos;
    matchInfos.push_back({DBusIntfMatch{"com.example.Firmware", cfgPropMap},
                          makeFirmwareInfo()});

    FirmwareInventoryInfo inv(matchInfos);

    EXPECT_FALSE(
        inv.isPropertyMatch(ifaceMap, cfgProp, "com.example.Firmware"));
}

TEST(MatchEntryInfoTest, isPropertyMatchFirmwareValueMismatch)
{
    dbus::PropertyMap dbusProps = {{"Model", dbus::Value{std::string("FW2")}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Firmware", dbusProps}};

    std::pair<dbus::Property, dbus::Value> cfgProp{
        "Model", dbus::Value{std::string("FW1")}};
    dbus::PropertyMap cfgPropMap = {cfgProp};
    MatchFirmwareInfo matchInfos;
    matchInfos.push_back({DBusIntfMatch{"com.example.Firmware", cfgPropMap},
                          makeFirmwareInfo()});

    FirmwareInventoryInfo inv(matchInfos);

    EXPECT_FALSE(
        inv.isPropertyMatch(ifaceMap, cfgProp, "com.example.Firmware"));
}

// ---------------------------------------------------------------------------
// matchInventoryEntry – property match path (lines 352-363)
//
// D-Bus has extra properties beyond config → isDirectMatch fails.
// But all config properties match individually → std::all_of succeeds.
// This exercises the lambda at line 355 for both instantiations.
// ---------------------------------------------------------------------------

TEST(MatchEntryInfoTest, matchInventoryEntryPropertyMatch)
{
    dbus::PropertyMap cfgProps = {{"Name", dbus::Value{std::string("GPU0")}}};
    dbus::PropertyMap dbusProps = {{"Name", dbus::Value{std::string("GPU0")}},
                                   {"Extra", dbus::Value{uint32_t(99)}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Device", dbusProps}};

    FirmwareInfo expected = makeFirmwareInfo();

    MatchFirmwareInfo matchInfos;
    matchInfos.push_back(
        {DBusIntfMatch{"com.example.Device", cfgProps}, expected});

    FirmwareInventoryInfo inv(matchInfos);
    FirmwareInfo result{};

    EXPECT_TRUE(inv.matchInventoryEntry(ifaceMap, result));
    EXPECT_EQ(result, expected);
}

TEST(MatchEntryInfoTest, matchInventoryEntryPropertyMatchPartialFail)
{
    // Interface exists and direct match fails (extra props), but one config
    // property value differs → std::all_of returns false → no match.
    dbus::PropertyMap cfgProps = {{"Name", dbus::Value{std::string("GPU0")}},
                                  {"Id", dbus::Value{uint32_t(7)}}};
    dbus::PropertyMap dbusProps = {{"Name", dbus::Value{std::string("GPU0")}},
                                   {"Id", dbus::Value{uint32_t(99)}},
                                   {"Extra", dbus::Value{uint32_t(1)}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Device", dbusProps}};

    MatchFirmwareInfo matchInfos;
    matchInfos.push_back(
        {DBusIntfMatch{"com.example.Device", cfgProps}, makeFirmwareInfo()});

    FirmwareInventoryInfo inv(matchInfos);
    FirmwareInfo result{};

    EXPECT_FALSE(inv.matchInventoryEntry(ifaceMap, result));
}

// ---------------------------------------------------------------------------
// FirmwareInventoryInfo – matchInventoryEntry (all paths)
// ---------------------------------------------------------------------------

TEST(MatchEntryInfoTest, matchInventoryEntryFirmwareDirectMatch)
{
    dbus::PropertyMap cfgProps = {{"Model", dbus::Value{std::string("FW1")}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Firmware", cfgProps}};

    FirmwareInfo expected = makeFirmwareInfo();

    MatchFirmwareInfo matchInfos;
    matchInfos.push_back(
        {DBusIntfMatch{"com.example.Firmware", cfgProps}, expected});

    FirmwareInventoryInfo inv(matchInfos);
    FirmwareInfo result{};

    EXPECT_TRUE(inv.matchInventoryEntry(ifaceMap, result));
    EXPECT_EQ(result, expected);
}

TEST(MatchEntryInfoTest, matchInventoryEntryFirmwarePropertyMatch)
{
    dbus::PropertyMap cfgProps = {{"Model", dbus::Value{std::string("FW1")}}};
    dbus::PropertyMap dbusProps = {{"Model", dbus::Value{std::string("FW1")}},
                                   {"Extra", dbus::Value{uint32_t(1)}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Firmware", dbusProps}};

    FirmwareInfo expected = makeFirmwareInfo();

    MatchFirmwareInfo matchInfos;
    matchInfos.push_back(
        {DBusIntfMatch{"com.example.Firmware", cfgProps}, expected});

    FirmwareInventoryInfo inv(matchInfos);
    FirmwareInfo result{};

    EXPECT_TRUE(inv.matchInventoryEntry(ifaceMap, result));
    EXPECT_EQ(result, expected);
}

TEST(MatchEntryInfoTest, matchInventoryEntryFirmwareNoMatch)
{
    dbus::PropertyMap dbusProps = {{"Model", dbus::Value{std::string("FW1")}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Other", dbusProps}};

    dbus::PropertyMap cfgProps = {{"Model", dbus::Value{std::string("FW1")}}};
    MatchFirmwareInfo matchInfos;
    matchInfos.push_back(
        {DBusIntfMatch{"com.example.Firmware", cfgProps}, makeFirmwareInfo()});

    FirmwareInventoryInfo inv(matchInfos);
    FirmwareInfo result{};

    EXPECT_FALSE(inv.matchInventoryEntry(ifaceMap, result));
}

TEST(MatchEntryInfoTest, matchInventoryEntryFirmwarePropertyMatchPartialFail)
{
    dbus::PropertyMap cfgProps = {{"Model", dbus::Value{std::string("FW1")}},
                                  {"Rev", dbus::Value{uint32_t(2)}}};
    dbus::PropertyMap dbusProps = {{"Model", dbus::Value{std::string("FW1")}},
                                   {"Rev", dbus::Value{uint32_t(99)}},
                                   {"Extra", dbus::Value{uint32_t(1)}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Firmware", dbusProps}};

    MatchFirmwareInfo matchInfos;
    matchInfos.push_back(
        {DBusIntfMatch{"com.example.Firmware", cfgProps}, makeFirmwareInfo()});

    FirmwareInventoryInfo inv(matchInfos);
    FirmwareInfo result{};

    EXPECT_FALSE(inv.matchInventoryEntry(ifaceMap, result));
}

/*
 * ComponentNameMapInfo tests
 *   ComponentNameMapInfo
 *   = MatchEntryInfo<MatchComponentNameMapInfo, ComponentIdNameMap>
 *   where:
 *   MatchComponentNameMapInfo = vector<tuple<DBusIntfMatch,
 * ComponentIdNameMap>> ComponentIdNameMap = unordered_map<CompIdentifier,
 * ComponentName> = unordered_map<uint16_t, string>
 */

TEST(MatchEntryInfoTest, isDirectMatchComponentNameMapSuccess)
{
    dbus::PropertyMap props = {{"Name", dbus::Value{std::string("GPU0")}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Component", props}};

    ComponentNameMapInfo inv(MatchComponentNameMapInfo{
        {DBusIntfMatch{"com.example.Component", props},
         ComponentIdNameMap{{1, "comp1"}, {2, "comp2"}}}});

    EXPECT_TRUE(inv.isDirectMatch(ifaceMap, "com.example.Component", props));
}

TEST(MatchEntryInfoTest, isDirectMatchComponentNameMapMissingInterface)
{
    dbus::PropertyMap props = {{"Name", dbus::Value{std::string("GPU0")}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Other", props}};

    ComponentNameMapInfo inv(MatchComponentNameMapInfo{
        {DBusIntfMatch{"com.example.Component", props},
         ComponentIdNameMap{{1, "comp1"}}}});

    EXPECT_FALSE(inv.isDirectMatch(ifaceMap, "com.example.Component", props));
}

TEST(MatchEntryInfoTest, isPropertyMatchComponentNameMapSuccess)
{
    dbus::PropertyMap dbusProps = {{"Name", dbus::Value{std::string("GPU0")}},
                                   {"Extra", dbus::Value{uint32_t(42)}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Component", dbusProps}};

    dbus::PropertyMap cfgProps = {{"Name", dbus::Value{std::string("GPU0")}}};

    ComponentNameMapInfo inv(MatchComponentNameMapInfo{
        {DBusIntfMatch{"com.example.Component", cfgProps},
         ComponentIdNameMap{{1, "comp1"}}}});

    EXPECT_TRUE(inv.isPropertyMatch(
        ifaceMap,
        std::pair<dbus::Property, dbus::Value>{
            "Name", dbus::Value{std::string("GPU0")}},
        "com.example.Component"));
}

TEST(MatchEntryInfoTest, isPropertyMatchComponentNameMapMissingInterface)
{
    dbus::PropertyMap dbusProps = {{"Name", dbus::Value{std::string("GPU0")}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Other", dbusProps}};

    std::pair<dbus::Property, dbus::Value> cfgProp{
        "Name", dbus::Value{std::string("GPU0")}};

    ComponentNameMapInfo inv(MatchComponentNameMapInfo{
        {DBusIntfMatch{"com.example.Component", dbus::PropertyMap{cfgProp}},
         ComponentIdNameMap{{1, "comp1"}}}});

    EXPECT_FALSE(
        inv.isPropertyMatch(ifaceMap, cfgProp, "com.example.Component"));
}

TEST(MatchEntryInfoTest, isPropertyMatchComponentNameMapMissingProperty)
{
    dbus::PropertyMap dbusProps = {{"Name", dbus::Value{std::string("GPU0")}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Component", dbusProps}};

    std::pair<dbus::Property, dbus::Value> cfgProp{"Missing",
                                                   dbus::Value{uint32_t(1)}};

    ComponentNameMapInfo inv(MatchComponentNameMapInfo{
        {DBusIntfMatch{"com.example.Component", dbus::PropertyMap{cfgProp}},
         ComponentIdNameMap{{1, "comp1"}}}});

    EXPECT_FALSE(
        inv.isPropertyMatch(ifaceMap, cfgProp, "com.example.Component"));
}

TEST(MatchEntryInfoTest, isPropertyMatchComponentNameMapValueMismatch)
{
    dbus::PropertyMap dbusProps = {{"Name", dbus::Value{std::string("GPU1")}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Component", dbusProps}};

    std::pair<dbus::Property, dbus::Value> cfgProp{
        "Name", dbus::Value{std::string("GPU0")}};

    ComponentNameMapInfo inv(MatchComponentNameMapInfo{
        {DBusIntfMatch{"com.example.Component", dbus::PropertyMap{cfgProp}},
         ComponentIdNameMap{{1, "comp1"}}}});

    EXPECT_FALSE(
        inv.isPropertyMatch(ifaceMap, cfgProp, "com.example.Component"));
}

TEST(MatchEntryInfoTest, isPropertyMatchComponentNameMapAllValueTypes)
{
    // Exercise the std::visit lambda (line 327) for every dbus::Value
    // alternative in the ComponentNameMapInfo instantiation.
    auto testWithComp = [](const char* name, dbus::Value val) {
        dbus::PropertyMap dbusProps = {{name, val}};
        dbus::InterfaceMap ifaceMap = {{"com.example.Comp", dbusProps}};
        std::pair<dbus::Property, dbus::Value> cfgProp{name, val};
        dbus::PropertyMap cfgPropMap = {cfgProp};
        MatchComponentNameMapInfo mi;
        mi.push_back({DBusIntfMatch{"com.example.Comp", cfgPropMap},
                      ComponentIdNameMap{{1, "c1"}}});
        ComponentNameMapInfo inv(mi);
        EXPECT_TRUE(inv.isPropertyMatch(ifaceMap, cfgProp, "com.example.Comp"))
            << "ComponentNameMapInfo failed for: " << name;
    };

    // All 12 dbus::Value alternatives
    testWithComp("b", dbus::Value{true});
    testWithComp("y", dbus::Value{uint8_t(1)});
    testWithComp("n", dbus::Value{int16_t(-1)});
    testWithComp("q", dbus::Value{uint16_t(2)});
    testWithComp("i", dbus::Value{int32_t(-3)});
    testWithComp("u", dbus::Value{uint32_t(4)});
    testWithComp("x", dbus::Value{int64_t(-5)});
    testWithComp("t", dbus::Value{uint64_t(6)});
    testWithComp("d", dbus::Value{double(7.7)});
    testWithComp("s", dbus::Value{std::string("hello")});
    testWithComp("ay", dbus::Value{std::vector<uint8_t>{1, 2}});
    testWithComp("at", dbus::Value{std::vector<uint64_t>{3, 4}});
}

TEST(MatchEntryInfoTest, matchInventoryEntryComponentNameMapDirectMatch)
{
    dbus::PropertyMap props = {{"Name", dbus::Value{std::string("GPU0")}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Component", props}};

    ComponentIdNameMap expected = {{1, "comp1"}, {2, "comp2"}};
    ComponentNameMapInfo inv(MatchComponentNameMapInfo{
        {DBusIntfMatch{"com.example.Component", props}, expected}});

    ComponentIdNameMap result;
    EXPECT_TRUE(inv.matchInventoryEntry(ifaceMap, result));
    EXPECT_EQ(result, expected);
}

TEST(MatchEntryInfoTest, matchInventoryEntryComponentNameMapPropertyMatch)
{
    dbus::PropertyMap cfgProps = {{"Name", dbus::Value{std::string("GPU0")}}};
    dbus::PropertyMap dbusProps = {{"Name", dbus::Value{std::string("GPU0")}},
                                   {"Extra", dbus::Value{uint32_t(42)}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Component", dbusProps}};

    ComponentIdNameMap expected = {{10, "compA"}};
    MatchComponentNameMapInfo matchInfos;
    matchInfos.push_back(
        {DBusIntfMatch{"com.example.Component", cfgProps}, expected});

    ComponentNameMapInfo inv(matchInfos);
    ComponentIdNameMap result;

    EXPECT_TRUE(inv.matchInventoryEntry(ifaceMap, result));
    EXPECT_EQ(result, expected);
}

TEST(MatchEntryInfoTest, matchInventoryEntryComponentNameMapNoMatch)
{
    dbus::PropertyMap dbusProps = {{"Name", dbus::Value{std::string("GPU0")}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Other", dbusProps}};

    dbus::PropertyMap cfgProps = {{"Name", dbus::Value{std::string("GPU0")}}};
    MatchComponentNameMapInfo matchInfos;
    matchInfos.push_back({DBusIntfMatch{"com.example.Component", cfgProps},
                          ComponentIdNameMap{{1, "comp1"}}});

    ComponentNameMapInfo inv(matchInfos);
    ComponentIdNameMap result;

    EXPECT_FALSE(inv.matchInventoryEntry(ifaceMap, result));
}

TEST(MatchEntryInfoTest, matchInventoryEntryComponentNameMapPartialFail)
{
    dbus::PropertyMap cfgProps = {{"Name", dbus::Value{std::string("GPU0")}},
                                  {"Id", dbus::Value{uint32_t(7)}}};
    dbus::PropertyMap dbusProps = {{"Name", dbus::Value{std::string("GPU0")}},
                                   {"Id", dbus::Value{uint32_t(99)}},
                                   {"Extra", dbus::Value{uint32_t(42)}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Component", dbusProps}};

    MatchComponentNameMapInfo matchInfos;
    matchInfos.push_back({DBusIntfMatch{"com.example.Component", cfgProps},
                          ComponentIdNameMap{{1, "comp1"}}});

    ComponentNameMapInfo inv(matchInfos);
    ComponentIdNameMap result;

    EXPECT_FALSE(inv.matchInventoryEntry(ifaceMap, result));
}

TEST(MatchEntryInfoTest, matchInventoryEntryAllOfPathSetsEntry)
{
    using MatchInfo = std::pair<DBusIntfMatch, int>;
    using MatchInfos = std::vector<MatchInfo>;
    using EntryMatcher = MatchEntryInfo<MatchInfos, int>;

    dbus::PropertyMap cfgProps = {{"Name", dbus::Value{std::string("GPU0")}}};
    dbus::PropertyMap dbusProps = {{"Name", dbus::Value{std::string("GPU0")}},
                                   {"Extra", dbus::Value{uint32_t(42)}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Component", dbusProps}};

    MatchInfos infos = {{DBusIntfMatch{"com.example.Component", cfgProps}, 42}};
    EntryMatcher matcher(infos);
    int entry = 0;

    EXPECT_TRUE(matcher.matchInventoryEntry(ifaceMap, entry));
    EXPECT_EQ(entry, 42);
}

TEST(MatchEntryInfoTest, matchInventoryEntryAllOfPathReturnsFalseOnPropMismatch)
{
    using MatchInfo = std::pair<DBusIntfMatch, int>;
    using MatchInfos = std::vector<MatchInfo>;
    using EntryMatcher = MatchEntryInfo<MatchInfos, int>;

    dbus::PropertyMap cfgProps = {{"Name", dbus::Value{std::string("GPU0")}}};
    dbus::PropertyMap dbusProps = {{"Name", dbus::Value{std::string("GPU1")}},
                                   {"Extra", dbus::Value{uint32_t(42)}}};
    dbus::InterfaceMap ifaceMap = {{"com.example.Component", dbusProps}};

    MatchInfos infos = {{DBusIntfMatch{"com.example.Component", cfgProps}, 77}};
    EntryMatcher matcher(infos);
    int entry = 0;

    EXPECT_FALSE(matcher.matchInventoryEntry(ifaceMap, entry));
}

TEST(GetInventoryObjects, cachedReferenceIsReused)
{
    auto& first = DBusHandler::getInventoryObjects<GetManagedObject>();
    auto* firstPtr = &first;
    auto& second = DBusHandler::getInventoryObjects<GetManagedObject>();

    EXPECT_EQ(firstPtr, &second);
}

struct ThrowingManagedObject
{
    static ObjectValueTree getManagedObj(const char* /*service*/,
                                         const char* /*path*/)
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

    static ObjectValueTree getManagedObj(const char* /*service*/,
                                         const char* /*path*/)
    {
        if (attempts()++ == 0)
        {
            throw std::runtime_error("transient initialization failure");
        }

        ObjectValueTree objects;
        objects.emplace(
            sdbusplus::object_path("/xyz/openbmc_project/inventory/flaky0"),
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

TEST(GetInventoryObjects, initializationFailureIsPropagated)
{
    EXPECT_THROW((DBusHandler::getInventoryObjects<ThrowingManagedObject>()),
                 std::runtime_error);
}

TEST(GetInventoryObjects, initializationFailureIsRetriedAfterThrow)
{
    EXPECT_THROW((DBusHandler::getInventoryObjects<ThrowingManagedObject>()),
                 std::runtime_error);
    EXPECT_THROW((DBusHandler::getInventoryObjects<ThrowingManagedObject>()),
                 std::runtime_error);
}

TEST(GetInventoryObjects, transientInitializationFailureEventuallyCachesValue)
{
    FlakyManagedObject::reset();

    EXPECT_THROW((DBusHandler::getInventoryObjects<FlakyManagedObject>()),
                 std::runtime_error);

    auto& first = DBusHandler::getInventoryObjects<FlakyManagedObject>();
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first.begin()->first, "/xyz/openbmc_project/inventory/flaky0");

    auto& second = DBusHandler::getInventoryObjects<FlakyManagedObject>();
    EXPECT_EQ(&first, &second);
}

TEST(ReadLEValueTest, decodesAllSupportedWidthsCoverage)
{
    const std::array<uint8_t, 15> bytes{0x34, 0x12, 0x78, 0x56, 0x34,
                                        0x12, 0xF0, 0xDE, 0xBC, 0x9A,
                                        0x78, 0x56, 0x34, 0x12, 0x5A};
    const uint8_t* ptr = bytes.data();

    EXPECT_EQ(readLEValue<uint16_t>(ptr), 0x1234u);
    EXPECT_EQ(readLEValue<uint32_t>(ptr), 0x12345678u);
    EXPECT_EQ(readLEValue<uint64_t>(ptr), 0x123456789ABCDEF0ull);
    EXPECT_EQ(readLEValue<uint8_t>(ptr), 0x5Au);
    EXPECT_EQ(ptr, bytes.data() + bytes.size());
}

TEST(DBusHandlerTemplate, getDbusPropertyHeapBackedValuesCoverage)
{
    MockdBusHandler handler;

    const std::string longString(96, 'Z');
    EXPECT_CALL(handler,
                getDbusPropertyVariant("/xyz/openbmc_project/example", "Value",
                                       "xyz.openbmc_project.Example"))
        .WillOnce(::testing::Return(pldm::utils::PropertyValue{longString}));
    EXPECT_EQ(handler.getDbusProperty<std::string>(
                  "/xyz/openbmc_project/example", "Value",
                  "xyz.openbmc_project.Example"),
              longString);

    const std::vector<std::string> longStrings{
        "/xyz/openbmc_project/inventory/" + std::string(48, 'a'),
        "/xyz/openbmc_project/inventory/" + std::string(52, 'b')};
    EXPECT_CALL(handler, getDbusPropertyVariant("/xyz/openbmc_project/example",
                                                "StringArray",
                                                "xyz.openbmc_project.Example"))
        .WillOnce(::testing::Return(pldm::utils::PropertyValue{longStrings}));
    EXPECT_EQ(handler.getDbusProperty<std::vector<std::string>>(
                  "/xyz/openbmc_project/example", "StringArray",
                  "xyz.openbmc_project.Example"),
              longStrings);

    const std::vector<sdbusplus::object_path> objectPaths{
        sdbusplus::object_path(
            "/xyz/openbmc_project/object/" + std::string(44, 'x')),
        sdbusplus::object_path(
            "/xyz/openbmc_project/object/" + std::string(46, 'y'))};
    EXPECT_CALL(handler, getDbusPropertyVariant("/xyz/openbmc_project/example",
                                                "ObjectPaths",
                                                "xyz.openbmc_project.Example"))
        .WillOnce(::testing::Return(pldm::utils::PropertyValue{objectPaths}));
    EXPECT_EQ(handler.getDbusProperty<std::vector<sdbusplus::object_path>>(
                  "/xyz/openbmc_project/example", "ObjectPaths",
                  "xyz.openbmc_project.Example"),
              objectPaths);
}

TEST(Split, skipsEmptyTokensFromConsecutiveDelimiters)
{
    std::string values = "aa,,bb,,,cc,";
    auto results = split(values, ",");
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0], "aa");
    EXPECT_EQ(results[1], "bb");
    EXPECT_EQ(results[2], "cc");
}

TEST_F(DBusHandlerBusMockTest, setDbusPropertyThrowsOnVariantTypeMismatch)
{
    auto expectTypeMismatch =
        [this](const char* propertyType, const PropertyValue& wrongValue) {
            DBusMapping mapping{"/xyz/openbmc_project/example",
                                "xyz.openbmc_project.Example", "Property",
                                propertyType};
            EXPECT_THROW(handler.setDbusProperty(mapping, wrongValue),
                         std::bad_variant_access);
        };

    expectTypeMismatch("uint8_t", PropertyValue{true});
    expectTypeMismatch("bool", PropertyValue{uint8_t(1)});
    expectTypeMismatch("int16_t", PropertyValue{uint16_t(2)});
    expectTypeMismatch("uint16_t", PropertyValue{int16_t(-2)});
    expectTypeMismatch("int32_t", PropertyValue{std::string("x")});
    expectTypeMismatch("uint32_t", PropertyValue{int32_t(-3)});
    expectTypeMismatch("int64_t", PropertyValue{double(2.5)});
    expectTypeMismatch("uint64_t", PropertyValue{int64_t(-4)});
    expectTypeMismatch("double", PropertyValue{uint64_t(5)});
    expectTypeMismatch("string", PropertyValue{false});
    expectTypeMismatch("array[string]", PropertyValue{std::vector<uint8_t>{1}});
    expectTypeMismatch("array[object_path]",
                       PropertyValue{std::vector<std::string>{"a"}});
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif
