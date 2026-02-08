#include "common/transport.hpp"

#define createMctpTransportRedfishEvent test_createMctpTransportRedfishEvent
#include "mock_request.hpp"
#include "requester/request.hpp"
#undef createMctpTransportRedfishEvent

#include <dlfcn.h>
#include <libpldm/base.h>
#include <libpldm/pldm.h>
#include <systemd/sd-event.h>

#include <sdbusplus/timer.hpp>
#include <sdeventplus/event.hpp>

#include <cerrno>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pldm::requester;
using namespace std::chrono;
using ::testing::AtLeast;
using ::testing::Between;
using ::testing::Exactly;
using ::testing::Return;

/** @brief Global flag to force unpack_pldm_header() to return failure.
 *         Used by tests that need to exercise the unpack failure branches.
 *
 *  This works via ELF symbol interposition: defining unpack_pldm_header in the
 *  executable overrides the version in libpldm.so.
 */
static bool g_forceUnpackFailure = false;
static bool g_failTimerStart = false;
static bool g_failTimerStop = false;
static size_t g_mctpTransportRedfishEventCalls = 0;

namespace pldm::transport
{

void test_createMctpTransportRedfishEvent(
    mctp_eid_t /*eid*/, const std::string& /*commandName*/,
    uint32_t /*errorCode*/, uint8_t /*binding*/, uint8_t /*direction*/,
    const std::string& /*logNamespace*/)
{
    ++g_mctpTransportRedfishEventCalls;
}

} // namespace pldm::transport

extern "C" int sd_event_source_set_time_relative(sd_event_source* source,
                                                 uint64_t usec)
{
    if (g_failTimerStart)
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
    if (g_failTimerStart && mode != SD_EVENT_OFF)
    {
        return -EINVAL;
    }

    if (g_failTimerStop && mode == SD_EVENT_OFF)
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

extern "C" uint8_t unpack_pldm_header(const struct pldm_msg_hdr* msg,
                                      struct pldm_header_info* hdr)
{
    if (g_forceUnpackFailure)
    {
        return PLDM_ERROR_INVALID_DATA;
    }
    if (!msg)
    {
        return PLDM_ERROR_INVALID_DATA;
    }
    if (msg->request == PLDM_RESPONSE)
    {
        hdr->msg_type = PLDM_RESPONSE;
    }
    else
    {
        hdr->msg_type =
            msg->datagram ? PLDM_ASYNC_REQUEST_NOTIFY : PLDM_REQUEST;
    }
    hdr->instance = msg->instance_id;
    hdr->pldm_type = msg->type;
    hdr->command = msg->command;
    return PLDM_SUCCESS;
}

/** @brief Mock PldmTransport to allow testing Request::send() paths that
 *         require sendMsg() to fail with a non-null transport.
 */
class MockPldmTransport : public PldmTransport
{
  public:
    MockPldmTransport() : PldmTransport(NoInit{}) {}
    MOCK_METHOD(pldm_requester_rc_t, sendMsg,
                (pldm_tid_t tid, const void* tx, size_t len), (override));
};

class RequestIntfTest : public testing::Test
{
  protected:
    RequestIntfTest() : event(sdeventplus::Event::get_default()) {}

    void SetUp() override
    {
        g_forceUnpackFailure = false;
        g_failTimerStart = false;
        g_failTimerStop = false;
        g_mctpTransportRedfishEventCalls = 0;
    }

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

    int fd = 0;
    mctp_eid_t eid = 0;
    PldmTransport* pldmTransport = nullptr;
    sdeventplus::Event event;
};

class DummyRetryRequest : public RequestRetryTimer
{
  public:
    DummyRetryRequest(sdeventplus::Event& event, uint8_t retries,
                      std::chrono::milliseconds timeout) :
        RequestRetryTimer(event, retries, timeout)
    {}

    int send() const override
    {
        return PLDM_SUCCESS;
    }
};

static void destroyRequestTimer(RequestRetryTimer* request)
{
    delete request;
}

TEST_F(RequestIntfTest, 0Retries100msTimeout)
{
    std::vector<uint8_t> requestMsg;
    MockRequest request(pldmTransport, eid, event, std::move(requestMsg), 0,
                        milliseconds(100), false);
    EXPECT_CALL(request, send())
        .Times(Exactly(1))
        .WillOnce(Return(PLDM_SUCCESS));
    auto rc = request.start();
    EXPECT_EQ(rc, PLDM_SUCCESS);
}

TEST_F(RequestIntfTest, 2Retries100msTimeout)
{
    std::vector<uint8_t> requestMsg;
    MockRequest request(pldmTransport, eid, event, std::move(requestMsg), 2,
                        milliseconds(100), false);
    // send() is called a total of 3 times, the original plus two retries
    EXPECT_CALL(request, send()).Times(3).WillRepeatedly(Return(PLDM_SUCCESS));
    auto rc = request.start();
    EXPECT_EQ(rc, PLDM_SUCCESS);
    waitEventExpiry(milliseconds(500));
}

TEST_F(RequestIntfTest, 9Retries100msTimeoutRequestStoppedAfter1sec)
{
    std::vector<uint8_t> requestMsg;
    MockRequest request(pldmTransport, eid, event, std::move(requestMsg), 9,
                        milliseconds(100), false);
    // send() will be called a total of 10 times, the original plus 9 retries.
    // In a ideal scenario send() would have been called 10 times in 1 sec (when
    // the timer is stopped) with a timeout of 100ms. Because there are delays
    // in dispatch, the range is kept between 5 and 10. This recreates the
    // situation where the Instance ID expires before the all the retries have
    // been completed and the timer is stopped.
    EXPECT_CALL(request, send())
        .Times(Between(5, 10))
        .WillRepeatedly(Return(PLDM_SUCCESS));
    auto rc = request.start();
    EXPECT_EQ(rc, PLDM_SUCCESS);

    auto requestStopCallback = [&](void) { request.stop(); };
    sdbusplus::Timer timer(event.get(), requestStopCallback);
    timer.start(duration_cast<microseconds>(seconds(1)));

    waitEventExpiry(milliseconds(500));
}

TEST_F(RequestIntfTest, 0RetriesSendFails)
{
    std::vector<uint8_t> requestMsg;
    MockRequest request(pldmTransport, eid, event, std::move(requestMsg), 0,
                        milliseconds(100), false);
    EXPECT_CALL(request, send()).Times(Exactly(1)).WillOnce(Return(PLDM_ERROR));
    auto rc = request.start();
    // With 0 retries and send failure, start should return the error
    EXPECT_EQ(rc, PLDM_ERROR);
}

TEST_F(RequestIntfTest, 2RetriesSendFailsInitiallyThenSucceeds)
{
    std::vector<uint8_t> requestMsg;
    MockRequest request(pldmTransport, eid, event, std::move(requestMsg), 2,
                        milliseconds(100), false);
    // First send fails, but since we have retries, timer starts anyway
    // Retries succeed
    EXPECT_CALL(request, send())
        .Times(3)
        .WillOnce(Return(PLDM_ERROR))
        .WillRepeatedly(Return(PLDM_SUCCESS));
    auto rc = request.start();
    // Even with initial send failure, start succeeds because retries are
    // configured
    EXPECT_EQ(rc, PLDM_SUCCESS);
    waitEventExpiry(milliseconds(500));
}

TEST_F(RequestIntfTest, 1Retry100msTimeoutCallbackExhaustsRetries)
{
    std::vector<uint8_t> requestMsg;
    MockRequest request(pldmTransport, eid, event, std::move(requestMsg), 1,
                        milliseconds(100), false);
    // send() called on start + 1 retry = 2 times, then callback with
    // numRetries==0 calls stop()
    EXPECT_CALL(request, send()).Times(2).WillRepeatedly(Return(PLDM_SUCCESS));
    auto rc = request.start();
    EXPECT_EQ(rc, PLDM_SUCCESS);
    waitEventExpiry(milliseconds(500));
}

TEST_F(RequestIntfTest, stopWithoutStart)
{
    std::vector<uint8_t> requestMsg;
    MockRequest request(pldmTransport, eid, event, std::move(requestMsg), 0,
                        milliseconds(100), false);
    // Just call stop without starting - should not crash
    request.stop();
}

TEST_F(RequestIntfTest, virtualDestructorThroughBasePointer)
{
    RequestRetryTimer* request =
        new DummyRetryRequest(event, 0, milliseconds(100));
    delete request;
}

TEST_F(RequestIntfTest, virtualDestructorThroughBasePointerHelper)
{
    RequestRetryTimer* request =
        new DummyRetryRequest(event, 0, milliseconds(100));
    destroyRequestTimer(request);
}

TEST_F(RequestIntfTest, startReturnsErrorIfRetryTimerStartThrows)
{
    std::vector<uint8_t> requestMsg;
    MockRequest request(pldmTransport, eid, event, std::move(requestMsg), 1,
                        milliseconds(100), false);
    EXPECT_CALL(request, send())
        .Times(Exactly(1))
        .WillOnce(Return(PLDM_SUCCESS));

    g_failTimerStart = true;
    auto rc = request.start();
    g_failTimerStart = false;
    EXPECT_EQ(rc, PLDM_ERROR);
}

TEST_F(RequestIntfTest, stopLogsErrorIfTimerStopFails)
{
    std::vector<uint8_t> requestMsg;
    MockRequest request(pldmTransport, eid, event, std::move(requestMsg), 1,
                        milliseconds(5000), false);
    EXPECT_CALL(request, send())
        .Times(Exactly(1))
        .WillOnce(Return(PLDM_SUCCESS));
    EXPECT_EQ(request.start(), PLDM_SUCCESS);

    g_failTimerStop = true;
    request.stop();
    g_failTimerStop = false;
}

// Tests for the concrete Request class (not MockRequest)

TEST_F(RequestIntfTest, concreteRequestNullTransport)
{
    // Build a valid PLDM request message with the request bit set
    pldm::Request requestMsg(sizeof(pldm_msg_hdr), 0);
    auto msg = reinterpret_cast<pldm_msg*>(requestMsg.data());
    auto rc = encode_get_tid_req(0, msg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    // Create concrete Request with null transport
    Request request(nullptr, eid, event, std::move(requestMsg), 0,
                    milliseconds(100), false);
    rc = request.start();
    // send() should fail with PLDM_ERROR due to null transport
    EXPECT_EQ(rc, PLDM_ERROR);
}

TEST_F(RequestIntfTest, concreteRequestNotRequestMsg)
{
    // Build a PLDM message with request bit NOT set (response message)
    pldm::Request requestMsg(sizeof(pldm_msg_hdr), 0);
    auto hdr = reinterpret_cast<pldm_msg_hdr*>(requestMsg.data());
    hdr->request = 0; // Not a request
    hdr->type = PLDM_BASE;
    hdr->command = PLDM_GET_TID;
    hdr->instance_id = 0;

    Request request(nullptr, eid, event, std::move(requestMsg), 0,
                    milliseconds(100), false);
    auto rc = request.start();
    // send() should return PLDM_REQUESTER_NOT_REQ_MSG
    EXPECT_NE(rc, PLDM_SUCCESS);
}

TEST_F(RequestIntfTest, concreteRequestNullTransportVerbose)
{
    // Build a valid PLDM request message
    pldm::Request requestMsg(sizeof(pldm_msg_hdr), 0);
    auto msg = reinterpret_cast<pldm_msg*>(requestMsg.data());
    auto rc = encode_get_tid_req(0, msg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    // Create with verbose=true to cover the verbose branch
    Request request(nullptr, eid, event, std::move(requestMsg), 0,
                    milliseconds(100), true);
    rc = request.start();
    EXPECT_EQ(rc, PLDM_ERROR);
}

TEST_F(RequestIntfTest, concreteRequestFwupOnRetry)
{
    // Build a FWUP type request to exercise onRetry() FWUP logging branch
    pldm::Request requestMsg(sizeof(pldm_msg_hdr), 0);
    auto hdr = reinterpret_cast<pldm_msg_hdr*>(requestMsg.data());
    hdr->request = 1;
    hdr->type = PLDM_FWUP;
    hdr->command = 0x01;
    hdr->instance_id = 0;

    // 1 retry + null transport: start() succeeds (retries>0), then timer fires
    // calling onRetry() (FWUP branch) then send() (fails with null transport)
    Request request(nullptr, eid, event, std::move(requestMsg), 1,
                    milliseconds(100), false);
    auto rc = request.start();
    EXPECT_EQ(rc, PLDM_SUCCESS);
    waitEventExpiry(milliseconds(500));
}

TEST_F(RequestIntfTest, concreteRequestNonFwupOnRetry)
{
    // Build a PLDM_BASE type request to exercise onRetry() non-FWUP path
    pldm::Request requestMsg(sizeof(pldm_msg_hdr), 0);
    auto msg = reinterpret_cast<pldm_msg*>(requestMsg.data());
    auto rc = encode_get_tid_req(0, msg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    // 1 retry + null transport: onRetry() called but type is BASE, not FWUP
    Request request(nullptr, eid, event, std::move(requestMsg), 1,
                    milliseconds(100), false);
    rc = request.start();
    EXPECT_EQ(rc, PLDM_SUCCESS);
    waitEventExpiry(milliseconds(500));
}

// Tests using MockPldmTransport to exercise sendMsg() failure paths

TEST_F(RequestIntfTest, sendMsgFailNonFwupNoRetries)
{
    // Build a PLDM_BASE type request
    pldm::Request requestMsg(sizeof(pldm_msg_hdr), 0);
    auto msg = reinterpret_cast<pldm_msg*>(requestMsg.data());
    auto rc = encode_get_tid_req(0, msg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    MockPldmTransport mockTransport;
    EXPECT_CALL(mockTransport, sendMsg(testing::_, testing::_, testing::_))
        .WillOnce(Return(static_cast<pldm_requester_rc_t>(-1)));

    // 0 retries + non-FWUP type: covers the non-FWUP sendMsg failure path
    Request request(&mockTransport, eid, event, std::move(requestMsg), 0,
                    milliseconds(100), false);
    rc = request.start();
    EXPECT_EQ(rc, PLDM_ERROR);
    EXPECT_EQ(g_mctpTransportRedfishEventCalls, 0u);
}

TEST_F(RequestIntfTest, sendMsgFailFwupNoRetries)
{
    // Build a FWUP request so send() exercises the redfish-event path
    pldm::Request requestMsg(sizeof(pldm_msg_hdr), 0);
    auto hdr = reinterpret_cast<pldm_msg_hdr*>(requestMsg.data());
    hdr->request = 1;
    hdr->type = PLDM_FWUP;
    hdr->command = 0x01;
    hdr->instance_id = 0;

    MockPldmTransport mockTransport;
    EXPECT_CALL(mockTransport, sendMsg(testing::_, testing::_, testing::_))
        .WillOnce(Return(static_cast<pldm_requester_rc_t>(-1)));

    Request request(&mockTransport, eid, event, std::move(requestMsg), 0,
                    milliseconds(100), false);
    auto rc = request.start();
    EXPECT_EQ(rc, PLDM_ERROR);
    EXPECT_EQ(g_mctpTransportRedfishEventCalls, 1u);
}

TEST_F(RequestIntfTest, sendMsgFailWithRetries)
{
    // Build a valid PLDM request
    pldm::Request requestMsg(sizeof(pldm_msg_hdr), 0);
    auto msg = reinterpret_cast<pldm_msg*>(requestMsg.data());
    auto rc = encode_get_tid_req(0, msg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    MockPldmTransport mockTransport;
    // sendMsg fails every time - covers the "retries remaining" warning path
    EXPECT_CALL(mockTransport, sendMsg(testing::_, testing::_, testing::_))
        .WillRepeatedly(Return(static_cast<pldm_requester_rc_t>(-1)));

    // 2 retries: first send fails with retries remaining (warning log),
    // then timer fires for retries which also fail
    Request request(&mockTransport, eid, event, std::move(requestMsg), 2,
                    milliseconds(100), false);
    rc = request.start();
    // start() returns SUCCESS because retries are configured
    EXPECT_EQ(rc, PLDM_SUCCESS);
    waitEventExpiry(milliseconds(500));
}

// Tests using unpack_pldm_header interposition to cover failure branches

TEST_F(RequestIntfTest, sendMsgFailUnpackFailsNoRetries)
{
    // Build a valid PLDM request
    pldm::Request requestMsg(sizeof(pldm_msg_hdr), 0);
    auto msg = reinterpret_cast<pldm_msg*>(requestMsg.data());
    auto rc = encode_get_tid_req(0, msg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    MockPldmTransport mockTransport;
    EXPECT_CALL(mockTransport, sendMsg(testing::_, testing::_, testing::_))
        .WillOnce(Return(static_cast<pldm_requester_rc_t>(-1)));

    Request request(&mockTransport, eid, event, std::move(requestMsg), 0,
                    milliseconds(100), false);

    // Force unpack to fail, covering the else branch (error log) in send()
    g_forceUnpackFailure = true;
    rc = request.start();
    g_forceUnpackFailure = false;
    EXPECT_EQ(rc, PLDM_ERROR);
}

TEST_F(RequestIntfTest, onRetryUnpackFails)
{
    // Build a FWUP type request
    pldm::Request requestMsg(sizeof(pldm_msg_hdr), 0);
    auto hdr = reinterpret_cast<pldm_msg_hdr*>(requestMsg.data());
    hdr->request = 1;
    hdr->type = PLDM_FWUP;
    hdr->command = 0x01;
    hdr->instance_id = 0;

    MockPldmTransport mockTransport;
    // First send succeeds (initial start()), retries will call onRetry+send
    EXPECT_CALL(mockTransport, sendMsg(testing::_, testing::_, testing::_))
        .WillRepeatedly(Return(static_cast<pldm_requester_rc_t>(0)));

    // 1 retry: start() sends successfully, then timer fires calling
    // onRetry() where unpack will fail and return early (line 192)
    Request request(&mockTransport, eid, event, std::move(requestMsg), 1,
                    milliseconds(100), false);
    auto rc = request.start();
    EXPECT_EQ(rc, PLDM_SUCCESS);

    // Force unpack failure before the timer fires for onRetry()
    g_forceUnpackFailure = true;
    waitEventExpiry(milliseconds(500));
    g_forceUnpackFailure = false;
}
