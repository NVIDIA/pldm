#include "common/instance_id.hpp"
#include "common/transport.hpp"
#include "pldmd/invoker.hpp"
#include "requester/handler.hpp"
#include "requester/request.hpp"
#include "test/test_instance_id.hpp"

#include <libpldm/base.h>
#include <poll.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <unistd.h>

#include <sdeventplus/event.hpp>
#include <sdeventplus/source/signal.hpp>

#include <cerrno>
#include <csignal>
#include <system_error>

#include <gtest/gtest.h>

#ifdef PLDM_TYPE2
#undef PLDM_TYPE2
#endif

#ifdef LIBPLDMRESPONDER
#undef LIBPLDMRESPONDER
#endif

#ifdef OEM_IBM
#undef OEM_IBM
#endif

#ifdef OEM_AMPERE
#undef OEM_AMPERE
#endif

#define main pldmd_main_for_coverage
#include "pldmd/pldmd.cpp" // NOLINT(bugprone-suspicious-include)
#undef main

namespace
{

struct FakeTransportState
{
    int readFd = -1;
    int writeFd = -1;
};

FakeTransportState& fakeTransportState()
{
    static FakeTransportState state;
    return state;
}

using RequestHandler = pldm::requester::Handler<pldm::requester::Request>;

std::vector<uint8_t> makeHeaderMessage(uint8_t msgType, uint8_t instance,
                                       uint8_t pldmType, uint8_t command)
{
    pldm_header_info header{};
    header.msg_type = static_cast<MessageType>(msgType);
    header.instance = instance;
    header.pldm_type = pldmType;
    header.command = command;

    std::vector<uint8_t> message(sizeof(pldm_msg_hdr), 0);
    auto* hdr = reinterpret_cast<pldm_msg_hdr*>(message.data());
    EXPECT_EQ(pack_pldm_header(&header, hdr), PLDM_SUCCESS);
    return message;
}

} // namespace

PldmTransport::PldmTransport() : pfd{}, impl{}, transport(nullptr)
{
    auto& state = fakeTransportState();
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0)
    {
        throw std::system_error(errno, std::generic_category(),
                                "pipe creation failed");
    }
    state.readFd = fds[0];
    state.writeFd = fds[1];

    // Preload one byte so event.loop() immediately gets EPOLLIN and runs the
    // IO callback in main().
    const uint8_t byte = 0x1;
    auto written = write(state.writeFd, &byte, sizeof(byte));
    if (written != static_cast<ssize_t>(sizeof(byte)))
    {
        throw std::system_error(errno, std::generic_category(),
                                "pipe write failed");
    }
}

PldmTransport::~PldmTransport()
{
    auto& state = fakeTransportState();
    if (state.readFd >= 0)
    {
        close(state.readFd);
        state.readFd = -1;
    }
    if (state.writeFd >= 0)
    {
        close(state.writeFd);
        state.writeFd = -1;
    }
}

int PldmTransport::getEventSource() const
{
    return fakeTransportState().readFd;
}

pldm_requester_rc_t PldmTransport::sendMsg(pldm_tid_t, const void*, size_t)
{
    return PLDM_REQUESTER_SUCCESS;
}

pldm_requester_rc_t PldmTransport::recvMsg(pldm_tid_t& tid, void*& rx,
                                           size_t& len)
{
    auto& state = fakeTransportState();
    if (state.readFd >= 0)
    {
        uint8_t byte = 0;
        [[maybe_unused]] auto bytesRead =
            read(state.readFd, &byte, sizeof(byte));
    }

    tid = 8;
    rx = nullptr;
    len = 0;
    return PLDM_REQUESTER_RECV_FAIL;
}

pldm_requester_rc_t PldmTransport::sendRecvMsg(pldm_tid_t, const void*, size_t,
                                               void*& rx, size_t& rxLen)
{
    rx = nullptr;
    rxLen = 0;
    return PLDM_REQUESTER_RECV_FAIL;
}

int PldmTransport::enableErrorQueue()
{
    return 0;
}

TEST(PldmdInternalCoverage, RequestServiceName)
{
    try
    {
        requestPLDMServiceName();
    }
    catch (...)
    {
        // The function is expected to handle sdbusplus exceptions.
    }
}

TEST(PldmdInternalCoverage, InterruptFlightRecorderCallback)
{
    // sd-event requires the watched signal to be blocked in the process.
    sigset_t mask{};
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR2);
    sigprocmask(SIG_BLOCK, &mask, nullptr);

    auto event = sdeventplus::Event::get_new();
    sdeventplus::source::Signal signal(
        event, SIGUSR2,
        [](sdeventplus::source::Signal&, const struct signalfd_siginfo*) {});

    EXPECT_NO_THROW(interruptFlightRecorderCallBack(signal, nullptr));
}

TEST(PldmdInternalCoverage, OptionUsage)
{
    EXPECT_NO_THROW(optionUsage());
}

TEST(PldmdInternalCoverage, ProcessRxMsgRequestPathReturnsUnsupportedCmd)
{
    pldm::responder::Invoker invoker;
    auto event = sdeventplus::Event::get_default();
    TestInstanceIdDb instanceIdDb;
    RequestHandler reqHandler(nullptr, event, instanceIdDb, false);

    constexpr uint8_t instanceId = 9;
    constexpr uint8_t command = 0x7E;

    auto requestMsg =
        makeHeaderMessage(PLDM_REQUEST, instanceId, PLDM_BASE, command);
    auto response = processRxMsg(requestMsg, invoker, reqHandler, nullptr, 8);

    ASSERT_TRUE(response.has_value());
    ASSERT_EQ(response->size(), sizeof(pldm_msg_hdr) + 1);

    auto* responseHdr = reinterpret_cast<const pldm_msg_hdr*>(response->data());
    pldm_header_info responseInfo{};
    ASSERT_EQ(unpack_pldm_header(responseHdr, &responseInfo), PLDM_SUCCESS);
    EXPECT_EQ(responseInfo.msg_type, PLDM_RESPONSE);
    EXPECT_EQ(responseInfo.instance, instanceId);
    EXPECT_EQ(responseInfo.pldm_type, PLDM_BASE);
    EXPECT_EQ(responseInfo.command, command);
    EXPECT_EQ(response->back(), PLDM_ERROR_UNSUPPORTED_PLDM_CMD);
}

TEST(PldmdInternalCoverage, ProcessRxMsgResponsePathReturnsNullopt)
{
    pldm::responder::Invoker invoker;
    auto event = sdeventplus::Event::get_default();
    TestInstanceIdDb instanceIdDb;
    RequestHandler reqHandler(nullptr, event, instanceIdDb, false);

    auto responseMsg = makeHeaderMessage(PLDM_RESPONSE, 1, PLDM_BASE, 1);
    auto output = processRxMsg(responseMsg, invoker, reqHandler, nullptr, 8);

    EXPECT_FALSE(output.has_value());
}

TEST(PldmdInternalCoverage, MainInvalidVerboseValueExitsFailure)
{
    EXPECT_EXIT(
        {
            char arg0[] = "pldmd";
            char arg1[] = "--verbose=2";
            char* argv[3];
            argv[0] = arg0;
            argv[1] = arg1;
            argv[2] = nullptr;
            pldmd_main_for_coverage(2, argv);
        },
        ::testing::ExitedWithCode(EXIT_FAILURE), ".*");
}

namespace
{

// Minimal stand-in for fw_update::Manager: throws from onResponseSendComplete.
struct ThrowingFwManager
{
    void onResponseSendComplete(mctp_eid_t /*eid*/, bool /*success*/)
    {
        throw std::runtime_error("injected test exception");
    }
};

// Minimal stand-in that records the arguments it received.
struct TrackingFwManager
{
    bool called = false;
    mctp_eid_t lastEid = 0;
    bool lastSuccess = false;

    void onResponseSendComplete(mctp_eid_t eid, bool success)
    {
        called = true;
        lastEid = eid;
        lastSuccess = success;
    }
};

} // namespace

// Verify that an exception thrown by onResponseSendComplete is caught by
// notifyFwUpdateSendComplete and does not propagate to the caller (which is
// the event-loop callback — an uncaught exception there would kill the daemon).
TEST(PldmdInternalCoverage, NotifyFwUpdateSendCompleteDoesNotPropagateException)
{
    ThrowingFwManager manager;
    EXPECT_NO_THROW(notifyFwUpdateSendComplete(&manager, 14, true));
}

// Verify that on the happy path the call is forwarded with the correct
// EID and success flag.
TEST(PldmdInternalCoverage,
     NotifyFwUpdateSendCompleteForwardsArgumentsOnSuccess)
{
    TrackingFwManager manager;
    notifyFwUpdateSendComplete(&manager, 14, true);
    EXPECT_TRUE(manager.called);
    EXPECT_EQ(manager.lastEid, static_cast<mctp_eid_t>(14));
    EXPECT_TRUE(manager.lastSuccess);
}
