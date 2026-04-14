#include "mock_request.hpp"
#include "requester/request.hpp"
#include "test/test_instance_id.hpp"

#include <dlfcn.h>
#include <libpldm/base.h>
#include <systemd/sd-event.h>

#include <type_traits>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#define protected public
#include "requester/handler.hpp"
#undef protected
#undef private
#ifdef __clang__
#pragma clang diagnostic pop
#endif

using namespace pldm::requester;
using namespace std::chrono;

using ::testing::DefaultValue;
using ::testing::NiceMock;
using ::testing::Return;

static bool g_failHandlerCoverageTimerStart = false;
static bool g_failHandlerCoverageTimerStop = false;
static uint64_t g_failHandlerCoverageTimerStartUsec = 0;
static int g_failHandlerCoverageTimerStartEnableCall = 0;
static int g_handlerCoverageTimerStartEnableCallCount = 0;

extern "C" int sd_event_source_set_time_relative(sd_event_source* source,
                                                 uint64_t usec)
{
    if (g_failHandlerCoverageTimerStart &&
        (g_failHandlerCoverageTimerStartUsec == 0 ||
         g_failHandlerCoverageTimerStartUsec == usec))
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
    if (mode != SD_EVENT_OFF && g_failHandlerCoverageTimerStartEnableCall > 0)
    {
        g_handlerCoverageTimerStartEnableCallCount++;
        if (g_handlerCoverageTimerStartEnableCallCount ==
            g_failHandlerCoverageTimerStartEnableCall)
        {
            return -EINVAL;
        }
    }

    if (g_failHandlerCoverageTimerStart &&
        g_failHandlerCoverageTimerStartUsec == 0 && mode != SD_EVENT_OFF)
    {
        return -EINVAL;
    }

    if (g_failHandlerCoverageTimerStop && mode == SD_EVENT_OFF)
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

class MockPldmTransport : public PldmTransport
{
  public:
    MockPldmTransport() : PldmTransport(NoInit{}) {}

    MOCK_METHOD(pldm_requester_rc_t, sendMsg,
                (pldm_tid_t tid, const void* tx, size_t len), (override));
};

class FailingSendRequest : public RequestRetryTimer
{
  public:
    FailingSendRequest(PldmTransport* /*pldmTransport*/, mctp_eid_t /*eid*/,
                       sdeventplus::Event& event,
                       pldm::Request&& /*requestMsg*/, uint8_t numRetries,
                       milliseconds responseTimeOut, bool /*verbose*/) :
        RequestRetryTimer(event, numRetries, responseTimeOut)
    {}

    int send() const override
    {
        return PLDM_ERROR;
    }
};

class HandlerCoverageTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        g_failHandlerCoverageTimerStart = false;
        g_failHandlerCoverageTimerStop = false;
        g_failHandlerCoverageTimerStartUsec = 0;
        g_failHandlerCoverageTimerStartEnableCall = 0;
        g_handlerCoverageTimerStartEnableCallCount = 0;
        resetTracking();
        DefaultValue<int>::Clear();
    }

    void TearDown() override
    {
        DefaultValue<int>::Clear();
    }

    void resetTracking()
    {
        callbackCount = 0;
        nullResponse = false;
        validResponse = false;
    }

    pldm::Request makeValidRequest(uint8_t instanceId = 0, uint8_t type = 0,
                                   uint8_t command = 0)
    {
        pldm::Request request(sizeof(pldm_msg_hdr), 0);
        auto* hdr = reinterpret_cast<pldm_msg_hdr*>(request.data());
        hdr->instance_id = instanceId;
        hdr->request = 1;
        hdr->type = type;
        hdr->command = command;
        return request;
    }

    ResponseHandler makeTrackingHandler()
    {
        return [this](mctp_eid_t /*eid*/, const pldm_msg* response,
                      size_t respMsgLen) {
            if (response == nullptr && respMsgLen == 0)
            {
                nullResponse = true;
            }
            else
            {
                validResponse = true;
            }
            callbackCount++;
        };
    }

    pldm::transport::MctpError makeTransportError(mctp_eid_t endpoint) const
    {
        pldm::transport::MctpError mctpError{};
        mctpError.error_code = 110;
        mctpError.direction = MCTP_DIR_TX;
        mctpError.binding = 2;
        mctpError.src_eid = 1;
        mctpError.dest_eid = endpoint;
        mctpError.timestamp_ns = 123456789;
        mctpError.msg_type = MCTP_MSG_TYPE_PLDM;
        mctpError.payload_len = 2;
        mctpError.payload[0] = 0x01;
        mctpError.payload[1] = 0x05;
        return mctpError;
    }

    template <typename RequestInterface>
    void seedHandlerEntry(Handler<RequestInterface>& handler, RequestKey key,
                          std::unique_ptr<RequestInterface> request,
                          bool activeRequest = true)
    {
        handler.endpointMessageQueues[key.eid] =
            std::make_shared<EndpointMessageQueue>(
                EndpointMessageQueue{key.eid, {}, activeRequest});
        auto timer = std::make_unique<sdbusplus::Timer>(event.get(), [] {});
        handler.handlers.emplace(
            key, std::make_tuple(std::move(request), makeTrackingHandler(),
                                 std::move(timer)));
    }

    template <typename RequestInterface>
    void addPendingRemoval(Handler<RequestInterface>& handler, RequestKey key)
    {
        handler.removeRequestContainer.emplace(
            key, std::make_unique<sdeventplus::source::Defer>(
                     event, [](sdeventplus::source::EventBase&) {}));
    }

    template <typename RequestInterface>
    std::unique_ptr<RequestInterface> makeSeedRequest(mctp_eid_t endpoint,
                                                      uint8_t instanceId)
    {
        if constexpr (std::is_same_v<RequestInterface, Request>)
        {
            return std::make_unique<Request>(nullptr, endpoint, event,
                                             makeValidRequest(instanceId), 0,
                                             milliseconds(100), false);
        }
        else
        {
            return std::make_unique<RequestInterface>(
                nullptr, endpoint, event, pldm::Request{}, 0, milliseconds(100),
                false);
        }
    }

    template <typename RequestInterface>
    void exerciseExpiryStopFailureAndSuppressedLog(mctp_eid_t endpoint)
    {
        Handler<RequestInterface> handler(nullptr, event, instanceIdDb, false,
                                          seconds(1), 0, milliseconds(100));

        auto instanceId1 = instanceIdDb.next(endpoint).value();
        RequestKey key1{endpoint, instanceId1, 0, 0};
        seedHandlerEntry(
            handler, key1,
            makeSeedRequest<RequestInterface>(endpoint, instanceId1));

        g_failHandlerCoverageTimerStop = true;
        handler.instanceIdExpiryCallBack(key1);
        g_failHandlerCoverageTimerStop = false;
        handler.removeRequestEntry(key1);

        auto instanceId2 = instanceIdDb.next(endpoint).value();
        RequestKey key2{endpoint, instanceId2, 0, 1};
        seedHandlerEntry(
            handler, key2,
            makeSeedRequest<RequestInterface>(endpoint, instanceId2));
        handler.instanceIdExpiryCallBack(key2);
        handler.removeRequestEntry(key2);
    }

    sdeventplus::Event event = sdeventplus::Event::get_default();
    TestInstanceIdDb instanceIdDb;
    mctp_eid_t eid = 8;
    int callbackCount = 0;
    bool nullResponse = false;
    bool validResponse = false;
};

TEST_F(HandlerCoverageTest, MockRequestRegisterRequestDuplicateKey)
{
    Handler<MockRequest> handler(nullptr, event, instanceIdDb, false,
                                 seconds(1), 0, milliseconds(100));
    auto instanceId = instanceIdDb.next(eid).value();

    EXPECT_EQ(handler.registerRequest(eid, instanceId, 0, 0, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_SUCCESS);
    EXPECT_EQ(handler.registerRequest(eid, instanceId, 0, 0, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_ERROR);
    EXPECT_EQ(handler.unregisterRequest(eid, instanceId, 0, 0), PLDM_SUCCESS);
}

TEST_F(HandlerCoverageTest, RequestRegisterRequestDuplicateKey)
{
    MockPldmTransport mockTransport;
    Handler<Request> handler(&mockTransport, event, instanceIdDb, false,
                             seconds(1), 0, milliseconds(100));
    auto instanceId = instanceIdDb.next(eid).value();

    EXPECT_CALL(mockTransport, sendMsg(testing::_, testing::_, testing::_))
        .WillOnce(Return(PLDM_REQUESTER_SUCCESS));

    EXPECT_EQ(handler.registerRequest(eid, instanceId, 0, 0,
                                      makeValidRequest(instanceId),
                                      makeTrackingHandler()),
              PLDM_SUCCESS);
    EXPECT_EQ(handler.registerRequest(eid, instanceId, 0, 0,
                                      makeValidRequest(instanceId),
                                      makeTrackingHandler()),
              PLDM_ERROR);
    EXPECT_EQ(handler.unregisterRequest(eid, instanceId, 0, 0), PLDM_SUCCESS);
}

TEST_F(HandlerCoverageTest, RequestUnregisterQueuedRequestSearchFoundAndMissing)
{
    MockPldmTransport mockTransport;
    Handler<Request> handler(&mockTransport, event, instanceIdDb, false,
                             seconds(1), 0, milliseconds(100));
    auto activeId = instanceIdDb.next(eid).value();
    auto queuedId = instanceIdDb.next(eid).value();

    EXPECT_CALL(mockTransport, sendMsg(testing::_, testing::_, testing::_))
        .WillOnce(Return(PLDM_REQUESTER_SUCCESS));

    EXPECT_EQ(handler.registerRequest(eid, activeId, 0, 0,
                                      makeValidRequest(activeId, 0, 0),
                                      makeTrackingHandler()),
              PLDM_SUCCESS);
    EXPECT_EQ(handler.registerRequest(eid, queuedId, 0, 1,
                                      makeValidRequest(queuedId, 0, 1),
                                      makeTrackingHandler()),
              PLDM_SUCCESS);

    EXPECT_EQ(handler.unregisterRequest(eid, queuedId, 1, 1), PLDM_ERROR);
    EXPECT_EQ(handler.unregisterRequest(eid, queuedId, 0, 1), PLDM_SUCCESS);
    EXPECT_EQ(handler.unregisterRequest(eid, activeId, 0, 0), PLDM_SUCCESS);
}

TEST_F(HandlerCoverageTest, FailingSendRequestRegisterRequestDuplicateKey)
{
    Handler<FailingSendRequest> handler(nullptr, event, instanceIdDb, false,
                                        seconds(1), 0, milliseconds(100));
    auto instanceId = instanceIdDb.next(eid).value();
    RequestKey key{eid, instanceId, 0, 0};
    seedHandlerEntry(handler, key,
                     makeSeedRequest<FailingSendRequest>(eid, instanceId));

    EXPECT_EQ(handler.registerRequest(eid, instanceId, 0, 0, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_ERROR);

    handler.handlers.erase(key);
    handler.endpointMessageQueues.erase(eid);
    instanceIdDb.free(eid, instanceId);
}

TEST_F(HandlerCoverageTest, FailingSendRequestPollEndpointQueueReturnsWhenBusy)
{
    Handler<FailingSendRequest> handler(nullptr, event, instanceIdDb, false,
                                        seconds(1), 0, milliseconds(100));
    auto instanceId = instanceIdDb.next(eid).value();
    auto registered = std::make_shared<RegisteredRequest>(RegisteredRequest{
        {eid, instanceId, 0, 0}, pldm::Request{}, makeTrackingHandler()});
    std::deque<std::shared_ptr<RegisteredRequest>> requestQueue{registered};
    handler.endpointMessageQueues[eid] = std::make_shared<EndpointMessageQueue>(
        EndpointMessageQueue{eid, requestQueue, true});

    EXPECT_EQ(handler.pollEndpointQueue(eid), PLDM_SUCCESS);
    EXPECT_EQ(handler.endpointMessageQueues[eid]->requestQueue.size(), 1u);
    EXPECT_TRUE(handler.endpointMessageQueues[eid]->activeRequest);

    instanceIdDb.free(eid, instanceId);
}

TEST_F(HandlerCoverageTest, MockRequestPollEndpointQueueSendFailure)
{
    Handler<MockRequest> handler(nullptr, event, instanceIdDb, false,
                                 seconds(1), 0, milliseconds(100));
    auto instanceId = instanceIdDb.next(eid).value();

    DefaultValue<int>::Set(PLDM_ERROR);
    EXPECT_EQ(handler.registerRequest(eid, instanceId, 0, 0, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_ERROR);
    DefaultValue<int>::Clear();

    EXPECT_EQ(callbackCount, 1);
    EXPECT_TRUE(nullResponse);
}

TEST_F(HandlerCoverageTest, NiceMockRequestPollEndpointQueueSendFailure)
{
    Handler<NiceMock<MockRequest>> handler(nullptr, event, instanceIdDb, false,
                                           seconds(1), 0, milliseconds(100));
    auto instanceId = instanceIdDb.next(eid).value();

    DefaultValue<int>::Set(PLDM_ERROR);
    EXPECT_EQ(handler.registerRequest(eid, instanceId, 0, 0, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_ERROR);
    DefaultValue<int>::Clear();

    EXPECT_EQ(callbackCount, 1);
    EXPECT_TRUE(nullResponse);
}

TEST_F(HandlerCoverageTest, MockRequestPollEndpointQueueTimerStartFailure)
{
    Handler<MockRequest> handler(nullptr, event, instanceIdDb, false,
                                 seconds(1), 0, milliseconds(100));
    auto instanceId = instanceIdDb.next(eid).value();

    g_failHandlerCoverageTimerStart = true;
    EXPECT_EQ(handler.registerRequest(eid, instanceId, 0, 0, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_ERROR);
    g_failHandlerCoverageTimerStart = false;

    EXPECT_EQ(callbackCount, 1);
    EXPECT_TRUE(nullResponse);
}

TEST_F(HandlerCoverageTest, RequestPollEndpointQueueTimerStartFailure)
{
    MockPldmTransport mockTransport;
    Handler<Request> handler(&mockTransport, event, instanceIdDb, false,
                             seconds(1), 0, milliseconds(100));
    auto instanceId = instanceIdDb.next(eid).value();

    EXPECT_CALL(mockTransport, sendMsg(testing::_, testing::_, testing::_))
        .WillOnce(Return(PLDM_REQUESTER_SUCCESS));

    g_failHandlerCoverageTimerStart = true;
    EXPECT_EQ(handler.registerRequest(eid, instanceId, 0, 0,
                                      makeValidRequest(instanceId),
                                      makeTrackingHandler()),
              PLDM_ERROR);
    g_failHandlerCoverageTimerStart = false;

    EXPECT_EQ(callbackCount, 1);
    EXPECT_TRUE(nullResponse);
}

TEST_F(HandlerCoverageTest,
       FailingSendRequestPollEndpointQueueHandlerTimerStartFailure)
{
    Handler<FailingSendRequest> handler(nullptr, event, instanceIdDb, false,
                                        seconds(1), 1, milliseconds(100));
    auto instanceId = instanceIdDb.next(eid).value();

    g_failHandlerCoverageTimerStartEnableCall = 2;
    EXPECT_EQ(handler.registerRequest(eid, instanceId, 0, 0, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_ERROR);
    g_failHandlerCoverageTimerStartEnableCall = 0;

    EXPECT_EQ(callbackCount, 1);
    EXPECT_TRUE(nullResponse);
}

TEST_F(HandlerCoverageTest, MockRequestHandleResponseUnknownKey)
{
    Handler<MockRequest> handler(nullptr, event, instanceIdDb, false,
                                 seconds(1), 0, milliseconds(100));
    pldm::Response response(sizeof(pldm_msg_hdr), 0);
    auto* responsePtr = reinterpret_cast<const pldm_msg*>(response.data());

    handler.handleResponse(eid, 0x44, 0, 0, responsePtr, response.size());

    EXPECT_EQ(callbackCount, 0);
    EXPECT_FALSE(nullResponse);
    EXPECT_FALSE(validResponse);
}

TEST_F(HandlerCoverageTest, NiceMockRequestHandleResponseSkipsPendingRemoval)
{
    Handler<NiceMock<MockRequest>> handler(nullptr, event, instanceIdDb, false,
                                           seconds(1), 0, milliseconds(100));
    auto instanceId = instanceIdDb.next(eid).value();
    RequestKey key{eid, instanceId, 0, 0};
    seedHandlerEntry(handler, key,
                     makeSeedRequest<NiceMock<MockRequest>>(eid, instanceId));
    addPendingRemoval(handler, key);

    pldm::Response response(sizeof(pldm_msg_hdr), 0);
    auto* responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    handler.handleResponse(eid, instanceId, 0, 0, responsePtr, response.size());

    EXPECT_EQ(callbackCount, 0);
    EXPECT_TRUE(handler.handlers.contains(key));
    EXPECT_TRUE(handler.removeRequestContainer.contains(key));

    handler.removeRequestEntry(key);
}

TEST_F(HandlerCoverageTest, MockRequestHandleResponseSkipsPendingRemoval)
{
    Handler<MockRequest> handler(nullptr, event, instanceIdDb, false,
                                 seconds(1), 0, milliseconds(100));
    auto instanceId = instanceIdDb.next(eid).value();
    RequestKey key{eid, instanceId, 0, 0};
    seedHandlerEntry(handler, key,
                     makeSeedRequest<MockRequest>(eid, instanceId));
    addPendingRemoval(handler, key);

    pldm::Response response(sizeof(pldm_msg_hdr), 0);
    auto* responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    handler.handleResponse(eid, instanceId, 0, 0, responsePtr, response.size());

    EXPECT_EQ(callbackCount, 0);
    EXPECT_TRUE(handler.handlers.contains(key));
    EXPECT_TRUE(handler.removeRequestContainer.contains(key));

    handler.removeRequestEntry(key);
}

TEST_F(HandlerCoverageTest, MockRequestHandleResponseTimerStopFailure)
{
    Handler<MockRequest> handler(nullptr, event, instanceIdDb, false,
                                 seconds(1), 0, milliseconds(100));
    auto instanceId = instanceIdDb.next(eid).value();

    EXPECT_EQ(handler.registerRequest(eid, instanceId, 0, 0, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_SUCCESS);

    pldm::Response response(sizeof(pldm_msg_hdr), 0);
    auto* responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    g_failHandlerCoverageTimerStop = true;
    handler.handleResponse(eid, instanceId, 0, 0, responsePtr, response.size());
    g_failHandlerCoverageTimerStop = false;

    EXPECT_EQ(callbackCount, 1);
    EXPECT_TRUE(validResponse);
}

TEST_F(HandlerCoverageTest, RequestHandleResponseTimerStopFailure)
{
    MockPldmTransport mockTransport;
    Handler<Request> handler(&mockTransport, event, instanceIdDb, false,
                             seconds(1), 0, milliseconds(100));
    auto instanceId = instanceIdDb.next(eid).value();

    EXPECT_CALL(mockTransport, sendMsg(testing::_, testing::_, testing::_))
        .WillOnce(Return(PLDM_REQUESTER_SUCCESS));

    EXPECT_EQ(handler.registerRequest(eid, instanceId, 0, 0,
                                      makeValidRequest(instanceId),
                                      makeTrackingHandler()),
              PLDM_SUCCESS);

    pldm::Response response(sizeof(pldm_msg_hdr), 0);
    auto* responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    g_failHandlerCoverageTimerStop = true;
    handler.handleResponse(eid, instanceId, 0, 0, responsePtr, response.size());
    g_failHandlerCoverageTimerStop = false;

    EXPECT_EQ(callbackCount, 1);
    EXPECT_TRUE(validResponse);
}

TEST_F(HandlerCoverageTest, RemoveRequestEntryIgnoresMissingKeysForAllHandlers)
{
    Handler<Request> requestHandler(nullptr, event, instanceIdDb, false,
                                    seconds(1), 0, milliseconds(100));
    Handler<MockRequest> mockHandler(nullptr, event, instanceIdDb, false,
                                     seconds(1), 0, milliseconds(100));
    Handler<FailingSendRequest> failingHandler(
        nullptr, event, instanceIdDb, false, seconds(1), 0, milliseconds(100));

    auto requestId = instanceIdDb.next(0x21).value();
    auto mockId = instanceIdDb.next(0x22).value();
    auto failingId = instanceIdDb.next(0x23).value();

    requestHandler.removeRequestEntry({0x21, requestId, 0, 0});
    mockHandler.removeRequestEntry({0x22, mockId, 0, 0});
    failingHandler.removeRequestEntry({0x23, failingId, 0, 0});

    instanceIdDb.free(0x21, requestId);
    instanceIdDb.free(0x22, mockId);
    instanceIdDb.free(0x23, failingId);
}

TEST_F(HandlerCoverageTest, NiceMockRemoveRequestEntryIgnoresMissingKey)
{
    Handler<NiceMock<MockRequest>> handler(nullptr, event, instanceIdDb, false,
                                           seconds(1), 0, milliseconds(100));
    auto instanceId = instanceIdDb.next(0x24).value();

    handler.removeRequestEntry({0x24, instanceId, 0, 0});

    instanceIdDb.free(0x24, instanceId);
}

TEST_F(HandlerCoverageTest,
       RemoveRequestEntrySkipsMissingKeyWithPendingEntriesForAllHandlers)
{
    Handler<Request> requestHandler(nullptr, event, instanceIdDb, false,
                                    seconds(1), 0, milliseconds(100));
    Handler<MockRequest> mockHandler(nullptr, event, instanceIdDb, false,
                                     seconds(1), 0, milliseconds(100));
    Handler<NiceMock<MockRequest>> niceHandler(
        nullptr, event, instanceIdDb, false, seconds(1), 0, milliseconds(100));
    Handler<FailingSendRequest> failingHandler(
        nullptr, event, instanceIdDb, false, seconds(1), 0, milliseconds(100));

    auto exercise = [this]<typename RequestInterface>(
                        Handler<RequestInterface>& handler, mctp_eid_t endpoint,
                        uint8_t instanceId, uint8_t missingId) {
        RequestKey key{endpoint, instanceId, 0, 0};
        RequestKey missingKey{endpoint, missingId, 0, 1};
        seedHandlerEntry(
            handler, key,
            makeSeedRequest<RequestInterface>(endpoint, instanceId));
        addPendingRemoval(handler, key);

        handler.removeRequestEntry(missingKey);
        EXPECT_TRUE(handler.removeRequestContainer.contains(key));

        handler.removeRequestEntry(key);
        EXPECT_FALSE(handler.removeRequestContainer.contains(key));
        EXPECT_FALSE(handler.handlers.contains(key));
    };

    auto requestId = instanceIdDb.next(0x51).value();
    auto requestMissingId = instanceIdDb.next(0x51).value();
    auto mockId = instanceIdDb.next(0x52).value();
    auto mockMissingId = instanceIdDb.next(0x52).value();
    auto niceId = instanceIdDb.next(0x53).value();
    auto niceMissingId = instanceIdDb.next(0x53).value();
    auto failingId = instanceIdDb.next(0x54).value();
    auto failingMissingId = instanceIdDb.next(0x54).value();

    exercise(requestHandler, 0x51, requestId, requestMissingId);
    exercise(mockHandler, 0x52, mockId, mockMissingId);
    exercise(niceHandler, 0x53, niceId, niceMissingId);
    exercise(failingHandler, 0x54, failingId, failingMissingId);

    instanceIdDb.free(0x51, requestMissingId);
    instanceIdDb.free(0x52, mockMissingId);
    instanceIdDb.free(0x53, niceMissingId);
    instanceIdDb.free(0x54, failingMissingId);
}

TEST_F(HandlerCoverageTest,
       InstanceIdExpiryCoversStopFailureAndRepeatedLogSuppression)
{
    exerciseExpiryStopFailureAndSuppressedLog<Request>(0x31);
    exerciseExpiryStopFailureAndSuppressedLog<MockRequest>(0x32);
    exerciseExpiryStopFailureAndSuppressedLog<FailingSendRequest>(0x33);

    EXPECT_EQ(callbackCount, 6);
    EXPECT_TRUE(nullResponse);
}

TEST_F(HandlerCoverageTest, NiceMockInstanceIdExpirySuppressesRepeatedLogs)
{
    exerciseExpiryStopFailureAndSuppressedLog<NiceMock<MockRequest>>(0x34);

    EXPECT_EQ(callbackCount, 2);
    EXPECT_TRUE(nullResponse);
}

TEST_F(HandlerCoverageTest,
       FailingSendRequestInstanceIdExpiryUsesTransportError)
{
    Handler<FailingSendRequest> handler(nullptr, event, instanceIdDb, false,
                                        seconds(1), 0, milliseconds(100));
    auto instanceId = instanceIdDb.next(eid).value();
    RequestKey key{eid, instanceId, 0, 0};
    seedHandlerEntry(handler, key,
                     makeSeedRequest<FailingSendRequest>(eid, instanceId));
    handler.storeTransportError(makeTransportError(eid));

    handler.instanceIdExpiryCallBack(key);

    EXPECT_EQ(callbackCount, 1);
    EXPECT_TRUE(nullResponse);
    handler.removeRequestEntry(key);
}

TEST_F(HandlerCoverageTest, PollEndpointQueueReturnsWhenQueueIsEmpty)
{
    Handler<Request> requestHandler(nullptr, event, instanceIdDb, false,
                                    seconds(1), 0, milliseconds(100));
    Handler<MockRequest> mockHandler(nullptr, event, instanceIdDb, false,
                                     seconds(1), 0, milliseconds(100));
    Handler<NiceMock<MockRequest>> niceHandler(
        nullptr, event, instanceIdDb, false, seconds(1), 0, milliseconds(100));

    requestHandler.endpointMessageQueues[eid] =
        std::make_shared<EndpointMessageQueue>(
            EndpointMessageQueue{eid, {}, false});
    mockHandler.endpointMessageQueues[eid] =
        std::make_shared<EndpointMessageQueue>(
            EndpointMessageQueue{eid, {}, false});
    niceHandler.endpointMessageQueues[eid] =
        std::make_shared<EndpointMessageQueue>(
            EndpointMessageQueue{eid, {}, false});

    EXPECT_EQ(requestHandler.pollEndpointQueue(eid), PLDM_SUCCESS);
    EXPECT_EQ(mockHandler.pollEndpointQueue(eid), PLDM_SUCCESS);
    EXPECT_EQ(niceHandler.pollEndpointQueue(eid), PLDM_SUCCESS);

    EXPECT_FALSE(requestHandler.endpointMessageQueues[eid]->activeRequest);
    EXPECT_FALSE(mockHandler.endpointMessageQueues[eid]->activeRequest);
    EXPECT_FALSE(niceHandler.endpointMessageQueues[eid]->activeRequest);
}

TEST_F(HandlerCoverageTest, PollEndpointQueueReturnsWhenRequestIsAlreadyActive)
{
    Handler<Request> requestHandler(nullptr, event, instanceIdDb, false,
                                    seconds(1), 0, milliseconds(100));
    Handler<MockRequest> mockHandler(nullptr, event, instanceIdDb, false,
                                     seconds(1), 0, milliseconds(100));
    Handler<NiceMock<MockRequest>> niceHandler(
        nullptr, event, instanceIdDb, false, seconds(1), 0, milliseconds(100));
    auto registered = std::make_shared<RegisteredRequest>(RegisteredRequest{
        {eid, 0, 0, 0}, pldm::Request{}, makeTrackingHandler()});
    std::deque<std::shared_ptr<RegisteredRequest>> queue{registered};

    requestHandler.endpointMessageQueues[eid] =
        std::make_shared<EndpointMessageQueue>(
            EndpointMessageQueue{eid, queue, true});
    mockHandler.endpointMessageQueues[eid] =
        std::make_shared<EndpointMessageQueue>(
            EndpointMessageQueue{eid, queue, true});
    niceHandler.endpointMessageQueues[eid] =
        std::make_shared<EndpointMessageQueue>(
            EndpointMessageQueue{eid, queue, true});

    EXPECT_EQ(requestHandler.pollEndpointQueue(eid), PLDM_SUCCESS);
    EXPECT_EQ(mockHandler.pollEndpointQueue(eid), PLDM_SUCCESS);
    EXPECT_EQ(niceHandler.pollEndpointQueue(eid), PLDM_SUCCESS);

    EXPECT_EQ(requestHandler.endpointMessageQueues[eid]->requestQueue.size(),
              1u);
    EXPECT_EQ(mockHandler.endpointMessageQueues[eid]->requestQueue.size(), 1u);
    EXPECT_EQ(niceHandler.endpointMessageQueues[eid]->requestQueue.size(), 1u);
}

TEST_F(HandlerCoverageTest, UnregisterRequestIgnoresMissingEndpointQueues)
{
    Handler<MockRequest> mockHandler(nullptr, event, instanceIdDb, false,
                                     seconds(1), 0, milliseconds(100));
    Handler<NiceMock<MockRequest>> niceHandler(
        nullptr, event, instanceIdDb, false, seconds(1), 0, milliseconds(100));
    Handler<FailingSendRequest> failingHandler(
        nullptr, event, instanceIdDb, false, seconds(1), 0, milliseconds(100));

    auto mockId = instanceIdDb.next(0x41).value();
    auto niceId = instanceIdDb.next(0x42).value();
    auto failingId = instanceIdDb.next(0x43).value();

    EXPECT_EQ(mockHandler.unregisterRequest(0x41, mockId, 0, 0), PLDM_ERROR);
    EXPECT_EQ(niceHandler.unregisterRequest(0x42, niceId, 0, 0), PLDM_ERROR);
    EXPECT_EQ(failingHandler.unregisterRequest(0x43, failingId, 0, 0),
              PLDM_ERROR);

    instanceIdDb.free(0x41, mockId);
    instanceIdDb.free(0x42, niceId);
    instanceIdDb.free(0x43, failingId);
}

TEST_F(HandlerCoverageTest,
       RequestUnregisterRequestIgnoresMissingEndpointQueues)
{
    MockPldmTransport mockTransport;
    Handler<Request> handler(&mockTransport, event, instanceIdDb, false,
                             seconds(1), 0, milliseconds(100));
    auto instanceId = instanceIdDb.next(0x44).value();

    EXPECT_EQ(handler.unregisterRequest(0x44, instanceId, 0, 0), PLDM_ERROR);

    instanceIdDb.free(0x44, instanceId);
}

TEST_F(HandlerCoverageTest, RequestUnregisterActiveStartsNextQueued)
{
    MockPldmTransport mockTransport;
    Handler<Request> handler(&mockTransport, event, instanceIdDb, false,
                             seconds(1), 0, milliseconds(100));
    auto activeId = instanceIdDb.next(eid).value();
    auto queuedId = instanceIdDb.next(eid).value();
    RequestKey queuedKey{eid, queuedId, 0, 1};

    EXPECT_CALL(mockTransport, sendMsg(testing::_, testing::_, testing::_))
        .Times(2)
        .WillRepeatedly(Return(PLDM_REQUESTER_SUCCESS));

    EXPECT_EQ(handler.registerRequest(eid, activeId, 0, 0,
                                      makeValidRequest(activeId, 0, 0),
                                      makeTrackingHandler()),
              PLDM_SUCCESS);
    EXPECT_EQ(handler.registerRequest(eid, queuedId, 0, 1,
                                      makeValidRequest(queuedId, 0, 1),
                                      makeTrackingHandler()),
              PLDM_SUCCESS);

    EXPECT_EQ(handler.unregisterRequest(eid, activeId, 0, 0), PLDM_SUCCESS);

    EXPECT_TRUE(handler.handlers.contains(queuedKey));
    EXPECT_TRUE(handler.endpointMessageQueues[eid]->activeRequest);
    EXPECT_EQ(handler.unregisterRequest(eid, queuedId, 0, 1), PLDM_SUCCESS);
}

TEST_F(HandlerCoverageTest, MockRequestUnregisterActiveStartsNextQueued)
{
    Handler<MockRequest> handler(nullptr, event, instanceIdDb, false,
                                 seconds(1), 0, milliseconds(100));
    auto activeId = instanceIdDb.next(eid).value();
    auto queuedId = instanceIdDb.next(eid).value();
    RequestKey queuedKey{eid, queuedId, 0, 1};

    EXPECT_EQ(handler.registerRequest(eid, activeId, 0, 0, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_SUCCESS);
    EXPECT_EQ(handler.registerRequest(eid, queuedId, 0, 1, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_SUCCESS);

    EXPECT_EQ(handler.unregisterRequest(eid, activeId, 0, 0), PLDM_SUCCESS);

    EXPECT_TRUE(handler.handlers.contains(queuedKey));
    EXPECT_TRUE(handler.endpointMessageQueues[eid]->activeRequest);
    EXPECT_EQ(handler.unregisterRequest(eid, queuedId, 0, 1), PLDM_SUCCESS);
}

TEST_F(HandlerCoverageTest, NiceMockRequestUnregisterActiveStartsNextQueued)
{
    Handler<NiceMock<MockRequest>> handler(nullptr, event, instanceIdDb, false,
                                           seconds(1), 0, milliseconds(100));
    auto activeId = instanceIdDb.next(eid).value();
    auto queuedId = instanceIdDb.next(eid).value();
    RequestKey queuedKey{eid, queuedId, 0, 1};

    EXPECT_EQ(handler.registerRequest(eid, activeId, 0, 0, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_SUCCESS);
    EXPECT_EQ(handler.registerRequest(eid, queuedId, 0, 1, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_SUCCESS);

    EXPECT_EQ(handler.unregisterRequest(eid, activeId, 0, 0), PLDM_SUCCESS);

    EXPECT_TRUE(handler.handlers.contains(queuedKey));
    EXPECT_TRUE(handler.endpointMessageQueues[eid]->activeRequest);
    EXPECT_EQ(handler.unregisterRequest(eid, queuedId, 0, 1), PLDM_SUCCESS);
}

TEST_F(HandlerCoverageTest, FailingSendRequestUnregisterActiveStartsNextQueued)
{
    Handler<FailingSendRequest> handler(nullptr, event, instanceIdDb, false,
                                        seconds(1), 1, milliseconds(100));
    auto activeId = instanceIdDb.next(eid).value();
    auto queuedId = instanceIdDb.next(eid).value();
    RequestKey queuedKey{eid, queuedId, 0, 1};

    EXPECT_EQ(handler.registerRequest(eid, activeId, 0, 0, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_SUCCESS);
    EXPECT_EQ(handler.registerRequest(eid, queuedId, 0, 1, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_SUCCESS);

    EXPECT_EQ(handler.unregisterRequest(eid, activeId, 0, 0), PLDM_SUCCESS);

    EXPECT_TRUE(handler.handlers.contains(queuedKey));
    EXPECT_TRUE(handler.endpointMessageQueues[eid]->activeRequest);
    EXPECT_EQ(handler.unregisterRequest(eid, queuedId, 0, 1), PLDM_SUCCESS);
}

TEST_F(HandlerCoverageTest, RequestUnregisterActiveTimerStopFailure)
{
    MockPldmTransport mockTransport;
    Handler<Request> handler(&mockTransport, event, instanceIdDb, false,
                             seconds(1), 0, milliseconds(100));
    auto instanceId = instanceIdDb.next(eid).value();

    EXPECT_CALL(mockTransport, sendMsg(testing::_, testing::_, testing::_))
        .WillOnce(Return(PLDM_REQUESTER_SUCCESS));

    EXPECT_EQ(handler.registerRequest(eid, instanceId, 0, 0,
                                      makeValidRequest(instanceId),
                                      makeTrackingHandler()),
              PLDM_SUCCESS);

    g_failHandlerCoverageTimerStop = true;
    EXPECT_EQ(handler.unregisterRequest(eid, instanceId, 0, 0), PLDM_SUCCESS);
    g_failHandlerCoverageTimerStop = false;
}

TEST_F(HandlerCoverageTest, MockRequestUnregisterActiveTimerStopFailure)
{
    Handler<MockRequest> handler(nullptr, event, instanceIdDb, false,
                                 seconds(1), 0, milliseconds(100));
    auto instanceId = instanceIdDb.next(eid).value();

    EXPECT_EQ(handler.registerRequest(eid, instanceId, 0, 0, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_SUCCESS);

    g_failHandlerCoverageTimerStop = true;
    EXPECT_EQ(handler.unregisterRequest(eid, instanceId, 0, 0), PLDM_SUCCESS);
    g_failHandlerCoverageTimerStop = false;
}

TEST_F(HandlerCoverageTest, FailingSendRequestUnregisterActiveTimerStopFailure)
{
    Handler<FailingSendRequest> handler(nullptr, event, instanceIdDb, false,
                                        seconds(1), 1, milliseconds(100));
    auto instanceId = instanceIdDb.next(eid).value();

    EXPECT_EQ(handler.registerRequest(eid, instanceId, 0, 0, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_SUCCESS);

    g_failHandlerCoverageTimerStop = true;
    EXPECT_EQ(handler.unregisterRequest(eid, instanceId, 0, 0), PLDM_SUCCESS);
    g_failHandlerCoverageTimerStop = false;
}

TEST_F(HandlerCoverageTest, StoreTransportErrorUsesSourceEidForRxDirection)
{
    Handler<NiceMock<MockRequest>> handler(nullptr, event, instanceIdDb, false,
                                           seconds(1), 0, milliseconds(100));
    auto error = makeTransportError(eid);
    error.direction = MCTP_DIR_RX;
    error.src_eid = 0x61;
    error.dest_eid = 0x62;

    handler.storeTransportError(error);

    auto stored = handler.getTransportError(0x61);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->srcEid, 0x61);
    EXPECT_EQ(stored->destEid, 0x62);
    EXPECT_FALSE(handler.getTransportError(0x62).has_value());
    EXPECT_FALSE(handler.getTransportError(0x61, 0x7F).has_value());
    EXPECT_TRUE(handler.getTransportError(0x61, 0xFF).has_value());
}

TEST_F(HandlerCoverageTest,
       RequestStoreTransportErrorUsesSourceEidForRxDirection)
{
    Handler<Request> handler(nullptr, event, instanceIdDb, false, seconds(1), 0,
                             milliseconds(100));
    auto error = makeTransportError(eid);
    error.direction = MCTP_DIR_RX;
    error.src_eid = 0x63;
    error.dest_eid = 0x64;

    handler.storeTransportError(error);

    auto stored = handler.getTransportError(0x63);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->srcEid, 0x63);
    EXPECT_EQ(stored->destEid, 0x64);
    EXPECT_FALSE(handler.getTransportError(0x64).has_value());
    EXPECT_FALSE(handler.getTransportError(0x63, 0x7F).has_value());
}

TEST_F(HandlerCoverageTest,
       MockRequestStoreTransportErrorUsesSourceEidForRxDirection)
{
    Handler<MockRequest> handler(nullptr, event, instanceIdDb, false,
                                 seconds(1), 0, milliseconds(100));
    auto error = makeTransportError(eid);
    error.direction = MCTP_DIR_RX;
    error.src_eid = 0x65;
    error.dest_eid = 0x66;

    handler.storeTransportError(error);

    auto stored = handler.getTransportError(0x65);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->srcEid, 0x65);
    EXPECT_EQ(stored->destEid, 0x66);
    EXPECT_FALSE(handler.getTransportError(0x66).has_value());
    EXPECT_FALSE(handler.getTransportError(0x65, 0x7F).has_value());
}

TEST_F(HandlerCoverageTest,
       FailingSendRequestStoreTransportErrorUsesSourceEidForRxDirection)
{
    Handler<FailingSendRequest> handler(nullptr, event, instanceIdDb, false,
                                        seconds(1), 0, milliseconds(100));
    auto error = makeTransportError(eid);
    error.direction = MCTP_DIR_RX;
    error.src_eid = 0x67;
    error.dest_eid = 0x68;

    handler.storeTransportError(error);

    auto stored = handler.getTransportError(0x67);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->srcEid, 0x67);
    EXPECT_EQ(stored->destEid, 0x68);
    EXPECT_FALSE(handler.getTransportError(0x68).has_value());
    EXPECT_FALSE(handler.getTransportError(0x67, 0x7F).has_value());
}

TEST_F(HandlerCoverageTest, NiceMockGetTransportErrorMatchesExplicitTypeFilter)
{
    Handler<NiceMock<MockRequest>> handler(nullptr, event, instanceIdDb, false,
                                           seconds(1), 0, milliseconds(100));

    auto error = makeTransportError(0x69);
    handler.storeTransportError(error);

    auto stored = handler.getTransportError(0x69, 0x05);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->pldmType, 0x05);
}

TEST_F(HandlerCoverageTest, RequestGetTransportErrorMatchesExplicitTypeFilter)
{
    Handler<Request> handler(nullptr, event, instanceIdDb, false, seconds(1), 0,
                             milliseconds(100));

    auto error = makeTransportError(0x6A);
    handler.storeTransportError(error);

    auto stored = handler.getTransportError(0x6A, 0x05);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->pldmType, 0x05);
}

TEST_F(HandlerCoverageTest,
       MockRequestGetTransportErrorMatchesExplicitTypeFilter)
{
    Handler<MockRequest> handler(nullptr, event, instanceIdDb, false,
                                 seconds(1), 0, milliseconds(100));

    auto error = makeTransportError(0x6B);
    handler.storeTransportError(error);

    auto stored = handler.getTransportError(0x6B, 0x05);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->pldmType, 0x05);
}

TEST_F(HandlerCoverageTest,
       FailingSendRequestGetTransportErrorMatchesExplicitTypeFilter)
{
    Handler<FailingSendRequest> handler(nullptr, event, instanceIdDb, false,
                                        seconds(1), 0, milliseconds(100));

    auto error = makeTransportError(0x6C);
    handler.storeTransportError(error);

    auto stored = handler.getTransportError(0x6C, 0x05);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->pldmType, 0x05);
}

TEST_F(HandlerCoverageTest, NiceMockGetTransportErrorUnknownTypeMatchesFilter)
{
    Handler<NiceMock<MockRequest>> handler(nullptr, event, instanceIdDb, false,
                                           seconds(1), 0, milliseconds(100));

    auto error = makeTransportError(0x6D);
    error.msg_type = 0x00;
    error.payload[0] = 0x00;
    error.payload[1] = 0x00;
    handler.storeTransportError(error);

    auto stored = handler.getTransportError(0x6D, 0x05);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->pldmType, 0xFF);
}

TEST_F(HandlerCoverageTest, RequestGetTransportErrorUnknownTypeMatchesFilter)
{
    Handler<Request> handler(nullptr, event, instanceIdDb, false, seconds(1), 0,
                             milliseconds(100));

    auto error = makeTransportError(0x6E);
    error.msg_type = 0x00;
    error.payload[0] = 0x00;
    error.payload[1] = 0x00;
    handler.storeTransportError(error);

    auto stored = handler.getTransportError(0x6E, 0x05);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->pldmType, 0xFF);
}

TEST_F(HandlerCoverageTest,
       MockRequestGetTransportErrorUnknownTypeMatchesFilter)
{
    Handler<MockRequest> handler(nullptr, event, instanceIdDb, false,
                                 seconds(1), 0, milliseconds(100));

    auto error = makeTransportError(0x6F);
    error.msg_type = 0x00;
    error.payload[0] = 0x00;
    error.payload[1] = 0x00;
    handler.storeTransportError(error);

    auto stored = handler.getTransportError(0x6F, 0x05);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->pldmType, 0xFF);
}

TEST_F(HandlerCoverageTest,
       FailingSendRequestGetTransportErrorUnknownTypeMatchesFilter)
{
    Handler<FailingSendRequest> handler(nullptr, event, instanceIdDb, false,
                                        seconds(1), 0, milliseconds(100));

    auto error = makeTransportError(0x70);
    error.msg_type = 0x00;
    error.payload[0] = 0x00;
    error.payload[1] = 0x00;
    handler.storeTransportError(error);

    auto stored = handler.getTransportError(0x70, 0x05);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->pldmType, 0xFF);
}

#ifndef NDEBUG
TEST_F(HandlerCoverageTest, RequestInstanceIdExpiryCallBackUnknownKey)
{
    Handler<Request> handler(nullptr, event, instanceIdDb, false, seconds(1), 0,
                             milliseconds(100));
    EXPECT_DEATH(handler.instanceIdExpiryCallBack(RequestKey{0x71, 0, 0, 0}),
                 "");
}

TEST_F(HandlerCoverageTest, NiceMockInstanceIdExpiryCallBackUnknownKey)
{
    Handler<NiceMock<MockRequest>> handler(nullptr, event, instanceIdDb, false,
                                           seconds(1), 0, milliseconds(100));
    EXPECT_DEATH(handler.instanceIdExpiryCallBack(RequestKey{0x72, 0, 0, 0}),
                 "");
}
#endif

TEST_F(HandlerCoverageTest,
       RequestUnregisterQueuedRequestSearchesPastMismatchedKeys)
{
    MockPldmTransport mockTransport;
    Handler<Request> handler(&mockTransport, event, instanceIdDb, false,
                             seconds(1), 0, milliseconds(100));
    auto activeId = instanceIdDb.next(eid).value();
    auto queuedId1 = instanceIdDb.next(eid).value();
    auto queuedId2 = instanceIdDb.next(eid).value();
    auto queuedId3 = instanceIdDb.next(eid).value();

    EXPECT_CALL(mockTransport, sendMsg(testing::_, testing::_, testing::_))
        .Times(1)
        .WillRepeatedly(Return(PLDM_REQUESTER_SUCCESS));

    EXPECT_EQ(handler.registerRequest(eid, activeId, 0, 0,
                                      makeValidRequest(activeId, 0, 0),
                                      makeTrackingHandler()),
              PLDM_SUCCESS);
    EXPECT_EQ(handler.registerRequest(eid, queuedId1, 0, 1,
                                      makeValidRequest(queuedId1, 0, 1),
                                      makeTrackingHandler()),
              PLDM_SUCCESS);
    EXPECT_EQ(handler.registerRequest(eid, queuedId2, 1, 0,
                                      makeValidRequest(queuedId2, 1, 0),
                                      makeTrackingHandler()),
              PLDM_SUCCESS);
    EXPECT_EQ(handler.registerRequest(eid, queuedId3, 1, 1,
                                      makeValidRequest(queuedId3, 1, 1),
                                      makeTrackingHandler()),
              PLDM_SUCCESS);

    EXPECT_EQ(handler.unregisterRequest(eid, queuedId2, 1, 0), PLDM_SUCCESS);
    EXPECT_EQ(handler.unregisterRequest(eid, queuedId1, 0, 1), PLDM_SUCCESS);
    EXPECT_EQ(handler.unregisterRequest(eid, queuedId3, 1, 1), PLDM_SUCCESS);
    EXPECT_EQ(handler.unregisterRequest(eid, activeId, 0, 0), PLDM_SUCCESS);
}

TEST_F(HandlerCoverageTest,
       FailingSendRequestUnregisterRequestNotInQueueAfterMismatches)
{
    Handler<FailingSendRequest> handler(nullptr, event, instanceIdDb, false,
                                        seconds(1), 1, milliseconds(100));
    auto activeId = instanceIdDb.next(eid).value();
    auto queuedId1 = instanceIdDb.next(eid).value();
    auto queuedId2 = instanceIdDb.next(eid).value();
    auto missingId = instanceIdDb.next(eid).value();

    EXPECT_EQ(handler.registerRequest(eid, activeId, 0, 0, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_SUCCESS);
    EXPECT_EQ(handler.registerRequest(eid, queuedId1, 0, 1, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_SUCCESS);
    EXPECT_EQ(handler.registerRequest(eid, queuedId2, 1, 0, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_SUCCESS);

    EXPECT_EQ(handler.unregisterRequest(eid, missingId, 1, 1), PLDM_ERROR);
    EXPECT_EQ(handler.unregisterRequest(eid, queuedId2, 1, 0), PLDM_SUCCESS);
    EXPECT_EQ(handler.unregisterRequest(eid, queuedId1, 0, 1), PLDM_SUCCESS);
    EXPECT_EQ(handler.unregisterRequest(eid, activeId, 0, 0), PLDM_SUCCESS);
    instanceIdDb.free(eid, missingId);
}

TEST_F(HandlerCoverageTest, RequestUnregisterRequestNotInQueueAfterMismatches)
{
    MockPldmTransport mockTransport;
    Handler<Request> handler(&mockTransport, event, instanceIdDb, false,
                             seconds(1), 0, milliseconds(100));
    auto activeId = instanceIdDb.next(eid).value();
    auto queuedId1 = instanceIdDb.next(eid).value();
    auto queuedId2 = instanceIdDb.next(eid).value();
    auto missingId = instanceIdDb.next(eid).value();

    EXPECT_CALL(mockTransport, sendMsg(testing::_, testing::_, testing::_))
        .Times(1)
        .WillRepeatedly(Return(PLDM_REQUESTER_SUCCESS));

    EXPECT_EQ(handler.registerRequest(eid, activeId, 0, 0,
                                      makeValidRequest(activeId, 0, 0),
                                      makeTrackingHandler()),
              PLDM_SUCCESS);
    EXPECT_EQ(handler.registerRequest(eid, queuedId1, 0, 1,
                                      makeValidRequest(queuedId1, 0, 1),
                                      makeTrackingHandler()),
              PLDM_SUCCESS);
    EXPECT_EQ(handler.registerRequest(eid, queuedId2, 1, 0,
                                      makeValidRequest(queuedId2, 1, 0),
                                      makeTrackingHandler()),
              PLDM_SUCCESS);

    EXPECT_EQ(handler.unregisterRequest(eid, missingId, 1, 1), PLDM_ERROR);
    EXPECT_EQ(handler.unregisterRequest(eid, queuedId2, 1, 0), PLDM_SUCCESS);
    EXPECT_EQ(handler.unregisterRequest(eid, queuedId1, 0, 1), PLDM_SUCCESS);
    EXPECT_EQ(handler.unregisterRequest(eid, activeId, 0, 0), PLDM_SUCCESS);
    instanceIdDb.free(eid, missingId);
}

TEST_F(HandlerCoverageTest,
       MockRequestUnregisterQueuedRequestSearchesPastMismatchedKeys)
{
    Handler<MockRequest> handler(nullptr, event, instanceIdDb, false,
                                 seconds(1), 0, milliseconds(100));
    auto activeId = instanceIdDb.next(eid).value();
    auto queuedId1 = instanceIdDb.next(eid).value();
    auto queuedId2 = instanceIdDb.next(eid).value();
    auto queuedId3 = instanceIdDb.next(eid).value();

    EXPECT_EQ(handler.registerRequest(eid, activeId, 0, 0, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_SUCCESS);
    EXPECT_EQ(handler.registerRequest(eid, queuedId1, 0, 1, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_SUCCESS);
    EXPECT_EQ(handler.registerRequest(eid, queuedId2, 1, 0, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_SUCCESS);
    EXPECT_EQ(handler.registerRequest(eid, queuedId3, 1, 1, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_SUCCESS);

    EXPECT_EQ(handler.unregisterRequest(eid, queuedId2, 1, 0), PLDM_SUCCESS);
    EXPECT_EQ(handler.unregisterRequest(eid, queuedId1, 0, 1), PLDM_SUCCESS);
    EXPECT_EQ(handler.unregisterRequest(eid, queuedId3, 1, 1), PLDM_SUCCESS);
    EXPECT_EQ(handler.unregisterRequest(eid, activeId, 0, 0), PLDM_SUCCESS);
}

TEST_F(HandlerCoverageTest,
       FailingSendRequestUnregisterQueuedRequestSearchesPastMismatchedKeys)
{
    Handler<FailingSendRequest> handler(nullptr, event, instanceIdDb, false,
                                        seconds(1), 1, milliseconds(100));
    auto activeId = instanceIdDb.next(eid).value();
    auto queuedId1 = instanceIdDb.next(eid).value();
    auto queuedId2 = instanceIdDb.next(eid).value();

    EXPECT_EQ(handler.registerRequest(eid, activeId, 0, 0, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_SUCCESS);
    EXPECT_EQ(handler.registerRequest(eid, queuedId1, 0, 1, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_SUCCESS);
    EXPECT_EQ(handler.registerRequest(eid, queuedId2, 1, 0, pldm::Request{},
                                      makeTrackingHandler()),
              PLDM_SUCCESS);

    EXPECT_EQ(handler.unregisterRequest(eid, queuedId2, 1, 0), PLDM_SUCCESS);
    EXPECT_EQ(handler.unregisterRequest(eid, queuedId1, 0, 1), PLDM_SUCCESS);
    EXPECT_EQ(handler.unregisterRequest(eid, activeId, 0, 0), PLDM_SUCCESS);
}
