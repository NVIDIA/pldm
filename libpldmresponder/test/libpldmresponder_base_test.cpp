#include "common/instance_id.hpp"
#include "common/transport.hpp"
#include "common/utils.hpp"
#include "libpldmresponder/base.hpp"
#include "test/pldmd_coverage_hooks.hpp"
#include "test/test_instance_id.hpp"

#include <libpldm/base.h>
#include <libpldm/platform.h>

#include <sdeventplus/event.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pldm::responder;
using ::testing::_;
using ::testing::Return;

namespace
{

class MockPldmTransport : public PldmTransport
{
  public:
    MockPldmTransport() : PldmTransport(NoInit{}) {}

    MOCK_METHOD(pldm_requester_rc_t, sendMsg,
                (pldm_tid_t tid, const void* tx, size_t len), (override));
};

} // namespace

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

    void buildOEMPDR(pldm::responder::pdr_utils::Repo&) override {}

    void checkAndDisableWatchDog() override
    {
        ++checkAndDisableWatchDogCount;
    }

    bool watchDogRunning() override
    {
        return false;
    }

    void resetWatchDogTimer() override {}

    void disableWatchDogTimer() override {}

    void countSetEventReceiver() override
    {
        ++countSetEventReceiverCalls;
    }

    int checkBMCState() override
    {
        return PLDM_SUCCESS;
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

    size_t checkAndDisableWatchDogCount = 0;
    size_t countSetEventReceiverCalls = 0;
};

class TestBaseCommands : public testing::Test
{
  protected:
    TestBaseCommands() : event(sdeventplus::Event::get_default()) {}

    uint8_t mctpEid = 0;
    TestInstanceIdDb instanceIdDb;
    sdeventplus::Event event;
};

TEST_F(TestBaseCommands, testPLDMTypesGoodRequest)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr)> requestPayload{};
    auto request = reinterpret_cast<pldm_msg*>(requestPayload.data());
    // payload length will be 0 in this case
    size_t requestPayloadLength = 0;
    base::Handler handler(mctpEid, instanceIdDb, event, nullptr, nullptr);
    auto response = handler.getPLDMTypes(request, requestPayloadLength);
    // Need to support OEM type.
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    uint8_t* payload_ptr = responsePtr->payload;
    ASSERT_EQ(payload_ptr[0], 0);
    ASSERT_EQ(payload_ptr[1], 29); // 0b11101 see DSP0240 table11
    ASSERT_EQ(payload_ptr[2], 0);
}

TEST_F(TestBaseCommands, testGetPLDMCommandsGoodRequest)
{
    // Need to support OEM type commands.
    std::array<uint8_t, sizeof(pldm_msg_hdr) + PLDM_GET_COMMANDS_REQ_BYTES>
        requestPayload{};
    auto request = reinterpret_cast<pldm_msg*>(requestPayload.data());
    size_t requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);
    base::Handler handler(mctpEid, instanceIdDb, event, nullptr, nullptr);
    auto response = handler.getPLDMCommands(request, requestPayloadLength);
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    uint8_t* payload_ptr = responsePtr->payload;
    ASSERT_EQ(payload_ptr[0], 0);
    ASSERT_EQ(payload_ptr[1], 60); // 60 = 0b111100
    ASSERT_EQ(payload_ptr[2], 0);
}

TEST_F(TestBaseCommands, testGetPLDMCommandsBadRequest)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr) + PLDM_GET_COMMANDS_REQ_BYTES>
        requestPayload{};
    auto request = reinterpret_cast<pldm_msg*>(requestPayload.data());

    request->payload[0] = 0xFF;
    size_t requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);
    base::Handler handler(mctpEid, instanceIdDb, event, nullptr, nullptr);
    auto response = handler.getPLDMCommands(request, requestPayloadLength);
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    uint8_t* payload_ptr = responsePtr->payload;
    ASSERT_EQ(payload_ptr[0], PLDM_ERROR_INVALID_PLDM_TYPE);
}

TEST_F(TestBaseCommands, testGetPLDMCommandsInvalidLength)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr)> requestPayload{};
    auto request = reinterpret_cast<pldm_msg*>(requestPayload.data());

    base::Handler handler(mctpEid, instanceIdDb, event, nullptr, nullptr);
    auto response = handler.getPLDMCommands(request, 0);
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    ASSERT_EQ(responsePtr->payload[0], PLDM_ERROR_INVALID_LENGTH);
}

TEST_F(TestBaseCommands, testGetPLDMVersionGoodRequest)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr) + PLDM_GET_VERSION_REQ_BYTES>
        requestPayload{};
    auto request = reinterpret_cast<pldm_msg*>(requestPayload.data());
    size_t requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);

    uint8_t pldmType = PLDM_BASE;
    uint32_t transferHandle = 0x0;
    uint8_t flag = PLDM_GET_FIRSTPART;
    uint8_t retFlag = PLDM_START_AND_END;
    ver32_t version = {0x00, 0xF0, 0xF0, 0xF1};

    auto rc =
        encode_get_version_req(0, transferHandle, flag, pldmType, request);

    ASSERT_EQ(0, rc);

    base::Handler handler(mctpEid, instanceIdDb, event, nullptr, nullptr);
    auto response = handler.getPLDMVersion(request, requestPayloadLength);
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());

    ASSERT_EQ(responsePtr->payload[0], 0);
    ASSERT_EQ(0, memcmp(responsePtr->payload + sizeof(responsePtr->payload[0]),
                        &transferHandle, sizeof(transferHandle)));
    ASSERT_EQ(0, memcmp(responsePtr->payload + sizeof(responsePtr->payload[0]) +
                            sizeof(transferHandle),
                        &retFlag, sizeof(flag)));
    ASSERT_EQ(0, memcmp(responsePtr->payload + sizeof(responsePtr->payload[0]) +
                            sizeof(transferHandle) + sizeof(flag),
                        &version, sizeof(version)));
}

TEST_F(TestBaseCommands, testGetPLDMVersionBadRequest)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr) + PLDM_GET_VERSION_REQ_BYTES>
        requestPayload{};
    auto request = reinterpret_cast<pldm_msg*>(requestPayload.data());
    size_t requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);

    uint8_t pldmType = 7;
    uint32_t transferHandle = 0x0;
    uint8_t flag = PLDM_GET_FIRSTPART;

    auto rc =
        encode_get_version_req(0, transferHandle, flag, pldmType, request);

    ASSERT_EQ(0, rc);

    base::Handler handler(mctpEid, instanceIdDb, event, nullptr, nullptr);
    auto response = handler.getPLDMVersion(request, requestPayloadLength - 1);
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());

    ASSERT_EQ(responsePtr->payload[0], PLDM_ERROR_INVALID_LENGTH);

    request = reinterpret_cast<pldm_msg*>(requestPayload.data());
    requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);

    rc = encode_get_version_req(0, transferHandle, flag, pldmType, request);

    ASSERT_EQ(0, rc);

    response = handler.getPLDMVersion(request, requestPayloadLength);
    responsePtr = reinterpret_cast<pldm_msg*>(response.data());

    ASSERT_EQ(responsePtr->payload[0], PLDM_ERROR_INVALID_PLDM_TYPE);
}

TEST_F(TestBaseCommands, testGetTIDGoodRequest)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr)> requestPayload{};
    auto request = reinterpret_cast<pldm_msg*>(requestPayload.data());
    size_t requestPayloadLength = 0;

    base::Handler handler(mctpEid, instanceIdDb, event, nullptr, nullptr);
    auto response = handler.getTID(request, requestPayloadLength);

    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    uint8_t* payload = responsePtr->payload;

    ASSERT_EQ(payload[0], 0);
    ASSERT_EQ(payload[1], 1);
}

TEST_F(TestBaseCommands, testPLDMTypesInvalidInstanceIdDiesOnEncodeFailure)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr)> requestPayload{};
    auto* request = reinterpret_cast<pldm_msg*>(requestPayload.data());
    auto run = [&] {
        pldm::test::coverage::ScopedHookStateReset hooks;
        pldm::test::coverage::setForcePackFailure();
        base::Handler handler(mctpEid, instanceIdDb, event, nullptr, nullptr);
        static_cast<void>(handler.getPLDMTypes(request, 0));
    };

    EXPECT_DEATH(run(), ".*");
}

TEST_F(TestBaseCommands,
       testGetPLDMCommandsInvalidInstanceIdDiesOnEncodeFailure)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr) + PLDM_GET_COMMANDS_REQ_BYTES>
        requestPayload{};
    auto* request = reinterpret_cast<pldm_msg*>(requestPayload.data());
    size_t requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);
    ASSERT_EQ(encode_get_commands_req(1, PLDM_BASE, ver32_t{}, request),
              PLDM_SUCCESS);
    auto run = [&] {
        pldm::test::coverage::ScopedHookStateReset hooks;
        pldm::test::coverage::setForcePackFailure();
        base::Handler handler(mctpEid, instanceIdDb, event, nullptr, nullptr);
        static_cast<void>(
            handler.getPLDMCommands(request, requestPayloadLength));
    };

    EXPECT_DEATH(run(), ".*");
}

TEST_F(TestBaseCommands, testGetPLDMVersionInvalidInstanceIdDiesOnEncodeFailure)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr) + PLDM_GET_VERSION_REQ_BYTES>
        requestPayload{};
    auto* request = reinterpret_cast<pldm_msg*>(requestPayload.data());
    size_t requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);

    ASSERT_EQ(
        encode_get_version_req(1, 0, PLDM_GET_FIRSTPART, PLDM_BASE, request),
        PLDM_SUCCESS);
    auto run = [&] {
        pldm::test::coverage::ScopedHookStateReset hooks;
        pldm::test::coverage::setForcePackFailure();
        base::Handler handler(mctpEid, instanceIdDb, event, nullptr, nullptr);
        static_cast<void>(
            handler.getPLDMVersion(request, requestPayloadLength));
    };

    EXPECT_DEATH(run(), ".*");
}

TEST_F(TestBaseCommands, testGetTIDInvalidInstanceIdDiesOnEncodeFailure)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr)> requestPayload{};
    auto* request = reinterpret_cast<pldm_msg*>(requestPayload.data());
    auto run = [&] {
        pldm::test::coverage::ScopedHookStateReset hooks;
        pldm::test::coverage::setForcePackFailure();
        base::Handler handler(mctpEid, instanceIdDb, event, nullptr, nullptr);
        static_cast<void>(handler.getTID(request, 0));
    };

    EXPECT_DEATH(run(), ".*");
}

TEST_F(TestBaseCommands, testGetTIDDeferredSetEventReceiverSuccess)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr)> requestPayload{};
    auto request = reinterpret_cast<pldm_msg*>(requestPayload.data());
    request->hdr.instance_id = 7;

    StubOemPlatformHandler oemHandler{};
    MockPldmTransport transport{};
    uint8_t deferredInstanceId = 0xFF;

    EXPECT_CALL(transport, sendMsg(mctpEid, _, _))
        .WillOnce([&](pldm_tid_t tid, const void* tx,
                      size_t len) -> pldm_requester_rc_t {
            EXPECT_EQ(tid, mctpEid);
            auto* msg = reinterpret_cast<const pldm_msg*>(tx);
            deferredInstanceId = msg->hdr.instance_id;

            uint8_t eventMessageGlobalEnable = 0;
            uint8_t transportProtocolType = 0;
            uint8_t eventReceiverAddressInfo = 0;
            uint16_t heartbeatTimer = 0;
            EXPECT_EQ(decode_set_event_receiver_req(
                          msg, len - sizeof(pldm_msg_hdr),
                          &eventMessageGlobalEnable, &transportProtocolType,
                          &eventReceiverAddressInfo, &heartbeatTimer),
                      PLDM_SUCCESS);
            EXPECT_EQ(eventMessageGlobalEnable,
                      PLDM_EVENT_MESSAGE_GLOBAL_ENABLE_ASYNC_KEEP_ALIVE);
            EXPECT_EQ(transportProtocolType, PLDM_TRANSPORT_PROTOCOL_TYPE_MCTP);
            EXPECT_EQ(eventReceiverAddressInfo,
                      pldm::responder::pdr::BmcMctpEid);
            EXPECT_EQ(heartbeatTimer, pldm::HEARTBEAT_TIMEOUT);
            return PLDM_REQUESTER_SUCCESS;
        });

    pldm::requester::Handler<pldm::requester::Request> requesterHandler(
        &transport, event, instanceIdDb, false, std::chrono::seconds(5), 0,
        std::chrono::milliseconds(1));
    base::Handler handler(mctpEid, instanceIdDb, event, &oemHandler,
                          &requesterHandler);

    auto response = handler.getTID(request, 0);
    auto* responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    EXPECT_EQ(responsePtr->payload[0], PLDM_SUCCESS);
    EXPECT_EQ(responsePtr->payload[1], 1);

    event.run(std::nullopt);
    EXPECT_EQ(oemHandler.countSetEventReceiverCalls, 1u);
    EXPECT_EQ(oemHandler.checkAndDisableWatchDogCount, 1u);
    ASSERT_NE(deferredInstanceId, 0xFF);

    std::array<uint8_t,
               sizeof(pldm_msg_hdr) + PLDM_SET_EVENT_RECEIVER_RESP_BYTES>
        responsePayload{};
    auto* setEventReceiverResponse =
        reinterpret_cast<pldm_msg*>(responsePayload.data());
    ASSERT_EQ(encode_set_event_receiver_resp(deferredInstanceId, PLDM_SUCCESS,
                                             setEventReceiverResponse),
              PLDM_SUCCESS);

    requesterHandler.handleResponse(
        mctpEid, deferredInstanceId, PLDM_PLATFORM, PLDM_SET_EVENT_RECEIVER,
        setEventReceiverResponse, PLDM_SET_EVENT_RECEIVER_RESP_BYTES);
}

TEST_F(TestBaseCommands, testGetTIDDeferredSetEventReceiverSendFailure)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr)> requestPayload{};
    auto request = reinterpret_cast<pldm_msg*>(requestPayload.data());
    request->hdr.instance_id = 1;

    StubOemPlatformHandler oemHandler{};
    MockPldmTransport transport{};
    EXPECT_CALL(transport, sendMsg(mctpEid, _, _))
        .WillOnce(Return(PLDM_REQUESTER_SEND_FAIL));

    pldm::requester::Handler<pldm::requester::Request> requesterHandler(
        &transport, event, instanceIdDb, false, std::chrono::seconds(5), 0,
        std::chrono::milliseconds(1));
    base::Handler handler(mctpEid, instanceIdDb, event, &oemHandler,
                          &requesterHandler);

    auto response = handler.getTID(request, 0);
    EXPECT_EQ(reinterpret_cast<pldm_msg*>(response.data())->payload[0],
              PLDM_SUCCESS);

    event.run(std::nullopt);

    EXPECT_EQ(oemHandler.countSetEventReceiverCalls, 1u);
    EXPECT_EQ(oemHandler.checkAndDisableWatchDogCount, 1u);
}

TEST_F(TestBaseCommands, testGetTIDDeferredSetEventReceiverNullResponse)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr)> requestPayload{};
    auto request = reinterpret_cast<pldm_msg*>(requestPayload.data());

    MockPldmTransport transport{};
    uint8_t deferredInstanceId = 0xFF;
    EXPECT_CALL(transport, sendMsg(mctpEid, _, _))
        .WillOnce(
            [&](pldm_tid_t, const void* tx, size_t) -> pldm_requester_rc_t {
                deferredInstanceId =
                    reinterpret_cast<const pldm_msg*>(tx)->hdr.instance_id;
                return PLDM_REQUESTER_SUCCESS;
            });

    pldm::requester::Handler<pldm::requester::Request> requesterHandler(
        &transport, event, instanceIdDb, false, std::chrono::seconds(5), 0,
        std::chrono::milliseconds(1));
    base::Handler handler(mctpEid, instanceIdDb, event, nullptr,
                          &requesterHandler);

    auto response = handler.getTID(request, 0);
    EXPECT_EQ(reinterpret_cast<pldm_msg*>(response.data())->payload[0],
              PLDM_SUCCESS);

    event.run(std::nullopt);
    ASSERT_NE(deferredInstanceId, 0xFF);

    requesterHandler.handleResponse(mctpEid, deferredInstanceId, PLDM_PLATFORM,
                                    PLDM_SET_EVENT_RECEIVER, nullptr, 0);
}

TEST_F(TestBaseCommands, testGetTIDDeferredSetEventReceiverDecodeFailure)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr)> requestPayload{};
    auto request = reinterpret_cast<pldm_msg*>(requestPayload.data());

    MockPldmTransport transport{};
    uint8_t deferredInstanceId = 0xFF;
    EXPECT_CALL(transport, sendMsg(mctpEid, _, _))
        .WillOnce(
            [&](pldm_tid_t, const void* tx, size_t) -> pldm_requester_rc_t {
                deferredInstanceId =
                    reinterpret_cast<const pldm_msg*>(tx)->hdr.instance_id;
                return PLDM_REQUESTER_SUCCESS;
            });

    pldm::requester::Handler<pldm::requester::Request> requesterHandler(
        &transport, event, instanceIdDb, false, std::chrono::seconds(5), 0,
        std::chrono::milliseconds(1));
    base::Handler handler(mctpEid, instanceIdDb, event, nullptr,
                          &requesterHandler);

    auto response = handler.getTID(request, 0);
    EXPECT_EQ(reinterpret_cast<pldm_msg*>(response.data())->payload[0],
              PLDM_SUCCESS);

    event.run(std::nullopt);
    ASSERT_NE(deferredInstanceId, 0xFF);

    std::array<uint8_t, sizeof(pldm_msg_hdr)> malformedResponse{};
    requesterHandler.handleResponse(
        mctpEid, deferredInstanceId, PLDM_PLATFORM, PLDM_SET_EVENT_RECEIVER,
        reinterpret_cast<pldm_msg*>(malformedResponse.data()),
        malformedResponse.size() - 1);
}

TEST_F(TestBaseCommands, testGetTIDDeferredSetEventReceiverErrorCompletionCode)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr)> requestPayload{};
    auto request = reinterpret_cast<pldm_msg*>(requestPayload.data());

    MockPldmTransport transport{};
    uint8_t deferredInstanceId = 0xFF;
    EXPECT_CALL(transport, sendMsg(mctpEid, _, _))
        .WillOnce(
            [&](pldm_tid_t, const void* tx, size_t) -> pldm_requester_rc_t {
                deferredInstanceId =
                    reinterpret_cast<const pldm_msg*>(tx)->hdr.instance_id;
                return PLDM_REQUESTER_SUCCESS;
            });

    pldm::requester::Handler<pldm::requester::Request> requesterHandler(
        &transport, event, instanceIdDb, false, std::chrono::seconds(5), 0,
        std::chrono::milliseconds(1));
    base::Handler handler(mctpEid, instanceIdDb, event, nullptr,
                          &requesterHandler);

    auto response = handler.getTID(request, 0);
    EXPECT_EQ(reinterpret_cast<pldm_msg*>(response.data())->payload[0],
              PLDM_SUCCESS);

    event.run(std::nullopt);
    ASSERT_NE(deferredInstanceId, 0xFF);

    std::array<uint8_t,
               sizeof(pldm_msg_hdr) + PLDM_SET_EVENT_RECEIVER_RESP_BYTES>
        responsePayload{};
    auto* setEventReceiverResponse =
        reinterpret_cast<pldm_msg*>(responsePayload.data());
    ASSERT_EQ(encode_set_event_receiver_resp(deferredInstanceId, PLDM_ERROR,
                                             setEventReceiverResponse),
              PLDM_SUCCESS);

    requesterHandler.handleResponse(
        mctpEid, deferredInstanceId, PLDM_PLATFORM, PLDM_SET_EVENT_RECEIVER,
        setEventReceiverResponse, PLDM_SET_EVENT_RECEIVER_RESP_BYTES);
}

TEST_F(TestBaseCommands, testProcessSetEventReceiverEncodeFailureCoverage)
{
    StubOemPlatformHandler oemHandler{};
    MockPldmTransport transport{};
    EXPECT_CALL(transport, sendMsg(_, _, _)).Times(0);

    pldm::requester::Handler<pldm::requester::Request> requesterHandler(
        &transport, event, instanceIdDb, false, std::chrono::seconds(5), 0,
        std::chrono::milliseconds(1));
    base::Handler handler(mctpEid, instanceIdDb, event, &oemHandler,
                          &requesterHandler);
    sdeventplus::source::Defer source(event,
                                      [](sdeventplus::source::EventBase&) {});

    pldm::test::coverage::ScopedHookStateReset hooks;
    pldm::test::coverage::setForcePackFailure();

    EXPECT_NO_THROW(handler.processSetEventReceiver(source));
    EXPECT_EQ(oemHandler.countSetEventReceiverCalls, 0u);
    EXPECT_EQ(oemHandler.checkAndDisableWatchDogCount, 0u);
}
