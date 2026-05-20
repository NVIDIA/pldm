#include "common/instance_id.hpp"
#include "common/types.hpp"
#include "common/utils.hpp"
#include "mock_request.hpp"
#include "requester/handler.hpp"
#include "test/test_instance_id.hpp"

#include <dlfcn.h>
#include <libpldm/base.h>
#include <libpldm/transport.h>
#include <systemd/sd-event.h>

#include <sdbusplus/async.hpp>

#include <cerrno>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pldm::requester;
using namespace std::chrono;

using ::testing::AtLeast;
using ::testing::Between;
using ::testing::Exactly;
using ::testing::NiceMock;
using ::testing::Return;

static bool g_failHandlerTimerStart = false;
static bool g_failHandlerTimerStop = false;

extern "C" int sd_event_source_set_time_relative(sd_event_source* source,
                                                 uint64_t usec)
{
    if (g_failHandlerTimerStart)
    {
        return -EINVAL;
    }

    using Fn = int (*)(sd_event_source*, uint64_t);
    static auto realFn = reinterpret_cast<Fn>(
        dlsym(RTLD_NEXT, "sd_event_source_set_time_relative"));
    if (realFn == nullptr)
    {
        return -EINVAL;
    }

    return realFn(source, usec);
}

extern "C" int sd_event_source_set_enabled(sd_event_source* source, int mode)
{
    if (g_failHandlerTimerStart && mode != SD_EVENT_OFF)
    {
        return -EINVAL;
    }

    if (g_failHandlerTimerStop && mode == SD_EVENT_OFF)
    {
        return -EINVAL;
    }

    using Fn = int (*)(sd_event_source*, int);
    static auto realFn =
        reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "sd_event_source_set_enabled"));
    if (realFn == nullptr)
    {
        return -EINVAL;
    }

    return realFn(source, mode);
}

/** @brief A Request implementation whose send() always fails.
 *         Used to test pollEndpointQueue error paths when request->start()
 *         returns an error.
 */
class FailingSendRequest : public RequestRetryTimer
{
  public:
    FailingSendRequest(PldmTransport* /*pldmTransport*/, mctp_eid_t /*eid*/,
                       sdeventplus::Event& event,
                       pldm::Request&& /*requestMsg*/, uint8_t numRetries,
                       std::chrono::milliseconds responseTimeOut,
                       bool /*verbose*/) :
        RequestRetryTimer(event, numRetries, responseTimeOut)
    {}

    int send() const override
    {
        return PLDM_ERROR;
    }
};

class HandlerTest : public testing::Test
{
  protected:
    HandlerTest() : event(sdeventplus::Event::get_default()), instanceIdDb() {}

    int fd = 0;
    mctp_eid_t eid = 0;
    PldmTransport* pldmTransport = nullptr;
    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;

    /** @brief This function runs the sd_event_run in a loop till all the events
     *         in the testcase are dispatched and exits when there are no events
     *         for the timeout time.
     *
     *  @param[in] timeout - maximum time to wait for an event
     */
    void waitEventExpiry(milliseconds timeout)
    {
        while (1)
        {
            auto sleepTime = duration_cast<microseconds>(timeout);
            // Returns 0 on timeout
            if (!sd_event_run(event.get(), sleepTime.count()))
            {
                break;
            }
        }
    }

  public:
    bool nullResponse = false;
    bool validResponse = false;
    int callbackCount = 0;
    bool response2 = false;

    void pldmResponseCallBack(mctp_eid_t /*eid*/, const pldm_msg* response,
                              size_t respMsgLen)
    {
        if (response == nullptr && respMsgLen == 0)
        {
            nullResponse = true;
        }
        else
        {
            validResponse = true;
        }
        callbackCount++;
    }
};

static exec::task<int> sendRecvRequestTaskInt(
    Handler<Request>& reqHandler, mctp_eid_t eid, uint8_t instanceId)
{
    pldm::Request request(sizeof(pldm_msg_hdr) + sizeof(uint8_t), 0);
    auto requestPtr = new (request.data()) pldm_msg;
    requestPtr->hdr.instance_id = instanceId;
    requestPtr->hdr.request = 1;

    const pldm_msg* responseMsg = nullptr;
    size_t responseLen = 0;
    int rc = PLDM_SUCCESS;
    std::tie(rc, responseMsg, responseLen) =
        co_await reqHandler.sendRecvMsg(eid, std::move(request));
    (void)responseMsg;
    (void)responseLen;
    co_return rc;
}

TEST(HandlerStandaloneTest, requestKeyHasher)
{
    RequestKey key{0x12, 0x34, 0x56, 0x78};
    RequestKeyHasher hasher;
    EXPECT_EQ(hasher(key), static_cast<size_t>(0x12345678));
}

TEST(HandlerStandaloneTest, requestKeyHasherDiverseValues)
{
    RequestKeyHasher hasher;
    for (uint8_t eidVal : {uint8_t(0x00), uint8_t(0x01), uint8_t(0x7F),
                           uint8_t(0x80), uint8_t(0xFF)})
    {
        for (uint8_t instVal : {uint8_t(0x00), uint8_t(0x01), uint8_t(0x55),
                                uint8_t(0xAA), uint8_t(0xFF)})
        {
            for (uint8_t typeVal : {uint8_t(0x00), uint8_t(0x01), uint8_t(0x33),
                                    uint8_t(0xCC), uint8_t(0xFF)})
            {
                for (uint8_t cmdVal :
                     {uint8_t(0x00), uint8_t(0x10), uint8_t(0x5A),
                      uint8_t(0xA5), uint8_t(0xFF)})
                {
                    RequestKey key{eidVal, instVal, typeVal, cmdVal};
                    auto expected = static_cast<size_t>(
                        (eidVal << 24) | (instVal << 16) | (typeVal << 8) |
                        cmdVal);
                    EXPECT_EQ(hasher(key), expected);
                }
            }
        }
    }
}

TEST_F(HandlerTest, singleRequestResponseScenario)
{
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(1), 2,
        milliseconds(100));
    pldm::Request request{};
    auto instanceIdResult = instanceIdDb.next(eid);
    ASSERT_TRUE(instanceIdResult);
    auto instanceId = instanceIdResult.value();
    EXPECT_EQ(instanceId, 0);
    auto rc = reqHandler.registerRequest(
        eid, instanceId, 0, 0, std::move(request),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    pldm::Response response(sizeof(pldm_msg_hdr) + sizeof(uint8_t));
    auto responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    reqHandler.handleResponse(eid, instanceId, 0, 0, responsePtr,
                              response.size());

    EXPECT_EQ(validResponse, true);
}

TEST_F(HandlerTest, singleRequestInstanceIdTimerExpired)
{
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(1), 2,
        milliseconds(100));
    pldm::Request request{};
    auto instanceIdResult = instanceIdDb.next(eid);
    ASSERT_TRUE(instanceIdResult);
    auto instanceId = instanceIdResult.value();
    EXPECT_EQ(instanceId, 0);
    auto rc = reqHandler.registerRequest(
        eid, instanceId, 0, 0, std::move(request),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    // Waiting for 500ms so that the instance ID expiry callback is invoked
    waitEventExpiry(milliseconds(500));

    EXPECT_EQ(nullResponse, true);
}

TEST_F(HandlerTest, singleRequestInstanceIdTimerExpiredTimerStopFails)
{
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(1), 2,
        milliseconds(100));
    pldm::Request request{};
    auto instanceId = instanceIdDb.next(eid).value();
    auto rc = reqHandler.registerRequest(
        eid, instanceId, 0, 0, std::move(request),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    g_failHandlerTimerStop = true;
    waitEventExpiry(milliseconds(500));
    g_failHandlerTimerStop = false;

    EXPECT_EQ(nullResponse, true);
}

TEST_F(HandlerTest, instanceIdExpiryCallBackUnknownKey)
{
#ifndef NDEBUG
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(1), 2,
        milliseconds(100));
    RequestKey key{eid, 0, 0, 0};
    EXPECT_DEATH(reqHandler.instanceIdExpiryCallBack(key), "");
#else
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(1), 2,
        milliseconds(100));
    RequestKey key{eid, 0, 0, 0};
    reqHandler.instanceIdExpiryCallBack(key);
#endif
}

TEST_F(HandlerTest, multipleRequestResponseScenario)
{
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(2), 2,
        milliseconds(100));
    pldm::Request request{};
    auto instanceIdResult = instanceIdDb.next(eid);
    ASSERT_TRUE(instanceIdResult);
    auto instanceId = instanceIdResult.value();
    EXPECT_EQ(instanceId, 0);
    auto rc = reqHandler.registerRequest(
        eid, instanceId, 0, 0, std::move(request),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    pldm::Request requestNxt{};
    auto instanceIdNxtResult = instanceIdDb.next(eid);
    ASSERT_TRUE(instanceIdNxtResult);
    auto instanceIdNxt = instanceIdNxtResult.value();
    EXPECT_EQ(instanceIdNxt, 1);
    rc = reqHandler.registerRequest(
        eid, instanceIdNxt, 0, 0, std::move(requestNxt),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    pldm::Response response(sizeof(pldm_msg_hdr) + sizeof(uint8_t));
    auto responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    reqHandler.handleResponse(eid, instanceId, 0, 0, responsePtr,
                              response.size());
    EXPECT_EQ(validResponse, true);
    EXPECT_EQ(callbackCount, 1);
    validResponse = false;

    // Waiting for 500ms and handle the response for the first request, to
    // simulate a delayed response for the first request
    waitEventExpiry(milliseconds(500));

    reqHandler.handleResponse(eid, instanceIdNxt, 0, 0, responsePtr,
                              response.size());

    EXPECT_EQ(validResponse, true);
    EXPECT_EQ(callbackCount, 2);
}

TEST_F(HandlerTest, singleRequestResponseScenarioUsingCoroutine)
{
    exec::async_scope scope;
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(1), 2,
        milliseconds(100));

    auto instanceIdResult = instanceIdDb.next(eid);
    ASSERT_TRUE(instanceIdResult);
    auto instanceId = instanceIdResult.value();
    EXPECT_EQ(instanceId, 0);

    scope.spawn(
        stdexec::just() | stdexec::let_value([&]() -> exec::task<void> {
            pldm::Request request(sizeof(pldm_msg_hdr) + sizeof(uint8_t), 0);
            const pldm_msg* responseMsg;
            size_t responseLen;
            int rc = PLDM_SUCCESS;

            auto requestPtr = new (request.data()) pldm_msg;
            requestPtr->hdr.instance_id = instanceId;

            try
            {
                std::tie(rc, responseMsg, responseLen) =
                    co_await reqHandler.sendRecvMsg(eid, std::move(request));
            }
            catch (...)
            {
                std::rethrow_exception(std::current_exception());
            }

            EXPECT_NE(responseLen, 0);

            this->pldmResponseCallBack(eid, responseMsg, responseLen);

            EXPECT_EQ(validResponse, true);
        }),
        exec::default_task_context<void>(stdexec::inline_scheduler{}));

    pldm::Response mockResponse(sizeof(pldm_msg_hdr) + sizeof(uint8_t), 0);
    auto mockResponsePtr =
        reinterpret_cast<const pldm_msg*>(mockResponse.data());
    reqHandler.handleResponse(eid, instanceId, 0, 0, mockResponsePtr,
                              mockResponse.size() - sizeof(pldm_msg_hdr));

    stdexec::sync_wait(scope.on_empty());
}

TEST_F(HandlerTest, singleRequestCancellationScenarioUsingCoroutine)
{
    exec::async_scope scope;
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(1), 2,
        milliseconds(100));
    auto instanceIdResult = instanceIdDb.next(eid);
    ASSERT_TRUE(instanceIdResult);
    auto instanceId = instanceIdResult.value();
    EXPECT_EQ(instanceId, 0);

    bool stopped = false;

    scope.spawn(
        stdexec::just() | stdexec::let_value([&]() -> exec::task<void> {
            pldm::Request request(sizeof(pldm_msg_hdr) + sizeof(uint8_t), 0);
            pldm::Response response;

            auto requestPtr = new (request.data()) pldm_msg;
            requestPtr->hdr.instance_id = instanceId;

            co_await reqHandler.sendRecvMsg(eid, std::move(request));

            EXPECT_TRUE(false); // unreachable
        }) | stdexec::upon_stopped([&] { stopped = true; }),
        exec::default_task_context<void>(stdexec::inline_scheduler{}));

    scope.request_stop();

    EXPECT_TRUE(stopped);

    stdexec::sync_wait(scope.on_empty());
}

TEST_F(HandlerTest, registerRequestDuplicateKey)
{
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(2), 2,
        milliseconds(100));
    pldm::Request request{};
    auto instanceId = instanceIdDb.next(eid).value();

    auto rc = reqHandler.registerRequest(
        eid, instanceId, 0, 0, std::move(request),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    // Respond to the first request to move it into handlers map
    pldm::Response response(sizeof(pldm_msg_hdr) + sizeof(uint8_t));
    auto responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    reqHandler.handleResponse(eid, instanceId, 0, 0, responsePtr,
                              response.size());

    // Now register a second request with the SAME instanceId (re-obtained)
    // and respond immediately, then try a duplicate
    auto instanceId2 = instanceIdDb.next(eid).value();
    pldm::Request request2{};
    rc = reqHandler.registerRequest(
        eid, instanceId2, 0, 0, std::move(request2),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    // Try to register again with the same key - should fail since the
    // request is already active in the handlers map
    pldm::Request request3{};
    rc = reqHandler.registerRequest(
        eid, instanceId2, 0, 0, std::move(request3),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_ERROR);
}

TEST_F(HandlerTest, unregisterRequestFromQueue)
{
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(5), 2,
        milliseconds(100));

    // Register first request - it becomes active (sent)
    pldm::Request request1{};
    auto instanceId1 = instanceIdDb.next(eid).value();
    auto rc = reqHandler.registerRequest(
        eid, instanceId1, 0, 0, std::move(request1),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    // Register second request - it goes to queue (endpoint is busy)
    pldm::Request request2{};
    auto instanceId2 = instanceIdDb.next(eid).value();
    rc = reqHandler.registerRequest(
        eid, instanceId2, 0, 1, std::move(request2),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    // Unregister the second request from the queue (not yet sent)
    rc = reqHandler.unregisterRequest(eid, instanceId2, 0, 1);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    // Clean up by responding to the first (active) request
    pldm::Response response(sizeof(pldm_msg_hdr) + sizeof(uint8_t));
    auto responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    reqHandler.handleResponse(eid, instanceId1, 0, 0, responsePtr,
                              response.size());
}

TEST_F(HandlerTest, unregisterRequestNotFound)
{
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(2), 2,
        milliseconds(100));

    // Try to unregister a request that doesn't exist
    auto rc = reqHandler.unregisterRequest(eid, 0, 0, 0);
    EXPECT_EQ(rc, PLDM_ERROR);
}

TEST_F(HandlerTest, unregisterActiveRequest)
{
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(2), 2,
        milliseconds(100));

    pldm::Request request{};
    auto instanceId = instanceIdDb.next(eid).value();
    auto rc = reqHandler.registerRequest(
        eid, instanceId, 0, 0, std::move(request),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    // Unregister the active (already sent) request
    rc = reqHandler.unregisterRequest(eid, instanceId, 0, 0);
    EXPECT_EQ(rc, PLDM_SUCCESS);
}

TEST_F(HandlerTest, unregisterActiveRequestTimerStopFails)
{
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(2), 2,
        milliseconds(100));

    pldm::Request request{};
    auto instanceId = instanceIdDb.next(eid).value();
    auto rc = reqHandler.registerRequest(
        eid, instanceId, 0, 0, std::move(request),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    g_failHandlerTimerStop = true;
    rc = reqHandler.unregisterRequest(eid, instanceId, 0, 0);
    g_failHandlerTimerStop = false;
    EXPECT_EQ(rc, PLDM_SUCCESS);
}

TEST_F(HandlerTest, unregisterRequestNotInQueue)
{
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(2), 2,
        milliseconds(100));

    // Register first request - it becomes active
    pldm::Request request1{};
    auto instanceId1 = instanceIdDb.next(eid).value();
    auto rc = reqHandler.registerRequest(
        eid, instanceId1, 0, 0, std::move(request1),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    // Try to unregister a different instance ID that's not in the queue
    auto instanceId2 = instanceIdDb.next(eid).value();
    rc = reqHandler.unregisterRequest(eid, instanceId2, 0, 1);
    EXPECT_EQ(rc, PLDM_ERROR);
}

TEST_F(HandlerTest, handleResponseUnknownKey)
{
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(1), 2,
        milliseconds(100));

    // Handle a response for a key that was never registered - should be a no-op
    pldm::Response response(sizeof(pldm_msg_hdr) + sizeof(uint8_t));
    auto responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    reqHandler.handleResponse(eid, 99, 0, 0, responsePtr, response.size());

    EXPECT_EQ(validResponse, false);
    EXPECT_EQ(nullResponse, false);
}

TEST_F(HandlerTest, handleResponseTimerStopFails)
{
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(1), 2,
        milliseconds(100));
    pldm::Request request{};
    auto instanceId = instanceIdDb.next(eid).value();
    auto rc = reqHandler.registerRequest(
        eid, instanceId, 0, 0, std::move(request),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    pldm::Response response(sizeof(pldm_msg_hdr) + sizeof(uint8_t));
    auto responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    g_failHandlerTimerStop = true;
    reqHandler.handleResponse(eid, instanceId, 0, 0, responsePtr,
                              response.size());
    g_failHandlerTimerStop = false;

    EXPECT_EQ(validResponse, true);
}

TEST_F(HandlerTest, storeAndGetTransportError)
{
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(1), 2,
        milliseconds(100));

    // Verify no transport error initially
    EXPECT_FALSE(reqHandler.hasTransportError(10));
    EXPECT_EQ(reqHandler.getTransportError(10), std::nullopt);

    // Store a TX transport error (direction=TX, so affectedEid=destEid)
    pldm::transport::MctpError mctpError{};
    mctpError.error_code = 110; // ETIMEDOUT
    mctpError.direction = MCTP_DIR_TX;
    mctpError.binding = 2;      // I2C
    mctpError.src_eid = 1;
    mctpError.dest_eid = 10;
    mctpError.timestamp_ns = 123456789;
    mctpError.msg_type = MCTP_MSG_TYPE_PLDM;
    mctpError.payload_len = 3;
    mctpError.payload[0] = 0x01; // MCTP msg type
    mctpError.payload[1] = 0x05; // PLDM type 5 (FWUP)
    mctpError.payload[2] = 0x00;

    reqHandler.storeTransportError(mctpError);

    EXPECT_TRUE(reqHandler.hasTransportError(10));

    auto errOpt = reqHandler.getTransportError(10);
    ASSERT_TRUE(errOpt.has_value());
    EXPECT_EQ(errOpt->errorCode, 110u);
    EXPECT_EQ(errOpt->binding, 2);
    EXPECT_EQ(errOpt->direction, MCTP_DIR_TX);
    EXPECT_EQ(errOpt->srcEid, 1);
    EXPECT_EQ(errOpt->destEid, 10);
    EXPECT_EQ(errOpt->timestampNs, 123456789u);
    EXPECT_EQ(errOpt->pldmType, 5);
    EXPECT_EQ(errOpt->requestPayload.size(), 3u);
}

TEST_F(HandlerTest, storeTransportErrorRxDirection)
{
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(1), 2,
        milliseconds(100));

    // Store an RX transport error (direction=RX, so affectedEid=srcEid)
    pldm::transport::MctpError mctpError{};
    mctpError.error_code = 113; // EHOSTUNREACH
    mctpError.direction = MCTP_DIR_RX;
    mctpError.binding = 3;      // PCIe
    mctpError.src_eid = 20;
    mctpError.dest_eid = 1;
    mctpError.timestamp_ns = 987654321;
    mctpError.msg_type = 0x00; // Not PLDM
    mctpError.payload_len = 2;
    mctpError.payload[0] = 0xAA;
    mctpError.payload[1] = 0xBB;

    reqHandler.storeTransportError(mctpError);

    // For RX, affectedEid = srcEid = 20
    EXPECT_TRUE(reqHandler.hasTransportError(20));
    EXPECT_FALSE(reqHandler.hasTransportError(1));

    auto errOpt = reqHandler.getTransportError(20);
    ASSERT_TRUE(errOpt.has_value());
    EXPECT_EQ(errOpt->errorCode, 113u);
    EXPECT_EQ(errOpt->direction, MCTP_DIR_RX);
    EXPECT_EQ(errOpt->pldmType, 0xFF); // Not PLDM msg type
}

TEST_F(HandlerTest, getTransportErrorWithTypeFilter)
{
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(1), 2,
        milliseconds(100));

    pldm::transport::MctpError mctpError{};
    mctpError.error_code = 110;
    mctpError.direction = MCTP_DIR_TX;
    mctpError.src_eid = 1;
    mctpError.dest_eid = 10;
    mctpError.msg_type = MCTP_MSG_TYPE_PLDM;
    mctpError.payload_len = 2;
    mctpError.payload[0] = 0x01;
    mctpError.payload[1] = 0x05; // PLDM type 5 (FWUP)
    mctpError.binding = 2;
    mctpError.timestamp_ns = 0;

    reqHandler.storeTransportError(mctpError);

    // Match with correct type filter
    auto errOpt = reqHandler.getTransportError(10, 5);
    ASSERT_TRUE(errOpt.has_value());
    EXPECT_EQ(errOpt->pldmType, 5);

    // Mismatch with wrong type filter - should return nullopt
    errOpt = reqHandler.getTransportError(10, 2);
    EXPECT_FALSE(errOpt.has_value());

    // Default filter (0xFF) should match any
    errOpt = reqHandler.getTransportError(10);
    ASSERT_TRUE(errOpt.has_value());
}

TEST_F(HandlerTest, getTransportErrorWithUnknownPldmType)
{
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(1), 2,
        milliseconds(100));

    // Store error with pldmType 0xFF (unknown) - should match any filter
    pldm::transport::MctpError mctpError{};
    mctpError.error_code = 110;
    mctpError.direction = MCTP_DIR_TX;
    mctpError.src_eid = 1;
    mctpError.dest_eid = 10;
    mctpError.msg_type = 0x00; // Not PLDM -> pldmType will be 0xFF
    mctpError.payload_len = 2;
    mctpError.payload[0] = 0x00;
    mctpError.payload[1] = 0x00;
    mctpError.binding = 0;
    mctpError.timestamp_ns = 0;

    reqHandler.storeTransportError(mctpError);

    // When stored pldmType is 0xFF, any filter should match
    auto errOpt = reqHandler.getTransportError(10, 5);
    ASSERT_TRUE(errOpt.has_value());
    EXPECT_EQ(errOpt->pldmType, 0xFF);
}

TEST_F(HandlerTest, clearTransportError)
{
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(1), 2,
        milliseconds(100));

    pldm::transport::MctpError mctpError{};
    mctpError.error_code = 110;
    mctpError.direction = MCTP_DIR_TX;
    mctpError.src_eid = 1;
    mctpError.dest_eid = 10;
    mctpError.msg_type = MCTP_MSG_TYPE_PLDM;
    mctpError.payload_len = 2;
    mctpError.payload[0] = 0x01;
    mctpError.payload[1] = 0x05;
    mctpError.binding = 0;
    mctpError.timestamp_ns = 0;

    reqHandler.storeTransportError(mctpError);
    EXPECT_TRUE(reqHandler.hasTransportError(10));

    reqHandler.clearTransportError(10);
    EXPECT_FALSE(reqHandler.hasTransportError(10));
    EXPECT_EQ(reqHandler.getTransportError(10), std::nullopt);

    // Clearing non-existent error should be a no-op
    reqHandler.clearTransportError(99);
}

TEST_F(HandlerTest, instanceIdExpiryWithTransportError)
{
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(1), 2,
        milliseconds(100));

    pldm::Request request{};
    auto instanceId = instanceIdDb.next(eid).value();
    auto rc = reqHandler.registerRequest(
        eid, instanceId, 0, 0, std::move(request),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    // Store transport error AFTER registration so it's not cleared by
    // pollEndpointQueue's clearTransportError call
    pldm::transport::MctpError mctpError{};
    mctpError.error_code = 110;
    mctpError.direction = MCTP_DIR_TX;
    mctpError.src_eid = 1;
    mctpError.dest_eid = eid;
    mctpError.msg_type = MCTP_MSG_TYPE_PLDM;
    mctpError.payload_len = 2;
    mctpError.payload[0] = 0x01;
    mctpError.payload[1] = 0x05;
    mctpError.binding = 2;
    mctpError.timestamp_ns = 0;

    reqHandler.storeTransportError(mctpError);

    // Wait for instance ID expiry - the transport error branch should be hit
    waitEventExpiry(milliseconds(500));

    EXPECT_EQ(nullResponse, true);
}

TEST_F(HandlerTest, multipleEndpointQueues)
{
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(2), 2,
        milliseconds(100));

    mctp_eid_t eid1 = 10;
    mctp_eid_t eid2 = 20;

    // Register requests for two different endpoints
    pldm::Request request1{};
    auto instanceId1 = instanceIdDb.next(eid1).value();
    auto rc = reqHandler.registerRequest(
        eid1, instanceId1, 0, 0, std::move(request1),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    pldm::Request request2{};
    auto instanceId2 = instanceIdDb.next(eid2).value();
    rc = reqHandler.registerRequest(
        eid2, instanceId2, 0, 0, std::move(request2),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    // Respond to first endpoint
    pldm::Response response(sizeof(pldm_msg_hdr) + sizeof(uint8_t));
    auto responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    reqHandler.handleResponse(eid1, instanceId1, 0, 0, responsePtr,
                              response.size());
    EXPECT_EQ(validResponse, true);
    EXPECT_EQ(callbackCount, 1);

    // Respond to second endpoint
    reqHandler.handleResponse(eid2, instanceId2, 0, 0, responsePtr,
                              response.size());
    EXPECT_EQ(callbackCount, 2);
}

TEST_F(HandlerTest, queuedRequestSentAfterActiveCompletes)
{
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(2), 2,
        milliseconds(100));

    // Register first request - becomes active
    pldm::Request request1{};
    auto instanceId1 = instanceIdDb.next(eid).value();
    auto rc = reqHandler.registerRequest(
        eid, instanceId1, 0, 0, std::move(request1),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    // Register second request - goes to queue
    pldm::Request request2{};
    auto instanceId2 = instanceIdDb.next(eid).value();
    rc = reqHandler.registerRequest(
        eid, instanceId2, 0, 1, std::move(request2),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    // Respond to first request - this triggers pollEndpointQueue for the second
    pldm::Response response(sizeof(pldm_msg_hdr) + sizeof(uint8_t));
    auto responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    reqHandler.handleResponse(eid, instanceId1, 0, 0, responsePtr,
                              response.size());
    EXPECT_EQ(validResponse, true);
    EXPECT_EQ(callbackCount, 1);
    validResponse = false;

    // Respond to second request (now active after first completed)
    reqHandler.handleResponse(eid, instanceId2, 0, 1, responsePtr,
                              response.size());
    EXPECT_EQ(validResponse, true);
    EXPECT_EQ(callbackCount, 2);
}

TEST_F(HandlerTest, coroutineRequestTimeoutNoTransportError)
{
    exec::async_scope scope;
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(1), 2,
        milliseconds(100));

    auto instanceId = instanceIdDb.next(eid).value();
    int resultRc = PLDM_SUCCESS;

    scope.spawn(
        stdexec::just() | stdexec::let_value([&]() -> exec::task<void> {
            pldm::Request request(sizeof(pldm_msg_hdr) + sizeof(uint8_t), 0);
            const pldm_msg* responseMsg;
            size_t responseLen;

            auto requestPtr = new (request.data()) pldm_msg;
            requestPtr->hdr.instance_id = instanceId;

            std::tie(resultRc, responseMsg, responseLen) =
                co_await reqHandler.sendRecvMsg(eid, std::move(request));

            EXPECT_EQ(responseMsg, nullptr);
            EXPECT_EQ(responseLen, 0u);
        }),
        exec::default_task_context<void>(stdexec::inline_scheduler{}));

    // Don't send response - let instance ID expire
    waitEventExpiry(milliseconds(500));

    stdexec::sync_wait(scope.on_empty());
    EXPECT_EQ(resultRc, PLDM_ERROR_NOT_READY);
}

TEST_F(HandlerTest, coroutineRequestTimeoutWithTransportError)
{
    exec::async_scope scope;
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(1), 2,
        milliseconds(100));

    auto instanceId = instanceIdDb.next(eid).value();
    int resultRc = PLDM_SUCCESS;

    scope.spawn(
        stdexec::just() | stdexec::let_value([&]() -> exec::task<void> {
            pldm::Request request(sizeof(pldm_msg_hdr) + sizeof(uint8_t), 0);
            const pldm_msg* responseMsg;
            size_t responseLen;

            auto requestPtr = new (request.data()) pldm_msg;
            requestPtr->hdr.instance_id = instanceId;

            std::tie(resultRc, responseMsg, responseLen) =
                co_await reqHandler.sendRecvMsg(eid, std::move(request));

            EXPECT_EQ(responseMsg, nullptr);
            EXPECT_EQ(responseLen, 0u);
        }),
        exec::default_task_context<void>(stdexec::inline_scheduler{}));

    // Store transport error after registration
    pldm::transport::MctpError mctpError{};
    mctpError.error_code = 110;
    mctpError.direction = MCTP_DIR_TX;
    mctpError.src_eid = 1;
    mctpError.dest_eid = eid;
    mctpError.msg_type = MCTP_MSG_TYPE_PLDM;
    mctpError.payload_len = 2;
    mctpError.payload[0] = 0x01;
    mctpError.payload[1] = 0x05;
    mctpError.binding = 2;
    mctpError.timestamp_ns = 0;
    reqHandler.storeTransportError(mctpError);

    // Wait for instance ID expiry
    waitEventExpiry(milliseconds(500));

    stdexec::sync_wait(scope.on_empty());
    EXPECT_EQ(resultRc, PLDM_REQUESTER_MCTP_TRANSPORT_ERROR);
}

TEST_F(HandlerTest, asyncRequestResponseByCoroutine)
{
    struct _
    {
        static exec::task<uint8_t> getTIDTask(Handler<MockRequest>& handler,
                                              mctp_eid_t eid,
                                              uint8_t instanceId, uint8_t& tid)
        {
            pldm::Request request(sizeof(pldm_msg_hdr), 0);
            auto requestMsg = new (request.data()) pldm_msg;
            const pldm_msg* responseMsg;
            size_t responseLen;

            auto rc = encode_get_tid_req(instanceId, requestMsg);
            EXPECT_EQ(rc, PLDM_SUCCESS);

            std::tie(rc, responseMsg, responseLen) =
                co_await handler.sendRecvMsg(eid, std::move(request));
            EXPECT_NE(responseLen, 0);

            uint8_t cc = 0;
            rc = decode_get_tid_resp(responseMsg, responseLen, &cc, &tid);
            EXPECT_EQ(rc, PLDM_SUCCESS);

            co_return cc;
        }
    };

    exec::async_scope scope;
    Handler<MockRequest> reqHandler(pldmTransport, event, instanceIdDb, false,
                                    seconds(1), 2, milliseconds(100));
    auto instanceIdResult = instanceIdDb.next(eid);
    ASSERT_TRUE(instanceIdResult);
    auto instanceId = instanceIdResult.value();

    uint8_t expectedTid = 1;

    // Execute a coroutine to send getTID command. The coroutine is suspended
    // until reqHandler.handleResponse() is received.
    scope.spawn(stdexec::just() | stdexec::let_value([&]() -> exec::task<void> {
                    uint8_t respTid = 0;

                    co_await _::getTIDTask(reqHandler, eid, instanceId,
                                           respTid);

                    EXPECT_EQ(expectedTid, respTid);
                }),
                exec::default_task_context<void>(stdexec::inline_scheduler{}));

    pldm::Response mockResponse(sizeof(pldm_msg_hdr) + PLDM_GET_TID_RESP_BYTES,
                                0);
    auto mockResponseMsg = new (mockResponse.data()) pldm_msg;

    // Compose response message of getTID command
    encode_get_tid_resp(instanceId, PLDM_SUCCESS, expectedTid, mockResponseMsg);

    // Send response back to resume getTID coroutine to update respTid by
    // calling  reqHandler.handleResponse() manually
    reqHandler.handleResponse(eid, instanceId, PLDM_BASE, PLDM_GET_TID,
                              mockResponseMsg,
                              mockResponse.size() - sizeof(pldm_msg_hdr));

    stdexec::sync_wait(scope.on_empty());
}

TEST_F(HandlerTest, pollEndpointQueueSendFailure)
{
    // Use FailingSendRequest so pollEndpointQueue's request->start() fails.
    // numRetries=0 ensures start() returns the send error immediately.
    Handler<FailingSendRequest> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(1), 0,
        milliseconds(100));

    pldm::Request request{};
    auto instanceId = instanceIdDb.next(eid).value();
    auto rc = reqHandler.registerRequest(
        eid, instanceId, 0, 0, std::move(request),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });

    // registerRequest calls pollEndpointQueue which calls start() which fails.
    // pollEndpointQueue returns error, registerRequest propagates it.
    EXPECT_EQ(rc, PLDM_ERROR);
    // The response handler is called with null response on failure
    EXPECT_EQ(nullResponse, true);
}

TEST_F(HandlerTest, pollEndpointQueueTimerStartFailure)
{
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(1), 0,
        milliseconds(100));

    pldm::Request request{};
    auto instanceId = instanceIdDb.next(eid).value();
    g_failHandlerTimerStart = true;
    auto rc = reqHandler.registerRequest(
        eid, instanceId, 0, 0, std::move(request),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    g_failHandlerTimerStart = false;

    EXPECT_EQ(rc, PLDM_ERROR);
    EXPECT_EQ(nullResponse, true);
}

// =====================================================================
// Handler<Request> standalone tests — methods that don't need transport
// =====================================================================

TEST_F(HandlerTest, requestTypeTransportErrorMethods)
{
    Handler<Request> reqHandler(pldmTransport, event, instanceIdDb, false);

    EXPECT_EQ(reqHandler.getTransportError(10), std::nullopt);

    pldm::transport::MctpError mctpError{};
    mctpError.error_code = 110;
    mctpError.direction = MCTP_DIR_TX;
    mctpError.src_eid = 1;
    mctpError.dest_eid = 10;
    mctpError.msg_type = MCTP_MSG_TYPE_PLDM;
    mctpError.payload_len = 2;
    mctpError.payload[0] = 0x01;
    mctpError.payload[1] = 0x05;
    mctpError.binding = 2;
    mctpError.timestamp_ns = 0;
    reqHandler.storeTransportError(mctpError);

    auto errOpt = reqHandler.getTransportError(10);
    ASSERT_TRUE(errOpt.has_value());
    EXPECT_EQ(errOpt->errorCode, 110u);
}

TEST_F(HandlerTest, requestTypeHandleResponseUnknownKey)
{
    Handler<Request> reqHandler(pldmTransport, event, instanceIdDb, false);

    pldm::Response response(sizeof(pldm_msg_hdr) + sizeof(uint8_t));
    auto responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    reqHandler.handleResponse(eid, 0, 0, 0, responsePtr, response.size());
}

TEST_F(HandlerTest, requestTypeInstanceIdExpiryCallBackUnknownKey)
{
#ifndef NDEBUG
    Handler<Request> reqHandler(pldmTransport, event, instanceIdDb, false);
    RequestKey key{eid, 0, 0, 0};
    EXPECT_DEATH(reqHandler.instanceIdExpiryCallBack(key), "");
#endif
}

TEST_F(HandlerTest, requestTypeUnregisterNotFound)
{
    Handler<Request> reqHandler(pldmTransport, event, instanceIdDb, false);
    auto rc = reqHandler.unregisterRequest(eid, 0, 0, 0);
    EXPECT_EQ(rc, PLDM_ERROR);
}

TEST_F(HandlerTest, requestTypeSendRecvMsgCreation)
{
    Handler<Request> reqHandler(pldmTransport, event, instanceIdDb, false);
    auto sender = reqHandler.sendRecvMsg(eid, pldm::Request{});
    (void)sender;
}

// =====================================================================
// Handler<Request> coroutine tests — send fails but timer-based flow works
// Request::send() returns PLDM_ERROR with nullptr pldmTransport.
// With numRetries>0, start() starts the retry timer and returns SUCCESS,
// so the full instance-ID-expiry flow executes.
// =====================================================================

TEST_F(HandlerTest, requestTypeCoroutineTimeout)
{
    exec::async_scope scope;
    Handler<Request> reqHandler(pldmTransport, event, instanceIdDb, false,
                                seconds(1), 2, milliseconds(100));

    auto instanceId = instanceIdDb.next(eid).value();
    int resultRc = PLDM_SUCCESS;

    scope.spawn(
        stdexec::just() | stdexec::let_value([&]() -> exec::task<void> {
            pldm::Request request(sizeof(pldm_msg_hdr) + sizeof(uint8_t), 0);
            const pldm_msg* responseMsg;
            size_t responseLen;

            auto requestPtr = new (request.data()) pldm_msg;
            requestPtr->hdr.instance_id = instanceId;
            requestPtr->hdr.request = 1;

            std::tie(resultRc, responseMsg, responseLen) =
                co_await reqHandler.sendRecvMsg(eid, std::move(request));

            EXPECT_EQ(responseMsg, nullptr);
            EXPECT_EQ(responseLen, 0u);
        }),
        exec::default_task_context<void>(stdexec::inline_scheduler{}));

    waitEventExpiry(milliseconds(500));

    stdexec::sync_wait(scope.on_empty());
    EXPECT_EQ(resultRc, PLDM_ERROR_NOT_READY);
}

TEST_F(HandlerTest, requestTypeCoroutineCancellation)
{
    exec::async_scope scope;
    Handler<Request> reqHandler(pldmTransport, event, instanceIdDb, false,
                                seconds(5), 2, milliseconds(100));

    auto instanceId = instanceIdDb.next(eid).value();
    bool stopped = false;

    scope.spawn(
        stdexec::just() | stdexec::let_value([&]() -> exec::task<void> {
            pldm::Request request(sizeof(pldm_msg_hdr) + sizeof(uint8_t), 0);
            const pldm_msg* responseMsg;
            size_t responseLen;

            auto requestPtr = new (request.data()) pldm_msg;
            requestPtr->hdr.instance_id = instanceId;
            requestPtr->hdr.request = 1;
            int rc;

            std::tie(rc, responseMsg, responseLen) =
                co_await reqHandler.sendRecvMsg(eid, std::move(request));

            EXPECT_TRUE(false);
        }) | stdexec::upon_stopped([&] { stopped = true; }),
        exec::default_task_context<void>(stdexec::inline_scheduler{}));

    scope.request_stop();
    EXPECT_TRUE(stopped);
    stdexec::sync_wait(scope.on_empty());
}

TEST_F(HandlerTest, requestTypeCoroutineTimeoutTaskInt)
{
    exec::async_scope scope;
    Handler<Request> reqHandler(pldmTransport, event, instanceIdDb, false,
                                seconds(1), 2, milliseconds(100));

    auto instanceId = instanceIdDb.next(eid).value();
    int resultRc = PLDM_SUCCESS;

    scope.spawn(stdexec::just() | stdexec::let_value([&]() -> exec::task<void> {
                    resultRc = co_await sendRecvRequestTaskInt(reqHandler, eid,
                                                               instanceId);
                }),
                exec::default_task_context<void>(stdexec::inline_scheduler{}));

    waitEventExpiry(milliseconds(500));

    stdexec::sync_wait(scope.on_empty());
    EXPECT_EQ(resultRc, PLDM_ERROR_NOT_READY);
}

TEST_F(HandlerTest, requestTypeCoroutineCancellationTaskInt)
{
    exec::async_scope scope;
    Handler<Request> reqHandler(pldmTransport, event, instanceIdDb, false,
                                seconds(5), 2, milliseconds(100));

    auto instanceId = instanceIdDb.next(eid).value();
    bool stopped = false;

    scope.spawn(stdexec::just() | stdexec::let_value([&]() -> exec::task<void> {
                    auto rc = co_await sendRecvRequestTaskInt(reqHandler, eid,
                                                              instanceId);
                    (void)rc;
                    EXPECT_TRUE(false);
                }) | stdexec::upon_stopped([&] { stopped = true; }),
                exec::default_task_context<void>(stdexec::inline_scheduler{}));

    scope.request_stop();
    EXPECT_TRUE(stopped);
    stdexec::sync_wait(scope.on_empty());
}

TEST_F(HandlerTest, requestTypeCoroutineTaskIntCompletesWithResponse)
{
    exec::async_scope scope;
    Handler<Request> reqHandler(pldmTransport, event, instanceIdDb, false,
                                seconds(5), 2, milliseconds(100));

    auto instanceId = instanceIdDb.next(eid).value();
    int resultRc = PLDM_ERROR;

    scope.spawn(stdexec::just() | stdexec::let_value([&]() -> exec::task<void> {
                    resultRc = co_await sendRecvRequestTaskInt(reqHandler, eid,
                                                               instanceId);
                }),
                exec::default_task_context<void>(stdexec::inline_scheduler{}));

    // Let the coroutine register the request before injecting a response.
    waitEventExpiry(milliseconds(1));

    pldm::Response response(sizeof(pldm_msg_hdr) + sizeof(uint8_t), 0);
    auto responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    reqHandler.handleResponse(eid, instanceId, 0, 0, responsePtr,
                              response.size());

    stdexec::sync_wait(scope.on_empty());
    EXPECT_EQ(resultRc, PLDM_SUCCESS);
}

TEST_F(HandlerTest, requestTypeSendRecvMsgRegisterFailure)
{
    // numRetries=0 with null transport makes Request::start() fail immediately.
    Handler<Request> reqHandler(pldmTransport, event, instanceIdDb, false,
                                seconds(1), 0, milliseconds(100));

    auto instanceId = instanceIdDb.next(eid).value();
    pldm::Request request(sizeof(pldm_msg_hdr) + sizeof(uint8_t), 0);
    auto requestPtr = new (request.data()) pldm_msg;
    requestPtr->hdr.instance_id = instanceId;
    requestPtr->hdr.request = 1;

    auto respOpt =
        stdexec::sync_wait(reqHandler.sendRecvMsg(eid, std::move(request)));
    ASSERT_TRUE(respOpt.has_value());
    auto& sendRecvResp = std::get<0>(*respOpt);
    auto& [rc, response, responseLen] = sendRecvResp;
    EXPECT_EQ(rc, PLDM_ERROR);
    EXPECT_EQ(response, nullptr);
    EXPECT_EQ(responseLen, 0u);
}

TEST_F(HandlerTest, requestTypeCoroutinePreCancelledTaskInt)
{
    exec::async_scope scope;
    Handler<Request> reqHandler(pldmTransport, event, instanceIdDb, false,
                                seconds(5), 2, milliseconds(100));

    auto instanceId = instanceIdDb.next(eid).value();
    bool stopped = false;

    scope.request_stop();
    scope.spawn(stdexec::just() | stdexec::let_value([&]() -> exec::task<void> {
                    auto rc = co_await sendRecvRequestTaskInt(reqHandler, eid,
                                                              instanceId);
                    (void)rc;
                    EXPECT_TRUE(false);
                }) | stdexec::upon_stopped([&] { stopped = true; }),
                exec::default_task_context<void>(stdexec::inline_scheduler{}));

    EXPECT_TRUE(stopped);
    stdexec::sync_wait(scope.on_empty());
}

// =====================================================================
// Handler<MockRequest> coverage — exercise uncalled instantiation funcs
// =====================================================================

TEST_F(HandlerTest, mockRequestInstanceIdTimerExpired)
{
    Handler<MockRequest> reqHandler(pldmTransport, event, instanceIdDb, false,
                                    seconds(1), 2, milliseconds(100));
    pldm::Request request{};
    auto instanceId = instanceIdDb.next(eid).value();
    auto rc = reqHandler.registerRequest(
        eid, instanceId, 0, 0, std::move(request),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    waitEventExpiry(milliseconds(500));
    EXPECT_EQ(nullResponse, true);
}

TEST_F(HandlerTest, mockRequestUnregisterActive)
{
    Handler<MockRequest> reqHandler(pldmTransport, event, instanceIdDb, false,
                                    seconds(2), 2, milliseconds(100));
    pldm::Request request{};
    auto instanceId = instanceIdDb.next(eid).value();
    auto rc = reqHandler.registerRequest(
        eid, instanceId, 0, 0, std::move(request),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    rc = reqHandler.unregisterRequest(eid, instanceId, 0, 0);
    EXPECT_EQ(rc, PLDM_SUCCESS);
}

// =====================================================================
// Handler<FailingSendRequest> timer expiry with numRetries=1
// =====================================================================

TEST_F(HandlerTest, failingSendRequestInstanceIdTimerExpiry)
{
    // send() always fails but start() starts the retry timer and returns
    // success. The instance ID expiry then fires normally.
    Handler<FailingSendRequest> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(1), 2,
        milliseconds(100));
    pldm::Request request{};
    auto instanceId = instanceIdDb.next(eid).value();
    auto rc = reqHandler.registerRequest(
        eid, instanceId, 0, 0, std::move(request),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    waitEventExpiry(milliseconds(500));
    EXPECT_EQ(nullResponse, true);
}

// =====================================================================
// SendRecvMsgOperation<MockRequest>::onStop — coroutine cancellation
// =====================================================================

TEST_F(HandlerTest, asyncRequestCancellationByCoroutine)
{
    exec::async_scope scope;
    Handler<MockRequest> reqHandler(pldmTransport, event, instanceIdDb, false,
                                    seconds(1), 2, milliseconds(100));
    auto instanceId = instanceIdDb.next(eid).value();
    bool stopped = false;

    scope.spawn(
        stdexec::just() | stdexec::let_value([&]() -> exec::task<void> {
            pldm::Request request(sizeof(pldm_msg_hdr) + sizeof(uint8_t), 0);
            const pldm_msg* responseMsg;
            size_t responseLen;

            auto requestPtr = new (request.data()) pldm_msg;
            requestPtr->hdr.instance_id = instanceId;
            int rc;

            std::tie(rc, responseMsg, responseLen) =
                co_await reqHandler.sendRecvMsg(eid, std::move(request));

            EXPECT_TRUE(false);
        }) | stdexec::upon_stopped([&] { stopped = true; }),
        exec::default_task_context<void>(stdexec::inline_scheduler{}));

    scope.request_stop();
    EXPECT_TRUE(stopped);
    stdexec::sync_wait(scope.on_empty());
}

// =====================================================================
// Additional SendRecvMsgOperation receiver-type coverage tests
// Each exec::task<T> creates a distinct receiver type with its own
// gcov function counters for onComplete/onStop.
// =====================================================================

TEST_F(HandlerTest, mockRequestCoroutineCompletion)
{
    // Exercises SendRecvMsgOperation<MockRequest, ...basic_task<void>...>::
    //   onComplete — a response completes the coroutine normally.
    exec::async_scope scope;
    Handler<MockRequest> reqHandler(pldmTransport, event, instanceIdDb, false,
                                    seconds(1), 2, milliseconds(100));
    auto instanceId = instanceIdDb.next(eid).value();
    bool completed = false;

    scope.spawn(
        stdexec::just() | stdexec::let_value([&]() -> exec::task<void> {
            pldm::Request request(sizeof(pldm_msg_hdr) + sizeof(uint8_t), 0);
            const pldm_msg* responseMsg;
            size_t responseLen;
            int rc;

            auto requestPtr = new (request.data()) pldm_msg;
            requestPtr->hdr.instance_id = instanceId;

            std::tie(rc, responseMsg, responseLen) =
                co_await reqHandler.sendRecvMsg(eid, std::move(request));

            completed = true;
        }),
        exec::default_task_context<void>(stdexec::inline_scheduler{}));

    pldm::Response response(sizeof(pldm_msg_hdr) + sizeof(uint8_t));
    auto responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    reqHandler.handleResponse(eid, instanceId, 0, 0, responsePtr,
                              response.size());

    stdexec::sync_wait(scope.on_empty());
    EXPECT_TRUE(completed);
}

TEST_F(HandlerTest, mockRequestCoroutineCancellationUint8)
{
    // Exercises SendRecvMsgOperation<MockRequest, ...basic_task<uint8_t>...>::
    //   onStop — cancellation via a nested exec::task<uint8_t> coroutine
    //   to match the receiver type from asyncRequestResponseByCoroutine.
    struct _
    {
        static exec::task<uint8_t> cancelTask(
            Handler<MockRequest>& handler, mctp_eid_t eid, uint8_t instanceId)
        {
            pldm::Request request(sizeof(pldm_msg_hdr) + sizeof(uint8_t), 0);
            auto requestPtr = new (request.data()) pldm_msg;
            requestPtr->hdr.instance_id = instanceId;
            const pldm_msg* responseMsg;
            size_t responseLen;
            int rc;

            std::tie(rc, responseMsg, responseLen) =
                co_await handler.sendRecvMsg(eid, std::move(request));

            EXPECT_TRUE(false);
            co_return 0;
        }
    };

    exec::async_scope scope;
    Handler<MockRequest> reqHandler(pldmTransport, event, instanceIdDb, false,
                                    seconds(1), 2, milliseconds(100));
    auto instanceId = instanceIdDb.next(eid).value();
    bool stopped = false;

    scope.spawn(stdexec::just() | stdexec::let_value([&]() -> exec::task<void> {
                    co_await _::cancelTask(reqHandler, eid, instanceId);
                }) | stdexec::upon_stopped([&] { stopped = true; }),
                exec::default_task_context<void>(stdexec::inline_scheduler{}));

    scope.request_stop();
    EXPECT_TRUE(stopped);
    stdexec::sync_wait(scope.on_empty());
}

TEST(HandlerStandaloneTest, RequestKeyEqualityAllFieldMismatchPaths)
{
    RequestKey base{1, 2, 3, 4};

    EXPECT_TRUE((base == RequestKey{1, 2, 3, 4}));
    EXPECT_FALSE((base == RequestKey{9, 2, 3, 4}));
    EXPECT_FALSE((base == RequestKey{1, 9, 3, 4}));
    EXPECT_FALSE((base == RequestKey{1, 2, 9, 4}));
    EXPECT_FALSE((base == RequestKey{1, 2, 3, 9}));
}

TEST(HandlerStandaloneTest, EndpointMessageQueueEqualityFalseBranch)
{
    EndpointMessageQueue queue{12, {}, false};
    EXPECT_TRUE(queue == 12);
    EXPECT_FALSE(queue == 13);
}

TEST_F(HandlerTest, requestTypeSingleRequestResponseScenario)
{
    Handler<Request> reqHandler(pldmTransport, event, instanceIdDb, false,
                                seconds(2), 2, milliseconds(100));

    auto instanceId = instanceIdDb.next(eid).value();
    pldm::Request request(sizeof(pldm_msg_hdr), 0);
    auto requestPtr = reinterpret_cast<pldm_msg*>(request.data());
    EXPECT_EQ(encode_get_tid_req(instanceId, requestPtr), PLDM_SUCCESS);

    auto rc = reqHandler.registerRequest(
        eid, instanceId, PLDM_BASE, PLDM_GET_TID, std::move(request),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    pldm::Response response(sizeof(pldm_msg_hdr) + sizeof(uint8_t), 0);
    auto responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    reqHandler.handleResponse(eid, instanceId, PLDM_BASE, PLDM_GET_TID,
                              responsePtr, response.size());

    EXPECT_TRUE(validResponse);
    EXPECT_EQ(callbackCount, 1);
}

TEST_F(HandlerTest, requestTypeUnregisterQueuedRequest)
{
    Handler<Request> reqHandler(pldmTransport, event, instanceIdDb, false,
                                seconds(2), 2, milliseconds(100));

    auto instanceId1 = instanceIdDb.next(eid).value();
    pldm::Request request1(sizeof(pldm_msg_hdr), 0);
    auto requestPtr1 = reinterpret_cast<pldm_msg*>(request1.data());
    EXPECT_EQ(encode_get_tid_req(instanceId1, requestPtr1), PLDM_SUCCESS);
    auto rc = reqHandler.registerRequest(
        eid, instanceId1, PLDM_BASE, PLDM_GET_TID, std::move(request1),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    auto instanceId2 = instanceIdDb.next(eid).value();
    pldm::Request request2(sizeof(pldm_msg_hdr), 0);
    auto requestPtr2 = reinterpret_cast<pldm_msg*>(request2.data());
    EXPECT_EQ(encode_get_tid_req(instanceId2, requestPtr2), PLDM_SUCCESS);
    rc = reqHandler.registerRequest(
        eid, instanceId2, PLDM_BASE, PLDM_GET_TID, std::move(request2),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    rc =
        reqHandler.unregisterRequest(eid, instanceId2, PLDM_BASE, PLDM_GET_TID);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    pldm::Response response(sizeof(pldm_msg_hdr) + sizeof(uint8_t), 0);
    auto responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    reqHandler.handleResponse(eid, instanceId1, PLDM_BASE, PLDM_GET_TID,
                              responsePtr, response.size());
}

TEST_F(HandlerTest, requestTypeUnregisterQueuedRequestMismatchedKeyScan)
{
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(60), 2,
        milliseconds(100));

    auto activeInstanceId = instanceIdDb.next(eid).value();
    auto rc = reqHandler.registerRequest(
        eid, activeInstanceId, PLDM_BASE, PLDM_GET_TID, pldm::Request{},
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    ASSERT_EQ(rc, PLDM_SUCCESS);

    auto sharedInstanceId = instanceIdDb.next(eid).value();
    auto differentInstanceId = instanceIdDb.next(eid).value();

    rc = reqHandler.registerRequest(
        eid, differentInstanceId, PLDM_BASE, PLDM_GET_TID, pldm::Request{},
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    ASSERT_EQ(rc, PLDM_SUCCESS);

    rc = reqHandler.registerRequest(
        eid, sharedInstanceId, 0x7f, PLDM_GET_TID, pldm::Request{},
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    ASSERT_EQ(rc, PLDM_SUCCESS);

    rc = reqHandler.registerRequest(
        eid, sharedInstanceId, PLDM_BASE, 0x7f, pldm::Request{},
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    ASSERT_EQ(rc, PLDM_SUCCESS);

    rc = reqHandler.unregisterRequest(eid, sharedInstanceId, PLDM_BASE,
                                      PLDM_GET_TID);
    EXPECT_EQ(rc, PLDM_ERROR);
}

TEST_F(HandlerTest, requestTypeUnregisterQueuedRequestSearchPatterns)
{
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(60), 2,
        milliseconds(100));

    auto activeInstanceId = instanceIdDb.next(eid).value();
    auto rc = reqHandler.registerRequest(
        eid, activeInstanceId, PLDM_BASE, PLDM_GET_TID, pldm::Request{},
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    ASSERT_EQ(rc, PLDM_SUCCESS);

    auto i1 = instanceIdDb.next(eid).value();
    auto i2 = instanceIdDb.next(eid).value();
    auto i3 = instanceIdDb.next(eid).value();
    auto i4 = instanceIdDb.next(eid).value();
    auto i5 = instanceIdDb.next(eid).value();

    ASSERT_EQ(reqHandler.registerRequest(
                  eid, i1, 0x10, 0x20, pldm::Request{},
                  [this](mctp_eid_t eid, const pldm_msg* response,
                         size_t respMsgLen) {
                      this->pldmResponseCallBack(eid, response, respMsgLen);
                  }),
              PLDM_SUCCESS);
    ASSERT_EQ(reqHandler.registerRequest(
                  eid, i2, 0x10, 0x21, pldm::Request{},
                  [this](mctp_eid_t eid, const pldm_msg* response,
                         size_t respMsgLen) {
                      this->pldmResponseCallBack(eid, response, respMsgLen);
                  }),
              PLDM_SUCCESS);
    ASSERT_EQ(reqHandler.registerRequest(
                  eid, i3, 0x11, 0x20, pldm::Request{},
                  [this](mctp_eid_t eid, const pldm_msg* response,
                         size_t respMsgLen) {
                      this->pldmResponseCallBack(eid, response, respMsgLen);
                  }),
              PLDM_SUCCESS);
    ASSERT_EQ(reqHandler.registerRequest(
                  eid, i4, 0x11, 0x21, pldm::Request{},
                  [this](mctp_eid_t eid, const pldm_msg* response,
                         size_t respMsgLen) {
                      this->pldmResponseCallBack(eid, response, respMsgLen);
                  }),
              PLDM_SUCCESS);
    ASSERT_EQ(reqHandler.registerRequest(
                  eid, i5, 0x12, 0x22, pldm::Request{},
                  [this](mctp_eid_t eid, const pldm_msg* response,
                         size_t respMsgLen) {
                      this->pldmResponseCallBack(eid, response, respMsgLen);
                  }),
              PLDM_SUCCESS);

    EXPECT_EQ(reqHandler.unregisterRequest(eid, i3, 0x11, 0x20), PLDM_SUCCESS);
    EXPECT_EQ(reqHandler.unregisterRequest(eid, i5, 0x12, 0x22), PLDM_SUCCESS);
    EXPECT_EQ(reqHandler.unregisterRequest(eid, 0x7f, 0x10, 0x20), PLDM_ERROR);
    EXPECT_EQ(reqHandler.unregisterRequest(eid, i1, 0x10, 0x20), PLDM_SUCCESS);
    EXPECT_EQ(reqHandler.unregisterRequest(eid, i4, 0x11, 0x21), PLDM_SUCCESS);
    EXPECT_EQ(reqHandler.unregisterRequest(eid, i2, 0x10, 0x21), PLDM_SUCCESS);
    EXPECT_EQ(reqHandler.unregisterRequest(eid, activeInstanceId, PLDM_BASE,
                                           PLDM_GET_TID),
              PLDM_SUCCESS);
}

TEST_F(HandlerTest, requestTypeBulkRegisterAndUnregisterAcrossEndpoints)
{
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(60), 2,
        milliseconds(100));

    for (mctp_eid_t endpoint = 1; endpoint <= 4; ++endpoint)
    {
        auto activeId = instanceIdDb.next(endpoint).value();
        ASSERT_EQ(reqHandler.registerRequest(
                      endpoint, activeId, 0x30, 0x40, pldm::Request{},
                      [this](mctp_eid_t eid, const pldm_msg* response,
                             size_t respMsgLen) {
                          this->pldmResponseCallBack(eid, response, respMsgLen);
                      }),
                  PLDM_SUCCESS);

        std::vector<uint8_t> queuedIds;
        queuedIds.reserve(6);
        for (uint8_t i = 0; i < 6; ++i)
        {
            auto inst = instanceIdDb.next(endpoint).value();
            queuedIds.push_back(inst);
            ASSERT_EQ(
                reqHandler.registerRequest(
                    endpoint, inst, static_cast<uint8_t>(0x30 + (i % 3)),
                    static_cast<uint8_t>(0x50 + i), pldm::Request{},
                    [this](mctp_eid_t eid, const pldm_msg* response,
                           size_t respMsgLen) {
                        this->pldmResponseCallBack(eid, response, respMsgLen);
                    }),
                PLDM_SUCCESS);
        }

        for (int i = static_cast<int>(queuedIds.size()) - 1; i >= 0; --i)
        {
            EXPECT_EQ(reqHandler.unregisterRequest(
                          endpoint, queuedIds[static_cast<size_t>(i)],
                          static_cast<uint8_t>(0x30 + (i % 3)),
                          static_cast<uint8_t>(0x50 + i)),
                      PLDM_SUCCESS);
        }

        EXPECT_EQ(reqHandler.unregisterRequest(endpoint, activeId, 0x30, 0x40),
                  PLDM_SUCCESS);
    }
}

TEST_F(HandlerTest, requestTypeLargeScaleQueueHashAndSearchActivity)
{
    Handler<NiceMock<MockRequest>> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(120), 2,
        milliseconds(100));

    struct QueuedKey
    {
        uint8_t instanceId;
        uint8_t type;
        uint8_t cmd;
    };

    for (mctp_eid_t endpoint = 10; endpoint < 16; ++endpoint)
    {
        auto activeId = instanceIdDb.next(endpoint).value();
        ASSERT_EQ(reqHandler.registerRequest(
                      endpoint, activeId, 0x20, 0x30, pldm::Request{},
                      [this](mctp_eid_t eid, const pldm_msg* response,
                             size_t respMsgLen) {
                          this->pldmResponseCallBack(eid, response, respMsgLen);
                      }),
                  PLDM_SUCCESS);

        std::vector<QueuedKey> keys;
        keys.reserve(8);
        for (uint8_t i = 0; i < 8; ++i)
        {
            auto inst = instanceIdDb.next(endpoint).value();
            auto type = static_cast<uint8_t>(0x40 + (i % 4));
            auto cmd = static_cast<uint8_t>(0x60 + i);
            keys.push_back({inst, type, cmd});
            ASSERT_EQ(
                reqHandler.registerRequest(
                    endpoint, inst, type, cmd, pldm::Request{},
                    [this](mctp_eid_t eid, const pldm_msg* response,
                           size_t respMsgLen) {
                        this->pldmResponseCallBack(eid, response, respMsgLen);
                    }),
                PLDM_SUCCESS);
        }

        // Mismatch scans: same instance + command but wrong type to force
        // deeper key comparison in find_if.
        for (size_t i = 0; i < 4; ++i)
        {
            EXPECT_EQ(reqHandler.unregisterRequest(endpoint, keys[i].instanceId,
                                                   0x7f, keys[i].cmd),
                      PLDM_ERROR);
        }

        // Remove all queued requests in reverse order to exercise scans
        // through shrinking queues.
        for (int i = static_cast<int>(keys.size()) - 1; i >= 0; --i)
        {
            EXPECT_EQ(reqHandler.unregisterRequest(
                          endpoint, keys[static_cast<size_t>(i)].instanceId,
                          keys[static_cast<size_t>(i)].type,
                          keys[static_cast<size_t>(i)].cmd),
                      PLDM_SUCCESS);
        }

        EXPECT_EQ(reqHandler.unregisterRequest(endpoint, activeId, 0x20, 0x30),
                  PLDM_SUCCESS);
    }
}

TEST_F(HandlerTest, requestTypeResponseIgnoredAfterExpiryMarkedForRemoval)
{
    Handler<Request> reqHandler(pldmTransport, event, instanceIdDb, false,
                                seconds(1), 2, milliseconds(100));

    auto instanceId = instanceIdDb.next(eid).value();
    pldm::Request request(sizeof(pldm_msg_hdr), 0);
    auto requestPtr = reinterpret_cast<pldm_msg*>(request.data());
    EXPECT_EQ(encode_get_tid_req(instanceId, requestPtr), PLDM_SUCCESS);

    auto rc = reqHandler.registerRequest(
        eid, instanceId, PLDM_BASE, PLDM_GET_TID, std::move(request),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    RequestKey key{eid, instanceId, PLDM_BASE, PLDM_GET_TID};
    reqHandler.instanceIdExpiryCallBack(key);
    const int callbacksAfterExpiry = callbackCount;
    EXPECT_TRUE(nullResponse);

    pldm::Response response(sizeof(pldm_msg_hdr) + sizeof(uint8_t), 0);
    auto responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    reqHandler.handleResponse(eid, instanceId, PLDM_BASE, PLDM_GET_TID,
                              responsePtr, response.size());

    EXPECT_EQ(callbackCount, callbacksAfterExpiry);
    waitEventExpiry(milliseconds(100));
}

TEST_F(HandlerTest, requestTypeCoroutineNullResponseNonZeroLength)
{
    exec::async_scope scope;
    Handler<Request> reqHandler(pldmTransport, event, instanceIdDb, false,
                                seconds(2), 2, milliseconds(100));

    auto instanceId = instanceIdDb.next(eid).value();
    int resultRc = PLDM_ERROR;
    const pldm_msg* resultResp = reinterpret_cast<const pldm_msg*>(0x1);
    size_t resultRespLen = 0;

    scope.spawn(
        stdexec::just() | stdexec::let_value([&]() -> exec::task<void> {
            pldm::Request request(sizeof(pldm_msg_hdr) + sizeof(uint8_t), 0);
            auto requestPtr = new (request.data()) pldm_msg;
            requestPtr->hdr.instance_id = instanceId;
            requestPtr->hdr.request = 1;

            std::tie(resultRc, resultResp, resultRespLen) =
                co_await reqHandler.sendRecvMsg(eid, std::move(request));
        }),
        exec::default_task_context<void>(stdexec::inline_scheduler{}));

    reqHandler.handleResponse(eid, instanceId, 0, 0, nullptr, 1);
    stdexec::sync_wait(scope.on_empty());

    EXPECT_EQ(resultRc, PLDM_SUCCESS);
    EXPECT_EQ(resultResp, nullptr);
    EXPECT_EQ(resultRespLen, 1u);
}

TEST_F(HandlerTest, mockRequestSingleRequestResponseScenario)
{
    Handler<MockRequest> reqHandler(pldmTransport, event, instanceIdDb, false,
                                    seconds(1), 2, milliseconds(100));
    pldm::Request request{};
    auto instanceId = instanceIdDb.next(eid).value();
    auto rc = reqHandler.registerRequest(
        eid, instanceId, 0, 0, std::move(request),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    pldm::Response response(sizeof(pldm_msg_hdr) + sizeof(uint8_t), 0);
    auto responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    reqHandler.handleResponse(eid, instanceId, 0, 0, responsePtr,
                              response.size());

    EXPECT_TRUE(validResponse);
    EXPECT_EQ(callbackCount, 1);
}

TEST_F(HandlerTest, mockRequestUnregisterQueuedRequest)
{
    Handler<MockRequest> reqHandler(pldmTransport, event, instanceIdDb, false,
                                    seconds(2), 2, milliseconds(100));

    pldm::Request request1{};
    auto instanceId1 = instanceIdDb.next(eid).value();
    auto rc = reqHandler.registerRequest(
        eid, instanceId1, 0, 0, std::move(request1),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    pldm::Request request2{};
    auto instanceId2 = instanceIdDb.next(eid).value();
    rc = reqHandler.registerRequest(
        eid, instanceId2, 0, 0, std::move(request2),
        [this](mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(eid, response, respMsgLen);
        });
    EXPECT_EQ(rc, PLDM_SUCCESS);

    rc = reqHandler.unregisterRequest(eid, instanceId2, 0, 0);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    pldm::Response response(sizeof(pldm_msg_hdr) + sizeof(uint8_t), 0);
    auto responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    reqHandler.handleResponse(eid, instanceId1, 0, 0, responsePtr,
                              response.size());
}

TEST_F(HandlerTest, requestTypeCoroutineCompletionTaskInt)
{
    exec::async_scope scope;
    Handler<Request> reqHandler(pldmTransport, event, instanceIdDb, false,
                                seconds(1), 2, milliseconds(100));

    auto instanceId = instanceIdDb.next(eid).value();
    bool completed = false;

    scope.spawn(stdexec::just() | stdexec::let_value([&]() -> exec::task<void> {
                    auto rc = co_await sendRecvRequestTaskInt(reqHandler, eid,
                                                              instanceId);
                    (void)rc;
                    completed = true;
                }),
                exec::default_task_context<void>(stdexec::inline_scheduler{}));

    waitEventExpiry(milliseconds(500));
    stdexec::sync_wait(scope.on_empty());
    EXPECT_TRUE(completed);
}

TEST_F(HandlerTest, failingSendRequestTransportErrorMethods)
{
    Handler<FailingSendRequest> reqHandler(
        pldmTransport, event, instanceIdDb, false, seconds(1), 2,
        milliseconds(100));

    EXPECT_FALSE(reqHandler.hasTransportError(15));

    pldm::transport::MctpError mctpError{};
    mctpError.error_code = 113;
    mctpError.direction = MCTP_DIR_TX;
    mctpError.src_eid = 1;
    mctpError.dest_eid = 15;
    mctpError.msg_type = MCTP_MSG_TYPE_PLDM;
    mctpError.payload_len = 2;
    mctpError.payload[0] = 0x01;
    mctpError.payload[1] = 0x02;
    mctpError.binding = 2;
    mctpError.timestamp_ns = 0;

    reqHandler.storeTransportError(mctpError);
    EXPECT_TRUE(reqHandler.hasTransportError(15));

    reqHandler.clearTransportError(15);
    EXPECT_FALSE(reqHandler.hasTransportError(15));
}

TEST_F(HandlerTest, mockRequestInstanceIdExpiryCallBackUnknownKey)
{
#ifndef NDEBUG
    Handler<MockRequest> reqHandler(pldmTransport, event, instanceIdDb, false);
    RequestKey key{eid, 0, 0, 0};
    EXPECT_DEATH(reqHandler.instanceIdExpiryCallBack(key), "");
#endif
}

TEST_F(HandlerTest, failingSendRequestInstanceIdExpiryCallBackUnknownKey)
{
#ifndef NDEBUG
    Handler<FailingSendRequest> reqHandler(pldmTransport, event, instanceIdDb,
                                           false);
    RequestKey key{eid, 0, 0, 0};
    EXPECT_DEATH(reqHandler.instanceIdExpiryCallBack(key), "");
#endif
}

TEST_F(HandlerTest, mockRequestUnregisterQueuedRequestSearchPatterns)
{
    Handler<MockRequest> reqHandler(pldmTransport, event, instanceIdDb, false,
                                    seconds(60), 2, milliseconds(100));

    auto callback =
        [this](mctp_eid_t cbEid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(cbEid, response, respMsgLen);
        };

    auto activeInstanceId = instanceIdDb.next(eid).value();
    ASSERT_EQ(reqHandler.registerRequest(eid, activeInstanceId, 0x30, 0x40,
                                         pldm::Request{}, callback),
              PLDM_SUCCESS);

    struct QueuedKey
    {
        uint8_t instanceId;
        uint8_t type;
        uint8_t cmd;
    };

    std::vector<QueuedKey> keys;
    keys.reserve(8);

    for (uint8_t i = 0; i < 8; ++i)
    {
        auto inst = instanceIdDb.next(eid).value();
        auto type = static_cast<uint8_t>(0x50 + (i % 4));
        auto cmd = static_cast<uint8_t>(0x60 + i);
        keys.push_back({inst, type, cmd});
        ASSERT_EQ(reqHandler.registerRequest(eid, inst, type, cmd,
                                             pldm::Request{}, callback),
                  PLDM_SUCCESS);
    }

    for (size_t i = 0; i < 5; ++i)
    {
        EXPECT_EQ(reqHandler.unregisterRequest(eid, keys[i].instanceId, 0x7f,
                                               keys[i].cmd),
                  PLDM_ERROR);
        EXPECT_EQ(reqHandler.unregisterRequest(eid, keys[i].instanceId,
                                               keys[i].type, 0x7f),
                  PLDM_ERROR);
    }

    const std::array<size_t, 8> order = {5, 1, 7, 0, 6, 2, 4, 3};
    for (size_t idx : order)
    {
        EXPECT_EQ(reqHandler.unregisterRequest(eid, keys[idx].instanceId,
                                               keys[idx].type, keys[idx].cmd),
                  PLDM_SUCCESS);
    }

    EXPECT_EQ(reqHandler.unregisterRequest(eid, activeInstanceId, 0x30, 0x40),
              PLDM_SUCCESS);
}

TEST_F(HandlerTest, requestTypeUnregisterQueuedRequestSearchPatternsEncoded)
{
    Handler<Request> reqHandler(pldmTransport, event, instanceIdDb, false,
                                seconds(60), 2, milliseconds(100));

    auto callback =
        [this](mctp_eid_t cbEid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(cbEid, response, respMsgLen);
        };

    auto makeRequest =
        [](uint8_t instanceId, uint8_t type, uint8_t command) -> pldm::Request {
        pldm::Request request(sizeof(pldm_msg_hdr), 0);
        auto requestPtr = new (request.data()) pldm_msg;
        requestPtr->hdr.instance_id = instanceId;
        requestPtr->hdr.request = 1;
        requestPtr->hdr.type = type;
        requestPtr->hdr.command = command;
        return request;
    };

    auto activeInstanceId = instanceIdDb.next(eid).value();
    ASSERT_EQ(reqHandler.registerRequest(
                  eid, activeInstanceId, PLDM_BASE, PLDM_GET_TID,
                  makeRequest(activeInstanceId, PLDM_BASE, PLDM_GET_TID),
                  callback),
              PLDM_SUCCESS);

    struct QueuedKey
    {
        uint8_t instanceId;
        uint8_t type;
        uint8_t cmd;
    };

    std::vector<QueuedKey> keys;
    keys.reserve(6);

    for (uint8_t i = 0; i < 6; ++i)
    {
        auto inst = instanceIdDb.next(eid).value();
        auto type = static_cast<uint8_t>(0x20 + (i % 3));
        auto cmd = static_cast<uint8_t>(0x30 + i);
        keys.push_back({inst, type, cmd});
        ASSERT_EQ(
            reqHandler.registerRequest(eid, inst, type, cmd,
                                       makeRequest(inst, type, cmd), callback),
            PLDM_SUCCESS);
    }

    for (size_t i = 0; i < keys.size(); ++i)
    {
        EXPECT_EQ(reqHandler.unregisterRequest(eid, keys[i].instanceId, 0x7f,
                                               keys[i].cmd),
                  PLDM_ERROR);
    }

    const std::array<size_t, 6> order = {4, 1, 5, 0, 3, 2};
    for (size_t idx : order)
    {
        EXPECT_EQ(reqHandler.unregisterRequest(eid, keys[idx].instanceId,
                                               keys[idx].type, keys[idx].cmd),
                  PLDM_SUCCESS);
    }

    EXPECT_EQ(reqHandler.unregisterRequest(eid, activeInstanceId, PLDM_BASE,
                                           PLDM_GET_TID),
              PLDM_SUCCESS);
}

TEST_F(HandlerTest, mockRequestCoroutineTimeoutNoTransportError)
{
    exec::async_scope scope;
    Handler<MockRequest> reqHandler(pldmTransport, event, instanceIdDb, false,
                                    seconds(1), 2, milliseconds(100));

    auto instanceId = instanceIdDb.next(eid).value();
    int resultRc = PLDM_SUCCESS;
    const pldm_msg* resultResp = reinterpret_cast<const pldm_msg*>(0x1);
    size_t resultRespLen = 1;

    scope.spawn(
        stdexec::just() | stdexec::let_value([&]() -> exec::task<void> {
            pldm::Request request(sizeof(pldm_msg_hdr) + sizeof(uint8_t), 0);
            auto requestPtr = new (request.data()) pldm_msg;
            requestPtr->hdr.instance_id = instanceId;
            requestPtr->hdr.request = 1;

            std::tie(resultRc, resultResp, resultRespLen) =
                co_await reqHandler.sendRecvMsg(eid, std::move(request));
        }),
        exec::default_task_context<void>(stdexec::inline_scheduler{}));

    waitEventExpiry(milliseconds(500));
    stdexec::sync_wait(scope.on_empty());

    EXPECT_EQ(resultRc, PLDM_ERROR_NOT_READY);
    EXPECT_EQ(resultResp, nullptr);
    EXPECT_EQ(resultRespLen, 0u);
}

TEST_F(HandlerTest, mockRequestTransportErrorStateTransitions)
{
    Handler<MockRequest> reqHandler(pldmTransport, event, instanceIdDb, false);

    EXPECT_FALSE(reqHandler.hasTransportError(21));
    reqHandler.clearTransportError(21);
    EXPECT_FALSE(reqHandler.hasTransportError(21));

    pldm::transport::MctpError mctpError{};
    mctpError.error_code = 111;
    mctpError.direction = MCTP_DIR_TX;
    mctpError.src_eid = 1;
    mctpError.dest_eid = 21;
    mctpError.msg_type = MCTP_MSG_TYPE_PLDM;
    mctpError.payload_len = 2;
    mctpError.payload[0] = 0x01;
    mctpError.payload[1] = 0x06;
    mctpError.binding = 2;
    mctpError.timestamp_ns = 0;

    reqHandler.storeTransportError(mctpError);
    EXPECT_TRUE(reqHandler.hasTransportError(21));

    reqHandler.clearTransportError(22);
    EXPECT_TRUE(reqHandler.hasTransportError(21));

    reqHandler.clearTransportError(21);
    EXPECT_FALSE(reqHandler.hasTransportError(21));
}

TEST_F(HandlerTest, requestTypeCoroutineTimeoutWithTransportError)
{
    exec::async_scope scope;
    Handler<Request> reqHandler(pldmTransport, event, instanceIdDb, false,
                                seconds(1), 2, milliseconds(100));
    auto instanceId = instanceIdDb.next(eid).value();
    int resultRc = PLDM_SUCCESS;
    const pldm_msg* resultResp = nullptr;
    size_t resultRespLen = 0;

    scope.spawn(
        stdexec::just() | stdexec::let_value([&]() -> exec::task<void> {
            pldm::Request request(sizeof(pldm_msg_hdr) + sizeof(uint8_t), 0);
            auto requestPtr = new (request.data()) pldm_msg;
            requestPtr->hdr.instance_id = instanceId;
            requestPtr->hdr.request = 1;

            std::tie(resultRc, resultResp, resultRespLen) =
                co_await reqHandler.sendRecvMsg(eid, std::move(request));
        }),
        exec::default_task_context<void>(stdexec::inline_scheduler{}));

    pldm::transport::MctpError mctpError{};
    mctpError.error_code = EHOSTUNREACH;
    mctpError.direction = MCTP_DIR_TX;
    mctpError.src_eid = 1;
    mctpError.dest_eid = eid;
    mctpError.msg_type = MCTP_MSG_TYPE_PLDM;
    mctpError.payload_len = 2;
    mctpError.payload[0] = PLDM_FWUP;
    mctpError.payload[1] = 0x01;
    mctpError.binding = 2;
    mctpError.timestamp_ns = 0;
    reqHandler.storeTransportError(mctpError);

    waitEventExpiry(milliseconds(500));
    stdexec::sync_wait(scope.on_empty());

    EXPECT_EQ(resultRc, PLDM_REQUESTER_MCTP_TRANSPORT_ERROR);
    EXPECT_EQ(resultResp, nullptr);
    EXPECT_EQ(resultRespLen, 0u);
}

TEST_F(HandlerTest, requestTypeCoroutineTimeoutTaskIntWithTransportError)
{
    exec::async_scope scope;
    Handler<Request> reqHandler(pldmTransport, event, instanceIdDb, false,
                                seconds(1), 2, milliseconds(100));

    auto instanceId = instanceIdDb.next(eid).value();
    int result = PLDM_SUCCESS;

    scope.spawn(stdexec::just() | stdexec::let_value([&]() -> exec::task<void> {
                    result = co_await sendRecvRequestTaskInt(reqHandler, eid,
                                                             instanceId);
                }),
                exec::default_task_context<void>(stdexec::inline_scheduler{}));

    pldm::transport::MctpError mctpError{};
    mctpError.error_code = ETIMEDOUT;
    mctpError.direction = MCTP_DIR_TX;
    mctpError.src_eid = 1;
    mctpError.dest_eid = eid;
    mctpError.msg_type = MCTP_MSG_TYPE_PLDM;
    mctpError.payload_len = 2;
    mctpError.payload[0] = PLDM_BASE;
    mctpError.payload[1] = PLDM_GET_TID;
    mctpError.binding = 2;
    mctpError.timestamp_ns = 0;
    reqHandler.storeTransportError(mctpError);

    waitEventExpiry(milliseconds(500));
    stdexec::sync_wait(scope.on_empty());

    EXPECT_EQ(result, PLDM_REQUESTER_MCTP_TRANSPORT_ERROR);
}

TEST_F(HandlerTest, mockRequestCoroutineTimeoutWithTransportError)
{
    exec::async_scope scope;
    Handler<MockRequest> reqHandler(pldmTransport, event, instanceIdDb, false,
                                    seconds(1), 2, milliseconds(100));
    auto instanceId = instanceIdDb.next(eid).value();
    int resultRc = PLDM_SUCCESS;
    const pldm_msg* resultResp = nullptr;
    size_t resultRespLen = 0;

    scope.spawn(
        stdexec::just() | stdexec::let_value([&]() -> exec::task<void> {
            pldm::Request request(sizeof(pldm_msg_hdr) + sizeof(uint8_t), 0);
            auto requestPtr = new (request.data()) pldm_msg;
            requestPtr->hdr.instance_id = instanceId;
            requestPtr->hdr.request = 1;

            std::tie(resultRc, resultResp, resultRespLen) =
                co_await reqHandler.sendRecvMsg(eid, std::move(request));
        }),
        exec::default_task_context<void>(stdexec::inline_scheduler{}));

    pldm::transport::MctpError mctpError{};
    mctpError.error_code = EHOSTUNREACH;
    mctpError.direction = MCTP_DIR_TX;
    mctpError.src_eid = 1;
    mctpError.dest_eid = eid;
    mctpError.msg_type = MCTP_MSG_TYPE_PLDM;
    mctpError.payload_len = 2;
    mctpError.payload[0] = PLDM_FWUP;
    mctpError.payload[1] = 0x01;
    mctpError.binding = 2;
    mctpError.timestamp_ns = 0;
    reqHandler.storeTransportError(mctpError);

    waitEventExpiry(milliseconds(500));
    stdexec::sync_wait(scope.on_empty());

    EXPECT_EQ(resultRc, PLDM_REQUESTER_MCTP_TRANSPORT_ERROR);
    EXPECT_EQ(resultResp, nullptr);
    EXPECT_EQ(resultRespLen, 0u);
}

TEST_F(HandlerTest, mockRequestUnregisterSharedInstanceComparisons)
{
    Handler<MockRequest> reqHandler(pldmTransport, event, instanceIdDb, false,
                                    seconds(60), 2, milliseconds(100));

    auto callback =
        [this](mctp_eid_t cbEid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(cbEid, response, respMsgLen);
        };

    auto activeInstanceId = instanceIdDb.next(eid).value();
    ASSERT_EQ(reqHandler.registerRequest(eid, activeInstanceId, 0x10, 0x20,
                                         pldm::Request{}, callback),
              PLDM_SUCCESS);

    auto i1 = instanceIdDb.next(eid).value();
    auto i2 = instanceIdDb.next(eid).value();
    auto i3 = instanceIdDb.next(eid).value();
    auto i4 = instanceIdDb.next(eid).value();

    ASSERT_EQ(reqHandler.registerRequest(eid, i1, 0x30, 0x40, pldm::Request{},
                                         callback),
              PLDM_SUCCESS);
    ASSERT_EQ(reqHandler.registerRequest(eid, i2, 0x30, 0x41, pldm::Request{},
                                         callback),
              PLDM_SUCCESS);
    ASSERT_EQ(reqHandler.registerRequest(eid, i3, 0x31, 0x40, pldm::Request{},
                                         callback),
              PLDM_SUCCESS);
    ASSERT_EQ(reqHandler.registerRequest(eid, i4, 0x31, 0x41, pldm::Request{},
                                         callback),
              PLDM_SUCCESS);

    EXPECT_EQ(reqHandler.unregisterRequest(eid, i1, 0x30, 0x42), PLDM_ERROR);
    EXPECT_EQ(reqHandler.unregisterRequest(eid, i2, 0x32, 0x41), PLDM_ERROR);
    EXPECT_EQ(reqHandler.unregisterRequest(eid, i3, 0x30, 0x40), PLDM_ERROR);
    EXPECT_EQ(reqHandler.unregisterRequest(eid, 0x7f, 0x30, 0x40), PLDM_ERROR);

    EXPECT_EQ(reqHandler.unregisterRequest(eid, i2, 0x30, 0x41), PLDM_SUCCESS);
    EXPECT_EQ(reqHandler.unregisterRequest(eid, i3, 0x31, 0x40), PLDM_SUCCESS);
    EXPECT_EQ(reqHandler.unregisterRequest(eid, i4, 0x31, 0x41), PLDM_SUCCESS);
    EXPECT_EQ(reqHandler.unregisterRequest(eid, i1, 0x30, 0x40), PLDM_SUCCESS);
    EXPECT_EQ(reqHandler.unregisterRequest(eid, activeInstanceId, 0x10, 0x20),
              PLDM_SUCCESS);
}

TEST_F(HandlerTest,
       requestTypeCoroutineTimeoutTaskIntUponStoppedNoTransportError)
{
    exec::async_scope scope;
    Handler<Request> reqHandler(pldmTransport, event, instanceIdDb, false,
                                seconds(1), 2, milliseconds(100));

    auto instanceId = instanceIdDb.next(eid).value();
    int resultRc = PLDM_SUCCESS;
    bool stopped = false;

    scope.spawn(stdexec::just() | stdexec::let_value([&]() -> exec::task<void> {
                    resultRc = co_await sendRecvRequestTaskInt(reqHandler, eid,
                                                               instanceId);
                }) | stdexec::upon_stopped([&] { stopped = true; }),
                exec::default_task_context<void>(stdexec::inline_scheduler{}));

    waitEventExpiry(milliseconds(500));

    stdexec::sync_wait(scope.on_empty());
    EXPECT_FALSE(stopped);
    EXPECT_EQ(resultRc, PLDM_ERROR_NOT_READY);
}

TEST_F(HandlerTest,
       requestTypeCoroutineTimeoutTaskIntUponStoppedWithTransportError)
{
    exec::async_scope scope;
    Handler<Request> reqHandler(pldmTransport, event, instanceIdDb, false,
                                seconds(1), 2, milliseconds(100));

    auto instanceId = instanceIdDb.next(eid).value();
    int resultRc = PLDM_SUCCESS;
    bool stopped = false;

    scope.spawn(stdexec::just() | stdexec::let_value([&]() -> exec::task<void> {
                    resultRc = co_await sendRecvRequestTaskInt(reqHandler, eid,
                                                               instanceId);
                }) | stdexec::upon_stopped([&] { stopped = true; }),
                exec::default_task_context<void>(stdexec::inline_scheduler{}));

    pldm::transport::MctpError mctpError{};
    mctpError.error_code = ETIMEDOUT;
    mctpError.direction = MCTP_DIR_TX;
    mctpError.src_eid = 1;
    mctpError.dest_eid = eid;
    mctpError.msg_type = MCTP_MSG_TYPE_PLDM;
    mctpError.payload_len = 2;
    mctpError.payload[0] = PLDM_BASE;
    mctpError.payload[1] = PLDM_GET_TID;
    mctpError.binding = 2;
    mctpError.timestamp_ns = 0;
    reqHandler.storeTransportError(mctpError);

    waitEventExpiry(milliseconds(500));

    stdexec::sync_wait(scope.on_empty());
    EXPECT_FALSE(stopped);
    EXPECT_EQ(resultRc, PLDM_REQUESTER_MCTP_TRANSPORT_ERROR);
}

TEST_F(HandlerTest, mockRequestUnregisterSearchStressPatterns)
{
    Handler<MockRequest> reqHandler(pldmTransport, event, instanceIdDb, false,
                                    seconds(60), 2, milliseconds(100));

    auto callback =
        [this](mctp_eid_t cbEid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(cbEid, response, respMsgLen);
        };

    auto activeInstanceId = instanceIdDb.next(eid).value();
    ASSERT_EQ(reqHandler.registerRequest(eid, activeInstanceId, 0x10, 0x20,
                                         pldm::Request{}, callback),
              PLDM_SUCCESS);

    struct Key
    {
        uint8_t instanceId;
        uint8_t type;
        uint8_t cmd;
    };

    std::vector<Key> keys;
    keys.reserve(12);
    for (uint8_t i = 0; i < 12; ++i)
    {
        auto inst = instanceIdDb.next(eid).value();
        auto type = static_cast<uint8_t>(0x40 + (i % 4));
        auto cmd = static_cast<uint8_t>(0x60 + i);
        keys.push_back({inst, type, cmd});
        ASSERT_EQ(reqHandler.registerRequest(eid, inst, type, cmd,
                                             pldm::Request{}, callback),
                  PLDM_SUCCESS);
    }

    for (size_t i = 0; i < keys.size(); ++i)
    {
        EXPECT_EQ(
            reqHandler.unregisterRequest(eid, keys[i].instanceId, keys[i].type,
                                         static_cast<uint8_t>(keys[i].cmd + 1)),
            PLDM_ERROR);
        EXPECT_EQ(reqHandler.unregisterRequest(
                      eid, keys[i].instanceId,
                      static_cast<uint8_t>(keys[i].type + 1), keys[i].cmd),
                  PLDM_ERROR);
        EXPECT_EQ(reqHandler.unregisterRequest(
                      eid,
                      static_cast<uint8_t>((keys[i].instanceId + 13) & 0x1F),
                      keys[i].type, keys[i].cmd),
                  PLDM_ERROR);
    }

    for (size_t i = 0; i + 1 < keys.size(); ++i)
    {
        EXPECT_EQ(
            reqHandler.unregisterRequest(eid, keys[i].instanceId,
                                         keys[i + 1].type, keys[i + 1].cmd),
            PLDM_ERROR);
    }

    const std::array<size_t, 12> order = {7, 3, 10, 1, 9, 5, 11, 0, 8, 2, 6, 4};
    for (size_t idx : order)
    {
        EXPECT_EQ(reqHandler.unregisterRequest(eid, keys[idx].instanceId,
                                               keys[idx].type, keys[idx].cmd),
                  PLDM_SUCCESS);
    }

    EXPECT_EQ(reqHandler.unregisterRequest(eid, activeInstanceId, 0x10, 0x20),
              PLDM_SUCCESS);
}

TEST_F(HandlerTest, requestTypeUnregisterSearchStressPatterns)
{
    Handler<Request> reqHandler(pldmTransport, event, instanceIdDb, false,
                                seconds(60), 2, milliseconds(100));

    auto callback =
        [this](mctp_eid_t cbEid, const pldm_msg* response, size_t respMsgLen) {
            this->pldmResponseCallBack(cbEid, response, respMsgLen);
        };

    auto makeRequest =
        [](uint8_t instanceId, uint8_t type, uint8_t command) -> pldm::Request {
        pldm::Request request(sizeof(pldm_msg_hdr), 0);
        auto requestPtr = new (request.data()) pldm_msg;
        requestPtr->hdr.instance_id = instanceId;
        requestPtr->hdr.request = 1;
        requestPtr->hdr.type = type;
        requestPtr->hdr.command = command;
        return request;
    };

    auto activeInstanceId = instanceIdDb.next(eid).value();
    ASSERT_EQ(reqHandler.registerRequest(
                  eid, activeInstanceId, 0x12, 0x22,
                  makeRequest(activeInstanceId, 0x12, 0x22), callback),
              PLDM_SUCCESS);

    struct Key
    {
        uint8_t instanceId;
        uint8_t type;
        uint8_t cmd;
    };

    std::vector<Key> keys;
    keys.reserve(10);
    for (uint8_t i = 0; i < 10; ++i)
    {
        auto inst = instanceIdDb.next(eid).value();
        auto type = static_cast<uint8_t>(0x30 + (i % 5));
        auto cmd = static_cast<uint8_t>(0x50 + i);
        keys.push_back({inst, type, cmd});
        ASSERT_EQ(
            reqHandler.registerRequest(eid, inst, type, cmd,
                                       makeRequest(inst, type, cmd), callback),
            PLDM_SUCCESS);
    }

    for (size_t i = 0; i < keys.size(); ++i)
    {
        EXPECT_EQ(
            reqHandler.unregisterRequest(eid, keys[i].instanceId, keys[i].type,
                                         static_cast<uint8_t>(keys[i].cmd + 1)),
            PLDM_ERROR);
        EXPECT_EQ(reqHandler.unregisterRequest(
                      eid, keys[i].instanceId,
                      static_cast<uint8_t>(keys[i].type + 1), keys[i].cmd),
                  PLDM_ERROR);
        EXPECT_EQ(reqHandler.unregisterRequest(
                      eid,
                      static_cast<uint8_t>((keys[i].instanceId + 9) & 0x1F),
                      keys[i].type, keys[i].cmd),
                  PLDM_ERROR);
    }

    const std::array<size_t, 10> order = {6, 2, 8, 1, 9, 0, 7, 3, 5, 4};
    for (size_t idx : order)
    {
        EXPECT_EQ(reqHandler.unregisterRequest(eid, keys[idx].instanceId,
                                               keys[idx].type, keys[idx].cmd),
                  PLDM_SUCCESS);
    }

    EXPECT_EQ(reqHandler.unregisterRequest(eid, activeInstanceId, 0x12, 0x22),
              PLDM_SUCCESS);
}
