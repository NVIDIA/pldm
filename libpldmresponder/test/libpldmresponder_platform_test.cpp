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
#include "test/test_instance_id.hpp"

#include <sdbusplus/test/sdbus_mock.hpp>
#include <sdeventplus/event.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <type_traits>

using namespace pldm::pdr;
using namespace pldm::utils;
using namespace pldm::responder;
using namespace pldm::responder::platform;
using namespace pldm::responder::pdr;
using namespace pldm::responder::pdr_utils;

using ::testing::_;
using ::testing::Return;
using ::testing::StrEq;

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

class StubOemPlatformHandler : public pldm::responder::oem_platform::Handler
{
  public:
    StubOemPlatformHandler() :
        pldm::responder::oem_platform::Handler(
            static_cast<const pldm::utils::DBusHandler*>(nullptr))
    {}

    int getOemStateSensorReadingsHandler(
        pldm::EntityType, pldm::pdr::EntityInstance, pldm::pdr::StateSetId,
        pldm::pdr::CompositeCount,
        std::vector<get_sensor_state_field>&) override
    {
        return PLDM_SUCCESS;
    }

    int oemSetStateEffecterStatesHandler(uint16_t, uint16_t, uint16_t, uint8_t,
                                         std::vector<set_effecter_state_field>&,
                                         uint16_t) override
    {
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

    void countSetEventReceiver() override {}

    int checkBMCState() override
    {
        ++checkBMCStateCalls;
        return checkBMCStateResult;
    }

    size_t buildOEMPDRCalls = 0;
    size_t resetWatchDogCalls = 0;
    size_t checkBMCStateCalls = 0;
    int checkBMCStateResult = PLDM_SUCCESS;
};

} // namespace

TEST(getPDR, testGoodPath)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr) + PLDM_GET_PDR_REQ_BYTES>
        requestPayload{};
    auto req = reinterpret_cast<pldm_msg*>(requestPayload.data());
    size_t requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);

    struct pldm_get_pdr_req* request =
        reinterpret_cast<struct pldm_get_pdr_req*>(req->payload);
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
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());

    struct pldm_get_pdr_resp* resp =
        reinterpret_cast<struct pldm_get_pdr_resp*>(responsePtr->payload);
    ASSERT_EQ(PLDM_SUCCESS, resp->completion_code);
    ASSERT_EQ(2, resp->next_record_handle);
    ASSERT_EQ(true, resp->response_count != 0);

    pldm_pdr_hdr* hdr = reinterpret_cast<pldm_pdr_hdr*>(resp->record_data);
    ASSERT_EQ(hdr->record_handle, 1);
    ASSERT_EQ(hdr->version, 1);

    pldm_pdr_destroy(pdrRepo);
}

TEST(getPDR, testShortRead)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr) + PLDM_GET_PDR_REQ_BYTES>
        requestPayload{};
    auto req = reinterpret_cast<pldm_msg*>(requestPayload.data());
    size_t requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);

    struct pldm_get_pdr_req* request =
        reinterpret_cast<struct pldm_get_pdr_req*>(req->payload);
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
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    struct pldm_get_pdr_resp* resp =
        reinterpret_cast<struct pldm_get_pdr_resp*>(responsePtr->payload);
    ASSERT_EQ(PLDM_SUCCESS, resp->completion_code);
    ASSERT_EQ(1, resp->response_count);
    pldm_pdr_destroy(pdrRepo);
}

TEST(getPDR, testBadRecordHandle)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr) + PLDM_GET_PDR_REQ_BYTES>
        requestPayload{};
    auto req = reinterpret_cast<pldm_msg*>(requestPayload.data());
    size_t requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);

    struct pldm_get_pdr_req* request =
        reinterpret_cast<struct pldm_get_pdr_req*>(req->payload);
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
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());

    ASSERT_EQ(responsePtr->payload[0], PLDM_PLATFORM_INVALID_RECORD_HANDLE);

    pldm_pdr_destroy(pdrRepo);
}

TEST(getPDR, testNoNextRecord)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr) + PLDM_GET_PDR_REQ_BYTES>
        requestPayload{};
    auto req = reinterpret_cast<pldm_msg*>(requestPayload.data());
    size_t requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);

    struct pldm_get_pdr_req* request =
        reinterpret_cast<struct pldm_get_pdr_req*>(req->payload);
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
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    struct pldm_get_pdr_resp* resp =
        reinterpret_cast<struct pldm_get_pdr_resp*>(responsePtr->payload);
    ASSERT_EQ(PLDM_SUCCESS, resp->completion_code);
    ASSERT_EQ(2, resp->next_record_handle);

    pldm_pdr_destroy(pdrRepo);
}

TEST(getPDR, testFindPDR)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr) + PLDM_GET_PDR_REQ_BYTES>
        requestPayload{};
    auto req = reinterpret_cast<pldm_msg*>(requestPayload.data());
    size_t requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);

    struct pldm_get_pdr_req* request =
        reinterpret_cast<struct pldm_get_pdr_req*>(req->payload);
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
        auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());
        struct pldm_get_pdr_resp* resp =
            reinterpret_cast<struct pldm_get_pdr_resp*>(responsePtr->payload);
        ASSERT_EQ(PLDM_SUCCESS, resp->completion_code);

        handle = resp->next_record_handle; // point to the next pdr in case
                                           // current is not what we want

        pldm_pdr_hdr* hdr = reinterpret_cast<pldm_pdr_hdr*>(resp->record_data);
        std::cerr << "PDR next record handle " << handle << "\n";
        std::cerr << "PDR type " << hdr->type << "\n";
        if (hdr->type == PLDM_STATE_EFFECTER_PDR)
        {
            pldm_state_effecter_pdr* pdr =
                reinterpret_cast<pldm_state_effecter_pdr*>(resp->record_data);
            std::cerr << "PDR entity type " << pdr->entity_type << "\n";
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

TEST(setStateEffecterStatesHandler, testGoodRequest)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr) + PLDM_GET_PDR_REQ_BYTES>
        requestPayload{};
    auto req = reinterpret_cast<pldm_msg*>(requestPayload.data());
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
        reinterpret_cast<pldm_state_effecter_pdr*>(e.data);
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
    auto req = reinterpret_cast<pldm_msg*>(requestPayload.data());
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
        reinterpret_cast<pldm_state_effecter_pdr*>(e.data);
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
        reinterpret_cast<pldm_numeric_effecter_value_pdr*>(e.data);
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
        reinterpret_cast<pldm_numeric_effecter_value_pdr*>(e.data);
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
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_UINT8);
        uint8_t value = 17;
        auto [rc, dbusValue] = platform_numeric_effecter::convertToDbusValue(
            &pdr, PLDM_EFFECTER_DATA_SIZE_UINT8, &value, "uint64_t");
        EXPECT_EQ(rc, PLDM_SUCCESS);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<uint64_t>(dbusValue.value()), 17u);
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_SINT8);
        int8_t signedValue = -7;
        uint8_t value = *reinterpret_cast<uint8_t*>(&signedValue);
        auto [rc, dbusValue] = platform_numeric_effecter::convertToDbusValue(
            &pdr, PLDM_EFFECTER_DATA_SIZE_SINT8, &value, "int8_t");
        EXPECT_EQ(rc, PLDM_SUCCESS);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_TRUE(isNumericProperty(dbusValue.value()));
        EXPECT_EQ(numericProperty(dbusValue.value()), -7.0L);
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_UINT16);
        uint16_t input = 1024;
        uint8_t value[sizeof(input)]{};
        std::memcpy(value, &input, sizeof(input));
        auto [rc, dbusValue] = platform_numeric_effecter::convertToDbusValue(
            &pdr, PLDM_EFFECTER_DATA_SIZE_UINT16, value, "uint32_t");
        EXPECT_EQ(rc, PLDM_SUCCESS);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<uint32_t>(dbusValue.value()), 1024u);
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_SINT16);
        int16_t input = -321;
        uint8_t value[sizeof(input)]{};
        std::memcpy(value, &input, sizeof(input));
        auto [rc, dbusValue] = platform_numeric_effecter::convertToDbusValue(
            &pdr, PLDM_EFFECTER_DATA_SIZE_SINT16, value, "uint64_t");
        EXPECT_EQ(rc, PLDM_SUCCESS);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<uint64_t>(dbusValue.value()),
                  static_cast<uint64_t>(input));
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_UINT32);
        uint32_t input = 4242;
        uint8_t value[sizeof(input)]{};
        std::memcpy(value, &input, sizeof(input));
        auto [rc, dbusValue] = platform_numeric_effecter::convertToDbusValue(
            &pdr, PLDM_EFFECTER_DATA_SIZE_UINT32, value, "uint32_t");
        EXPECT_EQ(rc, PLDM_SUCCESS);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<uint32_t>(dbusValue.value()), 4242u);
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_SINT32);
        int32_t input = 1024;
        uint8_t value[sizeof(input)]{};
        std::memcpy(value, &input, sizeof(input));
        auto [rc, dbusValue] = platform_numeric_effecter::convertToDbusValue(
            &pdr, PLDM_EFFECTER_DATA_SIZE_SINT32, value, "uint32_t");
        EXPECT_EQ(rc, PLDM_SUCCESS);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<uint32_t>(dbusValue.value()),
                  static_cast<uint32_t>(input));
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_UINT64);
        uint64_t input = 55555;
        uint8_t value[sizeof(input)]{};
        std::memcpy(value, &input, sizeof(input));
        auto [rc, dbusValue] = platform_numeric_effecter::convertToDbusValue(
            &pdr, PLDM_EFFECTER_DATA_SIZE_UINT64, value, "uint64_t");
        EXPECT_EQ(rc, PLDM_SUCCESS);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_EQ(std::get<uint64_t>(dbusValue.value()), 55555u);
    }
    {
        auto pdr = makeNumericEffecterPdr(PLDM_EFFECTER_DATA_SIZE_SINT64);
        int64_t input = -7777;
        uint8_t value[sizeof(input)]{};
        std::memcpy(value, &input, sizeof(input));
        auto [rc, dbusValue] = platform_numeric_effecter::convertToDbusValue(
            &pdr, PLDM_EFFECTER_DATA_SIZE_SINT64, value, "int64_t");
        EXPECT_EQ(rc, PLDM_SUCCESS);
        ASSERT_TRUE(dbusValue.has_value());
        EXPECT_TRUE(isNumericProperty(dbusValue.value()));
        EXPECT_EQ(numericProperty(dbusValue.value()), -7777.0L);
    }
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
        reinterpret_cast<pldm_state_sensor_pdr*>(e.data);
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
        reinterpret_cast<pldm_state_sensor_pdr*>(e.data);
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

TEST(PlatformHandlerWrapper, setStateEffecterStatesInvalidLength)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(_, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(Return("foo.bar"));

    auto pdrRepo = pldm_pdr_init();
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_effecter/good", pdrRepo,
                    nullptr, nullptr, nullptr, nullptr, event);

    std::array<uint8_t, sizeof(pldm_msg_hdr)> requestBuffer{};
    auto* request = reinterpret_cast<pldm_msg*>(requestBuffer.data());
    request->hdr.instance_id = 1;

    auto response = handler.setStateEffecterStates(request, 0);
    EXPECT_EQ(completionCode(response), PLDM_ERROR_INVALID_LENGTH);

    pldm_pdr_destroy(pdrRepo);
}

TEST(PlatformHandlerWrapper, setStateEffecterStatesInvalidEffecter)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(_, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(Return("foo.bar"));

    auto pdrRepo = pldm_pdr_init();
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_effecter/good", pdrRepo,
                    nullptr, nullptr, nullptr, nullptr, event);

    std::vector<set_effecter_state_field> stateField{{PLDM_REQUEST_SET, 1}};
    std::vector<uint8_t> request(
        sizeof(pldm_msg_hdr) + sizeof(uint16_t) + sizeof(uint8_t) +
        sizeof(set_effecter_state_field));
    auto* msg = asMsg(request);
    ASSERT_EQ(encode_set_state_effecter_states_req(1, 0x7777, 1,
                                                   stateField.data(), msg),
              PLDM_SUCCESS);

    auto response = handler.setStateEffecterStates(
        msg, request.size() - sizeof(pldm_msg_hdr));
    EXPECT_EQ(completionCode(response), PLDM_PLATFORM_INVALID_EFFECTER_ID);

    pldm_pdr_destroy(pdrRepo);
}

TEST(PlatformHandlerWrapper, setNumericEffecterValueCoveragePaths)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(_, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(Return("foo.bar"));

    auto pdrRepo = pldm_pdr_init();
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_effecter/good", pdrRepo,
                    nullptr, nullptr, nullptr, nullptr, event);

    std::array<uint8_t, sizeof(pldm_msg_hdr)> shortRequest{};
    auto* shortMsg = reinterpret_cast<pldm_msg*>(shortRequest.data());
    shortMsg->hdr.instance_id = 1;
    auto shortResponse = handler.setNumericEffecterValue(shortMsg, 0);
    EXPECT_EQ(completionCode(shortResponse), PLDM_ERROR_INVALID_LENGTH);

    uint8_t effecterValue = 1;
    std::vector<uint8_t> request(
        sizeof(pldm_msg_hdr) + PLDM_SET_NUMERIC_EFFECTER_VALUE_MIN_REQ_BYTES);
    auto* msg = asMsg(request);
    ASSERT_EQ(encode_set_numeric_effecter_value_req(
                  1, 0x7777, PLDM_EFFECTER_DATA_SIZE_UINT8, &effecterValue, msg,
                  PLDM_SET_NUMERIC_EFFECTER_VALUE_MIN_REQ_BYTES),
              PLDM_SUCCESS);
    auto response = handler.setNumericEffecterValue(
        msg, PLDM_SET_NUMERIC_EFFECTER_VALUE_MIN_REQ_BYTES);
    EXPECT_TRUE(completionCode(response) == PLDM_PLATFORM_INVALID_EFFECTER_ID ||
                completionCode(response) == PLDM_ERROR_INVALID_DATA);

    pldm_pdr_destroy(pdrRepo);
}

TEST(PlatformHandlerWrapper, getStateSensorReadingsCoveragePaths)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(_, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(Return("foo.bar"));

    auto pdrRepo = pldm_pdr_init();
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_sensor/good", pdrRepo,
                    nullptr, nullptr, nullptr, nullptr, event);

    std::array<uint8_t, sizeof(pldm_msg_hdr)> shortRequest{};
    auto* shortMsg = reinterpret_cast<pldm_msg*>(shortRequest.data());
    shortMsg->hdr.instance_id = 1;
    auto shortResponse = handler.getStateSensorReadings(shortMsg, 0);
    EXPECT_EQ(completionCode(shortResponse), PLDM_ERROR_INVALID_LENGTH);

    bitfield8_t sensorRearm{};
    sensorRearm.byte = 0x1;
    std::vector<uint8_t> request(
        sizeof(pldm_msg_hdr) + PLDM_GET_STATE_SENSOR_READINGS_REQ_BYTES);
    auto* msg = asMsg(request);
    ASSERT_EQ(
        encode_get_state_sensor_readings_req(1, 0x7777, sensorRearm, 0, msg),
        PLDM_SUCCESS);
    auto response = handler.getStateSensorReadings(
        msg, PLDM_GET_STATE_SENSOR_READINGS_REQ_BYTES);
    EXPECT_FALSE(response.empty());

    pldm_pdr_destroy(pdrRepo);
}

TEST(PlatformHandlerWrapper, requestDecodeFailureCoveragePaths)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(_, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(Return("foo.bar"));

    auto pdrRepo = pldm_pdr_init();
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_effecter/good", pdrRepo,
                    nullptr, nullptr, nullptr, nullptr, event);

    std::array<uint8_t, sizeof(pldm_msg_hdr)> shortRequest{};
    auto* shortMsg = reinterpret_cast<pldm_msg*>(shortRequest.data());
    shortMsg->hdr.instance_id = 1;

    auto badPdrLength = handler.getPDR(shortMsg, 0);
    EXPECT_EQ(completionCode(badPdrLength), PLDM_ERROR_INVALID_LENGTH);

    auto badEventMsg = handler.platformEventMessage(shortMsg, 0);
    EXPECT_EQ(completionCode(badEventMsg), PLDM_ERROR_INVALID_LENGTH);

    pldm_pdr_destroy(pdrRepo);
}

TEST(PlatformHandlerWrapper, oemWrapperCoveragePaths)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(_, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(Return("foo.bar"));

    auto pdrRepo = pldm_pdr_init();
    auto event = sdeventplus::Event::get_default();
    StubOemPlatformHandler oemHandler{};
    Handler handler(&mockedUtils, "./pdr_jsons/state_effecter/good", pdrRepo,
                    nullptr, nullptr, nullptr, &oemHandler, event);

    constexpr uint16_t oemStateSetId = 0x8000;
    uint32_t recordHandle = 0;
    auto effecterPdr = makeStateEffecterPdr(0x9002, 1, 64, 1, 1, oemStateSetId);
    ASSERT_EQ(pldm_pdr_add(pdrRepo, effecterPdr.data(), effecterPdr.size(),
                           true, TERMINUS_HANDLE, &recordHandle),
              0);

    std::vector<set_effecter_state_field> effecterStateField{
        {PLDM_REQUEST_SET, 1}};
    std::vector<uint8_t> effecterReq(
        sizeof(pldm_msg_hdr) + sizeof(uint16_t) + sizeof(uint8_t) +
        sizeof(set_effecter_state_field));
    auto* effecterMsg = asMsg(effecterReq);
    ASSERT_EQ(encode_set_state_effecter_states_req(
                  2, 0x9002, 1, effecterStateField.data(), effecterMsg),
              PLDM_SUCCESS);
    auto effecterResp = handler.setStateEffecterStates(
        effecterMsg, effecterReq.size() - sizeof(pldm_msg_hdr));
    EXPECT_EQ(completionCode(effecterResp), PLDM_SUCCESS);

    pldm_pdr_destroy(pdrRepo);
}

TEST(PlatformHandlerWrapper, oemHelperDetectionCoveragePaths)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(_, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(Return("foo.bar"));

    auto event = sdeventplus::Event::get_default();
    uint8_t compositeCount{};
    uint16_t entityType{};
    uint16_t entityInstance{};
    uint16_t stateSetId{};

    auto sensorOnlyRepo = pldm_pdr_init();
    Handler sensorOnlyHandler(&mockedUtils, "./pdr_jsons/state_effecter/good",
                              sensorOnlyRepo, nullptr, nullptr, nullptr,
                              nullptr, event);
    EXPECT_FALSE(isOemStateSensor(sensorOnlyHandler, 0x1234, 1, compositeCount,
                                  entityType, entityInstance, stateSetId));

    auto sensorPdr = makeStateSensorPdr(0x9200, 1, PLDM_OEM_ENTITY_TYPE_START);
    uint32_t recordHandle = 0;
    ASSERT_EQ(pldm_pdr_add(sensorOnlyRepo, sensorPdr.data(), sensorPdr.size(),
                           true, TERMINUS_HANDLE, &recordHandle),
              0);
    EXPECT_FALSE(isOemStateSensor(sensorOnlyHandler, 0xEE00, 1, compositeCount,
                                  entityType, entityInstance, stateSetId));
    EXPECT_TRUE(isOemStateSensor(sensorOnlyHandler, 0x9200, 1, compositeCount,
                                 entityType, entityInstance, stateSetId));

    auto effecterOnlyRepo = pldm_pdr_init();
    Handler effecterOnlyHandler(&mockedUtils, "./pdr_jsons/state_sensor/good",
                                effecterOnlyRepo, nullptr, nullptr, nullptr,
                                nullptr, event);
    EXPECT_FALSE(isOemStateEffecter(effecterOnlyHandler, 0x1235, 1, entityType,
                                    entityInstance, stateSetId));

    auto effecterPdr =
        makeStateEffecterPdr(0x9201, 1, PLDM_OEM_ENTITY_TYPE_START);
    ASSERT_EQ(pldm_pdr_add(effecterOnlyRepo, effecterPdr.data(),
                           effecterPdr.size(), true, TERMINUS_HANDLE,
                           &recordHandle),
              0);
    EXPECT_TRUE(isOemStateEffecter(effecterOnlyHandler, 0x9201, 1, entityType,
                                   entityInstance, stateSetId));

    pldm_pdr_destroy(sensorOnlyRepo);
    pldm_pdr_destroy(effecterOnlyRepo);
}

TEST(PlatformHandlerWrapper, platformEventMessageCoveragePaths)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(_, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(Return("foo.bar"));

    auto pdrRepo = pldm_pdr_init();
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_effecter/good", pdrRepo,
                    nullptr, nullptr, nullptr, nullptr, event);

    const uint8_t eventData = 0;
    {
        constexpr size_t payloadLength =
            PLDM_PLATFORM_EVENT_MESSAGE_MIN_REQ_BYTES + sizeof(eventData);
        std::vector<uint8_t> request(sizeof(pldm_msg_hdr) + payloadLength);
        auto* msg = asMsg(request);
        ASSERT_EQ(encode_platform_event_message_req(
                      1, PLDM_PLATFORM_EVENT_MESSAGE_FORMAT_VERSION, 9,
                      PLDM_HEARTBEAT_TIMER_ELAPSED_EVENT, &eventData,
                      sizeof(eventData), msg, payloadLength),
                  PLDM_SUCCESS);
        auto response = handler.platformEventMessage(msg, payloadLength);
        EXPECT_EQ(completionCode(response), PLDM_SUCCESS);
    }

    {
        constexpr size_t payloadLength =
            PLDM_PLATFORM_EVENT_MESSAGE_MIN_REQ_BYTES + sizeof(eventData);
        std::vector<uint8_t> request(sizeof(pldm_msg_hdr) + payloadLength);
        auto* msg = asMsg(request);
        msg->hdr.instance_id = 1;
        msg->payload[0] = PLDM_PLATFORM_EVENT_MESSAGE_FORMAT_VERSION;
        msg->payload[1] = 9;
        msg->payload[2] = 0xFF;
        msg->payload[3] = eventData;
        auto response = handler.platformEventMessage(msg, payloadLength);
        EXPECT_EQ(completionCode(response), PLDM_ERROR_INVALID_DATA);
    }

    pldm_pdr_destroy(pdrRepo);
}

TEST(PlatformHandlerWrapper, sensorEventStatePathNoHostPdr)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(_, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(Return("foo.bar"));

    auto pdrRepo = pldm_pdr_init();
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_sensor/good", pdrRepo,
                    nullptr, nullptr, nullptr, nullptr, event);

    size_t eventDataLength = 0;
    ASSERT_EQ(encode_sensor_event_data(nullptr, 0, 1, PLDM_STATE_SENSOR_STATE,
                                       0, 1, 0, &eventDataLength),
              PLDM_SUCCESS);

    std::vector<uint8_t> eventData(eventDataLength);
    ASSERT_EQ(encode_sensor_event_data(
                  reinterpret_cast<pldm_sensor_event_data*>(eventData.data()),
                  eventData.size(), 1, PLDM_STATE_SENSOR_STATE, 0, 1, 0,
                  &eventDataLength),
              PLDM_SUCCESS);

    const auto payloadLength =
        PLDM_PLATFORM_EVENT_MESSAGE_MIN_REQ_BYTES + eventDataLength;
    std::vector<uint8_t> request(sizeof(pldm_msg_hdr) + payloadLength);
    auto* msg = asMsg(request);
    ASSERT_EQ(encode_platform_event_message_req(
                  1, PLDM_PLATFORM_EVENT_MESSAGE_FORMAT_VERSION, 9,
                  PLDM_SENSOR_EVENT, eventData.data(), eventDataLength, msg,
                  payloadLength),
              PLDM_SUCCESS);

    uint8_t platformEventStatus = PLDM_EVENT_NO_LOGGING;
    auto rc = handler.sensorEvent(
        msg, payloadLength, PLDM_PLATFORM_EVENT_MESSAGE_FORMAT_VERSION, 9,
        PLDM_PLATFORM_EVENT_MESSAGE_MIN_REQ_BYTES, platformEventStatus);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    pldm_pdr_destroy(pdrRepo);
}

TEST(PlatformHandlerWrapper, pdrRepositoryChangeEventPaths)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(_, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(Return("foo.bar"));

    auto pdrRepo = pldm_pdr_init();
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_sensor/good", pdrRepo,
                    nullptr, nullptr, nullptr, nullptr, event);

    {
        const std::array<uint8_t, 2> eventData{FORMAT_IS_PDR_TYPES, 0};
        const auto payloadLength =
            PLDM_PLATFORM_EVENT_MESSAGE_MIN_REQ_BYTES + eventData.size();
        std::vector<uint8_t> request(sizeof(pldm_msg_hdr) + payloadLength);
        auto* msg = asMsg(request);
        ASSERT_EQ(encode_platform_event_message_req(
                      1, PLDM_PLATFORM_EVENT_MESSAGE_FORMAT_VERSION, 9,
                      PLDM_PDR_REPOSITORY_CHG_EVENT, eventData.data(),
                      eventData.size(), msg, payloadLength),
                  PLDM_SUCCESS);
        uint8_t platformEventStatus = PLDM_EVENT_NO_LOGGING;
        auto rc = handler.pldmPDRRepositoryChgEvent(
            msg, payloadLength, PLDM_PLATFORM_EVENT_MESSAGE_FORMAT_VERSION, 9,
            PLDM_PLATFORM_EVENT_MESSAGE_MIN_REQ_BYTES, platformEventStatus);
        EXPECT_EQ(rc, PLDM_ERROR_INVALID_DATA);
    }

    {
        uint8_t eventDataOperation[1] = {PLDM_RECORDS_ADDED};
        uint8_t numberOfEntries[1] = {1};
        uint32_t changeEntry = 123;
        const uint32_t* changeEntries[1] = {&changeEntry};
        size_t eventDataLength = 0;

        ASSERT_EQ(encode_pldm_pdr_repository_chg_event_data(
                      FORMAT_IS_PDR_HANDLES, 1, eventDataOperation,
                      numberOfEntries, changeEntries, nullptr, &eventDataLength,
                      0),
                  PLDM_SUCCESS);

        std::vector<uint8_t> eventData(eventDataLength);
        ASSERT_EQ(encode_pldm_pdr_repository_chg_event_data(
                      FORMAT_IS_PDR_HANDLES, 1, eventDataOperation,
                      numberOfEntries, changeEntries,
                      reinterpret_cast<pldm_pdr_repository_chg_event_data*>(
                          eventData.data()),
                      &eventDataLength, eventData.size()),
                  PLDM_SUCCESS);

        const auto payloadLength =
            PLDM_PLATFORM_EVENT_MESSAGE_MIN_REQ_BYTES + eventDataLength;
        std::vector<uint8_t> request(sizeof(pldm_msg_hdr) + payloadLength);
        auto* msg = asMsg(request);
        ASSERT_EQ(encode_platform_event_message_req(
                      1, PLDM_PLATFORM_EVENT_MESSAGE_FORMAT_VERSION, 9,
                      PLDM_PDR_REPOSITORY_CHG_EVENT, eventData.data(),
                      eventDataLength, msg, payloadLength),
                  PLDM_SUCCESS);
        uint8_t platformEventStatus = PLDM_EVENT_NO_LOGGING;
        auto rc = handler.pldmPDRRepositoryChgEvent(
            msg, payloadLength, PLDM_PLATFORM_EVENT_MESSAGE_FORMAT_VERSION, 9,
            PLDM_PLATFORM_EVENT_MESSAGE_MIN_REQ_BYTES, platformEventStatus);
        EXPECT_EQ(rc, PLDM_SUCCESS);
    }

    pldm_pdr_destroy(pdrRepo);
}

TEST(PlatformHandlerWrapper, getPDRRecordHandlesAndOemChecks)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(_, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(Return("foo.bar"));

    auto effecterPdrRepo = pldm_pdr_init();
    auto sensorPdrRepo = pldm_pdr_init();
    auto event = sdeventplus::Event::get_default();
    Handler effecterHandler(&mockedUtils, "./pdr_jsons/state_effecter/good",
                            effecterPdrRepo, nullptr, nullptr, nullptr, nullptr,
                            event);
    Handler sensorHandler(&mockedUtils, "./pdr_jsons/state_sensor/good",
                          sensorPdrRepo, nullptr, nullptr, nullptr, nullptr,
                          event);

    pldm::PDRRecordHandles handles;
    const pldm::ChangeEntry entries[2] = {11, 22};
    const auto* alignedBytes = reinterpret_cast<const uint8_t*>(entries);
    EXPECT_EQ(effecterHandler.getPDRRecordHandles(alignedBytes, sizeof(entries),
                                                  2, handles),
              PLDM_SUCCESS);
    EXPECT_EQ(handles.size(), 2u);
    EXPECT_EQ(effecterHandler.getPDRRecordHandles(alignedBytes, sizeof(entries),
                                                  3, handles),
              PLDM_ERROR_INVALID_DATA);

    std::array<uint8_t, sizeof(entries) + 1> unalignedEntries{};
    memcpy(unalignedEntries.data() + 1, entries, sizeof(entries));
    pldm::PDRRecordHandles unalignedHandles;
    EXPECT_EQ(effecterHandler.getPDRRecordHandles(unalignedEntries.data() + 1,
                                                  sizeof(entries), 2,
                                                  unalignedHandles),
              PLDM_SUCCESS);
    EXPECT_EQ(unalignedHandles.size(), 2u);
    EXPECT_EQ(unalignedHandles[0], entries[0]);
    EXPECT_EQ(unalignedHandles[1], entries[1]);

    uint16_t entityType{};
    uint16_t entityInstance{};
    uint16_t stateSetId{};
    uint8_t compSensorCnt{};
    EXPECT_FALSE(isOemStateEffecter(effecterHandler, 1, 1, entityType,
                                    entityInstance, stateSetId));
    EXPECT_FALSE(isOemStateEffecter(effecterHandler, 1, 8, entityType,
                                    entityInstance, stateSetId));
    EXPECT_FALSE(isOemStateSensor(sensorHandler, 1, 1, compSensorCnt,
                                  entityType, entityInstance, stateSetId));
    EXPECT_FALSE(isOemStateSensor(sensorHandler, 1, 8, compSensorCnt,
                                  entityType, entityInstance, stateSetId));

    pldm_pdr_destroy(effecterPdrRepo);
    pldm_pdr_destroy(sensorPdrRepo);
}

TEST(PlatformHandlerWrapper, deferredPostGetPDRActionsCoverage)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(StrEq("/foo/bar"), _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(Return("foo.bar"));

    auto pdrRepo = pldm_pdr_init();
    auto event = sdeventplus::Event::get_default();
    TestInstanceIdDb instanceIdDb;
    pldm::state_sensor::DbusToPLDMEvent dbusToPLDMEvent(0, 8, instanceIdDb,
                                                        nullptr);

    Handler handler(&mockedUtils, "./pdr_jsons/state_sensor/good", pdrRepo,
                    nullptr, &dbusToPLDMEvent, nullptr, nullptr, event, true);

    std::vector<uint8_t> request(sizeof(pldm_msg_hdr) + PLDM_GET_PDR_REQ_BYTES,
                                 0);
    auto* msg = asMsg(request);
    auto* pdrReq = reinterpret_cast<pldm_get_pdr_req*>(msg->payload);
    pdrReq->request_count = 64;

    auto response = handler.getPDR(msg, PLDM_GET_PDR_REQ_BYTES);
    EXPECT_EQ(completionCode(response), PLDM_SUCCESS);

    event.run(std::nullopt);

    pldm_pdr_destroy(pdrRepo);
}

TEST(getStateSensorReadingsHandler, dbusTemplateCoverage)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(StrEq("/foo/bar"), _))
        .Times(1)
        .WillRepeatedly(Return("foo.bar"));

    auto inPDRRepo = pldm_pdr_init();
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_sensor/good", inPDRRepo,
                    nullptr, nullptr, nullptr, nullptr, event);

    std::map<pldm::responder::pdr_utils::State, pldm::utils::PropertyValue>
        stateToDbusVal{{1, PropertyValue{std::string("foo")}}};
    DBusMapping badDbusMapping{"/bad/path", "xyz.openbmc.Bad", "Bad", "string"};
    const pldm::utils::DBusHandler dBusIntf;
    EXPECT_EQ(platform_state_sensor::getStateSensorEventState(
                  dBusIntf, stateToDbusVal, badDbusMapping),
              PLDM_SENSOR_UNKNOWN);

    uint8_t compSensorCnt{};
    std::vector<get_sensor_state_field> stateField;
    auto rc = platform_state_sensor::getStateSensorReadingsHandler(
        dBusIntf, handler, 1, 1, compSensorCnt, stateField);
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(compSensorCnt, 1);
    ASSERT_EQ(stateField.size(), 1u);
    EXPECT_TRUE(stateField[0].present_state == PLDM_SENSOR_UNKNOWN ||
                stateField[0].present_state == 1);
    EXPECT_TRUE(stateField[0].sensor_op_state == PLDM_SENSOR_UNAVAILABLE ||
                stateField[0].sensor_op_state == PLDM_SENSOR_ENABLED);

    pldm_pdr_destroy(inPDRRepo);
}

TEST(PlatformHandlerWrapper, commandDispatchAndAddOnHandlersCoverage)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(_, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(Return("foo.bar"));

    auto pdrRepo = pldm_pdr_init();
    auto event = sdeventplus::Event::get_default();

    EventMap addOnHandlers{};
    constexpr uint8_t oemEventClass = 0xF0;
    addOnHandlers[oemEventClass].emplace_back(
        [](const pldm_msg*, size_t, uint8_t, uint8_t, size_t, uint8_t&) {
            return PLDM_ERROR_INVALID_DATA;
        });

    Handler handler(&mockedUtils, "./pdr_jsons/state_sensor/good", pdrRepo,
                    nullptr, nullptr, nullptr, nullptr, event, false,
                    std::optional<EventMap>{addOnHandlers});

    std::vector<uint8_t> getPdrReq(
        sizeof(pldm_msg_hdr) + PLDM_GET_PDR_REQ_BYTES, 0);
    auto* getPdrMsg = asMsg(getPdrReq);
    auto* getPdrPayload =
        reinterpret_cast<pldm_get_pdr_req*>(getPdrMsg->payload);
    getPdrPayload->request_count = 32;
    auto getPdrResp =
        handler.handle(0, PLDM_GET_PDR, getPdrMsg, PLDM_GET_PDR_REQ_BYTES);
    EXPECT_FALSE(getPdrResp.empty());

    std::array<uint8_t, sizeof(pldm_msg_hdr)> shortRequest{};
    auto* shortMsg = reinterpret_cast<pldm_msg*>(shortRequest.data());
    shortMsg->hdr.instance_id = 1;

    auto setStateResp =
        handler.handle(0, PLDM_SET_STATE_EFFECTER_STATES, shortMsg, 0);
    EXPECT_EQ(completionCode(setStateResp), PLDM_ERROR_INVALID_LENGTH);

    auto setNumericResp =
        handler.handle(0, PLDM_SET_NUMERIC_EFFECTER_VALUE, shortMsg, 0);
    EXPECT_EQ(completionCode(setNumericResp), PLDM_ERROR_INVALID_LENGTH);

    auto getStateResp =
        handler.handle(0, PLDM_GET_STATE_SENSOR_READINGS, shortMsg, 0);
    EXPECT_EQ(completionCode(getStateResp), PLDM_ERROR_INVALID_LENGTH);

    const uint8_t eventData = 0;
    constexpr size_t payloadLength =
        PLDM_PLATFORM_EVENT_MESSAGE_MIN_REQ_BYTES + sizeof(eventData);
    std::vector<uint8_t> eventReq(sizeof(pldm_msg_hdr) + payloadLength, 0);
    auto* eventMsg = asMsg(eventReq);
    ASSERT_EQ(encode_platform_event_message_req(
                  1, PLDM_PLATFORM_EVENT_MESSAGE_FORMAT_VERSION, 9,
                  oemEventClass, &eventData, sizeof(eventData), eventMsg,
                  payloadLength),
              PLDM_SUCCESS);
    auto eventResp =
        handler.handle(0, PLDM_PLATFORM_EVENT_MESSAGE, eventMsg, payloadLength);
    EXPECT_EQ(completionCode(eventResp), PLDM_ERROR_INVALID_DATA);

    pldm_pdr_destroy(pdrRepo);
}

TEST(PlatformHandlerWrapper, sensorEventWithHostPdrCoveragePaths)
{
    using namespace std::chrono_literals;

    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(_, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(Return("foo.bar"));

    auto platformRepo = pldm_pdr_init();
    auto hostRepo = pldm_pdr_init();
    auto entityTree = pldm_entity_association_tree_init();
    auto bmcEntityTree = pldm_entity_association_tree_init();

    auto event = sdeventplus::Event::get_default();
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler(
        nullptr, event, instanceIdDb, false, 1s, 0, 1ms);

    {
        pldm::HostPDRHandler hostPDRHandler(
            0, 8, event, hostRepo, "./event_jsons/good", entityTree,
            bmcEntityTree, instanceIdDb, &reqHandler);
        Handler handler(&mockedUtils, "./pdr_jsons/state_sensor/good",
                        platformRepo, &hostPDRHandler, nullptr, nullptr,
                        nullptr, event);

        uint8_t platformEventStatus = PLDM_EVENT_NO_LOGGING;

        auto noMapReq = makeSensorEventRequest(1, 9, 1, 0, 1, 0);
        auto* noMapMsg = asMsg(noMapReq);
        auto rc = handler.sensorEvent(
            noMapMsg, noMapReq.size() - sizeof(pldm_msg_hdr),
            PLDM_PLATFORM_EVENT_MESSAGE_FORMAT_VERSION, 9,
            PLDM_PLATFORM_EVENT_MESSAGE_MIN_REQ_BYTES, platformEventStatus);
        EXPECT_EQ(rc, PLDM_SUCCESS);

        hostPDRHandler.tlPDRInfo[1] =
            std::make_tuple(static_cast<uint8_t>(9), static_cast<uint8_t>(8),
                            static_cast<uint8_t>(PLDM_TL_PDR_VALID));
        hostPDRHandler.parseStateSensorPDRs({makeStateSensorPdr(1, 1)});
        auto offsetReq = makeSensorEventRequest(1, 9, 1, 2, 1, 0);
        auto* offsetMsg = asMsg(offsetReq);
        rc = handler.sensorEvent(
            offsetMsg, offsetReq.size() - sizeof(pldm_msg_hdr),
            PLDM_PLATFORM_EVENT_MESSAGE_FORMAT_VERSION, 9,
            PLDM_PLATFORM_EVENT_MESSAGE_MIN_REQ_BYTES, platformEventStatus);
        EXPECT_EQ(rc, PLDM_ERROR_INVALID_DATA);

        hostPDRHandler.tlPDRInfo[1] =
            std::make_tuple(static_cast<uint8_t>(9), static_cast<uint8_t>(8),
                            static_cast<uint8_t>(PLDM_TL_PDR_VALID));
        hostPDRHandler.parseStateSensorPDRs({makeStateSensorPdr(2, 1)});

        auto badStateReq = makeSensorEventRequest(1, 9, 2, 0, 7, 0);
        auto* badStateMsg = asMsg(badStateReq);
        rc = handler.sensorEvent(
            badStateMsg, badStateReq.size() - sizeof(pldm_msg_hdr),
            PLDM_PLATFORM_EVENT_MESSAGE_FORMAT_VERSION, 9,
            PLDM_PLATFORM_EVENT_MESSAGE_MIN_REQ_BYTES, platformEventStatus);
        EXPECT_EQ(rc, PLDM_ERROR_INVALID_DATA);

        auto validReq = makeSensorEventRequest(1, 9, 2, 0, 1, 0);
        auto* validMsg = asMsg(validReq);
        rc = handler.sensorEvent(
            validMsg, validReq.size() - sizeof(pldm_msg_hdr),
            PLDM_PLATFORM_EVENT_MESSAGE_FORMAT_VERSION, 9,
            PLDM_PLATFORM_EVENT_MESSAGE_MIN_REQ_BYTES, platformEventStatus);
        EXPECT_TRUE(rc == PLDM_SUCCESS || rc == PLDM_ERROR);
    }

    pldm_entity_association_tree_destroy(entityTree);
    pldm_entity_association_tree_destroy(bmcEntityTree);
    pldm_pdr_destroy(hostRepo);
    pldm_pdr_destroy(platformRepo);
}

TEST(PlatformHandlerWrapper, getPdrHostFruOemCoveragePaths)
{
    using namespace std::chrono_literals;

    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(_, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(Return("foo.bar"));

    auto platformRepo = pldm_pdr_init();
    auto hostRepo = pldm_pdr_init();
    auto fruRepo = pldm_pdr_init();
    auto hostEntityTree = pldm_entity_association_tree_init();
    auto hostBmcEntityTree = pldm_entity_association_tree_init();
    auto fruEntityTree = pldm_entity_association_tree_init();
    auto fruBmcEntityTree = pldm_entity_association_tree_init();

    auto event = sdeventplus::Event::get_default();
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler(
        nullptr, event, instanceIdDb, false, 1s, 1, 5ms);
    StubOemPlatformHandler oemHandler{};
    pldm::responder::fru::Handler fruHandler(
        "./fru_jsons/good", "./fru_jsons/fru_master/fru_master.json", fruRepo,
        fruEntityTree, fruBmcEntityTree);

    {
        pldm::HostPDRHandler hostPDRHandler(
            0, 8, event, hostRepo, "./event_jsons/good", hostEntityTree,
            hostBmcEntityTree, instanceIdDb, &reqHandler);

        hostPDRHandler.setHostFirmwareCondition();
        std::vector<uint8_t> versionResp(
            sizeof(pldm_msg_hdr) + PLDM_GET_VERSION_RESP_BYTES, 0);
        auto* versionMsg = asMsg(versionResp);
        ver32_t version{0x00, 0xf0, 0xf0, 0xf1};
        ASSERT_EQ(encode_get_version_resp(0, PLDM_SUCCESS, 0,
                                          PLDM_START_AND_END, &version,
                                          sizeof(pldm_version), versionMsg),
                  PLDM_SUCCESS);
        for (uint8_t instance = 0; instance < 32; ++instance)
        {
            reqHandler.handleResponse(8, instance, PLDM_BASE,
                                      PLDM_GET_PLDM_VERSION, versionMsg,
                                      versionResp.size());
        }

        Handler handler(&mockedUtils, "./pdr_jsons/state_sensor/good",
                        platformRepo, &hostPDRHandler, nullptr, &fruHandler,
                        &oemHandler, event, true);

        std::vector<uint8_t> request(
            sizeof(pldm_msg_hdr) + PLDM_GET_PDR_REQ_BYTES, 0);
        auto* msg = asMsg(request);
        auto* pdrReq = reinterpret_cast<pldm_get_pdr_req*>(msg->payload);
        pdrReq->request_count = 64;

        oemHandler.checkBMCStateResult = PLDM_ERROR;
        auto notReady = handler.getPDR(msg, PLDM_GET_PDR_REQ_BYTES);
        EXPECT_EQ(completionCode(notReady), PLDM_ERROR_NOT_READY);
        EXPECT_GE(oemHandler.checkBMCStateCalls, 1u);

        oemHandler.checkBMCStateResult = PLDM_SUCCESS;
        auto success = handler.getPDR(msg, PLDM_GET_PDR_REQ_BYTES);
        EXPECT_EQ(completionCode(success), PLDM_SUCCESS);
        EXPECT_GE(oemHandler.buildOEMPDRCalls, 1u);
        EXPECT_FALSE(hostPDRHandler.tlPDRInfo.empty());

        EXPECT_NO_THROW(handler.getAssociateEntityMap());
    }

    pldm_entity_association_tree_destroy(hostEntityTree);
    pldm_entity_association_tree_destroy(hostBmcEntityTree);
    pldm_entity_association_tree_destroy(fruEntityTree);
    pldm_entity_association_tree_destroy(fruBmcEntityTree);
    pldm_pdr_destroy(hostRepo);
    pldm_pdr_destroy(fruRepo);
    pldm_pdr_destroy(platformRepo);
}

TEST(PlatformHandlerWrapper, getAssociateEntityMapNullFruThrows)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(_, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(Return("foo.bar"));

    auto pdrRepo = pldm_pdr_init();
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_sensor/good", pdrRepo,
                    nullptr, nullptr, nullptr, nullptr, event);

    EXPECT_THROW(
        handler.getAssociateEntityMap(),
        sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure);

    pldm_pdr_destroy(pdrRepo);
}

TEST(PlatformHandlerWrapper, heartbeatEventWithOemCoveragePath)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(_, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(Return("foo.bar"));

    auto pdrRepo = pldm_pdr_init();
    auto event = sdeventplus::Event::get_default();
    StubOemPlatformHandler oemHandler{};
    Handler handler(&mockedUtils, "./pdr_jsons/state_effecter/good", pdrRepo,
                    nullptr, nullptr, nullptr, &oemHandler, event);

    const uint8_t eventData = 0;
    constexpr size_t payloadLength =
        PLDM_PLATFORM_EVENT_MESSAGE_MIN_REQ_BYTES + sizeof(eventData);
    std::vector<uint8_t> request(sizeof(pldm_msg_hdr) + payloadLength);
    auto* msg = asMsg(request);
    ASSERT_EQ(encode_platform_event_message_req(
                  1, PLDM_PLATFORM_EVENT_MESSAGE_FORMAT_VERSION, 9,
                  PLDM_HEARTBEAT_TIMER_ELAPSED_EVENT, &eventData,
                  sizeof(eventData), msg, payloadLength),
              PLDM_SUCCESS);
    auto response = handler.platformEventMessage(msg, payloadLength);
    EXPECT_EQ(completionCode(response), PLDM_SUCCESS);
    EXPECT_EQ(oemHandler.resetWatchDogCalls, 1u);

    pldm_pdr_destroy(pdrRepo);
}

TEST(EventParser, stateSensorEntryOrderingCoverage)
{
    pldm::responder::events::StateSensorEntry entryA{1, 64, 1, 0, 0, false};
    pldm::responder::events::StateSensorEntry entryB{1, 64, 1, 1, 0, false};

    std::set<pldm::responder::events::StateSensorEntry> entries;
    entries.insert(entryB);
    entries.insert(entryA);

    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries.begin()->sensorOffset, 0u);
}

TEST(EventParser, stateSensorEntryOrderingSkipContainerCoverage)
{
    pldm::responder::events::StateSensorEntry entryA{1, 64, 1, 0, 0, true};
    pldm::responder::events::StateSensorEntry entryB{2, 64, 1, 1, 0, true};

    std::set<pldm::responder::events::StateSensorEntry> entries;
    entries.insert(entryB);
    entries.insert(entryA);

    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries.begin()->sensorOffset, 0u);
    EXPECT_TRUE(entryA < entryB);
}
