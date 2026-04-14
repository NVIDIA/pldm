#include "common/instance_id.hpp"
#include "common/transport.hpp"
#include "pldmd/invoker.hpp"
#include "pldmd_coverage_hooks.hpp"
#include "requester/handler.hpp"
#include "requester/request.hpp"
#include "test/test_instance_id.hpp"
#include "test/test_tmp_utils.hpp"
#include "test_valgrind_utils.hpp"

#include <libpldm/base.h>
#include <libpldm/instance-id.h>
#include <poll.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <sdeventplus/event.hpp>
#include <sdeventplus/source/signal.hpp>

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#ifdef OEM_IBM
#undef OEM_IBM
#endif

#ifdef OEM_AMPERE
#undef OEM_AMPERE
#endif

extern "C" int __wrap_pldm_instance_db_init_default(
    struct pldm_instance_db** ctx)
{
    static uint64_t dbIndex = 0;
    static std::deque<std::string> dbPaths;
    auto root = pldm::test::ensureTempDir();
    dbPaths.emplace_back(
        (root / ("pldmd_test_iid_" + std::to_string(::getpid()) + "_" +
                 std::to_string(dbIndex++)))
            .string());
    auto& dbPath = dbPaths.back();
    std::ofstream ofs(dbPath, std::ios::binary | std::ios::trunc);
    std::string data(256 * 32, '\0');
    ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
    return pldm_instance_db_init(ctx, dbPath.c_str());
}

#define main pldmd_main_for_coverage
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "pldmd/pldmd.cpp"
#undef main

namespace
{

struct FakeRecvAction
{
    pldm_requester_rc_t rc = PLDM_REQUESTER_RECV_FAIL;
    pldm_tid_t tid = 8;
    std::vector<uint8_t> payload{};
};

struct FakeTransportState
{
    int readFd = -1;
    int writeFd = -1;
    int enableErrorQueueRc = 0;
    pldm_requester_rc_t sendRc = PLDM_REQUESTER_SUCCESS;
    int sendErrnoValue = EIO;
    bool closeWriteFdAfterConstruct = false;
    std::deque<FakeRecvAction> recvActions{};
    std::vector<std::vector<uint8_t>> sentMessages{};
};

FakeTransportState& fakeTransportState()
{
    static FakeTransportState state;
    return state;
}

struct DeathTestStyleInitializer
{
    DeathTestStyleInitializer()
    {
        GTEST_FLAG_SET(death_test_style, "threadsafe");
    }
};

[[maybe_unused]] DeathTestStyleInitializer deathTestStyleInitializer{};

void resetFakeTransportState()
{
    auto& state = fakeTransportState();
    if (state.readFd >= 0)
    {
        close(state.readFd);
    }
    if (state.writeFd >= 0)
    {
        close(state.writeFd);
    }
    state = {};
}

FakeRecvAction makeRecvAction(pldm_requester_rc_t rc,
                              std::vector<uint8_t> payload = {},
                              pldm_tid_t tid = 8)
{
    return FakeRecvAction{rc, tid, std::move(payload)};
}

void configureFakeTransport(
    std::initializer_list<FakeRecvAction> actions, int enableErrorQueueRc = 0,
    pldm_requester_rc_t sendRc = PLDM_REQUESTER_SUCCESS,
    int sendErrnoValue = EIO, bool closeWriteFdAfterConstruct = false)
{
    resetFakeTransportState();
    auto& state = fakeTransportState();
    state.enableErrorQueueRc = enableErrorQueueRc;
    state.sendRc = sendRc;
    state.sendErrnoValue = sendErrnoValue;
    state.closeWriteFdAfterConstruct = closeWriteFdAfterConstruct;
    state.recvActions =
        std::deque<FakeRecvAction>(actions.begin(), actions.end());
    if (state.recvActions.empty() && !state.closeWriteFdAfterConstruct)
    {
        state.recvActions.emplace_back();
    }
}

using RequestHandler = pldm::requester::Handler<pldm::requester::Request>;
namespace CoverageHooks = pldm::test::coverage;

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

std::vector<uint8_t> makePlatformEventMessage(
    uint8_t eventClass, const std::vector<uint8_t>& eventData,
    uint8_t instanceId = 1, pldm_tid_t tid = 8)
{
    const auto payloadLength =
        PLDM_PLATFORM_EVENT_MESSAGE_MIN_REQ_BYTES + eventData.size();
    std::vector<uint8_t> request(sizeof(pldm_msg_hdr) + payloadLength, 0);
    auto* msg = reinterpret_cast<pldm_msg*>(request.data());
    auto* eventDataPtr = eventData.empty() ? nullptr : eventData.data();

    EXPECT_EQ(encode_platform_event_message_req(
                  instanceId, PLDM_PLATFORM_EVENT_MESSAGE_FORMAT_VERSION, tid,
                  eventClass, eventDataPtr, eventData.size(), msg,
                  payloadLength),
              PLDM_SUCCESS);
    return request;
}

std::vector<uint8_t> makeStateSensorEventData(
    uint16_t sensorId, uint8_t sensorEventClass, uint8_t sensorOffset,
    uint8_t eventState, uint8_t previousEventState)
{
    return {static_cast<uint8_t>(sensorId & 0xFF),
            static_cast<uint8_t>((sensorId >> 8) & 0xFF),
            sensorEventClass,
            sensorOffset,
            eventState,
            previousEventState};
}

CoverageHooks::SyntheticIoEvent makeSyntheticIoEvent(
    uint32_t revents, int fdOverride = std::numeric_limits<int>::min())
{
    return CoverageHooks::SyntheticIoEvent{fdOverride, revents};
}

void invokePldmd(std::initializer_list<const char*> args)
{
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto* arg : args)
    {
        argv.push_back(const_cast<char*>(arg));
    }
    argv.push_back(nullptr);
    pldmd_main_for_coverage(static_cast<int>(args.size()), argv.data());
}

void runMainForTest(
    std::initializer_list<const char*> args,
    std::initializer_list<FakeRecvAction> actions, int enableErrorQueueRc = 0,
    pldm_requester_rc_t sendRc = PLDM_REQUESTER_SUCCESS,
    int sendErrnoValue = EIO, bool closeWriteFdAfterConstruct = false)
{
    configureFakeTransport(actions, enableErrorQueueRc, sendRc, sendErrnoValue,
                           closeWriteFdAfterConstruct);
    invokePldmd(args);
}

const char* getPldmdMainCoverageHelperPath()
{
    const char* helperPath = std::getenv("PLDMD_MAIN_COVERAGE_HELPER");
    EXPECT_NE(helperPath, nullptr);
    if (helperPath == nullptr)
    {
        return "";
    }
    EXPECT_NE(helperPath[0], '\0');
    return helperPath;
}

int runPldmdMainCoverageHelper(std::initializer_list<const char*> args,
                               const char* scenario = nullptr)
{
    const char* helperPath = getPldmdMainCoverageHelperPath();
    if (helperPath[0] == '\0')
    {
        return -1;
    }

    const pid_t pid = fork();
    EXPECT_GE(pid, 0);
    if (pid == 0)
    {
        if (scenario != nullptr)
        {
            setenv("PLDMD_HELPER_SCENARIO", scenario, 1);
        }
        else
        {
            unsetenv("PLDMD_HELPER_SCENARIO");
        }

        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(const_cast<char*>(helperPath));
        for (const auto* arg : args)
        {
            argv.push_back(const_cast<char*>(arg));
        }
        argv.push_back(nullptr);

        execv(helperPath, argv.data());
        _exit(127);
    }

    int status = 0;
    EXPECT_EQ(waitpid(pid, &status, 0), pid);
    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status))
    {
        return 128 + WTERMSIG(status);
    }
    return -1;
}

void expectPldmdMainCoverageHelperExit(std::initializer_list<const char*> args,
                                       int expectedStatus,
                                       const char* scenario = nullptr)
{
    if (pldm::test::runningOnValgrind())
    {
        GTEST_SKIP() << "subprocess coverage runs in the normal pass";
    }

    EXPECT_EQ(runPldmdMainCoverageHelper(args, scenario), expectedStatus);
}

void expectMainExitWithHostEid(
    std::initializer_list<FakeRecvAction> actions,
    pldm_requester_rc_t sendRc = PLDM_REQUESTER_SUCCESS,
    int sendErrnoValue = EIO)
{
    EXPECT_EXIT(
        {
            CoverageHooks::ScopedHookStateReset hooks;
            CoverageHooks::setHostEid(8);
            runMainForTest({"pldmd"}, actions, 0, sendRc, sendErrnoValue);
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

class StaticResponseHandler : public pldm::responder::CmdHandler
{
  public:
    StaticResponseHandler(uint8_t command, pldm::responder::Response response)
    {
        handlers.emplace(command, [response = std::move(response)](
                                      pldm_tid_t, const pldm_msg*,
                                      size_t) mutable { return response; });
    }
};

class EmptyCmdHandler : public pldm::responder::CmdHandler
{};

class ThrowingOutOfRangeHandler : public pldm::responder::CmdHandler
{
  public:
    explicit ThrowingOutOfRangeHandler(uint8_t command)
    {
        handlers.emplace(command, [](pldm_tid_t, const pldm_msg*, size_t) {
            throw std::out_of_range("coverage missing handler");
            return pldm::responder::Response{};
        });
    }
};

} // namespace

PldmTransport::PldmTransport([[maybe_unused]] bool listening) :
    PldmTransport(NoInit{})
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

    if (state.closeWriteFdAfterConstruct)
    {
        close(state.writeFd);
        state.writeFd = -1;
        return;
    }

    const uint8_t byte = 0x1;
    for (size_t i = 0; i < state.recvActions.size(); ++i)
    {
        auto written = write(state.writeFd, &byte, sizeof(byte));
        if (written != static_cast<ssize_t>(sizeof(byte)))
        {
            throw std::system_error(errno, std::generic_category(),
                                    "pipe write failed");
        }
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

pldm_requester_rc_t PldmTransport::sendMsg(pldm_tid_t, const void* tx,
                                           size_t len)
{
    auto& state = fakeTransportState();
    const auto* bytes = static_cast<const uint8_t*>(tx);
    state.sentMessages.emplace_back(bytes, bytes + len);
    errno = state.sendErrnoValue;
    return state.sendRc;
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

    if (state.recvActions.empty())
    {
        tid = 8;
        rx = nullptr;
        len = 0;
        return PLDM_REQUESTER_RECV_FAIL;
    }

    auto action = std::move(state.recvActions.front());
    state.recvActions.pop_front();

    tid = action.tid;
    len = action.payload.size();
    rx = nullptr;

    if (action.rc == PLDM_REQUESTER_SUCCESS && !action.payload.empty())
    {
        rx = malloc(action.payload.size());
        EXPECT_NE(rx, nullptr);
        std::memcpy(rx, action.payload.data(), action.payload.size());
    }

    return action.rc;
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
    return fakeTransportState().enableErrorQueueRc;
}

class TemporaryDbusNameOwner
{
  public:
    explicit TemporaryDbusNameOwner(const char* name) :
        bus(sdbusplus::bus::new_default())
    {
        bus.request_name(name);
    }

  private:
    sdbusplus::bus::bus bus;
};

class TemporaryHostEidFile
{
  public:
    explicit TemporaryHostEidFile(const std::string& eid)
    {
        const std::filesystem::path eidPath(HOST_EID_PATH);
        std::filesystem::create_directories(eidPath.parent_path());
        std::ofstream ofs(eidPath, std::ios::trunc);
        ofs << eid;
    }

    ~TemporaryHostEidFile()
    {
        std::filesystem::remove(HOST_EID_PATH);
    }
};

std::vector<uint8_t> makeMinimalStateEffecterPdr(uint16_t entityId,
                                                 uint16_t stateSetId)
{
    std::vector<uint8_t> pdr(sizeof(pldm_state_effecter_pdr) - sizeof(uint8_t) +
                             sizeof(state_effecter_possible_states));

    auto* record = new (pdr.data()) pldm_state_effecter_pdr;
    auto* state = new (record->possible_states) state_effecter_possible_states;

    record->hdr.type = PLDM_STATE_EFFECTER_PDR;
    record->hdr.record_handle = 1;
    record->entity_type = entityId;
    record->container_id = 0;
    record->composite_effecter_count = 1;
    state->state_set_id = stateSetId;
    state->possible_states_size = 1;

    return pdr;
}

std::vector<uint8_t> makeMinimalStateSensorPdr(uint16_t entityId,
                                               uint16_t stateSetId)
{
    std::vector<uint8_t> pdr(sizeof(pldm_state_sensor_pdr) - sizeof(uint8_t) +
                             sizeof(state_sensor_possible_states));

    auto* record = new (pdr.data()) pldm_state_sensor_pdr;
    auto* state = new (record->possible_states) state_sensor_possible_states;

    record->hdr.type = PLDM_STATE_SENSOR_PDR;
    record->hdr.record_handle = 1;
    record->entity_type = entityId;
    record->container_id = 0;
    record->composite_sensor_count = 1;
    state->state_set_id = stateSetId;
    state->possible_states_size = 1;

    return pdr;
}

int runRequestPLDMServiceNameInSubprocess()
{
    const pid_t pid = fork();
    EXPECT_GE(pid, 0);
    if (pid == 0)
    {
        requestPLDMServiceName();
        exit(EXIT_SUCCESS);
    }

    int status = 0;
    EXPECT_EQ(waitpid(pid, &status, 0), pid);
    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    return -1;
}

enum class MainInitFailureScenario
{
    pdrRepo,
    entityTree,
    bmcEntityTree,
};

int runMainInitFailureInSubprocess(MainInitFailureScenario scenario)
{
    const pid_t pid = fork();
    EXPECT_GE(pid, 0);
    if (pid == 0)
    {
        CoverageHooks::ScopedHookStateReset hooks;
        switch (scenario)
        {
            case MainInitFailureScenario::pdrRepo:
                CoverageHooks::setForcePdrInitFailure();
                break;
            case MainInitFailureScenario::entityTree:
                CoverageHooks::setFailEntityInitCall(1);
                break;
            case MainInitFailureScenario::bmcEntityTree:
                CoverageHooks::setFailEntityInitCall(2);
                break;
        }

        try
        {
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
            exit(EXIT_SUCCESS);
        }
        catch (const std::exception&)
        {
            exit(EXIT_FAILURE);
        }
    }

    int status = 0;
    EXPECT_EQ(waitpid(pid, &status, 0), pid);
    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status))
    {
        return 128 + WTERMSIG(status);
    }
    return -1;
}

TEST(PldmdInternalCoverage, RequestServiceNameFreshProcessSucceeds)
{
    if (pldm::test::runningOnValgrind())
    {
        GTEST_SKIP() << "subprocess coverage runs in the normal pass";
    }

    EXPECT_EQ(runRequestPLDMServiceNameInSubprocess(), EXIT_SUCCESS);
}

TEST(PldmdInternalCoverage, RequestServiceNameFreshProcessHandlesTakenName)
{
    if (pldm::test::runningOnValgrind())
    {
        GTEST_SKIP() << "subprocess coverage runs in the normal pass";
    }

    TemporaryDbusNameOwner owner(PLDMService);
    EXPECT_EQ(runRequestPLDMServiceNameInSubprocess(), EXIT_SUCCESS);
}

TEST(PldmdInternalCoverage, RequestServiceNameHandlesRequestNameFailureCoverage)
{
    CoverageHooks::ScopedHookStateReset hooks;
    CoverageHooks::setForceRequestNameFailure();
    EXPECT_NO_THROW(requestPLDMServiceName());
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
    invoker.registerHandler(PLDM_BASE, std::make_unique<EmptyCmdHandler>());
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

TEST(PldmdInternalCoverage, ProcessRxMsgInvalidHeaderReturnsNullopt)
{
    pldm::responder::Invoker invoker;
    auto event = sdeventplus::Event::get_default();
    TestInstanceIdDb instanceIdDb;
    RequestHandler reqHandler(nullptr, event, instanceIdDb, false);

    std::vector<uint8_t> invalidRequest(sizeof(pldm_msg_hdr) - 1, 0);
    auto response =
        processRxMsg(invalidRequest, invoker, reqHandler, nullptr, 8);

    EXPECT_FALSE(response.has_value());
}

TEST(PldmdInternalCoverage, ProcessRxMsgForcedUnpackFailureReturnsNullopt)
{
    CoverageHooks::ScopedHookStateReset hooks;

    pldm::responder::Invoker invoker;
    auto event = sdeventplus::Event::get_default();
    TestInstanceIdDb instanceIdDb;
    RequestHandler reqHandler(nullptr, event, instanceIdDb, false);

    auto requestMsg = makeHeaderMessage(PLDM_REQUEST, 1, PLDM_BASE, 1);
    CoverageHooks::setForceUnpackFailure();

    auto response = processRxMsg(requestMsg, invoker, reqHandler, nullptr, 8);
    EXPECT_FALSE(response.has_value());
}

TEST(PldmdInternalCoverage, ProcessRxMsgForcedPackFailureReturnsNullopt)
{
    CoverageHooks::ScopedHookStateReset hooks;

    pldm::responder::Invoker invoker;
    auto event = sdeventplus::Event::get_default();
    TestInstanceIdDb instanceIdDb;
    RequestHandler reqHandler(nullptr, event, instanceIdDb, false);

    constexpr uint8_t command = 0x7E;
    invoker.registerHandler(
        PLDM_BASE, std::make_unique<ThrowingOutOfRangeHandler>(command));
    auto requestMsg = makeHeaderMessage(PLDM_REQUEST, 1, PLDM_BASE, command);
    CoverageHooks::setForcePackFailure();

    auto response = processRxMsg(requestMsg, invoker, reqHandler, nullptr, 8);
    EXPECT_FALSE(response.has_value());
}

TEST(PldmdInternalCoverage, ProcessRxMsgRequestPathInvokesRegisteredHandler)
{
    pldm::responder::Invoker invoker;
    auto event = sdeventplus::Event::get_default();
    TestInstanceIdDb instanceIdDb;
    RequestHandler reqHandler(nullptr, event, instanceIdDb, false);

    constexpr uint8_t command = 0x55;
    auto requestMsg = makeHeaderMessage(PLDM_REQUEST, 7, PLDM_BASE, command);
    auto expected = pldm::responder::CmdHandler::ccOnlyResponse(
        reinterpret_cast<const pldm_msg*>(requestMsg.data()), PLDM_SUCCESS);

    invoker.registerHandler(
        PLDM_BASE, std::make_unique<StaticResponseHandler>(command, expected));

    auto response = processRxMsg(requestMsg, invoker, reqHandler, nullptr, 8);

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(*response, expected);
}

TEST(PldmdInternalCoverage,
     ProcessRxMsgThrownOutOfRangeReturnsUnsupportedCommandResponse)
{
    pldm::responder::Invoker invoker;
    auto event = sdeventplus::Event::get_default();
    TestInstanceIdDb instanceIdDb;
    RequestHandler reqHandler(nullptr, event, instanceIdDb, false);

    constexpr uint8_t command = 0x66;
    auto requestMsg = makeHeaderMessage(PLDM_REQUEST, 5, PLDM_BASE, command);
    invoker.registerHandler(
        PLDM_BASE, std::make_unique<ThrowingOutOfRangeHandler>(command));

    auto response = processRxMsg(requestMsg, invoker, reqHandler, nullptr, 8);

    ASSERT_TRUE(response.has_value());
    ASSERT_EQ(response->size(), sizeof(pldm_msg_hdr) + 1);

    pldm_header_info responseInfo{};
    auto* responseHdr = reinterpret_cast<const pldm_msg_hdr*>(response->data());
    ASSERT_EQ(unpack_pldm_header(responseHdr, &responseInfo), PLDM_SUCCESS);
    EXPECT_EQ(responseInfo.msg_type, PLDM_RESPONSE);
    EXPECT_EQ(responseInfo.instance, 5);
    EXPECT_EQ(responseInfo.pldm_type, PLDM_BASE);
    EXPECT_EQ(responseInfo.command, command);
    EXPECT_EQ(response->back(), PLDM_ERROR_UNSUPPORTED_PLDM_CMD);
}

TEST(PldmdInternalCoverage, ProcessRxMsgFwUpdateRequestUsesFwManagerPath)
{
    pldm::responder::Invoker invoker;
    auto event = sdeventplus::Event::get_default();
    TestInstanceIdDb instanceIdDb;
    RequestHandler reqHandler(nullptr, event, instanceIdDb, false);
    fw_update::Manager fwManager(
        nullptr, event, reqHandler, instanceIdDb,
        "../fw-update/test/fw_update_jsons/fw_update_config_single_entry.json",
        false);

    constexpr uint8_t instanceId = 4;
    constexpr uint8_t command = 0x7E;

    auto requestMsg =
        makeHeaderMessage(PLDM_REQUEST, instanceId, PLDM_FWUP, command);
    auto customResponse = pldm::responder::CmdHandler::ccOnlyResponse(
        reinterpret_cast<const pldm_msg*>(requestMsg.data()), PLDM_SUCCESS);
    invoker.registerHandler(PLDM_FWUP, std::make_unique<StaticResponseHandler>(
                                           command, customResponse));

    auto response =
        processRxMsg(requestMsg, invoker, reqHandler, &fwManager, 8);

    ASSERT_TRUE(response.has_value());
    ASSERT_EQ(response->size(), sizeof(pldm_msg));

    pldm_header_info responseInfo{};
    auto* responseHdr = reinterpret_cast<const pldm_msg_hdr*>(response->data());
    ASSERT_EQ(unpack_pldm_header(responseHdr, &responseInfo), PLDM_SUCCESS);
    EXPECT_EQ(responseInfo.msg_type, PLDM_RESPONSE);
    EXPECT_EQ(responseInfo.instance, instanceId);
    EXPECT_EQ(responseInfo.pldm_type, PLDM_FWUP);
    EXPECT_EQ(responseInfo.command, command);
    EXPECT_EQ((*response)[sizeof(pldm_msg_hdr)],
              PLDM_FWUP_COMMAND_NOT_EXPECTED);
    EXPECT_NE(*response, customResponse);
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

TEST(PldmdInternalCoverage, ProcessRxMsgFwUpdateResponsePathReturnsNullopt)
{
    pldm::responder::Invoker invoker;
    auto event = sdeventplus::Event::get_default();
    TestInstanceIdDb instanceIdDb;
    RequestHandler reqHandler(nullptr, event, instanceIdDb, false);

    auto responseMsg = makeHeaderMessage(PLDM_RESPONSE, 1, PLDM_FWUP, 1);
    auto output = processRxMsg(responseMsg, invoker, reqHandler, nullptr, 8);

    EXPECT_FALSE(output.has_value());
}

TEST(PldmdInternalCoverage, MainInvalidVerboseValueExitsFailure)
{
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd", "--verbose=2"},
                           {makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_FAILURE), ".*");
}

namespace
{

struct ThrowingFwManager
{
    void onResponseSendComplete(mctp_eid_t /*eid*/, bool /*success*/)
    {
        throw std::runtime_error("injected test exception");
    }
};

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

TEST(PldmdInternalCoverage, NotifyFwUpdateSendCompleteDoesNotPropagateException)
{
    ThrowingFwManager manager;
    EXPECT_NO_THROW(notifyFwUpdateSendComplete(&manager, 14, true));
}

TEST(PldmdInternalCoverage,
     NotifyFwUpdateSendCompleteForwardsArgumentsOnSuccess)
{
    TrackingFwManager manager;
    notifyFwUpdateSendComplete(&manager, 14, true);
    EXPECT_TRUE(manager.called);
    EXPECT_EQ(manager.lastEid, static_cast<mctp_eid_t>(14));
    EXPECT_TRUE(manager.lastSuccess);
}

TEST(PldmdInternalCoverage, MainThrowsWhenPdrRepoInitFails)
{
    if (pldm::test::runningOnValgrind())
    {
        GTEST_SKIP() << "subprocess coverage runs in the normal pass";
    }

    EXPECT_EQ(runMainInitFailureInSubprocess(MainInitFailureScenario::pdrRepo),
              EXIT_FAILURE);
}

TEST(PldmdInternalCoverage, MainThrowsWhenEntityTreeInitFails)
{
    if (pldm::test::runningOnValgrind())
    {
        GTEST_SKIP() << "subprocess coverage runs in the normal pass";
    }

    EXPECT_EQ(
        runMainInitFailureInSubprocess(MainInitFailureScenario::entityTree),
        EXIT_FAILURE);
}

TEST(PldmdInternalCoverage, MainThrowsWhenBmcEntityTreeInitFails)
{
    if (pldm::test::runningOnValgrind())
    {
        GTEST_SKIP() << "subprocess coverage runs in the normal pass";
    }

    EXPECT_EQ(
        runMainInitFailureInSubprocess(MainInitFailureScenario::bmcEntityTree),
        EXIT_FAILURE);
}

TEST(PldmdInternalCoverage, MainLambdaReturnsWhenNoEvents)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "synthetic_no_readable");
}

TEST(PldmdInternalCoverage, MainUnknownOptionExitsFailure)
{
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd", "--unknown"},
                           {makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_FAILURE), ".*");
}

TEST(PldmdInternalCoverage, MainDefaultArgumentsExitSuccess)
{
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage, MainVerboseUnsupportedBaseRequestExitsSuccess)
{
    auto requestMsg = makeHeaderMessage(PLDM_REQUEST, 1, PLDM_BASE, 0x7E);
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd", "--verbose=1"},
                           {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
                            makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage, MainVerboseZeroExitSuccess)
{
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd", "--verbose=0"},
                           {makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage, MainVerboseDebugAndNumericSensorArgsExitSuccess)
{
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd", "--verbose=1", "--fw-debug",
                            "--num-sens-wo-aux-name"},
                           {makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage, MainEnableErrorQueueFailureStillExitsSuccess)
{
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_RECV_FAIL)}, -EIO);
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage, MainHandlesRequestNameFailureAndExitsSuccess)
{
    EXPECT_EXIT(
        {
            CoverageHooks::ScopedHookStateReset hooks;
            CoverageHooks::setForceRequestNameFailure();
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
            exit(EXIT_SUCCESS);
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), "");
}

TEST(PldmdInternalCoverage,
     RequestServiceNameHandlesForcedFailureInSubprocessCoverage)
{
    EXPECT_EXIT(
        {
            CoverageHooks::ScopedHookStateReset hooks;
            CoverageHooks::setForceRequestNameFailure();
            requestPLDMServiceName();
            exit(EXIT_SUCCESS);
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), "");
}

TEST(PldmdInternalCoverage,
     MainHandlesHostEidAndRequestNameFailureAndExitsSuccess)
{
    EXPECT_EXIT(
        {
            CoverageHooks::ScopedHookStateReset hooks;
            CoverageHooks::setHostEid(8);
            CoverageHooks::setForceRequestNameFailure();
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
            exit(EXIT_SUCCESS);
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), "");
}

TEST(PldmdInternalCoverage, MainTransportHangupExitsSuccess)
{
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd"}, {}, 0, PLDM_REQUESTER_SUCCESS, EIO, true);
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage, MainProcessesUnsupportedBaseRequestAndExitsSuccess)
{
    auto requestMsg = makeHeaderMessage(PLDM_REQUEST, 1, PLDM_BASE, 0x7E);
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
                            makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage, MainProcessesResponseMessageAndExitsSuccess)
{
    auto responseMsg = makeHeaderMessage(PLDM_RESPONSE, 1, PLDM_BASE, 1);
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_SUCCESS, responseMsg),
                            makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage, MainProcessesSendFailureAndExitsSuccess)
{
    auto requestMsg = makeHeaderMessage(PLDM_REQUEST, 1, PLDM_BASE, 0x7E);
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
                            makeRecvAction(PLDM_REQUESTER_RECV_FAIL)},
                           0, PLDM_REQUESTER_SEND_FAIL, EIO);
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage, MainProcessesOtherReceiveErrorAndExitsSuccess)
{
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_SEND_FAIL),
                            makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage, MainProcessesInvalidHeaderAndExitsSuccess)
{
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_SUCCESS,
                                           std::vector<uint8_t>(
                                               sizeof(pldm_msg_hdr) - 1, 0)),
                            makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage, MainProcessesFwUpdateRequestAndExitsSuccess)
{
    auto requestMsg = makeHeaderMessage(PLDM_REQUEST, 1, PLDM_FWUP, 0x7E);
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
                            makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage, MainProcessesFwUpdateSendFailureAndExitsSuccess)
{
    auto requestMsg = makeHeaderMessage(PLDM_REQUEST, 1, PLDM_FWUP, 0x7E);
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
                            makeRecvAction(PLDM_REQUESTER_RECV_FAIL)},
                           0, PLDM_REQUESTER_SEND_FAIL, EIO);
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage, MainProcessesSensorEventAndExitsSuccess)
{
    auto requestMsg = makePlatformEventMessage(
        PLDM_SENSOR_EVENT,
        makeStateSensorEventData(1, PLDM_STATE_SENSOR_STATE, 0,
                                 PLDM_STATESET_HEALTH_STATE_CRITICAL,
                                 PLDM_STATESET_HEALTH_STATE_NORMAL));
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
                            makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage,
     MainReturnsWhenOnlyNonReadableEventsViaSyntheticLoop)
{
    EXPECT_EXIT(
        {
            CoverageHooks::ScopedHookStateReset hooks;
            CoverageHooks::setSyntheticEventLoop(
                {makeSyntheticIoEvent(EPOLLOUT)});
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage, MainReturnsWhenSyntheticLoopUsesNegativeFd)
{
    EXPECT_EXIT(
        {
            CoverageHooks::ScopedHookStateReset hooks;
            CoverageHooks::setSyntheticEventLoop(
                {makeSyntheticIoEvent(EPOLLIN, -1)});
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage,
     MainHandlesSyntheticTransportErrorWithoutInputAndExitsSuccess)
{
    EXPECT_EXIT(
        {
            CoverageHooks::ScopedHookStateReset hooks;
            CoverageHooks::setReadMctpErrorQueueResult(0);
            CoverageHooks::setExtractedPldmType(PLDM_FWUP);
            CoverageHooks::setSyntheticEventLoop(
                {makeSyntheticIoEvent(EPOLLERR)});
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage,
     MainHandlesSyntheticTransportErrorWithInputAndRecvFailExitsSuccess)
{
    EXPECT_EXIT(
        {
            CoverageHooks::ScopedHookStateReset hooks;
            CoverageHooks::setReadMctpErrorQueueResult(-EIO);
            CoverageHooks::setSyntheticEventLoop(
                {makeSyntheticIoEvent(EPOLLERR | EPOLLIN)});
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage,
     MainHandlesSyntheticTransportErrorWithInputAndNonFwupTypeExitsSuccess)
{
    EXPECT_EXIT(
        {
            CoverageHooks::ScopedHookStateReset hooks;
            CoverageHooks::setReadMctpErrorQueueResult(0);
            CoverageHooks::setExtractedPldmType(PLDM_BASE);
            CoverageHooks::setSyntheticEventLoop(
                {makeSyntheticIoEvent(EPOLLERR | EPOLLIN)});
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage,
     MainHandlesSyntheticTransportErrorWithoutInputAndNonFwupTypeExitsSuccess)
{
    EXPECT_EXIT(
        {
            CoverageHooks::ScopedHookStateReset hooks;
            CoverageHooks::setReadMctpErrorQueueResult(0);
            CoverageHooks::setExtractedPldmType(PLDM_BASE);
            CoverageHooks::setSyntheticEventLoop(
                {makeSyntheticIoEvent(EPOLLERR)});
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage,
     MainHandlesSyntheticTransportErrorWithInputAndFwupRequestExitsSuccess)
{
    auto requestMsg = makeHeaderMessage(PLDM_REQUEST, 1, PLDM_FWUP, 0x7E);
    EXPECT_EXIT(
        {
            CoverageHooks::ScopedHookStateReset hooks;
            CoverageHooks::setReadMctpErrorQueueResult(0);
            CoverageHooks::setExtractedPldmType(PLDM_FWUP);
            CoverageHooks::setSyntheticEventLoop(
                {makeSyntheticIoEvent(EPOLLERR | EPOLLIN)});
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
                            makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage,
     MainHandlesSyntheticTransportErrorWithInputAndFwupSendFailureExitsSuccess)
{
    auto requestMsg = makeHeaderMessage(PLDM_REQUEST, 1, PLDM_FWUP, 0x7E);
    EXPECT_EXIT(
        {
            CoverageHooks::ScopedHookStateReset hooks;
            CoverageHooks::setReadMctpErrorQueueResult(0);
            CoverageHooks::setExtractedPldmType(PLDM_FWUP);
            CoverageHooks::setSyntheticEventLoop(
                {makeSyntheticIoEvent(EPOLLERR | EPOLLIN)});
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
                            makeRecvAction(PLDM_REQUESTER_RECV_FAIL)},
                           0, PLDM_REQUESTER_SEND_FAIL, EIO);
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage, MainHandlesSyntheticHangupEventAndExitsSuccess)
{
    EXPECT_EXIT(
        {
            CoverageHooks::ScopedHookStateReset hooks;
            CoverageHooks::setSyntheticEventLoop(
                {makeSyntheticIoEvent(EPOLLHUP)});
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage,
     MainHandlesSyntheticHangupReadableEventAndExitsSuccess)
{
    EXPECT_EXIT(
        {
            CoverageHooks::ScopedHookStateReset hooks;
            CoverageHooks::setSyntheticEventLoop(
                {makeSyntheticIoEvent(EPOLLHUP | EPOLLIN)});
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage,
     MainHandlesSyntheticTransportErrorFollowedByHangupAndExitsSuccess)
{
    EXPECT_EXIT(
        {
            CoverageHooks::ScopedHookStateReset hooks;
            CoverageHooks::setReadMctpErrorQueueResult(0);
            CoverageHooks::setExtractedPldmType(PLDM_BASE);
            CoverageHooks::setSyntheticEventLoop(
                {makeSyntheticIoEvent(EPOLLERR | EPOLLHUP | EPOLLIN)});
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage, MainEventLoopFailureExitsFailure)
{
    EXPECT_EXIT(
        {
            CoverageHooks::ScopedHookStateReset hooks;
            CoverageHooks::setSyntheticEventLoop({}, 1);
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_FAILURE), ".*");
}

TEST(PldmdInternalCoverage, MainHandlesServiceNameAlreadyOwnedAndExitsSuccess)
{
    TemporaryDbusNameOwner owner(PLDMService);
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage,
     MainInitializesHostHandlersWhenHostEidExistsAndExitsSuccess)
{
    EXPECT_EXIT(
        {
            CoverageHooks::ScopedHookStateReset hooks;
            CoverageHooks::setHostEid(8);
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage,
     MainInitializesHostHandlersAndProcessesUnsupportedBaseWithHostEid)
{
    auto requestMsg = makeHeaderMessage(PLDM_REQUEST, 1, PLDM_BASE, 0x7E);
    expectMainExitWithHostEid(
        {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
         makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
}

TEST(PldmdInternalCoverage,
     MainInitializesHostHandlersAndProcessesFwUpdateRequestWithHostEid)
{
    auto requestMsg = makeHeaderMessage(PLDM_REQUEST, 1, PLDM_FWUP, 0x7E);
    expectMainExitWithHostEid(
        {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
         makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
}

TEST(PldmdInternalCoverage,
     MainInitializesHostHandlersAndProcessesFwUpdateSendFailureWithHostEid)
{
    auto requestMsg = makeHeaderMessage(PLDM_REQUEST, 1, PLDM_FWUP, 0x7E);
    expectMainExitWithHostEid(
        {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
         makeRecvAction(PLDM_REQUESTER_RECV_FAIL)},
        PLDM_REQUESTER_SEND_FAIL, EIO);
}

TEST(PldmdInternalCoverage,
     MainInitializesHostHandlersAndProcessesResponseWithHostEid)
{
    auto responseMsg = makeHeaderMessage(PLDM_RESPONSE, 1, PLDM_BASE, 1);
    expectMainExitWithHostEid(
        {makeRecvAction(PLDM_REQUESTER_SUCCESS, responseMsg),
         makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
}

TEST(PldmdInternalCoverage,
     MainInitializesHostHandlersAndProcessesSensorEventWithHostEid)
{
    auto requestMsg = makePlatformEventMessage(
        PLDM_SENSOR_EVENT,
        makeStateSensorEventData(1, PLDM_STATE_SENSOR_STATE, 0,
                                 PLDM_STATESET_HEALTH_STATE_CRITICAL,
                                 PLDM_STATESET_HEALTH_STATE_NORMAL));
    expectMainExitWithHostEid(
        {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
         makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
}

TEST(PldmdInternalCoverage,
     MainInitializesHostHandlersAndProcessesMessagePollEventWithHostEid)
{
    auto requestMsg = makePlatformEventMessage(PLDM_MESSAGE_POLL_EVENT, {0x00});
    expectMainExitWithHostEid(
        {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
         makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
}

TEST(PldmdInternalCoverage,
     MainInitializesHostHandlersAndProcessesCperEventWithHostEid)
{
    auto requestMsg =
        makePlatformEventMessage(PLDM_CPER_EVENT, {0x01, 0x00, 0x00, 0x00});
    expectMainExitWithHostEid(
        {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
         makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
}

TEST(PldmdInternalCoverage,
     MainInitializesHostHandlersAndProcessesOemCperEventWithHostEid)
{
    auto requestMsg = makePlatformEventMessage(PLDM_OEM_EVENT_CLASS_0xFA,
                                               {0x01, 0x00, 0x00, 0x00});
    expectMainExitWithHostEid(
        {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
         makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
}

TEST(PldmdInternalCoverage,
     MainInitializesHostHandlersAndProcessesActiveFwVersionEventWithHostEid)
{
    auto requestMsg =
        makePlatformEventMessage(PLDM_OEM_EVENT_CLASS_0xFB, {0x00});
    expectMainExitWithHostEid(
        {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
         makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
}

TEST(PldmdInternalCoverage,
     MainInitializesHostHandlersAndProcessesSmbiosEventWithHostEid)
{
    auto requestMsg =
        makePlatformEventMessage(PLDM_OEM_EVENT_CLASS_0xFC, {0x01});
    expectMainExitWithHostEid(
        {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
         makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
}

TEST(PldmdInternalCoverage,
     MainInitializesHostHandlersAndProcessesInventoryJsonEventWithHostEid)
{
    auto requestMsg = makePlatformEventMessage(
        pldm::platform::PLDM_OEM_EVENT_CLASS_0xF3, {0x01, 0x00, 0x00});
    expectMainExitWithHostEid(
        {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
         makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
}

TEST(PldmdInternalCoverage,
     MainInitializesHostHandlersAndProcessesTelemetryManagementEventWithHostEid)
{
    auto requestMsg =
        makePlatformEventMessage(pldm::platform::PLDM_OEM_EVENT_CLASS_0xFD,
                                 {0x02, 0x01, 0x01, 0x00, 0x5A});
    expectMainExitWithHostEid(
        {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
         makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
}

TEST(PldmdInternalCoverage,
     MainInitializesHostHandlersAndProcessesErrorCounterEventWithHostEid)
{
    auto requestMsg = makePlatformEventMessage(
        pldm::platform::PLDM_OEM_EVENT_CLASS_ERROR_COUNTER, {0x01, 0x00});
    expectMainExitWithHostEid(
        {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
         makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
}

TEST(PldmdInternalCoverage,
     MainInitializesHostHandlersAndProcessesPcieTelemetryEventWithHostEid)
{
    auto requestMsg = makePlatformEventMessage(
        pldm::platform::PLDM_OEM_EVENT_CLASS_PCIE_TELEMETRY,
        {0x02, 0x01, 0x01, 0x00, 0x5A});
    expectMainExitWithHostEid(
        {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
         makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
}

TEST(PldmdInternalCoverage,
     MainInitializesHostHandlersAndProcessesPcieLtssmEventWithHostEid)
{
    auto requestMsg = makePlatformEventMessage(
        pldm::platform::PLDM_OEM_EVENT_CLASS_PCIE_LTSSM,
        {0x02, 0x01, 0x01, 0x00, 0x5A});
    expectMainExitWithHostEid(
        {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
         makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
}

TEST(PldmdInternalCoverage, MainProcessesMessagePollEventAndExitsSuccess)
{
    auto requestMsg = makePlatformEventMessage(PLDM_MESSAGE_POLL_EVENT, {0x00});
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
                            makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage, MainProcessesCperEventAndExitsSuccess)
{
    auto requestMsg =
        makePlatformEventMessage(PLDM_CPER_EVENT, {0x01, 0x00, 0x00, 0x00});
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
                            makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage, MainProcessesOemCperEventAndExitsSuccess)
{
    auto requestMsg = makePlatformEventMessage(PLDM_OEM_EVENT_CLASS_0xFA,
                                               {0x01, 0x00, 0x00, 0x00});
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
                            makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage,
     MainProcessesActiveFwVersionChangeEventAndExitsSuccess)
{
    auto requestMsg =
        makePlatformEventMessage(PLDM_OEM_EVENT_CLASS_0xFB, {0x00});
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
                            makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage, MainProcessesSmbiosEventAndExitsSuccess)
{
    auto requestMsg =
        makePlatformEventMessage(PLDM_OEM_EVENT_CLASS_0xFC, {0x01});
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
                            makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage, MainProcessesInventoryJsonEventAndExitsSuccess)
{
    auto requestMsg = makePlatformEventMessage(
        pldm::platform::PLDM_OEM_EVENT_CLASS_0xF3, {0x01, 0x00, 0x00});
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
                            makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage,
     MainProcessesTelemetryManagementEventAndExitsSuccess)
{
    auto requestMsg =
        makePlatformEventMessage(pldm::platform::PLDM_OEM_EVENT_CLASS_0xFD,
                                 {0x02, 0x01, 0x01, 0x00, 0x5A});
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
                            makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage, MainProcessesErrorCounterEventAndExitsSuccess)
{
    auto requestMsg = makePlatformEventMessage(
        pldm::platform::PLDM_OEM_EVENT_CLASS_ERROR_COUNTER, {0x01, 0x00});
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
                            makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage, MainProcessesPcieTelemetryEventAndExitsSuccess)
{
    auto requestMsg = makePlatformEventMessage(
        pldm::platform::PLDM_OEM_EVENT_CLASS_PCIE_TELEMETRY,
        {0x02, 0x01, 0x01, 0x00, 0x5A});
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
                            makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage, MainProcessesPcieLtssmEventAndExitsSuccess)
{
    auto requestMsg = makePlatformEventMessage(
        pldm::platform::PLDM_OEM_EVENT_CLASS_PCIE_LTSSM,
        {0x02, 0x01, 0x01, 0x00, 0x5A});
    EXPECT_EXIT(
        {
            runMainForTest({"pldmd"},
                           {makeRecvAction(PLDM_REQUESTER_SUCCESS, requestMsg),
                            makeRecvAction(PLDM_REQUESTER_RECV_FAIL)});
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), ".*");
}

TEST(PldmdInternalCoverage, RealMainInvalidVerboseValueExitsFailure)
{
    expectPldmdMainCoverageHelperExit({"--verbose=2"}, EXIT_FAILURE);
}

TEST(PldmdInternalCoverage, RealMainUnknownOptionExitsFailure)
{
    expectPldmdMainCoverageHelperExit({"--unknown"}, EXIT_FAILURE);
}

TEST(PldmdInternalCoverage, RealMainDefaultArgumentsExitSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS);
}

TEST(PldmdInternalCoverage, RealMainHandlesSigusr1AndHangupExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "sigusr1_then_hangup");
}

TEST(PldmdInternalCoverage, RealMainThrowsWhenPdrRepoInitFails)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_FAILURE, "pdr_init_fail");
}

TEST(PldmdInternalCoverage, RealMainThrowsWhenEntityTreeInitFails)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_FAILURE, "entity_init_fail");
}

TEST(PldmdInternalCoverage, RealMainThrowsWhenBmcEntityTreeInitFails)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_FAILURE, "bmc_entity_init_fail");
}

TEST(PldmdInternalCoverage, RealMainVerboseZeroExitSuccess)
{
    expectPldmdMainCoverageHelperExit({"--verbose=0"}, EXIT_SUCCESS);
}

TEST(PldmdInternalCoverage, RealMainVerboseUnsupportedBaseRequestExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({"--verbose=1"}, EXIT_SUCCESS,
                                      "unsupported_base");
}

TEST(PldmdInternalCoverage, RealMainVerboseDebugAndNumericSensorArgsExitSuccess)
{
    expectPldmdMainCoverageHelperExit(
        {"--verbose=1", "--fw-debug", "--num-sens-wo-aux-name"}, EXIT_SUCCESS);
}

TEST(PldmdInternalCoverage, RealMainEnableErrorQueueFailureStillExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "enable_error_fail");
}

TEST(PldmdInternalCoverage, RealMainHandlesRequestNameFailureAndExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "request_name_fail");
}

TEST(PldmdInternalCoverage, RealMainTransportHangupExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "hangup");
}

TEST(PldmdInternalCoverage,
     RealMainProcessesUnsupportedBaseRequestAndExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "unsupported_base");
}

TEST(PldmdInternalCoverage, RealMainProcessesResponseMessageAndExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "response_msg");
}

TEST(PldmdInternalCoverage,
     RealMainProcessesFwUpdateResponseMessageAndExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "fwup_response_msg");
}

TEST(PldmdInternalCoverage, RealMainProcessesSendFailureAndExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "send_fail");
}

TEST(PldmdInternalCoverage, RealMainProcessesOtherReceiveErrorAndExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "other_recv_error");
}

TEST(PldmdInternalCoverage, RealMainProcessesInvalidHeaderAndExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "invalid_header");
}

TEST(PldmdInternalCoverage, RealMainProcessesFwUpdateRequestAndExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "fwup_request");
}

TEST(PldmdInternalCoverage, RealMainProcessesFwUpdateSendFailureAndExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "fwup_send_fail");
}

TEST(PldmdInternalCoverage, RealMainProcessesSensorEventAndExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "sensor_event");
}

TEST(PldmdInternalCoverage,
     RealMainReturnsWhenOnlyNonReadableEventsViaSyntheticLoop)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "synthetic_no_readable");
}

TEST(PldmdInternalCoverage, RealMainReturnsWhenNegativeFdViaSyntheticLoop)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "synthetic_negative_fd");
}

TEST(PldmdInternalCoverage,
     RealMainHandlesSyntheticTransportErrorWithoutInputAndExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "epollerr_no_input_fwup");
}

TEST(
    PldmdInternalCoverage,
    RealMainHandlesSyntheticTransportErrorWithoutInputAndNonFwupTypeExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "epollerr_no_input_non_fwup");
}

TEST(PldmdInternalCoverage,
     RealMainHandlesSyntheticTransportErrorWithInputAndRecvFailExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "epollerr_with_input_recv_fail");
}

TEST(PldmdInternalCoverage,
     RealMainHandlesSyntheticTransportErrorWithInputAndNonFwupTypeExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "epollerr_with_input_non_fwup");
}

TEST(PldmdInternalCoverage,
     RealMainHandlesSyntheticTransportErrorWithInputAndFwupRequestExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "epollerr_with_input_fwup_request");
}

TEST(
    PldmdInternalCoverage,
    RealMainHandlesSyntheticTransportErrorWithInputAndFwupSendFailureExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "epollerr_with_input_fwup_send_fail");
}

TEST(PldmdInternalCoverage, RealMainEventLoopFailureExitsFailure)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_FAILURE, "event_loop_fail");
}

TEST(PldmdInternalCoverage, RealMainHandlesSyntheticHangupEventAndExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "synthetic_hangup");
}

TEST(PldmdInternalCoverage,
     RealMainHandlesSyntheticHangupReadableEventAndExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "synthetic_hangup_readable");
}

TEST(PldmdInternalCoverage,
     RealMainHandlesSyntheticTransportErrorFollowedByHangupAndExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "epollerr_followed_by_hangup");
}

TEST(PldmdInternalCoverage, RealMainProcessesMessagePollEventAndExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "message_poll_event");
}

TEST(PldmdInternalCoverage, RealMainProcessesCperEventAndExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "cper_event");
}

TEST(PldmdInternalCoverage, RealMainProcessesOemCperEventAndExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "oem_cper_event");
}

TEST(PldmdInternalCoverage,
     RealMainProcessesActiveFwVersionChangeEventAndExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "active_fw_version");
}

TEST(PldmdInternalCoverage, RealMainProcessesSmbiosEventAndExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "smbios_event");
}

TEST(PldmdInternalCoverage, RealMainProcessesInventoryJsonEventAndExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "inventory_json");
}

TEST(PldmdInternalCoverage,
     RealMainProcessesTelemetryManagementEventAndExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "telemetry_management");
}

TEST(PldmdInternalCoverage, RealMainProcessesErrorCounterEventAndExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "error_counter");
}

TEST(PldmdInternalCoverage, RealMainProcessesPcieTelemetryEventAndExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "pcie_telemetry");
}

TEST(PldmdInternalCoverage, RealMainProcessesPcieLtssmEventAndExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "pcie_ltssm");
}

TEST(PldmdInternalCoverage,
     RealMainHandlesServiceNameAlreadyOwnedAndExitsSuccess)
{
    TemporaryDbusNameOwner owner(PLDMService);
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS);
}

TEST(PldmdInternalCoverage,
     RealMainInitializesHostHandlersWhenHostEidExistsAndExitsSuccess)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "host_eid");
}

TEST(PldmdInternalCoverage,
     RealMainInitializesHostHandlersAndProcessesUnsupportedBaseWithHostEid)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "host_eid_unsupported_base");
}

TEST(PldmdInternalCoverage,
     RealMainInitializesHostHandlersAndProcessesFwUpdateRequestWithHostEid)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "host_eid_fwup_request");
}

TEST(PldmdInternalCoverage,
     RealMainInitializesHostHandlersAndProcessesFwUpdateSendFailureWithHostEid)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "host_eid_fwup_send_fail");
}

TEST(PldmdInternalCoverage,
     RealMainInitializesHostHandlersAndProcessesResponseWithHostEid)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "host_eid_response_msg");
}

TEST(PldmdInternalCoverage,
     RealMainInitializesHostHandlersAndProcessesFwUpdateResponseWithHostEid)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "host_eid_fwup_response_msg");
}

TEST(PldmdInternalCoverage,
     RealMainInitializesHostHandlersAndProcessesSensorEventWithHostEid)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "host_eid_sensor_event");
}

TEST(PldmdInternalCoverage,
     RealMainInitializesHostHandlersAndProcessesMessagePollEventWithHostEid)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "host_eid_message_poll_event");
}

TEST(PldmdInternalCoverage,
     RealMainInitializesHostHandlersAndProcessesCperEventWithHostEid)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "host_eid_cper_event");
}

TEST(PldmdInternalCoverage,
     RealMainInitializesHostHandlersAndProcessesOemCperEventWithHostEid)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "host_eid_oem_cper_event");
}

TEST(PldmdInternalCoverage,
     RealMainInitializesHostHandlersAndProcessesActiveFwVersionEventWithHostEid)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "host_eid_active_fw_version");
}

TEST(PldmdInternalCoverage,
     RealMainInitializesHostHandlersAndProcessesSmbiosEventWithHostEid)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "host_eid_smbios_event");
}

TEST(PldmdInternalCoverage,
     RealMainInitializesHostHandlersAndProcessesInventoryJsonEventWithHostEid)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "host_eid_inventory_json");
}

TEST(
    PldmdInternalCoverage,
    RealMainInitializesHostHandlersAndProcessesTelemetryManagementEventWithHostEid)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "host_eid_telemetry_management");
}

TEST(PldmdInternalCoverage,
     RealMainInitializesHostHandlersAndProcessesErrorCounterEventWithHostEid)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "host_eid_error_counter");
}

TEST(PldmdInternalCoverage,
     RealMainInitializesHostHandlersAndProcessesPcieTelemetryEventWithHostEid)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "host_eid_pcie_telemetry");
}

TEST(PldmdInternalCoverage,
     RealMainInitializesHostHandlersAndProcessesPcieLtssmEventWithHostEid)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS, "host_eid_pcie_ltssm");
}

TEST(PldmdInternalCoverage,
     RealMainInitializesHostHandlersAndHandlesRequestNameFailureWithHostEid)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "host_eid_request_name_fail");
}

TEST(PldmdInternalCoverage, DbusImplPdrFindStateEffecterPdrReturnsMatch)
{
    auto repo = std::unique_ptr<pldm_pdr, decltype(&pldm_pdr_destroy)>(
        pldm_pdr_init(), pldm_pdr_destroy);
    ASSERT_NE(repo, nullptr);

    auto expectedPdr = makeMinimalStateEffecterPdr(33, 196);
    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo.get(), expectedPdr.data(), expectedPdr.size(),
                           false, 1, &handle),
              0);

    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::dbus_api::Pdr pdrApi(bus, "/xyz/openbmc_project/pldm/test/effecter",
                               repo.get());

    auto pdrs = pdrApi.findStateEffecterPDR(1, 33, 196);
    ASSERT_EQ(pdrs.size(), 1u);
    EXPECT_EQ(pdrs[0], expectedPdr);
}

TEST(PldmdInternalCoverage, DbusImplPdrFindStateEffecterPdrThrowsWhenMissing)
{
    auto repo = std::unique_ptr<pldm_pdr, decltype(&pldm_pdr_destroy)>(
        pldm_pdr_init(), pldm_pdr_destroy);
    ASSERT_NE(repo, nullptr);

    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::dbus_api::Pdr pdrApi(
        bus, "/xyz/openbmc_project/pldm/test/effecter_empty", repo.get());

    EXPECT_THROW(
        pdrApi.findStateEffecterPDR(1, 33, 196),
        sdbusplus::xyz::openbmc_project::Common::Error::ResourceNotFound);
}

TEST(PldmdInternalCoverage, DbusImplPdrFindStateSensorPdrReturnsMatch)
{
    auto repo = std::unique_ptr<pldm_pdr, decltype(&pldm_pdr_destroy)>(
        pldm_pdr_init(), pldm_pdr_destroy);
    ASSERT_NE(repo, nullptr);

    auto expectedPdr = makeMinimalStateSensorPdr(5, 1);
    uint32_t handle = 0;
    ASSERT_EQ(pldm_pdr_add(repo.get(), expectedPdr.data(), expectedPdr.size(),
                           false, 1, &handle),
              0);

    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::dbus_api::Pdr pdrApi(bus, "/xyz/openbmc_project/pldm/test/sensor",
                               repo.get());

    auto pdrs = pdrApi.findStateSensorPDR(1, 5, 1);
    ASSERT_EQ(pdrs.size(), 1u);
    EXPECT_EQ(pdrs[0], expectedPdr);
}

TEST(PldmdInternalCoverage, DbusImplPdrFindStateSensorPdrThrowsWhenMissing)
{
    auto repo = std::unique_ptr<pldm_pdr, decltype(&pldm_pdr_destroy)>(
        pldm_pdr_init(), pldm_pdr_destroy);
    ASSERT_NE(repo, nullptr);

    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::dbus_api::Pdr pdrApi(
        bus, "/xyz/openbmc_project/pldm/test/sensor_empty", repo.get());

    EXPECT_THROW(
        pdrApi.findStateSensorPDR(1, 5, 1),
        sdbusplus::xyz::openbmc_project::Common::Error::ResourceNotFound);
}

TEST(PldmdInternalCoverage, DbusImplPdrVirtualDestructorCoverage)
{
    auto repo = std::unique_ptr<pldm_pdr, decltype(&pldm_pdr_destroy)>(
        pldm_pdr_init(), pldm_pdr_destroy);
    ASSERT_NE(repo, nullptr);

    auto& bus = pldm::utils::DBusHandler::getBus();
    auto* base = static_cast<pldm::dbus_api::PdrIntf*>(new pldm::dbus_api::Pdr(
        bus, "/xyz/openbmc_project/pldm/test/delete", repo.get()));

    EXPECT_NO_THROW(delete base);
}

TEST(PldmdInternalCoverage, DbusImplPdrDeletingDestructorCoverage)
{
    auto repo = std::unique_ptr<pldm_pdr, decltype(&pldm_pdr_destroy)>(
        pldm_pdr_init(), pldm_pdr_destroy);
    ASSERT_NE(repo, nullptr);

    auto& bus = pldm::utils::DBusHandler::getBus();
    auto* pdr = new pldm::dbus_api::Pdr(
        bus, "/xyz/openbmc_project/pldm/test/delete_direct", repo.get());

    EXPECT_NO_THROW(delete pdr);
}

TEST(PldmdInternalCoverage, MainLambdaReturnsWhenNoReadableEvents)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "synthetic_no_readable");
}

TEST(PldmdInternalCoverage, MainLambdaReturnsWhenFdIsNegative)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "synthetic_negative_fd");
}

TEST(PldmdInternalCoverage,
     MainLambdaReturnsAfterTransportErrorQueueReadSucceedsWithoutReadableEvents)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "epollerr_no_input_non_fwup");
}

TEST(PldmdInternalCoverage,
     MainLambdaReturnsAfterTransportErrorWithoutReadableEvents)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "epollerr_no_input_error");
}

TEST(
    PldmdInternalCoverage,
    MainLambdaReturnsAfterTransportErrorQueueReadIsPositiveWithoutReadableEvents)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "epollerr_no_input_positive");
}

TEST(PldmdInternalCoverage, MainLambdaReturnsAfterErrorAndNegativeFd)
{
    expectPldmdMainCoverageHelperExit({}, EXIT_SUCCESS,
                                      "epollerr_negative_fd_with_input_error");
}

TEST(PldmdInternalCoverage,
     MainLambdaReturnsAfterTransportErrorQueueReadSucceedsAndFdIsNegative)
{
    expectPldmdMainCoverageHelperExit(
        {}, EXIT_SUCCESS, "epollerr_negative_fd_with_input_non_fwup");
}

TEST(PldmdInternalCoverage,
     MainLambdaReturnsAfterPositiveTransportErrorQueueReadAndNegativeFd)
{
    expectPldmdMainCoverageHelperExit(
        {}, EXIT_SUCCESS, "epollerr_negative_fd_with_input_positive");
}
