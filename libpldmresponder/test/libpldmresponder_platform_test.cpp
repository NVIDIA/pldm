#include "common/start_lifetime_as.hpp"
#include "common/test/mocked_utils.hpp"
#include "common/utils.hpp"
#include "host-bmc/dbus_to_event_handler.hpp"
#include "libpldmresponder/event_parser.hpp"
#include "libpldmresponder/pdr.hpp"
#include "libpldmresponder/pdr_utils.hpp"
#include "libpldmresponder/platform.hpp"
#include "libpldmresponder/platform_numeric_effecter.hpp"
#include "libpldmresponder/platform_state_effecter.hpp"
#include "libpldmresponder/platform_state_sensor.hpp"
#include "test/pldmd_coverage_hooks.hpp"
#include "test/test_instance_id.hpp"
#include "test/test_tmp_utils.hpp"

#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>
#include <sdeventplus/event.hpp>

#include <fstream>
#include <memory>

using namespace pldm::pdr;
using namespace pldm::utils;
using namespace pldm::responder;
using namespace pldm::responder::platform;
using namespace pldm::responder::pdr;
using namespace pldm::responder::pdr_utils;
using pldm::TERMINUS_HANDLE;
using pldm::TERMINUS_ID;

TEST(ParseFruRecordTable, RejectsFieldPastEndOfBuffer)
{
    const std::vector<uint8_t> table{
        1,   0, /* record set id */
        1,      /* record type */
        1,      /* number of fields */
        1,      /* encoding */
        2,      /* field type */
        4,      /* field length */
        'x',    /* truncated field value */
    };

    EXPECT_TRUE(parseFruRecordTable(table.data(), table.size()).empty());
}

TEST(ParseFruRecordTable, RejectsTruncatedNextRecord)
{
    const std::vector<uint8_t> table{
        1,   0, /* record set id */
        1,      /* record type */
        1,      /* number of fields */
        1,      /* encoding */
        2,      /* field type */
        1,      /* field length */
        'x',    /* field value */
        0,      /* truncated next record */
    };

    EXPECT_TRUE(parseFruRecordTable(table.data(), table.size()).empty());
}

TEST(ParseFruRecordTable, ParsesBoundedField)
{
    const std::vector<uint8_t> table{
        1,   0, /* record set id */
        1,      /* record type */
        1,      /* number of fields */
        1,      /* encoding */
        2,      /* field type */
        1,      /* field length */
        'x',    /* field value */
    };

    const auto records = parseFruRecordTable(table.data(), table.size());
    ASSERT_EQ(records.size(), 1);
    ASSERT_EQ(records[0].fruTLV.size(), 1);
    EXPECT_EQ(records[0].fruTLV[0].fruFieldValue, std::vector<uint8_t>({'x'}));
}

using ::testing::_;
using ::testing::Return;
using ::testing::StrEq;
using ::testing::Throw;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
namespace
{

uint8_t completionCode(const Response& response)
{
    return reinterpret_cast<const pldm_msg*>(response.data())->payload[0];
}

pldm_msg* asMsg(std::vector<uint8_t>& request)
{
    return reinterpret_cast<pldm_msg*>(request.data());
}

bool isNumericProperty(const pldm::utils::PropertyValue& value)
{
    return std::visit(
        [](const auto& v) {
            return std::is_arithmetic_v<std::decay_t<decltype(v)>>;
        },
        value);
}

long double numericProperty(const pldm::utils::PropertyValue& value)
{
    return std::visit(
        [](const auto& v) -> long double {
            if constexpr (std::is_arithmetic_v<std::decay_t<decltype(v)>>)
            {
                return static_cast<long double>(v);
            }
            return 0.0L;
        },
        value);
}

pldm_numeric_effecter_value_pdr makeNumericEffecterPdr(uint8_t dataSize)
{
    pldm_numeric_effecter_value_pdr pdr{};
    pdr.effecter_data_size = dataSize;
    pdr.resolution = 1.0;
    pdr.offset = 0.0;
    switch (dataSize)
    {
        case PLDM_EFFECTER_DATA_SIZE_UINT8:
            pdr.min_settable.value_u8 = 0;
            pdr.max_settable.value_u8 = 250;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT8:
            pdr.min_settable.value_s8 = -100;
            pdr.max_settable.value_s8 = 100;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT16:
            pdr.min_settable.value_u16 = 0;
            pdr.max_settable.value_u16 = 65000;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT16:
            pdr.min_settable.value_s16 = -30000;
            pdr.max_settable.value_s16 = 30000;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT32:
            pdr.min_settable.value_u32 = 0;
            pdr.max_settable.value_u32 = 4000000000u;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT32:
            pdr.min_settable.value_s32 = -2000000000;
            pdr.max_settable.value_s32 = 2000000000;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT64:
            pdr.min_settable.value_u64 = 0;
            pdr.max_settable.value_u64 = 9000000000ull;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT64:
            pdr.min_settable.value_s64 = -9000000000ll;
            pdr.max_settable.value_s64 = 9000000000ll;
            break;
        default:
            break;
    }
    return pdr;
}

std::vector<uint8_t> makeStateSensorPdr(
    uint16_t sensorId, uint16_t terminusHandle, uint16_t entityType = 64,
    uint16_t entityInstance = 1, uint16_t containerId = 1,
    uint16_t stateSetId = 0)
{
    state_sensor_possible_states possibleStates{};
    possibleStates.state_set_id = stateSetId;
    possibleStates.possible_states_size = 1;
    possibleStates.states[0].byte = 0x03;

    constexpr size_t possibleStatesSize = sizeof(state_sensor_possible_states);
    std::vector<uint8_t> pdr(
        sizeof(pldm_state_sensor_pdr) + possibleStatesSize - 1, 0);
    auto* sensor = reinterpret_cast<pldm_state_sensor_pdr*>(pdr.data());
    sensor->hdr.record_handle = 1;
    sensor->hdr.record_change_num = 0;
    sensor->terminus_handle = terminusHandle;
    sensor->sensor_id = sensorId;
    sensor->entity_type = entityType;
    sensor->entity_instance = entityInstance;
    sensor->container_id = containerId;
    sensor->sensor_init = PLDM_NO_INIT;
    sensor->sensor_auxiliary_names_pdr = false;
    sensor->composite_sensor_count = 1;

    size_t actualSize = 0;
    EXPECT_EQ(encode_state_sensor_pdr(sensor, pdr.size(), &possibleStates,
                                      possibleStatesSize, &actualSize),
              PLDM_SUCCESS);
    pdr.resize(actualSize);
    return pdr;
}

std::vector<uint8_t> makeStateEffecterPdr(
    uint16_t effecterId, uint16_t terminusHandle, uint16_t entityType = 64,
    uint16_t entityInstance = 1, uint16_t containerId = 1,
    uint16_t stateSetId = 0)
{
    state_effecter_possible_states possibleStates{};
    possibleStates.state_set_id = stateSetId;
    possibleStates.possible_states_size = 1;
    possibleStates.states[0].byte = 0x03;

    constexpr size_t possibleStatesSize =
        sizeof(state_effecter_possible_states);
    std::vector<uint8_t> pdr(
        sizeof(pldm_state_effecter_pdr) + possibleStatesSize - 1, 0);
    auto* effecter = reinterpret_cast<pldm_state_effecter_pdr*>(pdr.data());
    effecter->hdr.record_handle = 1;
    effecter->hdr.record_change_num = 0;
    effecter->terminus_handle = terminusHandle;
    effecter->effecter_id = effecterId;
    effecter->entity_type = entityType;
    effecter->entity_instance = entityInstance;
    effecter->container_id = containerId;
    effecter->composite_effecter_count = 1;

    size_t actualSize = 0;
    EXPECT_EQ(encode_state_effecter_pdr(effecter, pdr.size(), &possibleStates,
                                        possibleStatesSize, &actualSize),
              PLDM_SUCCESS);
    pdr.resize(actualSize);
    return pdr;
}

std::vector<uint8_t> makeStateEffecterPdrWithPossibleStates(
    uint16_t effecterId, uint16_t terminusHandle,
    const std::vector<std::pair<uint16_t, std::vector<uint8_t>>>& stateSets,
    uint16_t entityType = 64, uint16_t entityInstance = 1,
    uint16_t containerId = 1)
{
    size_t possibleStatesSize = 0;
    for (const auto& [stateSetId, states] : stateSets)
    {
        (void)stateSetId;
        possibleStatesSize += sizeof(state_effecter_possible_states) -
                              sizeof(bitfield8_t) + states.size();
    }

    std::vector<uint8_t> possibleStates(possibleStatesSize, 0);
    uint8_t* nextPossibleState = possibleStates.data();
    for (const auto& [stateSetId, states] : stateSets)
    {
        auto* possibleState = reinterpret_cast<state_effecter_possible_states*>(
            nextPossibleState);
        possibleState->state_set_id = stateSetId;
        possibleState->possible_states_size = states.size();
        for (size_t i = 0; i < states.size(); ++i)
        {
            possibleState->states[i].byte = states[i];
        }
        nextPossibleState += sizeof(state_effecter_possible_states) -
                             sizeof(bitfield8_t) + states.size();
    }

    std::vector<uint8_t> pdr(
        sizeof(pldm_state_effecter_pdr) + possibleStates.size() - 1, 0);
    auto* effecter = reinterpret_cast<pldm_state_effecter_pdr*>(pdr.data());
    effecter->hdr.record_handle = 1;
    effecter->hdr.record_change_num = 0;
    effecter->terminus_handle = terminusHandle;
    effecter->effecter_id = effecterId;
    effecter->entity_type = entityType;
    effecter->entity_instance = entityInstance;
    effecter->container_id = containerId;
    effecter->composite_effecter_count = stateSets.size();

    size_t actualSize = 0;
    EXPECT_EQ(encode_state_effecter_pdr(
                  effecter, pdr.size(),
                  reinterpret_cast<state_effecter_possible_states*>(
                      possibleStates.data()),
                  possibleStates.size(), &actualSize),
              PLDM_SUCCESS);
    pdr.resize(actualSize);
    return pdr;
}

std::vector<uint8_t> makeSensorEventRequest(
    uint8_t instanceId, uint8_t tid, uint16_t sensorId, uint8_t sensorOffset,
    uint8_t eventState, uint8_t previousState)
{
    size_t eventDataLength = 0;
    EXPECT_EQ(encode_sensor_event_data(
                  nullptr, 0, sensorId, PLDM_STATE_SENSOR_STATE, sensorOffset,
                  eventState, previousState, &eventDataLength),
              PLDM_SUCCESS);

    std::vector<uint8_t> eventData(eventDataLength, 0);
    EXPECT_EQ(encode_sensor_event_data(
                  reinterpret_cast<pldm_sensor_event_data*>(eventData.data()),
                  eventData.size(), sensorId, PLDM_STATE_SENSOR_STATE,
                  sensorOffset, eventState, previousState, &eventDataLength),
              PLDM_SUCCESS);

    const auto payloadLength =
        PLDM_PLATFORM_EVENT_MESSAGE_MIN_REQ_BYTES + eventDataLength;
    std::vector<uint8_t> request(sizeof(pldm_msg_hdr) + payloadLength, 0);
    auto* msg = asMsg(request);
    EXPECT_EQ(encode_platform_event_message_req(
                  instanceId, PLDM_PLATFORM_EVENT_MESSAGE_FORMAT_VERSION, tid,
                  PLDM_SENSOR_EVENT, eventData.data(), eventDataLength, msg,
                  payloadLength),
              PLDM_SUCCESS);
    return request;
}

void markHostPdrHandlerHostUp(
    pldm::HostPDRHandler& hostPDRHandler,
    pldm::requester::Handler<pldm::requester::Request>& reqHandler)
{
    hostPDRHandler.setHostFirmwareCondition();

    std::vector<uint8_t> versionResp(
        sizeof(pldm_msg_hdr) + PLDM_GET_VERSION_RESP_BYTES, 0);
    auto* versionMsg = asMsg(versionResp);
    ver32_t version{0x00, 0xf0, 0xf0, 0xf1};
    EXPECT_EQ(encode_get_version_resp(0, PLDM_SUCCESS, 0, PLDM_START_AND_END,
                                      &version, sizeof(pldm_version),
                                      versionMsg),
              PLDM_SUCCESS);

    for (uint8_t instance = 0; instance < 32; ++instance)
    {
        reqHandler.handleResponse(8, instance, PLDM_BASE, PLDM_GET_PLDM_VERSION,
                                  versionMsg, versionResp.size());
    }
}

class StubOemPlatformHandler : public pldm::responder::oem_platform::Handler
{
  public:
    StubOemPlatformHandler() :
        pldm::responder::oem_platform::Handler(
            static_cast<const pldm::utils::DBusHandler*>(nullptr))
    {}

    int getOemStateSensorReadingsHandler(
        pldm::pdr::EntityType, pldm::pdr::EntityInstance,
        pldm::pdr::ContainerID, pldm::pdr::StateSetId,
        pldm::pdr::CompositeCount, uint16_t,
        std::vector<get_sensor_state_field>& stateField) override
    {
        ++getOemStateSensorReadingsCalls;
        if (getOemStateSensorReadingsResult == PLDM_SUCCESS &&
            !stateField.empty())
        {
            stateField[0].sensor_op_state = PLDM_SENSOR_ENABLED;
            stateField[0].present_state = 1;
            stateField[0].previous_state = 0;
            stateField[0].event_state = 1;
            lastSensorStates = stateField;
        }
        return getOemStateSensorReadingsResult;
    }

    int oemSetStateEffecterStatesHandler(uint16_t, uint16_t, uint16_t, uint8_t,
                                         std::vector<set_effecter_state_field>&,
                                         uint16_t) override
    {
        ++oemSetStateEffecterStatesCalls;
        return PLDM_SUCCESS;
    }

    void buildOEMPDR(pldm::responder::pdr_utils::Repo&) override
    {
        ++buildOEMPDRCalls;
    }

    void checkAndDisableWatchDog() override {}

    bool watchDogRunning() override
    {
        return false;
    }

    void resetWatchDogTimer() override
    {
        ++resetWatchDogCalls;
    }

    void disableWatchDogTimer() override {}

    void countSetEventReceiver() override
    {
        ++countSetEventReceiverCalls;
    }

    int checkBMCState() override
    {
        ++checkBMCStateCalls;
        return checkBMCStateResult;
    }

    void updateOemDbusPaths(std::string&) override {}

    const pldm_pdr_record* fetchLastBMCRecord(const pldm_pdr*) override
    {
        return nullptr;
    }

    bool checkRecordHandleInRange(const uint32_t&) override
    {
        return false;
    }

    void processSetEventReceiver() override {}

    void setSurvTimer(uint8_t, bool) override {}

    void handleBootTypesAtPowerOn() override {}

    void handleBootTypesAtChassisOff() override {}

    size_t buildOEMPDRCalls = 0;
    size_t resetWatchDogCalls = 0;
    size_t checkBMCStateCalls = 0;
    size_t getOemStateSensorReadingsCalls = 0;
    size_t oemSetStateEffecterStatesCalls = 0;
    size_t countSetEventReceiverCalls = 0;
    std::vector<get_sensor_state_field> lastSensorStates{};
    int checkBMCStateResult = PLDM_SUCCESS;
    int getOemStateSensorReadingsResult = PLDM_SUCCESS;
};

class PlatformDbusFixture
{
  public:
    static constexpr auto mapperService = "xyz.openbmc_project.ObjectMapper";
    static constexpr auto mapperPath = "/xyz/openbmc_project/object_mapper";
    static constexpr auto mapperInterface = "xyz.openbmc_project.ObjectMapper";
    static constexpr auto serviceName = "foo.bar";
    static constexpr auto stringObjectPath = "/foo/bar";
    static constexpr auto stringInterface = "xyz.openbmc_project.Foo.Bar";
    static constexpr auto stringBazInterface =
        "xyz.openbmc_project.Foo.Bar.Baz";
    static constexpr auto numericObjectPath = "/foo/numeric";
    static constexpr auto numericInterface = "xyz.openbmc_project.Numeric";

    PlatformDbusFixture(
        std::string initialString = "xyz.openbmc_project.Foo.Bar.V0",
        std::string initialBaz = "xyz.openbmc_project.Foo.Bar.Baz.V1",
        uint64_t initialNumeric = 0) :
        stringValue(std::move(initialString)), bazValue(std::move(initialBaz)),
        numericValue(initialNumeric)
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
                if (path == stringObjectPath)
                {
                    response.emplace(serviceName,
                                     std::vector<std::string>{
                                         stringInterface, stringBazInterface});
                }
                else if (path == numericObjectPath)
                {
                    response.emplace(serviceName, std::vector<std::string>{
                                                      numericInterface});
                }
                return response;
            });
        mapperIface->initialize();

        serviceConnection = std::make_shared<sdbusplus::asio::connection>(
            io, sdbusplus::bus::new_bus());
        serviceConnection->request_name(serviceName);
        serviceServer =
            std::make_unique<sdbusplus::asio::object_server>(serviceConnection);

        stringIface =
            serviceServer->add_interface(stringObjectPath, stringInterface);
        stringIface->register_property(
            "propertyName", stringValue,
            [this](const std::string& requested, std::string& current) {
                current = requested;
                stringValue = requested;
                ++stringSetCount;
                return true;
            },
            [](const std::string& current) { return current; });
        stringIface->initialize();

        bazIface =
            serviceServer->add_interface(stringObjectPath, stringBazInterface);
        bazIface->register_property(
            "propertyName", bazValue,
            [this](const std::string& requested, std::string& current) {
                current = requested;
                bazValue = requested;
                ++bazSetCount;
                return true;
            },
            [](const std::string& current) { return current; });
        bazIface->initialize();

        numericIface =
            serviceServer->add_interface(numericObjectPath, numericInterface);
        numericIface->register_property(
            "numericProperty", numericValue,
            [this](const uint64_t& requested, uint64_t& current) {
                current = requested;
                numericValue = requested;
                ++numericSetCount;
                return true;
            },
            [](const uint64_t& current) { return current; });
        numericIface->initialize();

        ioThread = std::thread([this] { io.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ~PlatformDbusFixture()
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
    std::shared_ptr<sdbusplus::asio::dbus_interface> stringIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> bazIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> numericIface;
    std::string stringValue;
    std::string bazValue;
    uint64_t numericValue{};
    size_t stringSetCount = 0;
    size_t bazSetCount = 0;
    size_t numericSetCount = 0;
    std::thread ioThread;
};

fs::path makeNumericEffecterConfigDir()
{
    auto dir = pldm::test::makeTempDir("PlatformNumericEffecter.XXXXXX");
    std::ofstream(dir / "numeric_effecter.json") << R"({
            "effecterPDRs": [
                {
                    "pdrType": 9,
                    "entries": [
                        {
                            "entity_path": "/xyz/openbmc_project/foo",
                            "type": 0,
                            "instance": 0,
                            "container": 0,
                            "base_unit": 21,
                            "rate_unit": 3,
                            "effecter_resolution_init": 1,
                            "effecter_data_size": 4,
                            "range_field_format": 4,
                            "dbus": {
                                "path": "/foo/numeric",
                                "interface": "xyz.openbmc_project.Numeric",
                                "property_name": "numericProperty",
                                "property_type": "uint64_t"
                            }
                        }
                    ]
                }
            ]
        })";
    return dir;
}

fs::path makeComprehensivePlatformPdrConfigDir()
{
    auto dir = pldm::test::makeTempDir("PlatformPdrConfig.XXXXXX");

    const auto numericDbus = [](const std::string& path) {
        return Json{{"path", path},
                    {"interface", "xyz.openbmc_project.Numeric"},
                    {"property_name", "numericProperty"},
                    {"property_type", "uint64_t"}};
    };
    const auto stringDbus = [](const std::string& path, const Json& values) {
        return Json{{"path", path},
                    {"interface", "xyz.openbmc_project.Foo.Bar"},
                    {"property_name", "propertyName"},
                    {"property_type", "string"},
                    {"property_values", values}};
    };
    const auto typedDbus =
        [](const std::string& path, const char* type, const Json& values) {
            return Json{{"path", path},
                        {"interface", "xyz.openbmc_project.Foo.Bar"},
                        {"property_name", "propertyName"},
                        {"property_type", type},
                        {"property_values", values}};
        };

    Json numericEntries = Json::array();
    numericEntries.push_back(Json{
        {"entity_path", "/xyz/openbmc_project/foo"},
        {"type", 10},
        {"instance", 1},
        {"container", 2},
        {"effecter_data_size", PLDM_EFFECTER_DATA_SIZE_UINT8},
        {"range_field_format", PLDM_RANGE_FIELD_FORMAT_UINT8},
        {"nominal_value", 1},
        {"normal_max", 2},
        {"normal_min", 0},
        {"rated_max", 3},
        {"rated_min", 0},
        {"dbus", numericDbus("/foo/numeric")}});
    numericEntries.push_back(
        Json{{"type", 11},
             {"instance", 2},
             {"container", 3},
             {"effecter_data_size", PLDM_EFFECTER_DATA_SIZE_SINT8},
             {"range_field_format", PLDM_RANGE_FIELD_FORMAT_SINT8},
             {"nominal_value", -1},
             {"normal_max", 4},
             {"normal_min", -4},
             {"rated_max", 6},
             {"rated_min", -6},
             {"dbus", numericDbus("/foo/numeric")}});
    numericEntries.push_back(
        Json{{"type", 12},
             {"instance", 3},
             {"container", 4},
             {"effecter_data_size", PLDM_EFFECTER_DATA_SIZE_UINT16},
             {"range_field_format", PLDM_RANGE_FIELD_FORMAT_UINT16},
             {"nominal_value", 5},
             {"normal_max", 7},
             {"normal_min", 2},
             {"rated_max", 9},
             {"rated_min", 1},
             {"dbus", numericDbus("/foo/numeric")}});
    numericEntries.push_back(
        Json{{"type", 13},
             {"instance", 4},
             {"container", 5},
             {"effecter_data_size", PLDM_EFFECTER_DATA_SIZE_SINT16},
             {"range_field_format", PLDM_RANGE_FIELD_FORMAT_SINT16},
             {"nominal_value", -5},
             {"normal_max", 8},
             {"normal_min", -8},
             {"rated_max", 10},
             {"rated_min", -10},
             {"dbus", numericDbus("/foo/numeric")}});
    numericEntries.push_back(
        Json{{"type", 14},
             {"instance", 5},
             {"container", 6},
             {"effecter_data_size", PLDM_EFFECTER_DATA_SIZE_UINT32},
             {"range_field_format", PLDM_RANGE_FIELD_FORMAT_UINT32},
             {"nominal_value", 11},
             {"normal_max", 14},
             {"normal_min", 4},
             {"rated_max", 15},
             {"rated_min", 3},
             {"dbus", numericDbus("/foo/numeric")}});
    numericEntries.push_back(
        Json{{"type", 15},
             {"instance", 6},
             {"container", 7},
             {"effecter_data_size", PLDM_EFFECTER_DATA_SIZE_SINT32},
             {"range_field_format", PLDM_RANGE_FIELD_FORMAT_SINT32},
             {"nominal_value", -11},
             {"normal_max", 16},
             {"normal_min", -16},
             {"rated_max", 18},
             {"rated_min", -18},
             {"dbus", numericDbus("/foo/numeric")}});
    numericEntries.push_back(
        Json{{"type", 16},
             {"instance", 7},
             {"container", 8},
             {"effecter_data_size", PLDM_EFFECTER_DATA_SIZE_UINT64},
             {"range_field_format", PLDM_RANGE_FIELD_FORMAT_REAL32},
             {"nominal_value", 19.5},
             {"normal_max", 20.5},
             {"normal_min", 18.5},
             {"rated_max", 21.5},
             {"rated_min", 17.5},
             {"dbus", numericDbus("/foo/numeric")}});
    numericEntries.push_back(
        Json{{"type", 17},
             {"instance", 8},
             {"container", 9},
             {"effecter_data_size", PLDM_EFFECTER_DATA_SIZE_SINT64},
             {"range_field_format", 0xFF},
             {"dbus", numericDbus("/bad/path")}});

    Json stateEffecterEntries = Json::array();
    stateEffecterEntries.push_back(Json{
        {"entity_path", "/xyz/openbmc_project/inventory/system/chassis"},
        {"type", 40},
        {"instance", 1},
        {"container", 2},
        {"effecters",
         Json::array(
             {Json{
                  {"set",
                   {{"id", 196}, {"size", 1}, {"states", Json::array({1, 3})}}},
                  {"dbus",
                   stringDbus("/foo/bar",
                              Json::array({"xyz.openbmc_project.Foo.Bar."
                                           "V0",
                                           "xyz.openbmc_project.Foo.Bar."
                                           "V1"}))}},
              Json{
                  {"set",
                   {{"id", 197}, {"size", 2}, {"states", Json::array({1, 9})}}},
                  {"dbus",
                   typedDbus("/foo/bar", "uint8_t", Json::array({7, 8}))}}})}});
    stateEffecterEntries.push_back(Json{
        {"type", 41},
        {"instance", 2},
        {"container", 3},
        {"effecters",
         Json::array({Json{
             {"set",
              {{"id", 198}, {"size", 1}, {"states", Json::array({1, 2})}}},
             {"dbus",
              typedDbus("/bad/path", "uint16_t", Json::array({9, 10}))}}})}});

    Json stateSensorEntries = Json::array();
    stateSensorEntries.push_back(Json{
        {"entity_path", "/xyz/openbmc_project/inventory/system/board"},
        {"type", 60},
        {"instance", 4},
        {"container", 5},
        {"sensors",
         Json::array(
             {Json{
                  {"set",
                   {{"id", 128}, {"size", 1}, {"states", Json::array({0, 1})}}},
                  {"dbus",
                   stringDbus("/foo/bar",
                              Json::array({"xyz.openbmc_project.Foo.Bar."
                                           "V0",
                                           "xyz.openbmc_project.Foo.Bar."
                                           "V1"}))}},
              Json{
                  {"set",
                   {{"id", 129}, {"size", 2}, {"states", Json::array({1, 9})}}},
                  {"dbus",
                   typedDbus("/foo/bar", "uint8_t", Json::array({1, 2}))}}})}});
    stateSensorEntries.push_back(Json{
        {"type", 61},
        {"instance", 5},
        {"container", 6},
        {"sensors",
         Json::array({Json{
             {"set",
              {{"id", 130}, {"size", 1}, {"states", Json::array({0, 1})}}},
             {"dbus",
              typedDbus("/bad/path", "uint16_t", Json::array({3, 4}))}}})}});

    Json json = {
        {"effecterPDRs",
         Json::array({Json{{"pdrType", PLDM_NUMERIC_EFFECTER_PDR},
                           {"entries", std::move(numericEntries)}},
                      Json{{"pdrType", PLDM_STATE_EFFECTER_PDR},
                           {"entries", std::move(stateEffecterEntries)}}})},
        {"sensorPDRs",
         Json::array({Json{{"pdrType", PLDM_STATE_SENSOR_PDR},
                           {"entries", std::move(stateSensorEntries)}}})}};

    std::ofstream(dir / "platform_pdrs.json") << json.dump(2);
    return dir;
}

void addPdrRecord(Repo& repo, std::vector<uint8_t>& record)
{
    PdrEntry entry{};
    entry.data = record.data();
    entry.size = record.size();
    entry.handle.recordHandle = 0;
    repo.addRecord(entry);
}

std::vector<uint8_t> makeNumericEffecterPdrRecord(uint16_t effecterId,
                                                  uint8_t dataSize)
{
    auto pdr = makeNumericEffecterPdr(dataSize);
    pdr.hdr.record_handle = 1;
    pdr.hdr.version = 1;
    pdr.hdr.type = PLDM_NUMERIC_EFFECTER_PDR;
    pdr.hdr.record_change_num = 0;
    pdr.hdr.length =
        sizeof(pldm_numeric_effecter_value_pdr) - sizeof(pldm_pdr_hdr);
    pdr.effecter_id = effecterId;

    std::vector<uint8_t> record(sizeof(pdr));
    std::memcpy(record.data(), &pdr, sizeof(pdr));
    return record;
}

std::tuple<DbusMappings, DbusValMaps> makeStringDbusObjs(
    const std::string& path = "/foo/bar")
{
    DbusMappings mappings{DBusMapping{path, "xyz.openbmc_project.Foo.Bar",
                                      "propertyName", "string"}};
    StatestoDbusVal valueMap{
        {0, PropertyValue(std::string("xyz.openbmc_project.Foo.Bar.V0"))},
        {1, PropertyValue(std::string("xyz.openbmc_project.Foo.Bar.V1"))}};
    DbusValMaps valueMaps{valueMap};
    return {std::move(mappings), std::move(valueMaps)};
}

std::tuple<DbusMappings, DbusValMaps> makeStringDbusObjsWithValues(
    const std::map<State, std::string>& values,
    const std::string& path = "/foo/bar")
{
    DbusMappings mappings{DBusMapping{path, "xyz.openbmc_project.Foo.Bar",
                                      "propertyName", "string"}};
    StatestoDbusVal valueMap{};
    for (const auto& [state, value] : values)
    {
        valueMap.emplace(state, PropertyValue(value));
    }
    DbusValMaps valueMaps{valueMap};
    return {std::move(mappings), std::move(valueMaps)};
}

std::tuple<DbusMappings, DbusValMaps> makeNumericDbusObjs(
    const std::string& path = "/foo/numeric")
{
    DbusMappings mappings{DBusMapping{path, "xyz.openbmc_project.Numeric",
                                      "numericProperty", "uint64_t"}};
    DbusValMaps valueMaps{{}};
    return {std::move(mappings), std::move(valueMaps)};
}

template <typename T>
void expectRawValueSuccessMatrix()
{
    struct Case
    {
        uint8_t dataSize;
        const char* propertyType;
        long double expected;
    };

    constexpr std::array<Case, 8> cases{{
        {PLDM_EFFECTER_DATA_SIZE_UINT8, "uint64_t", 1.0L},
        {PLDM_EFFECTER_DATA_SIZE_SINT8, "int8_t", 2.0L},
        {PLDM_EFFECTER_DATA_SIZE_UINT16, "uint32_t", 3.0L},
        {PLDM_EFFECTER_DATA_SIZE_SINT16, "uint64_t", 4.0L},
        {PLDM_EFFECTER_DATA_SIZE_UINT32, "uint32_t", 5.0L},
        {PLDM_EFFECTER_DATA_SIZE_SINT32, "uint64_t", 6.0L},
        {PLDM_EFFECTER_DATA_SIZE_UINT64, "uint64_t", 7.0L},
        {PLDM_EFFECTER_DATA_SIZE_SINT64, "int64_t", 8.0L},
    }};

    for (const auto& testCase : cases)
    {
        auto pdr = makeNumericEffecterPdr(testCase.dataSize);
        T value = static_cast<T>(testCase.expected);
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, testCase.propertyType);

        ASSERT_EQ(rc, PLDM_SUCCESS);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_TRUE(isNumericProperty(dbusValue.value()));
        EXPECT_EQ(numericProperty(dbusValue.value()), testCase.expected);
    }
}

template <typename T>
void expectRawValueUpperBoundMatrix()
{
    struct Case
    {
        uint8_t dataSize;
        const char* propertyType;
    };

    constexpr std::array<Case, 8> cases{{
        {PLDM_EFFECTER_DATA_SIZE_UINT8, "uint64_t"},
        {PLDM_EFFECTER_DATA_SIZE_SINT8, "int8_t"},
        {PLDM_EFFECTER_DATA_SIZE_UINT16, "uint32_t"},
        {PLDM_EFFECTER_DATA_SIZE_SINT16, "uint64_t"},
        {PLDM_EFFECTER_DATA_SIZE_UINT32, "uint32_t"},
        {PLDM_EFFECTER_DATA_SIZE_SINT32, "uint64_t"},
        {PLDM_EFFECTER_DATA_SIZE_UINT64, "uint64_t"},
        {PLDM_EFFECTER_DATA_SIZE_SINT64, "int64_t"},
    }};

    for (const auto& testCase : cases)
    {
        auto pdr = makeNumericEffecterPdr(testCase.dataSize);
        switch (testCase.dataSize)
        {
            case PLDM_EFFECTER_DATA_SIZE_UINT8:
                pdr.min_settable.value_u8 = 0;
                pdr.max_settable.value_u8 = 1;
                break;
            case PLDM_EFFECTER_DATA_SIZE_SINT8:
                pdr.min_settable.value_s8 = 0;
                pdr.max_settable.value_s8 = 1;
                break;
            case PLDM_EFFECTER_DATA_SIZE_UINT16:
                pdr.min_settable.value_u16 = 0;
                pdr.max_settable.value_u16 = 1;
                break;
            case PLDM_EFFECTER_DATA_SIZE_SINT16:
                pdr.min_settable.value_s16 = 0;
                pdr.max_settable.value_s16 = 1;
                break;
            case PLDM_EFFECTER_DATA_SIZE_UINT32:
                pdr.min_settable.value_u32 = 0;
                pdr.max_settable.value_u32 = 1;
                break;
            case PLDM_EFFECTER_DATA_SIZE_SINT32:
                pdr.min_settable.value_s32 = 0;
                pdr.max_settable.value_s32 = 1;
                break;
            case PLDM_EFFECTER_DATA_SIZE_UINT64:
                pdr.min_settable.value_u64 = 0;
                pdr.max_settable.value_u64 = 1;
                break;
            case PLDM_EFFECTER_DATA_SIZE_SINT64:
                pdr.min_settable.value_s64 = 0;
                pdr.max_settable.value_s64 = 1;
                break;
        }

        T value = static_cast<T>(2);
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, testCase.propertyType);

        ASSERT_EQ(rc, PLDM_ERROR_INVALID_DATA);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_TRUE(isNumericProperty(dbusValue.value()));
        EXPECT_EQ(numericProperty(dbusValue.value()), 2.0L);
    }
}

template <typename T>
void expectRawValueDisabledBoundsMatrix()
{
    struct Case
    {
        uint8_t dataSize;
        const char* propertyType;
    };

    constexpr std::array<Case, 8> cases{{
        {PLDM_EFFECTER_DATA_SIZE_UINT8, "uint64_t"},
        {PLDM_EFFECTER_DATA_SIZE_SINT8, "int8_t"},
        {PLDM_EFFECTER_DATA_SIZE_UINT16, "uint32_t"},
        {PLDM_EFFECTER_DATA_SIZE_SINT16, "uint64_t"},
        {PLDM_EFFECTER_DATA_SIZE_UINT32, "uint32_t"},
        {PLDM_EFFECTER_DATA_SIZE_SINT32, "uint64_t"},
        {PLDM_EFFECTER_DATA_SIZE_UINT64, "uint64_t"},
        {PLDM_EFFECTER_DATA_SIZE_SINT64, "int64_t"},
    }};

    for (const auto& testCase : cases)
    {
        auto pdr = makeNumericEffecterPdr(testCase.dataSize);
        switch (testCase.dataSize)
        {
            case PLDM_EFFECTER_DATA_SIZE_UINT8:
                pdr.min_settable.value_u8 = 1;
                pdr.max_settable.value_u8 = 1;
                break;
            case PLDM_EFFECTER_DATA_SIZE_SINT8:
                pdr.min_settable.value_s8 = 1;
                pdr.max_settable.value_s8 = 1;
                break;
            case PLDM_EFFECTER_DATA_SIZE_UINT16:
                pdr.min_settable.value_u16 = 1;
                pdr.max_settable.value_u16 = 1;
                break;
            case PLDM_EFFECTER_DATA_SIZE_SINT16:
                pdr.min_settable.value_s16 = 1;
                pdr.max_settable.value_s16 = 1;
                break;
            case PLDM_EFFECTER_DATA_SIZE_UINT32:
                pdr.min_settable.value_u32 = 1;
                pdr.max_settable.value_u32 = 1;
                break;
            case PLDM_EFFECTER_DATA_SIZE_SINT32:
                pdr.min_settable.value_s32 = 1;
                pdr.max_settable.value_s32 = 1;
                break;
            case PLDM_EFFECTER_DATA_SIZE_UINT64:
                pdr.min_settable.value_u64 = 1;
                pdr.max_settable.value_u64 = 1;
                break;
            case PLDM_EFFECTER_DATA_SIZE_SINT64:
                pdr.min_settable.value_s64 = 1;
                pdr.max_settable.value_s64 = 1;
                break;
        }

        T value = static_cast<T>(2);
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, testCase.propertyType);

        ASSERT_EQ(rc, PLDM_SUCCESS);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_TRUE(isNumericProperty(dbusValue.value()));
        EXPECT_EQ(numericProperty(dbusValue.value()), 2.0L);
    }
}

void tightenEffecterBoundsForCoverage(pldm_numeric_effecter_value_pdr& pdr,
                                      uint8_t dataSize)
{
    switch (dataSize)
    {
        case PLDM_EFFECTER_DATA_SIZE_UINT8:
            pdr.min_settable.value_u8 = 0;
            pdr.max_settable.value_u8 = 1;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT8:
            pdr.min_settable.value_s8 = -1;
            pdr.max_settable.value_s8 = 1;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT16:
            pdr.min_settable.value_u16 = 0;
            pdr.max_settable.value_u16 = 1;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT16:
            pdr.min_settable.value_s16 = -1;
            pdr.max_settable.value_s16 = 1;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT32:
            pdr.min_settable.value_u32 = 0;
            pdr.max_settable.value_u32 = 1;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT32:
            pdr.min_settable.value_s32 = -1;
            pdr.max_settable.value_s32 = 1;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT64:
            pdr.min_settable.value_u64 = 0;
            pdr.max_settable.value_u64 = 1;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT64:
            pdr.min_settable.value_s64 = -1;
            pdr.max_settable.value_s64 = 1;
            break;
        default:
            break;
    }
}

void tightenEffecterLowerBoundsForCoverage(pldm_numeric_effecter_value_pdr& pdr,
                                           uint8_t dataSize)
{
    switch (dataSize)
    {
        case PLDM_EFFECTER_DATA_SIZE_UINT8:
            pdr.min_settable.value_u8 = 1;
            pdr.max_settable.value_u8 = 3;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT8:
            pdr.min_settable.value_s8 = 1;
            pdr.max_settable.value_s8 = 3;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT16:
            pdr.min_settable.value_u16 = 1;
            pdr.max_settable.value_u16 = 3;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT16:
            pdr.min_settable.value_s16 = 1;
            pdr.max_settable.value_s16 = 3;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT32:
            pdr.min_settable.value_u32 = 1;
            pdr.max_settable.value_u32 = 3;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT32:
            pdr.min_settable.value_s32 = 1;
            pdr.max_settable.value_s32 = 3;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT64:
            pdr.min_settable.value_u64 = 1;
            pdr.max_settable.value_u64 = 3;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT64:
            pdr.min_settable.value_s64 = 1;
            pdr.max_settable.value_s64 = 3;
            break;
        default:
            break;
    }
}

template <typename T>
void expectInvalidRawValueDataSizeCoverage()
{
    pldm_numeric_effecter_value_pdr pdr{};
    pdr.effecter_data_size = 0xFF;
    T value = static_cast<T>(1);
    auto [rc, dbusValue] =
        platform_numeric_effecter::getEffecterRawValue(&pdr, value, "uint8_t");

    ASSERT_EQ(rc, PLDM_SUCCESS);
    ASSERT_TRUE(dbusValue.has_value());
    EXPECT_FALSE(std::get<bool>(dbusValue.value()));
}

template <typename T>
void expectOutOfRangeRawValueCoverage(uint8_t dataSize,
                                      const char* propertyType)
{
    auto pdr = makeNumericEffecterPdr(dataSize);
    tightenEffecterBoundsForCoverage(pdr, dataSize);

    T value = static_cast<T>(2);
    auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
        &pdr, value, propertyType);

    ASSERT_EQ(rc, PLDM_ERROR_INVALID_DATA);
    ASSERT_TRUE(dbusValue.has_value());
    EXPECT_TRUE(isNumericProperty(dbusValue.value()));
    EXPECT_EQ(numericProperty(dbusValue.value()), 2.0L);
}

template <typename T>
void expectLowerBoundRawValueCoverage(uint8_t dataSize,
                                      const char* propertyType)
{
    auto pdr = makeNumericEffecterPdr(dataSize);
    tightenEffecterLowerBoundsForCoverage(pdr, dataSize);

    T value = static_cast<T>(0);
    auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
        &pdr, value, propertyType);

    ASSERT_EQ(rc, PLDM_ERROR_INVALID_DATA);
    ASSERT_TRUE(dbusValue.has_value());
    EXPECT_TRUE(isNumericProperty(dbusValue.value()));
    EXPECT_EQ(numericProperty(dbusValue.value()), 0.0L);
}

template <typename T>
void expectNoConversionRawValueCoverage(uint8_t dataSize,
                                        const char* propertyType)
{
    auto pdr = makeNumericEffecterPdr(dataSize);
    T value = static_cast<T>(1);
    auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
        &pdr, value, propertyType);

    ASSERT_EQ(rc, PLDM_SUCCESS);
    ASSERT_TRUE(dbusValue.has_value());
    EXPECT_TRUE(isNumericProperty(dbusValue.value()));
    EXPECT_EQ(numericProperty(dbusValue.value()), 1.0L);
}

template <typename T>
void expectUint64ConversionRawValueCoverage(uint8_t dataSize)
{
    auto pdr = makeNumericEffecterPdr(dataSize);
    T value = static_cast<T>(1);
    auto [rc, dbusValue] =
        platform_numeric_effecter::getEffecterRawValue(&pdr, value, "uint64_t");

    ASSERT_EQ(rc, PLDM_SUCCESS);
    ASSERT_TRUE(dbusValue.has_value());
    ASSERT_TRUE(std::holds_alternative<uint64_t>(dbusValue.value()));
    EXPECT_EQ(std::get<uint64_t>(dbusValue.value()), 1u);
}

uint16_t firstNumericEffecterId(const Repo& repo)
{
    std::unique_ptr<pldm_pdr, decltype(&pldm_pdr_destroy)> numericRepo(
        pldm_pdr_init(), pldm_pdr_destroy);
    Repo numericEffecters(numericRepo.get());
    getRepoByType(repo, numericEffecters, PLDM_NUMERIC_EFFECTER_PDR);
    PdrEntry entry{};
    auto* record = numericEffecters.getFirstRecord(entry);
    EXPECT_NE(record, nullptr);
    if (record == nullptr)
    {
        return 0;
    }
    auto* pdr =
        reinterpret_cast<const pldm_numeric_effecter_value_pdr*>(entry.data);
    return pdr->effecter_id;
}

fs::path makeEventConfigDir(const std::string& json)
{
    auto dir = pldm::test::makeTempDir("PlatformEventConfig.XXXXXX");
    std::ofstream(dir / "event_state_sensor.json") << json;
    return dir;
}

} // namespace
#pragma GCC diagnostic pop

TEST(getPDR, testGoodPath)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr) + PLDM_GET_PDR_REQ_BYTES>
        requestPayload{};
    auto req = std::start_lifetime_as<pldm_msg>(requestPayload.data());
    size_t requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);

    struct pldm_get_pdr_req* request =
        std::start_lifetime_as<pldm_get_pdr_req>(req->payload);
    request->request_count = 100;

    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(StrEq("/foo/bar"), _))
        .Times(5)
        .WillRepeatedly(Return("foo.bar"));

    auto pdrRepo = pldm_pdr_init();
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_effecter/good", pdrRepo,
                    nullptr, nullptr, nullptr, nullptr, event);
    Repo repo(pdrRepo);
    ASSERT_EQ(repo.empty(), false);
    auto response = handler.getPDR(req, requestPayloadLength);
    auto responsePtr = std::start_lifetime_as<pldm_msg>(response.data());

    struct pldm_get_pdr_resp* resp =
        std::start_lifetime_as<pldm_get_pdr_resp>(responsePtr->payload);
    ASSERT_EQ(PLDM_SUCCESS, resp->completion_code);
    ASSERT_EQ(2, resp->next_record_handle);
    ASSERT_EQ(true, resp->response_count != 0);

    pldm_pdr_hdr* hdr = std::start_lifetime_as<pldm_pdr_hdr>(resp->record_data);
    ASSERT_EQ(hdr->record_handle, 1);
    ASSERT_EQ(hdr->version, 1);

    pldm_pdr_destroy(pdrRepo);
}

TEST(getPDR, testShortRead)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr) + PLDM_GET_PDR_REQ_BYTES>
        requestPayload{};
    auto req = std::start_lifetime_as<pldm_msg>(requestPayload.data());
    size_t requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);

    struct pldm_get_pdr_req* request =
        std::start_lifetime_as<pldm_get_pdr_req>(req->payload);
    request->request_count = 1;

    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(StrEq("/foo/bar"), _))
        .Times(5)
        .WillRepeatedly(Return("foo.bar"));

    auto pdrRepo = pldm_pdr_init();
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_effecter/good", pdrRepo,
                    nullptr, nullptr, nullptr, nullptr, event);
    Repo repo(pdrRepo);
    ASSERT_EQ(repo.empty(), false);
    auto response = handler.getPDR(req, requestPayloadLength);
    auto responsePtr = std::start_lifetime_as<pldm_msg>(response.data());
    struct pldm_get_pdr_resp* resp =
        std::start_lifetime_as<pldm_get_pdr_resp>(responsePtr->payload);
    ASSERT_EQ(PLDM_SUCCESS, resp->completion_code);
    ASSERT_EQ(1, resp->response_count);
    pldm_pdr_destroy(pdrRepo);
}

TEST(getPDR, testZeroRequestCountReturnsHeaderOnlyResponse)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr) + PLDM_GET_PDR_REQ_BYTES>
        requestPayload{};
    auto req = reinterpret_cast<pldm_msg*>(requestPayload.data());
    size_t requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);

    struct pldm_get_pdr_req* request =
        reinterpret_cast<struct pldm_get_pdr_req*>(req->payload);
    request->request_count = 0;

    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(StrEq("/foo/bar"), _))
        .Times(5)
        .WillRepeatedly(Return("foo.bar"));

    auto pdrRepo = pldm_pdr_init();
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_effecter/good", pdrRepo,
                    nullptr, nullptr, nullptr, nullptr, event);
    Repo repo(pdrRepo);
    ASSERT_EQ(repo.empty(), false);

    auto response = handler.getPDR(req, requestPayloadLength);
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    auto* resp =
        reinterpret_cast<struct pldm_get_pdr_resp*>(responsePtr->payload);

    ASSERT_EQ(PLDM_SUCCESS, resp->completion_code);
    ASSERT_EQ(2, resp->next_record_handle);
    ASSERT_EQ(0, resp->response_count);
    ASSERT_EQ(sizeof(pldm_msg_hdr) + PLDM_GET_PDR_MIN_RESP_BYTES,
              response.size());

    pldm_pdr_destroy(pdrRepo);
}

TEST(getPDR, testBadRecordHandle)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr) + PLDM_GET_PDR_REQ_BYTES>
        requestPayload{};
    auto req = std::start_lifetime_as<pldm_msg>(requestPayload.data());
    size_t requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);

    struct pldm_get_pdr_req* request =
        std::start_lifetime_as<pldm_get_pdr_req>(req->payload);
    request->record_handle = 100000;
    request->request_count = 1;

    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(StrEq("/foo/bar"), _))
        .Times(5)
        .WillRepeatedly(Return("foo.bar"));

    auto pdrRepo = pldm_pdr_init();
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_effecter/good", pdrRepo,
                    nullptr, nullptr, nullptr, nullptr, event);
    Repo repo(pdrRepo);
    ASSERT_EQ(repo.empty(), false);
    auto response = handler.getPDR(req, requestPayloadLength);
    auto responsePtr = std::start_lifetime_as<pldm_msg>(response.data());

    ASSERT_EQ(responsePtr->payload[0], PLDM_PLATFORM_INVALID_RECORD_HANDLE);

    pldm_pdr_destroy(pdrRepo);
}

TEST(getPDR, testNoNextRecord)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr) + PLDM_GET_PDR_REQ_BYTES>
        requestPayload{};
    auto req = std::start_lifetime_as<pldm_msg>(requestPayload.data());
    size_t requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);

    struct pldm_get_pdr_req* request =
        std::start_lifetime_as<pldm_get_pdr_req>(req->payload);
    request->record_handle = 1;

    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(StrEq("/foo/bar"), _))
        .Times(5)
        .WillRepeatedly(Return("foo.bar"));

    auto pdrRepo = pldm_pdr_init();
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_effecter/good", pdrRepo,
                    nullptr, nullptr, nullptr, nullptr, event);
    Repo repo(pdrRepo);
    ASSERT_EQ(repo.empty(), false);
    auto response = handler.getPDR(req, requestPayloadLength);
    auto responsePtr = std::start_lifetime_as<pldm_msg>(response.data());
    struct pldm_get_pdr_resp* resp =
        std::start_lifetime_as<pldm_get_pdr_resp>(responsePtr->payload);
    ASSERT_EQ(PLDM_SUCCESS, resp->completion_code);
    ASSERT_EQ(2, resp->next_record_handle);

    pldm_pdr_destroy(pdrRepo);
}

TEST(getPDR, testFindPDR)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr) + PLDM_GET_PDR_REQ_BYTES>
        requestPayload{};
    auto req = std::start_lifetime_as<pldm_msg>(requestPayload.data());
    size_t requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);

    struct pldm_get_pdr_req* request =
        std::start_lifetime_as<pldm_get_pdr_req>(req->payload);
    request->request_count = 100;

    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(StrEq("/foo/bar"), _))
        .Times(5)
        .WillRepeatedly(Return("foo.bar"));

    auto pdrRepo = pldm_pdr_init();
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_effecter/good", pdrRepo,
                    nullptr, nullptr, nullptr, nullptr, event);
    Repo repo(pdrRepo);
    ASSERT_EQ(repo.empty(), false);
    auto response = handler.getPDR(req, requestPayloadLength);

    // Let's try to find a PDR of type stateEffecter (= 11) and entity type =
    // 100
    bool found = false;
    uint32_t handle = 0; // start asking for PDRs from recordHandle 0
    while (!found)
    {
        request->record_handle = handle;
        auto response = handler.getPDR(req, requestPayloadLength);
        auto responsePtr = std::start_lifetime_as<pldm_msg>(response.data());
        struct pldm_get_pdr_resp* resp =
            std::start_lifetime_as<pldm_get_pdr_resp>(responsePtr->payload);
        ASSERT_EQ(PLDM_SUCCESS, resp->completion_code);

        handle = resp->next_record_handle;

        pldm_pdr_hdr* hdr =
            std::start_lifetime_as<pldm_pdr_hdr>(resp->record_data);
        if (hdr->type == PLDM_STATE_EFFECTER_PDR)
        {
            pldm_state_effecter_pdr* pdr =
                std::start_lifetime_as<pldm_state_effecter_pdr>(
                    resp->record_data);
            if (pdr->entity_type == 100)
            {
                found = true;
                // Rest of the PDR can be accessed as need be
                break;
            }
        }
        if (!resp->next_record_handle) // no more records
        {
            break;
        }
    }
    ASSERT_EQ(found, true);

    pldm_pdr_destroy(pdrRepo);
}

TEST(getPDR, testEncodeFailureOnInvalidInstanceId)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr) + PLDM_GET_PDR_REQ_BYTES>
        requestPayload{};
    auto* req = reinterpret_cast<pldm_msg*>(requestPayload.data());
    size_t requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);
    auto* request = reinterpret_cast<struct pldm_get_pdr_req*>(req->payload);
    request->request_count = 100;
    auto run = [&] {
        pldm::test::coverage::ScopedHookStateReset hooks;
        pldm::test::coverage::setForcePackFailure();

        MockdBusHandler mockedUtils;
        EXPECT_CALL(mockedUtils, getService(StrEq("/foo/bar"), _))
            .Times(5)
            .WillRepeatedly(Return("foo.bar"));

        auto* pdrRepo = pldm_pdr_init();
        auto event = sdeventplus::Event::get_default();
        Handler handler(&mockedUtils, "./pdr_jsons/state_effecter/good",
                        pdrRepo, nullptr, nullptr, nullptr, nullptr, event);
        static_cast<void>(handler.getPDR(req, requestPayloadLength));
    };

    EXPECT_DEATH(run(), ".*");
}

TEST(setStateEffecterStatesHandler, testGoodRequest)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr) + PLDM_GET_PDR_REQ_BYTES>
        requestPayload{};
    auto req = std::start_lifetime_as<pldm_msg>(requestPayload.data());
    size_t requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);

    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(StrEq("/foo/bar"), _))
        .Times(5)
        .WillRepeatedly(Return("foo.bar"));

    auto inPDRRepo = pldm_pdr_init();
    auto outPDRRepo = pldm_pdr_init();
    Repo outRepo(outPDRRepo);
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_effecter/good", inPDRRepo,
                    nullptr, nullptr, nullptr, nullptr, event);
    handler.getPDR(req, requestPayloadLength);
    Repo inRepo(inPDRRepo);
    getRepoByType(inRepo, outRepo, PLDM_STATE_EFFECTER_PDR);
    pdr_utils::PdrEntry e;
    auto record1 = pdr::getRecordByHandle(outRepo, 2, e);
    ASSERT_NE(record1, nullptr);
    pldm_state_effecter_pdr* pdr =
        std::start_lifetime_as<pldm_state_effecter_pdr>(e.data);
    EXPECT_EQ(pdr->hdr.type, PLDM_STATE_EFFECTER_PDR);

    std::vector<set_effecter_state_field> stateField;
    stateField.push_back({PLDM_REQUEST_SET, 1});
    stateField.push_back({PLDM_REQUEST_SET, 1});
    std::string value = "xyz.openbmc_project.Foo.Bar.V1";
    PropertyValue propertyValue = value;

    DBusMapping dbusMapping{"/foo/bar", "xyz.openbmc_project.Foo.Bar",
                            "propertyName", "string"};

    EXPECT_CALL(mockedUtils, setDbusProperty(dbusMapping, propertyValue))
        .Times(2);
    auto rc = platform_state_effecter::setStateEffecterStatesHandler<
        MockdBusHandler, Handler>(mockedUtils, handler, 0x1, stateField);
    ASSERT_EQ(rc, 0);

    pldm_pdr_destroy(inPDRRepo);
    pldm_pdr_destroy(outPDRRepo);
}

TEST(setStateEffecterStatesHandler, testBadRequest)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr) + PLDM_GET_PDR_REQ_BYTES>
        requestPayload{};
    auto req = std::start_lifetime_as<pldm_msg>(requestPayload.data());
    size_t requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);

    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(StrEq("/foo/bar"), _))
        .Times(5)
        .WillRepeatedly(Return("foo.bar"));

    auto inPDRRepo = pldm_pdr_init();
    auto outPDRRepo = pldm_pdr_init();
    Repo outRepo(outPDRRepo);
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_effecter/good", inPDRRepo,
                    nullptr, nullptr, nullptr, nullptr, event);
    handler.getPDR(req, requestPayloadLength);
    Repo inRepo(inPDRRepo);
    getRepoByType(inRepo, outRepo, PLDM_STATE_EFFECTER_PDR);
    pdr_utils::PdrEntry e;
    auto record1 = pdr::getRecordByHandle(outRepo, 2, e);
    ASSERT_NE(record1, nullptr);
    pldm_state_effecter_pdr* pdr =
        std::start_lifetime_as<pldm_state_effecter_pdr>(e.data);
    EXPECT_EQ(pdr->hdr.type, PLDM_STATE_EFFECTER_PDR);

    std::vector<set_effecter_state_field> stateField;
    stateField.push_back({PLDM_REQUEST_SET, 3});
    stateField.push_back({PLDM_REQUEST_SET, 4});

    auto rc = platform_state_effecter::setStateEffecterStatesHandler<
        MockdBusHandler, Handler>(mockedUtils, handler, 0x1, stateField);
    ASSERT_EQ(rc, PLDM_PLATFORM_SET_EFFECTER_UNSUPPORTED_SENSORSTATE);

    rc = platform_state_effecter::setStateEffecterStatesHandler<
        MockdBusHandler, Handler>(mockedUtils, handler, 0x9, stateField);
    ASSERT_EQ(rc, PLDM_PLATFORM_INVALID_EFFECTER_ID);

    stateField.push_back({PLDM_REQUEST_SET, 4});
    rc = platform_state_effecter::setStateEffecterStatesHandler<
        MockdBusHandler, Handler>(mockedUtils, handler, 0x1, stateField);
    ASSERT_EQ(rc, PLDM_ERROR_INVALID_DATA);

    pldm_pdr_destroy(inPDRRepo);
    pldm_pdr_destroy(outPDRRepo);
}

TEST(setNumericEffecterValueHandler, testGoodRequest)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(StrEq("/foo/bar"), _))
        .Times(5)
        .WillRepeatedly(Return("foo.bar"));

    auto inPDRRepo = pldm_pdr_init();
    auto numericEffecterPdrRepo = pldm_pdr_init();
    Repo numericEffecterPDRs(numericEffecterPdrRepo);
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_effecter/good", inPDRRepo,
                    nullptr, nullptr, nullptr, nullptr, event);
    Repo inRepo(inPDRRepo);
    getRepoByType(inRepo, numericEffecterPDRs, PLDM_NUMERIC_EFFECTER_PDR);

    pdr_utils::PdrEntry e;
    auto record4 = pdr::getRecordByHandle(numericEffecterPDRs, 4, e);
    ASSERT_NE(record4, nullptr);

    pldm_numeric_effecter_value_pdr* pdr =
        std::start_lifetime_as<pldm_numeric_effecter_value_pdr>(e.data);
    EXPECT_EQ(pdr->hdr.type, PLDM_NUMERIC_EFFECTER_PDR);

    uint16_t effecterId = 3;
    uint32_t effecterValue = 2100000000; // 2036-07-18 21:20:00
    PropertyValue propertyValue = static_cast<uint64_t>(effecterValue);

    DBusMapping dbusMapping{"/foo/bar", "xyz.openbmc_project.Foo.Bar",
                            "propertyName", "uint64_t"};
    EXPECT_CALL(mockedUtils, setDbusProperty(dbusMapping, propertyValue))
        .Times(1);

    auto rc = platform_numeric_effecter::setNumericEffecterValueHandler<
        MockdBusHandler, Handler>(
        mockedUtils, handler, effecterId, PLDM_EFFECTER_DATA_SIZE_UINT32,
        reinterpret_cast<uint8_t*>(&effecterValue), 4);
    ASSERT_EQ(rc, 0);

    pldm_pdr_destroy(inPDRRepo);
    pldm_pdr_destroy(numericEffecterPdrRepo);
}

TEST(setNumericEffecterValueHandler, testBadRequest)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(StrEq("/foo/bar"), _))
        .Times(5)
        .WillRepeatedly(Return("foo.bar"));

    auto inPDRRepo = pldm_pdr_init();
    auto numericEffecterPdrRepo = pldm_pdr_init();
    Repo numericEffecterPDRs(numericEffecterPdrRepo);
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_effecter/good", inPDRRepo,
                    nullptr, nullptr, nullptr, nullptr, event);
    Repo inRepo(inPDRRepo);
    getRepoByType(inRepo, numericEffecterPDRs, PLDM_NUMERIC_EFFECTER_PDR);

    pdr_utils::PdrEntry e;
    auto record4 = pdr::getRecordByHandle(numericEffecterPDRs, 4, e);
    ASSERT_NE(record4, nullptr);

    pldm_numeric_effecter_value_pdr* pdr =
        std::start_lifetime_as<pldm_numeric_effecter_value_pdr>(e.data);
    EXPECT_EQ(pdr->hdr.type, PLDM_NUMERIC_EFFECTER_PDR);

    uint16_t effecterId = 3;
    uint64_t effecterValue = 9876543210;
    auto rc = platform_numeric_effecter::setNumericEffecterValueHandler<
        MockdBusHandler, Handler>(
        mockedUtils, handler, effecterId, PLDM_EFFECTER_DATA_SIZE_SINT32,
        reinterpret_cast<uint8_t*>(&effecterValue), 3);
    ASSERT_EQ(rc, PLDM_ERROR_INVALID_DATA);

    pldm_pdr_destroy(inPDRRepo);
    pldm_pdr_destroy(numericEffecterPdrRepo);
}

TEST(convertToDbusValue, coversAllEffecterDataSizes)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(StrEq("/foo/bar"), _))
        .Times(5)
        .WillRepeatedly(Return("foo.bar"));

    auto inPDRRepo = pldm_pdr_init();
    auto numericEffecterPdrRepo = pldm_pdr_init();
    Repo numericEffecterPDRs(numericEffecterPdrRepo);
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_effecter/good", inPDRRepo,
                    nullptr, nullptr, nullptr, nullptr, event);
    Repo inRepo(inPDRRepo);
    getRepoByType(inRepo, numericEffecterPDRs, PLDM_NUMERIC_EFFECTER_PDR);

    pdr_utils::PdrEntry e;
    auto record4 = pdr::getRecordByHandle(numericEffecterPDRs, 4, e);
    ASSERT_NE(record4, nullptr);

    pldm_numeric_effecter_value_pdr* pdr =
        std::start_lifetime_as<pldm_numeric_effecter_value_pdr>(e.data);
    EXPECT_EQ(pdr->hdr.type, PLDM_NUMERIC_EFFECTER_PDR);

    uint16_t effecterId = 3;

    uint8_t effecterDataSize{};
    pldm::utils::PropertyValue dbusValue;
    std::string propertyType;

    // effecterValue return the present numeric setting
    uint32_t effecterValue = 2100000000;
    using effecterOperationalState = uint8_t;
    using completionCode = uint8_t;

    EXPECT_CALL(mockedUtils,
                getDbusPropertyVariant(StrEq("/foo/bar"), StrEq("propertyName"),
                                       StrEq("xyz.openbmc_project.Foo.Bar")))
        .WillOnce(Return(PropertyValue(static_cast<uint64_t>(effecterValue))));

    auto rc = platform_numeric_effecter::getNumericEffecterData<
        MockdBusHandler, Handler>(mockedUtils, handler, effecterId,
                                  effecterDataSize, propertyType, dbusValue);

    ASSERT_EQ(rc, 0);

    size_t responsePayloadLength =
        sizeof(completionCode) + sizeof(effecterDataSize) +
        sizeof(effecterOperationalState) +
        getEffecterDataSize(effecterDataSize) +
        getEffecterDataSize(effecterDataSize);

    Response response(responsePayloadLength + sizeof(pldm_msg_hdr));
    auto responsePtr = std::start_lifetime_as<pldm_msg>(response.data());

    rc = platform_numeric_effecter::getNumericEffecterValueHandler(
        propertyType, dbusValue, effecterDataSize, responsePtr,
        responsePayloadLength, 1);

    ASSERT_EQ(rc, 0);

    struct pldm_get_numeric_effecter_value_resp* resp =
        std::start_lifetime_as<pldm_get_numeric_effecter_value_resp>(
            responsePtr->payload);
    ASSERT_EQ(PLDM_SUCCESS, resp->completion_code);
    uint32_t valPresent = 0;
    memcpy(&valPresent, &resp->pending_and_present_values[4],
           sizeof(valPresent));

    ASSERT_EQ(effecterValue, valPresent);

    pldm_pdr_destroy(inPDRRepo);
    pldm_pdr_destroy(numericEffecterPdrRepo);
}

TEST(convertToDbusValue, outOfRangeAndInvalidSize)
{
    auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_UINT8);
    pdr.min_settable.value_u8 = 1;
    pdr.max_settable.value_u8 = 5;
    uint8_t input = 9;
    auto [rc, dbusValue] = platform_numeric_effecter::convertToDbusValue(
        &pdr, PLDM_EFFECTER_DATA_SIZE_UINT8, &input, "uint8_t");
    EXPECT_EQ(rc, PLDM_ERROR_INVALID_DATA);
    ASSERT_TRUE(dbusValue.has_value());

    auto [badRc, badValue] = platform_numeric_effecter::convertToDbusValue(
        &pdr, 0xFF, &input, "uint8_t");
    EXPECT_EQ(badRc, PLDM_ERROR);
    EXPECT_FALSE(badValue.has_value());
}

TEST(getEffecterRawValue, additionalConversionCoverage)
{
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_UINT8);
        uint8_t value = 9;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "uint32_t");
        EXPECT_EQ(rc, PLDM_SUCCESS);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<uint32_t>(dbusValue.value()), 9u);
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_UINT16);
        uint16_t value = 33;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "uint64_t");
        EXPECT_EQ(rc, PLDM_SUCCESS);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<uint64_t>(dbusValue.value()), 33u);
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_SINT16);
        int16_t value = -8;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "uint32_t");
        EXPECT_EQ(rc, PLDM_SUCCESS);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<uint32_t>(dbusValue.value()),
                  static_cast<uint32_t>(value));
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_UINT32);
        uint32_t value = 77;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "uint64_t");
        EXPECT_EQ(rc, PLDM_SUCCESS);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<uint64_t>(dbusValue.value()), 77u);
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_SINT32);
        int32_t value = -5;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "uint64_t");
        EXPECT_EQ(rc, PLDM_SUCCESS);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<uint64_t>(dbusValue.value()),
                  static_cast<uint64_t>(value));
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_UINT8);
        pdr.min_settable.value_u8 = 10;
        pdr.max_settable.value_u8 = 1;
        uint8_t value = 200;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "uint8_t");
        EXPECT_EQ(rc, PLDM_SUCCESS);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<uint8_t>(dbusValue.value()), 200u);
    }
}

TEST(getEffecterRawValue, lowerBoundCoverageAcrossTypes)
{
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_UINT8);
        pdr.min_settable.value_u8 = 5;
        pdr.max_settable.value_u8 = 10;
        uint8_t value = 4;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "uint8_t");
        EXPECT_EQ(rc, PLDM_ERROR_INVALID_DATA);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<uint8_t>(dbusValue.value()), 4u);
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_SINT8);
        pdr.min_settable.value_s8 = -4;
        pdr.max_settable.value_s8 = 4;
        int8_t value = -5;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "int8_t");
        EXPECT_EQ(rc, PLDM_ERROR_INVALID_DATA);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_TRUE(isNumericProperty(dbusValue.value()));
        EXPECT_EQ(numericProperty(dbusValue.value()),
                  static_cast<long double>(value));
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_UINT16);
        pdr.min_settable.value_u16 = 2;
        pdr.max_settable.value_u16 = 10;
        uint16_t value = 1;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "uint16_t");
        EXPECT_EQ(rc, PLDM_ERROR_INVALID_DATA);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<uint16_t>(dbusValue.value()), 1u);
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_SINT16);
        pdr.min_settable.value_s16 = -3;
        pdr.max_settable.value_s16 = 3;
        int16_t value = -4;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "int16_t");
        EXPECT_EQ(rc, PLDM_ERROR_INVALID_DATA);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<int16_t>(dbusValue.value()), value);
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_UINT32);
        pdr.min_settable.value_u32 = 7;
        pdr.max_settable.value_u32 = 20;
        uint32_t value = 6;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "uint16_t");
        EXPECT_EQ(rc, PLDM_ERROR_INVALID_DATA);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<uint32_t>(dbusValue.value()), 6u);
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_SINT32);
        pdr.min_settable.value_s32 = -2;
        pdr.max_settable.value_s32 = 2;
        int32_t value = -3;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "int32_t");
        EXPECT_EQ(rc, PLDM_ERROR_INVALID_DATA);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<int32_t>(dbusValue.value()), value);
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_UINT64);
        pdr.min_settable.value_u64 = 2;
        pdr.max_settable.value_u64 = 10;
        uint64_t value = 1;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "uint64_t");
        EXPECT_EQ(rc, PLDM_ERROR_INVALID_DATA);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<uint64_t>(dbusValue.value()), 1u);
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_SINT64);
        pdr.min_settable.value_s64 = -3;
        pdr.max_settable.value_s64 = 3;
        int64_t value = -4;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "int64_t");
        EXPECT_EQ(rc, PLDM_ERROR_INVALID_DATA);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<int64_t>(dbusValue.value()), value);
    }
}

TEST(getEffecterRawValue, upperBoundCoverageAcrossTypes)
{
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_SINT8);
        pdr.min_settable.value_s8 = -4;
        pdr.max_settable.value_s8 = 4;
        int8_t value = 5;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "int8_t");
        EXPECT_EQ(rc, PLDM_ERROR_INVALID_DATA);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_TRUE(isNumericProperty(dbusValue.value()));
        EXPECT_EQ(numericProperty(dbusValue.value()),
                  static_cast<long double>(value));
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_UINT16);
        pdr.min_settable.value_u16 = 2;
        pdr.max_settable.value_u16 = 10;
        uint16_t value = 11;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "uint16_t");
        EXPECT_EQ(rc, PLDM_ERROR_INVALID_DATA);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<uint16_t>(dbusValue.value()), value);
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_SINT16);
        pdr.min_settable.value_s16 = -3;
        pdr.max_settable.value_s16 = 3;
        int16_t value = 4;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "int16_t");
        EXPECT_EQ(rc, PLDM_ERROR_INVALID_DATA);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<int16_t>(dbusValue.value()), value);
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_UINT32);
        pdr.min_settable.value_u32 = 7;
        pdr.max_settable.value_u32 = 20;
        uint32_t value = 21;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "uint32_t");
        EXPECT_EQ(rc, PLDM_ERROR_INVALID_DATA);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<uint32_t>(dbusValue.value()), value);
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_SINT32);
        pdr.min_settable.value_s32 = -2;
        pdr.max_settable.value_s32 = 2;
        int32_t value = 3;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "int32_t");
        EXPECT_EQ(rc, PLDM_ERROR_INVALID_DATA);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<int32_t>(dbusValue.value()), value);
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_UINT64);
        pdr.min_settable.value_u64 = 2;
        pdr.max_settable.value_u64 = 10;
        uint64_t value = 11;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "uint64_t");
        EXPECT_EQ(rc, PLDM_ERROR_INVALID_DATA);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<uint64_t>(dbusValue.value()), value);
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_SINT64);
        pdr.min_settable.value_s64 = -3;
        pdr.max_settable.value_s64 = 3;
        int64_t value = 4;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "int64_t");
        EXPECT_EQ(rc, PLDM_ERROR_INVALID_DATA);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<int64_t>(dbusValue.value()), value);
    }
    {
        pldm_numeric_effecter_value_pdr pdr{};
        pdr.effecter_data_size = 0xFF;
        uint8_t value = 1;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "uint8_t");
        EXPECT_EQ(rc, PLDM_SUCCESS);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_FALSE(std::get<bool>(dbusValue.value()));
    }
}

TEST(getEffecterRawValue, disabledBoundsShortCircuitCoverage)
{
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_SINT8);
        pdr.min_settable.value_s8 = 4;
        pdr.max_settable.value_s8 = 4;
        int8_t value = 100;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "int8_t");
        EXPECT_EQ(rc, PLDM_SUCCESS);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_TRUE(isNumericProperty(dbusValue.value()));
        EXPECT_EQ(numericProperty(dbusValue.value()),
                  static_cast<long double>(value));
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_UINT16);
        pdr.min_settable.value_u16 = 20;
        pdr.max_settable.value_u16 = 20;
        uint16_t value = 1;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "uint16_t");
        EXPECT_EQ(rc, PLDM_SUCCESS);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<uint16_t>(dbusValue.value()), value);
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_SINT16);
        pdr.min_settable.value_s16 = 3;
        pdr.max_settable.value_s16 = 3;
        int16_t value = -9;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "int16_t");
        EXPECT_EQ(rc, PLDM_SUCCESS);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_TRUE(isNumericProperty(dbusValue.value()));
        EXPECT_EQ(numericProperty(dbusValue.value()),
                  static_cast<long double>(value));
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_UINT32);
        pdr.min_settable.value_u32 = 9;
        pdr.max_settable.value_u32 = 9;
        uint32_t value = 1;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "uint32_t");
        EXPECT_EQ(rc, PLDM_SUCCESS);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<uint32_t>(dbusValue.value()), value);
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_SINT32);
        pdr.min_settable.value_s32 = 2;
        pdr.max_settable.value_s32 = 2;
        int32_t value = -99;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "int32_t");
        EXPECT_EQ(rc, PLDM_SUCCESS);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_TRUE(isNumericProperty(dbusValue.value()));
        EXPECT_EQ(numericProperty(dbusValue.value()),
                  static_cast<long double>(value));
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_UINT64);
        pdr.min_settable.value_u64 = 5;
        pdr.max_settable.value_u64 = 5;
        uint64_t value = 1;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "uint64_t");
        EXPECT_EQ(rc, PLDM_SUCCESS);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<uint64_t>(dbusValue.value()), value);
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_SINT64);
        pdr.min_settable.value_s64 = 7;
        pdr.max_settable.value_s64 = 7;
        int64_t value = -1000;
        auto [rc, dbusValue] = platform_numeric_effecter::getEffecterRawValue(
            &pdr, value, "int64_t");
        EXPECT_EQ(rc, PLDM_SUCCESS);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_TRUE(isNumericProperty(dbusValue.value()));
        EXPECT_EQ(numericProperty(dbusValue.value()),
                  static_cast<long double>(value));
    }
}

TEST(getEffecterRawValue, successMatrixAcrossUnsignedTemplateInstantiations)
{
    expectRawValueSuccessMatrix<uint8_t>();
    expectRawValueSuccessMatrix<uint16_t>();
    expectRawValueSuccessMatrix<uint32_t>();
    expectRawValueSuccessMatrix<uint64_t>();
}

TEST(getEffecterRawValue, successMatrixAcrossSignedTemplateInstantiations)
{
    expectRawValueSuccessMatrix<int8_t>();
    expectRawValueSuccessMatrix<int16_t>();
    expectRawValueSuccessMatrix<int32_t>();
    expectRawValueSuccessMatrix<int64_t>();
}

TEST(getEffecterRawValue, upperBoundMatrixAcrossUnsignedTemplateInstantiations)
{
    expectRawValueUpperBoundMatrix<uint8_t>();
    expectRawValueUpperBoundMatrix<uint16_t>();
    expectRawValueUpperBoundMatrix<uint32_t>();
    expectRawValueUpperBoundMatrix<uint64_t>();
}

TEST(getEffecterRawValue, upperBoundMatrixAcrossSignedTemplateInstantiations)
{
    expectRawValueUpperBoundMatrix<int8_t>();
    expectRawValueUpperBoundMatrix<int16_t>();
    expectRawValueUpperBoundMatrix<int32_t>();
    expectRawValueUpperBoundMatrix<int64_t>();
}

TEST(getEffecterRawValue, disabledBoundsMatrixAcrossTemplateInstantiations)
{
    expectRawValueDisabledBoundsMatrix<uint8_t>();
    expectRawValueDisabledBoundsMatrix<uint16_t>();
    expectRawValueDisabledBoundsMatrix<uint32_t>();
    expectRawValueDisabledBoundsMatrix<uint64_t>();
    expectRawValueDisabledBoundsMatrix<int8_t>();
    expectRawValueDisabledBoundsMatrix<int16_t>();
    expectRawValueDisabledBoundsMatrix<int32_t>();
    expectRawValueDisabledBoundsMatrix<int64_t>();
}

TEST(getEffecterRawValue, invalidDataSizeAcrossTemplateInstantiations)
{
    expectInvalidRawValueDataSizeCoverage<uint8_t>();
    expectInvalidRawValueDataSizeCoverage<int8_t>();
    expectInvalidRawValueDataSizeCoverage<uint16_t>();
    expectInvalidRawValueDataSizeCoverage<int16_t>();
    expectInvalidRawValueDataSizeCoverage<uint32_t>();
    expectInvalidRawValueDataSizeCoverage<int32_t>();
    expectInvalidRawValueDataSizeCoverage<uint64_t>();
    expectInvalidRawValueDataSizeCoverage<int64_t>();
}

TEST(getEffecterRawValue, uint8OutOfRangeAcrossTemplateInstantiations)
{
    expectOutOfRangeRawValueCoverage<uint8_t>(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                              "uint32_t");
    expectOutOfRangeRawValueCoverage<int8_t>(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                             "uint32_t");
    expectOutOfRangeRawValueCoverage<uint16_t>(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                               "uint32_t");
    expectOutOfRangeRawValueCoverage<int16_t>(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                              "uint32_t");
    expectOutOfRangeRawValueCoverage<uint32_t>(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                               "uint32_t");
    expectOutOfRangeRawValueCoverage<int32_t>(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                              "uint32_t");
    expectOutOfRangeRawValueCoverage<uint64_t>(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                               "uint32_t");
    expectOutOfRangeRawValueCoverage<int64_t>(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                              "uint32_t");
}

TEST(getEffecterRawValue, uint8NoConversionAcrossTemplateInstantiations)
{
    expectNoConversionRawValueCoverage<uint8_t>(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                                "uint8_t");
    expectNoConversionRawValueCoverage<int8_t>(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                               "uint8_t");
    expectNoConversionRawValueCoverage<uint16_t>(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                                 "uint8_t");
    expectNoConversionRawValueCoverage<int16_t>(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                                "uint8_t");
    expectNoConversionRawValueCoverage<uint32_t>(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                                 "uint8_t");
    expectNoConversionRawValueCoverage<int32_t>(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                                "uint8_t");
    expectNoConversionRawValueCoverage<uint64_t>(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                                 "uint8_t");
    expectNoConversionRawValueCoverage<int64_t>(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                                "uint8_t");
}

TEST(getEffecterRawValue, sint8OutOfRangeAcrossTemplateInstantiations)
{
    expectOutOfRangeRawValueCoverage<uint8_t>(PLDM_EFFECTER_DATA_SIZE_SINT8,
                                              "int8_t");
    expectOutOfRangeRawValueCoverage<int8_t>(PLDM_EFFECTER_DATA_SIZE_SINT8,
                                             "int8_t");
    expectOutOfRangeRawValueCoverage<uint16_t>(PLDM_EFFECTER_DATA_SIZE_SINT8,
                                               "int8_t");
    expectOutOfRangeRawValueCoverage<int16_t>(PLDM_EFFECTER_DATA_SIZE_SINT8,
                                              "int8_t");
    expectOutOfRangeRawValueCoverage<uint32_t>(PLDM_EFFECTER_DATA_SIZE_SINT8,
                                               "int8_t");
    expectOutOfRangeRawValueCoverage<int32_t>(PLDM_EFFECTER_DATA_SIZE_SINT8,
                                              "int8_t");
    expectOutOfRangeRawValueCoverage<uint64_t>(PLDM_EFFECTER_DATA_SIZE_SINT8,
                                               "int8_t");
    expectOutOfRangeRawValueCoverage<int64_t>(PLDM_EFFECTER_DATA_SIZE_SINT8,
                                              "int8_t");
}

TEST(getEffecterRawValue, uint16OutOfRangeAcrossTemplateInstantiations)
{
    expectOutOfRangeRawValueCoverage<uint8_t>(PLDM_EFFECTER_DATA_SIZE_UINT16,
                                              "uint32_t");
    expectOutOfRangeRawValueCoverage<int8_t>(PLDM_EFFECTER_DATA_SIZE_UINT16,
                                             "uint32_t");
    expectOutOfRangeRawValueCoverage<uint16_t>(PLDM_EFFECTER_DATA_SIZE_UINT16,
                                               "uint32_t");
    expectOutOfRangeRawValueCoverage<int16_t>(PLDM_EFFECTER_DATA_SIZE_UINT16,
                                              "uint32_t");
    expectOutOfRangeRawValueCoverage<uint32_t>(PLDM_EFFECTER_DATA_SIZE_UINT16,
                                               "uint32_t");
    expectOutOfRangeRawValueCoverage<int32_t>(PLDM_EFFECTER_DATA_SIZE_UINT16,
                                              "uint32_t");
    expectOutOfRangeRawValueCoverage<uint64_t>(PLDM_EFFECTER_DATA_SIZE_UINT16,
                                               "uint32_t");
    expectOutOfRangeRawValueCoverage<int64_t>(PLDM_EFFECTER_DATA_SIZE_UINT16,
                                              "uint32_t");
}

TEST(getEffecterRawValue, uint16NoConversionAcrossTemplateInstantiations)
{
    expectNoConversionRawValueCoverage<uint8_t>(PLDM_EFFECTER_DATA_SIZE_UINT16,
                                                "uint16_t");
    expectNoConversionRawValueCoverage<int8_t>(PLDM_EFFECTER_DATA_SIZE_UINT16,
                                               "uint16_t");
    expectNoConversionRawValueCoverage<uint16_t>(PLDM_EFFECTER_DATA_SIZE_UINT16,
                                                 "uint16_t");
    expectNoConversionRawValueCoverage<int16_t>(PLDM_EFFECTER_DATA_SIZE_UINT16,
                                                "uint16_t");
    expectNoConversionRawValueCoverage<uint32_t>(PLDM_EFFECTER_DATA_SIZE_UINT16,
                                                 "uint16_t");
    expectNoConversionRawValueCoverage<int32_t>(PLDM_EFFECTER_DATA_SIZE_UINT16,
                                                "uint16_t");
    expectNoConversionRawValueCoverage<uint64_t>(PLDM_EFFECTER_DATA_SIZE_UINT16,
                                                 "uint16_t");
    expectNoConversionRawValueCoverage<int64_t>(PLDM_EFFECTER_DATA_SIZE_UINT16,
                                                "uint16_t");
}

TEST(getEffecterRawValue, sint16OutOfRangeAcrossTemplateInstantiations)
{
    expectOutOfRangeRawValueCoverage<uint8_t>(PLDM_EFFECTER_DATA_SIZE_SINT16,
                                              "uint32_t");
    expectOutOfRangeRawValueCoverage<int8_t>(PLDM_EFFECTER_DATA_SIZE_SINT16,
                                             "uint32_t");
    expectOutOfRangeRawValueCoverage<uint16_t>(PLDM_EFFECTER_DATA_SIZE_SINT16,
                                               "uint32_t");
    expectOutOfRangeRawValueCoverage<int16_t>(PLDM_EFFECTER_DATA_SIZE_SINT16,
                                              "uint32_t");
    expectOutOfRangeRawValueCoverage<uint32_t>(PLDM_EFFECTER_DATA_SIZE_SINT16,
                                               "uint32_t");
    expectOutOfRangeRawValueCoverage<int32_t>(PLDM_EFFECTER_DATA_SIZE_SINT16,
                                              "uint32_t");
    expectOutOfRangeRawValueCoverage<uint64_t>(PLDM_EFFECTER_DATA_SIZE_SINT16,
                                               "uint32_t");
    expectOutOfRangeRawValueCoverage<int64_t>(PLDM_EFFECTER_DATA_SIZE_SINT16,
                                              "uint32_t");
}

TEST(getEffecterRawValue, sint16NoConversionAcrossTemplateInstantiations)
{
    expectNoConversionRawValueCoverage<uint8_t>(PLDM_EFFECTER_DATA_SIZE_SINT16,
                                                "int16_t");
    expectNoConversionRawValueCoverage<int8_t>(PLDM_EFFECTER_DATA_SIZE_SINT16,
                                               "int16_t");
    expectNoConversionRawValueCoverage<uint16_t>(PLDM_EFFECTER_DATA_SIZE_SINT16,
                                                 "int16_t");
    expectNoConversionRawValueCoverage<int16_t>(PLDM_EFFECTER_DATA_SIZE_SINT16,
                                                "int16_t");
    expectNoConversionRawValueCoverage<uint32_t>(PLDM_EFFECTER_DATA_SIZE_SINT16,
                                                 "int16_t");
    expectNoConversionRawValueCoverage<int32_t>(PLDM_EFFECTER_DATA_SIZE_SINT16,
                                                "int16_t");
    expectNoConversionRawValueCoverage<uint64_t>(PLDM_EFFECTER_DATA_SIZE_SINT16,
                                                 "int16_t");
    expectNoConversionRawValueCoverage<int64_t>(PLDM_EFFECTER_DATA_SIZE_SINT16,
                                                "int16_t");
}

TEST(getEffecterRawValue, uint32OutOfRangeAcrossTemplateInstantiations)
{
    expectOutOfRangeRawValueCoverage<uint8_t>(PLDM_EFFECTER_DATA_SIZE_UINT32,
                                              "uint32_t");
    expectOutOfRangeRawValueCoverage<int8_t>(PLDM_EFFECTER_DATA_SIZE_UINT32,
                                             "uint32_t");
    expectOutOfRangeRawValueCoverage<uint16_t>(PLDM_EFFECTER_DATA_SIZE_UINT32,
                                               "uint32_t");
    expectOutOfRangeRawValueCoverage<int16_t>(PLDM_EFFECTER_DATA_SIZE_UINT32,
                                              "uint32_t");
    expectOutOfRangeRawValueCoverage<uint32_t>(PLDM_EFFECTER_DATA_SIZE_UINT32,
                                               "uint32_t");
    expectOutOfRangeRawValueCoverage<int32_t>(PLDM_EFFECTER_DATA_SIZE_UINT32,
                                              "uint32_t");
    expectOutOfRangeRawValueCoverage<uint64_t>(PLDM_EFFECTER_DATA_SIZE_UINT32,
                                               "uint32_t");
    expectOutOfRangeRawValueCoverage<int64_t>(PLDM_EFFECTER_DATA_SIZE_UINT32,
                                              "uint32_t");
}

TEST(getEffecterRawValue, uint32NoConversionAcrossTemplateInstantiations)
{
    expectNoConversionRawValueCoverage<uint8_t>(PLDM_EFFECTER_DATA_SIZE_UINT32,
                                                "uint16_t");
    expectNoConversionRawValueCoverage<int8_t>(PLDM_EFFECTER_DATA_SIZE_UINT32,
                                               "uint16_t");
    expectNoConversionRawValueCoverage<uint16_t>(PLDM_EFFECTER_DATA_SIZE_UINT32,
                                                 "uint16_t");
    expectNoConversionRawValueCoverage<int16_t>(PLDM_EFFECTER_DATA_SIZE_UINT32,
                                                "uint16_t");
    expectNoConversionRawValueCoverage<uint32_t>(PLDM_EFFECTER_DATA_SIZE_UINT32,
                                                 "uint16_t");
    expectNoConversionRawValueCoverage<int32_t>(PLDM_EFFECTER_DATA_SIZE_UINT32,
                                                "uint16_t");
    expectNoConversionRawValueCoverage<uint64_t>(PLDM_EFFECTER_DATA_SIZE_UINT32,
                                                 "uint16_t");
    expectNoConversionRawValueCoverage<int64_t>(PLDM_EFFECTER_DATA_SIZE_UINT32,
                                                "uint16_t");
}

TEST(getEffecterRawValue, sint32OutOfRangeAcrossTemplateInstantiations)
{
    expectOutOfRangeRawValueCoverage<uint8_t>(PLDM_EFFECTER_DATA_SIZE_SINT32,
                                              "uint32_t");
    expectOutOfRangeRawValueCoverage<int8_t>(PLDM_EFFECTER_DATA_SIZE_SINT32,
                                             "uint32_t");
    expectOutOfRangeRawValueCoverage<uint16_t>(PLDM_EFFECTER_DATA_SIZE_SINT32,
                                               "uint32_t");
    expectOutOfRangeRawValueCoverage<int16_t>(PLDM_EFFECTER_DATA_SIZE_SINT32,
                                              "uint32_t");
    expectOutOfRangeRawValueCoverage<uint32_t>(PLDM_EFFECTER_DATA_SIZE_SINT32,
                                               "uint32_t");
    expectOutOfRangeRawValueCoverage<int32_t>(PLDM_EFFECTER_DATA_SIZE_SINT32,
                                              "uint32_t");
    expectOutOfRangeRawValueCoverage<uint64_t>(PLDM_EFFECTER_DATA_SIZE_SINT32,
                                               "uint32_t");
    expectOutOfRangeRawValueCoverage<int64_t>(PLDM_EFFECTER_DATA_SIZE_SINT32,
                                              "uint32_t");
}

TEST(getEffecterRawValue, sint32NoConversionAcrossTemplateInstantiations)
{
    expectNoConversionRawValueCoverage<uint8_t>(PLDM_EFFECTER_DATA_SIZE_SINT32,
                                                "int32_t");
    expectNoConversionRawValueCoverage<int8_t>(PLDM_EFFECTER_DATA_SIZE_SINT32,
                                               "int32_t");
    expectNoConversionRawValueCoverage<uint16_t>(PLDM_EFFECTER_DATA_SIZE_SINT32,
                                                 "int32_t");
    expectNoConversionRawValueCoverage<int16_t>(PLDM_EFFECTER_DATA_SIZE_SINT32,
                                                "int32_t");
    expectNoConversionRawValueCoverage<uint32_t>(PLDM_EFFECTER_DATA_SIZE_SINT32,
                                                 "int32_t");
    expectNoConversionRawValueCoverage<int32_t>(PLDM_EFFECTER_DATA_SIZE_SINT32,
                                                "int32_t");
    expectNoConversionRawValueCoverage<uint64_t>(PLDM_EFFECTER_DATA_SIZE_SINT32,
                                                 "int32_t");
    expectNoConversionRawValueCoverage<int64_t>(PLDM_EFFECTER_DATA_SIZE_SINT32,
                                                "int32_t");
}

TEST(getEffecterRawValue, uint64OutOfRangeAcrossTemplateInstantiations)
{
    expectOutOfRangeRawValueCoverage<uint8_t>(PLDM_EFFECTER_DATA_SIZE_UINT64,
                                              "uint64_t");
    expectOutOfRangeRawValueCoverage<int8_t>(PLDM_EFFECTER_DATA_SIZE_UINT64,
                                             "uint64_t");
    expectOutOfRangeRawValueCoverage<uint16_t>(PLDM_EFFECTER_DATA_SIZE_UINT64,
                                               "uint64_t");
    expectOutOfRangeRawValueCoverage<int16_t>(PLDM_EFFECTER_DATA_SIZE_UINT64,
                                              "uint64_t");
    expectOutOfRangeRawValueCoverage<uint32_t>(PLDM_EFFECTER_DATA_SIZE_UINT64,
                                               "uint64_t");
    expectOutOfRangeRawValueCoverage<int32_t>(PLDM_EFFECTER_DATA_SIZE_UINT64,
                                              "uint64_t");
    expectOutOfRangeRawValueCoverage<uint64_t>(PLDM_EFFECTER_DATA_SIZE_UINT64,
                                               "uint64_t");
    expectOutOfRangeRawValueCoverage<int64_t>(PLDM_EFFECTER_DATA_SIZE_UINT64,
                                              "uint64_t");
}

TEST(getEffecterRawValue, sint64OutOfRangeAcrossTemplateInstantiations)
{
    expectOutOfRangeRawValueCoverage<uint8_t>(PLDM_EFFECTER_DATA_SIZE_SINT64,
                                              "int64_t");
    expectOutOfRangeRawValueCoverage<int8_t>(PLDM_EFFECTER_DATA_SIZE_SINT64,
                                             "int64_t");
    expectOutOfRangeRawValueCoverage<uint16_t>(PLDM_EFFECTER_DATA_SIZE_SINT64,
                                               "int64_t");
    expectOutOfRangeRawValueCoverage<int16_t>(PLDM_EFFECTER_DATA_SIZE_SINT64,
                                              "int64_t");
    expectOutOfRangeRawValueCoverage<uint32_t>(PLDM_EFFECTER_DATA_SIZE_SINT64,
                                               "int64_t");
    expectOutOfRangeRawValueCoverage<int32_t>(PLDM_EFFECTER_DATA_SIZE_SINT64,
                                              "int64_t");
    expectOutOfRangeRawValueCoverage<uint64_t>(PLDM_EFFECTER_DATA_SIZE_SINT64,
                                               "int64_t");
    expectOutOfRangeRawValueCoverage<int64_t>(PLDM_EFFECTER_DATA_SIZE_SINT64,
                                              "int64_t");
}

TEST(getEffecterRawValue, uint8LowerBoundAcrossTemplateInstantiations)
{
    expectLowerBoundRawValueCoverage<uint8_t>(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                              "uint32_t");
    expectLowerBoundRawValueCoverage<int8_t>(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                             "uint32_t");
    expectLowerBoundRawValueCoverage<uint16_t>(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                               "uint32_t");
    expectLowerBoundRawValueCoverage<int16_t>(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                              "uint32_t");
    expectLowerBoundRawValueCoverage<uint32_t>(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                               "uint32_t");
    expectLowerBoundRawValueCoverage<int32_t>(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                              "uint32_t");
    expectLowerBoundRawValueCoverage<uint64_t>(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                               "uint32_t");
    expectLowerBoundRawValueCoverage<int64_t>(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                              "uint32_t");
}

TEST(getEffecterRawValue, sint8LowerBoundAcrossTemplateInstantiations)
{
    expectLowerBoundRawValueCoverage<uint8_t>(PLDM_EFFECTER_DATA_SIZE_SINT8,
                                              "int8_t");
    expectLowerBoundRawValueCoverage<int8_t>(PLDM_EFFECTER_DATA_SIZE_SINT8,
                                             "int8_t");
    expectLowerBoundRawValueCoverage<uint16_t>(PLDM_EFFECTER_DATA_SIZE_SINT8,
                                               "int8_t");
    expectLowerBoundRawValueCoverage<int16_t>(PLDM_EFFECTER_DATA_SIZE_SINT8,
                                              "int8_t");
    expectLowerBoundRawValueCoverage<uint32_t>(PLDM_EFFECTER_DATA_SIZE_SINT8,
                                               "int8_t");
    expectLowerBoundRawValueCoverage<int32_t>(PLDM_EFFECTER_DATA_SIZE_SINT8,
                                              "int8_t");
    expectLowerBoundRawValueCoverage<uint64_t>(PLDM_EFFECTER_DATA_SIZE_SINT8,
                                               "int8_t");
    expectLowerBoundRawValueCoverage<int64_t>(PLDM_EFFECTER_DATA_SIZE_SINT8,
                                              "int8_t");
}

TEST(getEffecterRawValue, uint16LowerBoundAcrossTemplateInstantiations)
{
    expectLowerBoundRawValueCoverage<uint8_t>(PLDM_EFFECTER_DATA_SIZE_UINT16,
                                              "uint64_t");
    expectLowerBoundRawValueCoverage<int8_t>(PLDM_EFFECTER_DATA_SIZE_UINT16,
                                             "uint64_t");
    expectLowerBoundRawValueCoverage<uint16_t>(PLDM_EFFECTER_DATA_SIZE_UINT16,
                                               "uint64_t");
    expectLowerBoundRawValueCoverage<int16_t>(PLDM_EFFECTER_DATA_SIZE_UINT16,
                                              "uint64_t");
    expectLowerBoundRawValueCoverage<uint32_t>(PLDM_EFFECTER_DATA_SIZE_UINT16,
                                               "uint64_t");
    expectLowerBoundRawValueCoverage<int32_t>(PLDM_EFFECTER_DATA_SIZE_UINT16,
                                              "uint64_t");
    expectLowerBoundRawValueCoverage<uint64_t>(PLDM_EFFECTER_DATA_SIZE_UINT16,
                                               "uint64_t");
    expectLowerBoundRawValueCoverage<int64_t>(PLDM_EFFECTER_DATA_SIZE_UINT16,
                                              "uint64_t");
}

TEST(getEffecterRawValue, sint16LowerBoundAcrossTemplateInstantiations)
{
    expectLowerBoundRawValueCoverage<uint8_t>(PLDM_EFFECTER_DATA_SIZE_SINT16,
                                              "uint64_t");
    expectLowerBoundRawValueCoverage<int8_t>(PLDM_EFFECTER_DATA_SIZE_SINT16,
                                             "uint64_t");
    expectLowerBoundRawValueCoverage<uint16_t>(PLDM_EFFECTER_DATA_SIZE_SINT16,
                                               "uint64_t");
    expectLowerBoundRawValueCoverage<int16_t>(PLDM_EFFECTER_DATA_SIZE_SINT16,
                                              "uint64_t");
    expectLowerBoundRawValueCoverage<uint32_t>(PLDM_EFFECTER_DATA_SIZE_SINT16,
                                               "uint64_t");
    expectLowerBoundRawValueCoverage<int32_t>(PLDM_EFFECTER_DATA_SIZE_SINT16,
                                              "uint64_t");
    expectLowerBoundRawValueCoverage<uint64_t>(PLDM_EFFECTER_DATA_SIZE_SINT16,
                                               "uint64_t");
    expectLowerBoundRawValueCoverage<int64_t>(PLDM_EFFECTER_DATA_SIZE_SINT16,
                                              "uint64_t");
}

TEST(getEffecterRawValue, uint32LowerBoundAcrossTemplateInstantiations)
{
    expectLowerBoundRawValueCoverage<uint8_t>(PLDM_EFFECTER_DATA_SIZE_UINT32,
                                              "uint64_t");
    expectLowerBoundRawValueCoverage<int8_t>(PLDM_EFFECTER_DATA_SIZE_UINT32,
                                             "uint64_t");
    expectLowerBoundRawValueCoverage<uint16_t>(PLDM_EFFECTER_DATA_SIZE_UINT32,
                                               "uint64_t");
    expectLowerBoundRawValueCoverage<int16_t>(PLDM_EFFECTER_DATA_SIZE_UINT32,
                                              "uint64_t");
    expectLowerBoundRawValueCoverage<uint32_t>(PLDM_EFFECTER_DATA_SIZE_UINT32,
                                               "uint64_t");
    expectLowerBoundRawValueCoverage<int32_t>(PLDM_EFFECTER_DATA_SIZE_UINT32,
                                              "uint64_t");
    expectLowerBoundRawValueCoverage<uint64_t>(PLDM_EFFECTER_DATA_SIZE_UINT32,
                                               "uint64_t");
    expectLowerBoundRawValueCoverage<int64_t>(PLDM_EFFECTER_DATA_SIZE_UINT32,
                                              "uint64_t");
}

TEST(getEffecterRawValue, sint32LowerBoundAcrossTemplateInstantiations)
{
    expectLowerBoundRawValueCoverage<uint8_t>(PLDM_EFFECTER_DATA_SIZE_SINT32,
                                              "uint64_t");
    expectLowerBoundRawValueCoverage<int8_t>(PLDM_EFFECTER_DATA_SIZE_SINT32,
                                             "uint64_t");
    expectLowerBoundRawValueCoverage<uint16_t>(PLDM_EFFECTER_DATA_SIZE_SINT32,
                                               "uint64_t");
    expectLowerBoundRawValueCoverage<int16_t>(PLDM_EFFECTER_DATA_SIZE_SINT32,
                                              "uint64_t");
    expectLowerBoundRawValueCoverage<uint32_t>(PLDM_EFFECTER_DATA_SIZE_SINT32,
                                               "uint64_t");
    expectLowerBoundRawValueCoverage<int32_t>(PLDM_EFFECTER_DATA_SIZE_SINT32,
                                              "uint64_t");
    expectLowerBoundRawValueCoverage<uint64_t>(PLDM_EFFECTER_DATA_SIZE_SINT32,
                                               "uint64_t");
    expectLowerBoundRawValueCoverage<int64_t>(PLDM_EFFECTER_DATA_SIZE_SINT32,
                                              "uint64_t");
}

TEST(getEffecterRawValue, uint64LowerBoundAcrossTemplateInstantiations)
{
    expectLowerBoundRawValueCoverage<uint8_t>(PLDM_EFFECTER_DATA_SIZE_UINT64,
                                              "uint64_t");
    expectLowerBoundRawValueCoverage<int8_t>(PLDM_EFFECTER_DATA_SIZE_UINT64,
                                             "uint64_t");
    expectLowerBoundRawValueCoverage<uint16_t>(PLDM_EFFECTER_DATA_SIZE_UINT64,
                                               "uint64_t");
    expectLowerBoundRawValueCoverage<int16_t>(PLDM_EFFECTER_DATA_SIZE_UINT64,
                                              "uint64_t");
    expectLowerBoundRawValueCoverage<uint32_t>(PLDM_EFFECTER_DATA_SIZE_UINT64,
                                               "uint64_t");
    expectLowerBoundRawValueCoverage<int32_t>(PLDM_EFFECTER_DATA_SIZE_UINT64,
                                              "uint64_t");
    expectLowerBoundRawValueCoverage<uint64_t>(PLDM_EFFECTER_DATA_SIZE_UINT64,
                                               "uint64_t");
    expectLowerBoundRawValueCoverage<int64_t>(PLDM_EFFECTER_DATA_SIZE_UINT64,
                                              "uint64_t");
}

TEST(getEffecterRawValue, sint64LowerBoundAcrossTemplateInstantiations)
{
    expectLowerBoundRawValueCoverage<uint8_t>(PLDM_EFFECTER_DATA_SIZE_SINT64,
                                              "int64_t");
    expectLowerBoundRawValueCoverage<int8_t>(PLDM_EFFECTER_DATA_SIZE_SINT64,
                                             "int64_t");
    expectLowerBoundRawValueCoverage<uint16_t>(PLDM_EFFECTER_DATA_SIZE_SINT64,
                                               "int64_t");
    expectLowerBoundRawValueCoverage<int16_t>(PLDM_EFFECTER_DATA_SIZE_SINT64,
                                              "int64_t");
    expectLowerBoundRawValueCoverage<uint32_t>(PLDM_EFFECTER_DATA_SIZE_SINT64,
                                               "int64_t");
    expectLowerBoundRawValueCoverage<int32_t>(PLDM_EFFECTER_DATA_SIZE_SINT64,
                                              "int64_t");
    expectLowerBoundRawValueCoverage<uint64_t>(PLDM_EFFECTER_DATA_SIZE_SINT64,
                                               "int64_t");
    expectLowerBoundRawValueCoverage<int64_t>(PLDM_EFFECTER_DATA_SIZE_SINT64,
                                              "int64_t");
}

TEST(getEffecterRawValue, uint16Uint64ConversionAcrossTemplateInstantiations)
{
    expectUint64ConversionRawValueCoverage<uint8_t>(
        PLDM_EFFECTER_DATA_SIZE_UINT16);
    expectUint64ConversionRawValueCoverage<int8_t>(
        PLDM_EFFECTER_DATA_SIZE_UINT16);
    expectUint64ConversionRawValueCoverage<uint16_t>(
        PLDM_EFFECTER_DATA_SIZE_UINT16);
    expectUint64ConversionRawValueCoverage<int16_t>(
        PLDM_EFFECTER_DATA_SIZE_UINT16);
    expectUint64ConversionRawValueCoverage<uint32_t>(
        PLDM_EFFECTER_DATA_SIZE_UINT16);
    expectUint64ConversionRawValueCoverage<int32_t>(
        PLDM_EFFECTER_DATA_SIZE_UINT16);
    expectUint64ConversionRawValueCoverage<uint64_t>(
        PLDM_EFFECTER_DATA_SIZE_UINT16);
    expectUint64ConversionRawValueCoverage<int64_t>(
        PLDM_EFFECTER_DATA_SIZE_UINT16);
}

TEST(getEffecterRawValue, sint16Uint64ConversionAcrossTemplateInstantiations)
{
    expectUint64ConversionRawValueCoverage<uint8_t>(
        PLDM_EFFECTER_DATA_SIZE_SINT16);
    expectUint64ConversionRawValueCoverage<int8_t>(
        PLDM_EFFECTER_DATA_SIZE_SINT16);
    expectUint64ConversionRawValueCoverage<uint16_t>(
        PLDM_EFFECTER_DATA_SIZE_SINT16);
    expectUint64ConversionRawValueCoverage<int16_t>(
        PLDM_EFFECTER_DATA_SIZE_SINT16);
    expectUint64ConversionRawValueCoverage<uint32_t>(
        PLDM_EFFECTER_DATA_SIZE_SINT16);
    expectUint64ConversionRawValueCoverage<int32_t>(
        PLDM_EFFECTER_DATA_SIZE_SINT16);
    expectUint64ConversionRawValueCoverage<uint64_t>(
        PLDM_EFFECTER_DATA_SIZE_SINT16);
    expectUint64ConversionRawValueCoverage<int64_t>(
        PLDM_EFFECTER_DATA_SIZE_SINT16);
}

TEST(getEffecterRawValue, uint32Uint64ConversionAcrossTemplateInstantiations)
{
    expectUint64ConversionRawValueCoverage<uint8_t>(
        PLDM_EFFECTER_DATA_SIZE_UINT32);
    expectUint64ConversionRawValueCoverage<int8_t>(
        PLDM_EFFECTER_DATA_SIZE_UINT32);
    expectUint64ConversionRawValueCoverage<uint16_t>(
        PLDM_EFFECTER_DATA_SIZE_UINT32);
    expectUint64ConversionRawValueCoverage<int16_t>(
        PLDM_EFFECTER_DATA_SIZE_UINT32);
    expectUint64ConversionRawValueCoverage<uint32_t>(
        PLDM_EFFECTER_DATA_SIZE_UINT32);
    expectUint64ConversionRawValueCoverage<int32_t>(
        PLDM_EFFECTER_DATA_SIZE_UINT32);
    expectUint64ConversionRawValueCoverage<uint64_t>(
        PLDM_EFFECTER_DATA_SIZE_UINT32);
    expectUint64ConversionRawValueCoverage<int64_t>(
        PLDM_EFFECTER_DATA_SIZE_UINT32);
}

TEST(getEffecterRawValue, sint32Uint64ConversionAcrossTemplateInstantiations)
{
    expectUint64ConversionRawValueCoverage<uint8_t>(
        PLDM_EFFECTER_DATA_SIZE_SINT32);
    expectUint64ConversionRawValueCoverage<int8_t>(
        PLDM_EFFECTER_DATA_SIZE_SINT32);
    expectUint64ConversionRawValueCoverage<uint16_t>(
        PLDM_EFFECTER_DATA_SIZE_SINT32);
    expectUint64ConversionRawValueCoverage<int16_t>(
        PLDM_EFFECTER_DATA_SIZE_SINT32);
    expectUint64ConversionRawValueCoverage<uint32_t>(
        PLDM_EFFECTER_DATA_SIZE_SINT32);
    expectUint64ConversionRawValueCoverage<int32_t>(
        PLDM_EFFECTER_DATA_SIZE_SINT32);
    expectUint64ConversionRawValueCoverage<uint64_t>(
        PLDM_EFFECTER_DATA_SIZE_SINT32);
    expectUint64ConversionRawValueCoverage<int64_t>(
        PLDM_EFFECTER_DATA_SIZE_SINT32);
}

TEST(setNumericEffecterValueHandler, dbusFailureCoverage)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(StrEq("/foo/bar"), _))
        .Times(5)
        .WillRepeatedly(Return("foo.bar"));

    auto inPDRRepo = pldm_pdr_init();
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_effecter/good", inPDRRepo,
                    nullptr, nullptr, nullptr, nullptr, event);

    uint16_t effecterId = 3;
    uint32_t effecterValue = 1234;
    DBusMapping dbusMapping{"/foo/bar", "xyz.openbmc_project.Foo.Bar",
                            "propertyName", "uint64_t"};

    EXPECT_CALL(mockedUtils,
                setDbusProperty(dbusMapping, PropertyValue{uint64_t{1234}}))
        .WillOnce(Throw(std::runtime_error("dbus failure")));

    auto rc = platform_numeric_effecter::setNumericEffecterValueHandler<
        MockdBusHandler, Handler>(
        mockedUtils, handler, effecterId, PLDM_EFFECTER_DATA_SIZE_UINT32,
        reinterpret_cast<uint8_t*>(&effecterValue), 4);
    EXPECT_EQ(rc, PLDM_ERROR);

    pldm_pdr_destroy(inPDRRepo);
}

TEST(parseStateSensor, allScenarios)
{
    // Sample state sensor with SensorID - 1, EntityType - Processor Module(67)
    // State Set ID - Operational Running Status(11), Supported States - 3,4
    std::vector<uint8_t> sample1PDR{
        0x00, 0x00, 0x00, 0x00, 0x01, 0x04, 0x00, 0x00, 0x17,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x43, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x0b, 0x00, 0x01, 0x18};

    const auto& [terminusHandle1, sensorID1, sensorInfo1] =
        parseStateSensorPDR(sample1PDR);
    const auto& [containerID1, entityType1, entityInstance1] =
        std::get<0>(sensorInfo1);
    const auto& states1 = std::get<1>(sensorInfo1);
    CompositeSensorStates statesCmp1{{3u, 4u}};

    ASSERT_EQ(le16toh(terminusHandle1), 0u);
    ASSERT_EQ(le16toh(sensorID1), 1u);
    ASSERT_EQ(le16toh(containerID1), 0u);
    ASSERT_EQ(le16toh(entityType1), 67u);
    ASSERT_EQ(le16toh(entityInstance1), 1u);
    ASSERT_EQ(states1, statesCmp1);

    // Sample state sensor with SensorID - 2, EntityType - System Firmware(31)
    // State Set ID - Availability(2), Supported States - 3,4,9,10,11,13
    std::vector<uint8_t> sample2PDR{
        0x00, 0x00, 0x00, 0x00, 0x01, 0x04, 0x00, 0x00, 0x17, 0x00,
        0x00, 0x00, 0x02, 0x00, 0x1F, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x01, 0x02, 0x00, 0x02, 0x18, 0x2E};

    const auto& [terminusHandle2, sensorID2, sensorInfo2] =
        parseStateSensorPDR(sample2PDR);
    const auto& [containerID2, entityType2, entityInstance2] =
        std::get<0>(sensorInfo2);
    const auto& states2 = std::get<1>(sensorInfo2);
    CompositeSensorStates statesCmp2{{3u, 4u, 9u, 10u, 11u, 13u}};

    ASSERT_EQ(le16toh(terminusHandle2), 0u);
    ASSERT_EQ(le16toh(sensorID2), 2u);
    ASSERT_EQ(le16toh(containerID2), 0u);
    ASSERT_EQ(le16toh(entityType2), 31u);
    ASSERT_EQ(le16toh(entityInstance2), 1u);
    ASSERT_EQ(states2, statesCmp2);

    // Sample state sensor with SensorID - 3, EntityType - Virtual Machine
    // Manager(33), Composite State Sensor -2 , State Set ID - Link State(33),
    // Supported States - 1,2, State Set ID - Configuration State(15),
    // Supported States - 1,2,3,4
    std::vector<uint8_t> sample3PDR{
        0x00, 0x00, 0x00, 0x00, 0x01, 0x04, 0x00, 0x00, 0x17, 0x00, 0x00,
        0x00, 0x03, 0x00, 0x21, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x02, 0x21, 0x00, 0x01, 0x06, 0x0F, 0x00, 0x01, 0x1E};

    const auto& [terminusHandle3, sensorID3, sensorInfo3] =
        parseStateSensorPDR(sample3PDR);
    const auto& [containerID3, entityType3, entityInstance3] =
        std::get<0>(sensorInfo3);
    const auto& states3 = std::get<1>(sensorInfo3);
    CompositeSensorStates statesCmp3{{1u, 2u}, {1u, 2u, 3u, 4u}};

    ASSERT_EQ(le16toh(terminusHandle3), 0u);
    ASSERT_EQ(le16toh(sensorID3), 3u);
    ASSERT_EQ(le16toh(containerID3), 1u);
    ASSERT_EQ(le16toh(entityType3), 33u);
    ASSERT_EQ(le16toh(entityInstance3), 2u);
    ASSERT_EQ(states3, statesCmp3);
}

TEST(StateSensorHandler, allScenarios)
{
    using namespace pldm::responder::events;

    StateSensorHandler handler{"./event_jsons/good"};
    constexpr uint8_t eventState0 = 0;
    constexpr uint8_t eventState1 = 1;
    constexpr uint8_t eventState2 = 2;
    constexpr uint8_t eventState3 = 3;

    // Event Entry 1
    {
        StateSensorEntry entry{1, 64, 1, 0, 0, false};
        const auto& [dbusMapping, eventStateMap] = handler.getEventInfo(entry);
        DBusMapping mapping{"/xyz/abc/def",
                            "xyz.openbmc_project.example1.value", "value1",
                            "string"};
        ASSERT_EQ(mapping == dbusMapping, true);

        const auto& propValue0 = eventStateMap.at(eventState0);
        const auto& propValue1 = eventStateMap.at(eventState1);
        const auto& propValue2 = eventStateMap.at(eventState2);
        PropertyValue value0{std::in_place_type<std::string>,
                             "xyz.openbmc_project.State.Normal"};
        PropertyValue value1{std::in_place_type<std::string>,
                             "xyz.openbmc_project.State.Critical"};
        PropertyValue value2{std::in_place_type<std::string>,
                             "xyz.openbmc_project.State.Fatal"};
        ASSERT_EQ(value0 == propValue0, true);
        ASSERT_EQ(value1 == propValue1, true);
        ASSERT_EQ(value2 == propValue2, true);
    }

    // Event Entry 2
    {
        StateSensorEntry entry{1, 64, 1, 1, 0, false};
        const auto& [dbusMapping, eventStateMap] = handler.getEventInfo(entry);
        DBusMapping mapping{"/xyz/abc/def",
                            "xyz.openbmc_project.example2.value", "value2",
                            "uint8_t"};
        ASSERT_EQ(mapping == dbusMapping, true);

        const auto& propValue0 = eventStateMap.at(eventState2);
        const auto& propValue1 = eventStateMap.at(eventState3);
        PropertyValue value0{std::in_place_type<uint8_t>, 9};
        PropertyValue value1{std::in_place_type<uint8_t>, 10};
        ASSERT_EQ(value0 == propValue0, true);
        ASSERT_EQ(value1 == propValue1, true);
    }

    // Event Entry 3
    {
        StateSensorEntry entry{2, 67, 2, 0, 0, false};
        const auto& [dbusMapping, eventStateMap] = handler.getEventInfo(entry);
        DBusMapping mapping{"/xyz/abc/ghi",
                            "xyz.openbmc_project.example3.value", "value3",
                            "bool"};
        ASSERT_EQ(mapping == dbusMapping, true);

        const auto& propValue0 = eventStateMap.at(eventState0);
        const auto& propValue1 = eventStateMap.at(eventState1);
        PropertyValue value0{std::in_place_type<bool>, false};
        PropertyValue value1{std::in_place_type<bool>, true};
        ASSERT_EQ(value0 == propValue0, true);
        ASSERT_EQ(value1 == propValue1, true);
    }

    // Invalid Entry
    {
        StateSensorEntry entry{0, 0, 0, 0, 0, false};
        ASSERT_THROW(handler.getEventInfo(entry), std::out_of_range);
    }
}

TEST(StateSensorHandler, eventActionCoveragePaths)
{
    using namespace pldm::responder::events;
    StateSensorHandler handler{"./event_jsons/good"};
    StateSensorEntry entry{1, 64, 1, 0, 0, false};

    EXPECT_EQ(handler.eventAction(entry, 0xFF), PLDM_ERROR_INVALID_DATA);

    auto rc = handler.eventAction(entry, 0);
    EXPECT_TRUE(rc == PLDM_SUCCESS || rc == PLDM_ERROR);

    StateSensorEntry missingEntry{0, 0, 0, 0, 0, false};
    EXPECT_EQ(handler.eventAction(missingEntry, 0), PLDM_SUCCESS);
}

TEST(StateSensorHandler, invalidConfigCoveragePaths)
{
    namespace fs = std::filesystem;
    using namespace pldm::responder::events;

    auto tmpDir = fs::temp_directory_path() /
                  "pldm_event_json_invalid_config_test";
    fs::create_directories(tmpDir);
    std::ofstream(tmpDir / "broken.json") << "{invalid";

    StateSensorHandler badJsonHandler{tmpDir.string()};
    StateSensorEntry entry{1, 1, 1, 1, 1, false};
    EXPECT_EQ(badJsonHandler.eventAction(entry, 1), PLDM_SUCCESS);

    fs::remove_all(tmpDir);

    StateSensorHandler missingDirHandler{tmpDir.string()};
    EXPECT_EQ(missingDirHandler.eventAction(entry, 1), PLDM_SUCCESS);
}

TEST(StateSensorHandler, malformedEntryCoveragePaths)
{
    namespace fs = std::filesystem;
    using namespace pldm::responder::events;

    auto tmpDir = fs::temp_directory_path() /
                  "pldm_event_json_malformed_entry_test";
    fs::remove_all(tmpDir);
    fs::create_directories(tmpDir);

    std::ofstream(tmpDir / "invalid_dbus.json") << R"({
            "entries": [
                {
                    "containerID": 1,
                    "entityType": 64,
                    "entityInstance": 1,
                    "sensorOffset": 0,
                    "event_states": [0],
                    "dbus": {
                        "object_path": "/xyz/openbmc_project/example/path0",
                        "interface": "xyz.openbmc_project.example0",
                        "property_name": "ExampleProperty0",
                        "property_type": "not_a_supported_type",
                        "property_values": ["value0"]
                    }
                }
            ]
        })";

    std::ofstream(tmpDir / "invalid_event_state_sizes.json") << R"({
            "entries": [
                {
                    "containerID": 1,
                    "entityType": 64,
                    "entityInstance": 1,
                    "sensorOffset": 1,
                    "event_states": [0, 1],
                    "dbus": {
                        "object_path": "/xyz/openbmc_project/example/path1",
                        "interface": "xyz.openbmc_project.example1",
                        "property_name": "ExampleProperty1",
                        "property_type": "uint8_t",
                        "property_values": [9]
                    }
                }
            ]
        })";

    StateSensorHandler malformedHandler{tmpDir.string()};
    StateSensorEntry missingEntry0{1, 64, 1, 0, 0, false};
    StateSensorEntry missingEntry1{1, 64, 1, 1, 0, false};
    EXPECT_EQ(malformedHandler.eventAction(missingEntry0, 0), PLDM_SUCCESS);
    EXPECT_EQ(malformedHandler.eventAction(missingEntry1, 0), PLDM_SUCCESS);

    fs::remove_all(tmpDir);
}

TEST(StateSensorHandler, eventActionRealDbusSuccessCoverage)
{
    PlatformDbusFixture dbusFixture;

    auto dir = makeEventConfigDir(R"({
            "entries": [
                {
                    "containerID": 1,
                    "entityType": 64,
                    "entityInstance": 1,
                    "sensorOffset": 0,
                    "event_states": [0, 1],
                    "dbus": {
                        "object_path": "/foo/bar",
                        "interface": "xyz.openbmc_project.Foo.Bar",
                        "property_name": "propertyName",
                        "property_type": "string",
                        "property_values": [
                            "xyz.openbmc_project.Foo.Bar.V0",
                            "xyz.openbmc_project.Foo.Bar.V1"
                        ]
                    }
                }
            ]
        })");

    using namespace pldm::responder::events;
    StateSensorHandler handler{dir.string()};
    StateSensorEntry entry{1, 64, 1, 0, 0, false};

    EXPECT_EQ(handler.eventAction(entry, 1), PLDM_SUCCESS);
    EXPECT_EQ(dbusFixture.stringValue, "xyz.openbmc_project.Foo.Bar.V1");
    EXPECT_EQ(dbusFixture.stringSetCount, 1u);

    fs::remove_all(dir);
}

TEST(StateSensorHandler, eventActionRealDbusFailureCoverage)
{
    PlatformDbusFixture dbusFixture;

    auto dir = makeEventConfigDir(R"({
            "entries": [
                {
                    "containerID": 1,
                    "entityType": 64,
                    "entityInstance": 1,
                    "sensorOffset": 0,
                    "event_states": [0, 1],
                    "dbus": {
                        "object_path": "/foo/bar",
                        "interface": "xyz.openbmc_project.Foo.Bar",
                        "property_name": "missingProperty",
                        "property_type": "string",
                        "property_values": [
                            "xyz.openbmc_project.Foo.Bar.V0",
                            "xyz.openbmc_project.Foo.Bar.V1"
                        ]
                    }
                }
            ]
        })");

    using namespace pldm::responder::events;
    StateSensorHandler handler{dir.string()};
    StateSensorEntry entry{1, 64, 1, 0, 0, false};

    EXPECT_EQ(handler.eventAction(entry, 1), PLDM_ERROR);
    EXPECT_EQ(dbusFixture.stringSetCount, 0u);

    fs::remove_all(dir);
}

TEST(StateSensorHandler, malformedEntryMissingFieldsCoverage)
{
    using namespace pldm::responder::events;

    auto dir = makeEventConfigDir(R"({
            "entries": [
                {
                    "containerID": 1,
                    "entityType": 64,
                    "entityInstance": 1,
                    "sensorOffset": 0,
                    "event_states": [0],
                    "dbus": {
                        "interface": "xyz.openbmc_project.Foo.Bar",
                        "property_name": "propertyName",
                        "property_type": "string",
                        "property_values": ["value0"]
                    }
                },
                {
                    "containerID": 1,
                    "entityType": 64,
                    "entityInstance": 1,
                    "sensorOffset": 1,
                    "event_states": [0],
                    "dbus": {
                        "object_path": "/foo/bar",
                        "property_name": "propertyName",
                        "property_type": "string",
                        "property_values": ["value1"]
                    }
                },
                {
                    "containerID": 1,
                    "entityType": 64,
                    "entityInstance": 1,
                    "sensorOffset": 2,
                    "event_states": [0],
                    "dbus": {
                        "object_path": "/foo/bar",
                        "interface": "xyz.openbmc_project.Foo.Bar",
                        "property_type": "string",
                        "property_values": ["value2"]
                    }
                },
                {
                    "containerID": 1,
                    "entityType": 64,
                    "entityInstance": 1,
                    "sensorOffset": 3,
                    "event_states": [0],
                    "dbus": {
                        "object_path": "/foo/bar",
                        "interface": "xyz.openbmc_project.Foo.Bar",
                        "property_name": "propertyName",
                        "property_type": "string"
                    }
                }
            ]
        })");

    StateSensorHandler handler{dir.string()};

    EXPECT_THROW(handler.getEventInfo(StateSensorEntry{1, 64, 1, 0, 0, false}),
                 std::out_of_range);
    EXPECT_THROW(handler.getEventInfo(StateSensorEntry{1, 64, 1, 1, 0, false}),
                 std::out_of_range);
    EXPECT_THROW(handler.getEventInfo(StateSensorEntry{1, 64, 1, 2, 0, false}),
                 std::out_of_range);
    EXPECT_THROW(handler.getEventInfo(StateSensorEntry{1, 64, 1, 3, 0, false}),
                 std::out_of_range);

    fs::remove_all(dir);
}

TEST(StateSensorHandler, emptyDirCoverage)
{
    namespace fs = std::filesystem;
    using namespace pldm::responder::events;

    auto dir = pldm::test::makeTempDir("PlatformEventEmptyDir.XXXXXX");

    StateSensorHandler handler{dir.string()};
    EXPECT_EQ(handler.eventAction(StateSensorEntry{1, 64, 1, 0, 0, false}, 0),
              PLDM_SUCCESS);

    fs::remove_all(dir);
}

TEST(StateSensorHandler, emptyEventStatesCoverage)
{
    using namespace pldm::responder::events;

    auto dir = makeEventConfigDir(R"({
            "entries": [
                {
                    "containerID": 1,
                    "entityType": 64,
                    "entityInstance": 1,
                    "sensorOffset": 0,
                    "event_states": [],
                    "dbus": {
                        "object_path": "/foo/bar",
                        "interface": "xyz.openbmc_project.Foo.Bar",
                        "property_name": "propertyName",
                        "property_type": "string",
                        "property_values": ["value0"]
                    }
                }
            ]
        })");

    StateSensorHandler handler{dir.string()};
    EXPECT_THROW(handler.getEventInfo(StateSensorEntry{1, 64, 1, 0, 0, false}),
                 std::out_of_range);

    fs::remove_all(dir);
}

TEST(StateSensorHandler, emptyPropertyValuesCoverage)
{
    using namespace pldm::responder::events;

    auto dir = makeEventConfigDir(R"({
            "entries": [
                {
                    "containerID": 1,
                    "entityType": 64,
                    "entityInstance": 1,
                    "sensorOffset": 0,
                    "event_states": [0],
                    "dbus": {
                        "object_path": "/foo/bar",
                        "interface": "xyz.openbmc_project.Foo.Bar",
                        "property_name": "propertyName",
                        "property_type": "string",
                        "property_values": []
                    }
                }
            ]
        })");

    StateSensorHandler handler{dir.string()};
    EXPECT_THROW(handler.getEventInfo(StateSensorEntry{1, 64, 1, 0, 0, false}),
                 std::out_of_range);

    fs::remove_all(dir);
}

TEST(PlatformHandlerWrapper, pdrConfigLoadCoveragePaths)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(_, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(Return("foo.bar"));

    auto event = sdeventplus::Event::get_default();

    auto* missingRepo = pldm_pdr_init();
    {
        Handler handler(&mockedUtils, "./pdr_jsons/does_not_exist", missingRepo,
                        nullptr, nullptr, nullptr, nullptr, event);
        EXPECT_FALSE(handler.getRepo().empty());
    }
    pldm_pdr_destroy(missingRepo);

    auto* malformedRepo = pldm_pdr_init();
    {
        Handler handler(&mockedUtils, "./pdr_jsons/state_sensor/malformed",
                        malformedRepo, nullptr, nullptr, nullptr, nullptr,
                        event);
        EXPECT_FALSE(handler.getRepo().empty());
    }
    pldm_pdr_destroy(malformedRepo);
}

TEST(TerminusLocatorPDR, BMCTerminusLocatorPDR)
{
    auto inPDRRepo = pldm_pdr_init();
    auto outPDRRepo = pldm_pdr_init();
    Repo outRepo(outPDRRepo);
    MockdBusHandler mockedUtils;
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "", inPDRRepo, nullptr, nullptr, nullptr,
                    nullptr, event);
    Repo inRepo(inPDRRepo);
    getRepoByType(inRepo, outRepo, PLDM_TERMINUS_LOCATOR_PDR);

    // 1 BMC terminus locator PDR in the PDR repository
    ASSERT_EQ(outRepo.getRecordCount(), 1);

    pdr_utils::PdrEntry entry;
    auto record = pdr::getRecordByHandle(outRepo, 1, entry);
    ASSERT_NE(record, nullptr);

    auto pdr = reinterpret_cast<const pldm_terminus_locator_pdr*>(entry.data);
    EXPECT_EQ(pdr->hdr.record_handle, 1);
    EXPECT_EQ(pdr->hdr.version, 1);
    EXPECT_EQ(pdr->hdr.type, PLDM_TERMINUS_LOCATOR_PDR);
    EXPECT_EQ(pdr->hdr.record_change_num, 0);
    EXPECT_EQ(pdr->hdr.length,
              sizeof(pldm_terminus_locator_pdr) - sizeof(pldm_pdr_hdr));
    EXPECT_EQ(pdr->terminus_handle, pldm::TERMINUS_HANDLE);
    EXPECT_EQ(pdr->validity, PLDM_TL_PDR_VALID);
    EXPECT_EQ(pdr->tid, pldm::TERMINUS_ID);
    EXPECT_EQ(pdr->container_id, 0);
    EXPECT_EQ(pdr->terminus_locator_type, PLDM_TERMINUS_LOCATOR_TYPE_MCTP_EID);
    EXPECT_EQ(pdr->terminus_locator_value_size,
              sizeof(pldm_terminus_locator_type_mctp_eid));
    auto locatorValue =
        reinterpret_cast<const pldm_terminus_locator_type_mctp_eid*>(
            pdr->terminus_locator_value);
    EXPECT_EQ(locatorValue->eid, BmcMctpEid);
    pldm_pdr_destroy(inPDRRepo);
    pldm_pdr_destroy(outPDRRepo);
}

TEST(getStateSensorReadingsHandler, testGoodRequest)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(StrEq("/foo/bar"), _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(Return("foo.bar"));

    auto inPDRRepo = pldm_pdr_init();
    auto outPDRRepo = pldm_pdr_init();
    Repo outRepo(outPDRRepo);
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_sensor/good", inPDRRepo,
                    nullptr, nullptr, nullptr, nullptr, event);
    Repo inRepo(inPDRRepo);
    getRepoByType(inRepo, outRepo, PLDM_STATE_SENSOR_PDR);
    pdr_utils::PdrEntry e;
    auto record = pdr::getRecordByHandle(outRepo, 2, e);
    ASSERT_NE(record, nullptr);
    pldm_state_sensor_pdr* pdr =
        std::start_lifetime_as<pldm_state_sensor_pdr>(e.data);
    EXPECT_EQ(pdr->hdr.type, PLDM_STATE_SENSOR_PDR);

    std::vector<get_sensor_state_field> stateField;
    uint8_t compSensorCnt{};
    uint8_t sensorRearmCnt = 1;

    MockdBusHandler handlerObj;
    EXPECT_CALL(handlerObj,
                getDbusPropertyVariant(StrEq("/foo/bar"), StrEq("propertyName"),
                                       StrEq("xyz.openbmc_project.Foo.Bar")))
        .WillOnce(Return(
            PropertyValue(std::string("xyz.openbmc_project.Foo.Bar.V0"))));

    auto rc = platform_state_sensor::getStateSensorReadingsHandler<
        MockdBusHandler, Handler>(handlerObj, handler, 0x1, sensorRearmCnt,
                                  compSensorCnt, stateField);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(compSensorCnt, 1);
    ASSERT_EQ(stateField[0].sensor_op_state, PLDM_SENSOR_UNAVAILABLE);
    ASSERT_EQ(stateField[0].present_state, PLDM_SENSOR_NORMAL);
    ASSERT_EQ(stateField[0].previous_state, PLDM_SENSOR_UNKNOWN);
    ASSERT_EQ(stateField[0].event_state, PLDM_SENSOR_UNKNOWN);

    pldm_pdr_destroy(inPDRRepo);
    pldm_pdr_destroy(outPDRRepo);
}

TEST(getStateSensorReadingsHandler, testBadRequest)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(StrEq("/foo/bar"), _))
        .Times(1)
        .WillRepeatedly(Return("foo.bar"));

    auto inPDRRepo = pldm_pdr_init();
    auto outPDRRepo = pldm_pdr_init();
    Repo outRepo(outPDRRepo);
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_sensor/good", inPDRRepo,
                    nullptr, nullptr, nullptr, nullptr, event);
    Repo inRepo(inPDRRepo);
    getRepoByType(inRepo, outRepo, PLDM_STATE_SENSOR_PDR);
    pdr_utils::PdrEntry e;
    auto record = pdr::getRecordByHandle(outRepo, 2, e);
    ASSERT_NE(record, nullptr);
    pldm_state_sensor_pdr* pdr =
        std::start_lifetime_as<pldm_state_sensor_pdr>(e.data);
    EXPECT_EQ(pdr->hdr.type, PLDM_STATE_SENSOR_PDR);

    std::vector<get_sensor_state_field> stateField;
    uint8_t compSensorCnt{};
    uint8_t sensorRearmCnt = 3;

    MockdBusHandler handlerObj;
    auto rc = platform_state_sensor::getStateSensorReadingsHandler<
        MockdBusHandler, Handler>(handlerObj, handler, 0x1, sensorRearmCnt,
                                  compSensorCnt, stateField);
    ASSERT_EQ(rc, PLDM_PLATFORM_REARM_UNAVAILABLE_IN_PRESENT_STATE);

    pldm_pdr_destroy(inPDRRepo);
    pldm_pdr_destroy(outPDRRepo);
}

TEST(getStateSensorReadings, testNoDbusToPLDMEventHandler)
{
    // pldmd constructs dbusToPLDMEventHandler only when a host EID is
    // configured; the responder must serve GetStateSensorReadings without it
    std::array<uint8_t,
               sizeof(pldm_msg_hdr) + PLDM_GET_STATE_SENSOR_READINGS_REQ_BYTES>
        requestPayload{};
    auto req = std::start_lifetime_as<pldm_msg>(requestPayload.data());
    size_t requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);

    bitfield8_t sensorRearm{};
    sensorRearm.byte = 0x01;
    auto rc = encode_get_state_sensor_readings_req(0, 0x1, sensorRearm, 0, req);
    ASSERT_EQ(rc, PLDM_SUCCESS);

    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(StrEq("/foo/bar"), _))
        .Times(1)
        .WillRepeatedly(Return("foo.bar"));

    auto inPDRRepo = pldm_pdr_init();
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_sensor/good", inPDRRepo,
                    nullptr, nullptr, nullptr, nullptr, event);

    auto response = handler.getStateSensorReadings(req, requestPayloadLength);
    auto responsePtr = std::start_lifetime_as<pldm_msg>(response.data());

    uint8_t completionCode{};
    uint8_t compSensorCnt{};
    std::array<get_sensor_state_field, 1> stateField{};
    rc = decode_get_state_sensor_readings_resp(
        responsePtr, response.size() - sizeof(pldm_msg_hdr), &completionCode,
        &compSensorCnt, stateField.data());
    ASSERT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_SUCCESS);
    ASSERT_EQ(compSensorCnt, 1);
    // without the host event path there is no sensor cache, so the previous
    // state is unknown as on a first read
    EXPECT_EQ(stateField[0].previous_state, PLDM_SENSOR_UNKNOWN);

    pldm_pdr_destroy(inPDRRepo);
}

TEST(pldmPDRRepositoryChgEvent, testNoHostPDRHandler)
{
    // pldmd constructs hostPDRHandler only when a host EID is configured; a
    // terminus sending a repository change event with PLDM_RECORDS_MODIFIED
    // must not crash the responder
    std::array<uint8_t, 1> eventDataOps = {PLDM_RECORDS_MODIFIED};
    std::array<uint8_t, 1> numsOfChangeEntries = {1};
    std::array<uint32_t, 1> changeEntries = {1};
    const uint32_t* firstEntry = changeEntries.data();

    size_t maxSize = PLDM_PDR_REPOSITORY_CHG_EVENT_MIN_LENGTH +
                     PLDM_PDR_REPOSITORY_CHANGE_RECORD_MIN_LENGTH +
                     changeEntries.size() * sizeof(uint32_t);
    std::vector<uint8_t> requestPayload(sizeof(pldm_msg_hdr) + maxSize);
    auto req = std::start_lifetime_as<pldm_msg>(requestPayload.data());
    auto eventData = std::start_lifetime_as<pldm_pdr_repository_chg_event_data>(
        req->payload);
    size_t actualSize{};
    auto rc = encode_pldm_pdr_repository_chg_event_data(
        FORMAT_IS_PDR_HANDLES, 1, eventDataOps.data(),
        numsOfChangeEntries.data(), &firstEntry, eventData, &actualSize,
        maxSize);
    ASSERT_EQ(rc, PLDM_SUCCESS);

    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(StrEq("/foo/bar"), _))
        .Times(1)
        .WillRepeatedly(Return("foo.bar"));

    auto inPDRRepo = pldm_pdr_init();
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_sensor/good", inPDRRepo,
                    nullptr, nullptr, nullptr, nullptr, event);

    uint8_t platformEventStatus = PLDM_EVENT_NO_LOGGING;
    rc = handler.pldmPDRRepositoryChgEvent(req, actualSize, 0x01, 1, 0,
                                           platformEventStatus);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    pldm_pdr_destroy(inPDRRepo);
}
