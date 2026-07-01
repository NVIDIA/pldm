#include "common/platform_defs.hpp"
#include "common/transport.hpp"
#include "pldmd_coverage_hooks.hpp"
#include "test/test_tmp_utils.hpp"

#include <libpldm/base.h>
#include <libpldm/instance-id.h>
#include <libpldm/platform.h>
#include <poll.h>
#include <pthread.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef OEM_IBM
#undef OEM_IBM
#endif

#ifdef OEM_AMPERE
#undef OEM_AMPERE
#endif

namespace
{

namespace CoverageHooks = pldm::test::coverage;

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
};

FakeTransportState& fakeTransportState()
{
    static FakeTransportState state;
    return state;
}

std::string getActiveScenario()
{
    const char* scenario = std::getenv("PLDMD_HELPER_SCENARIO");
    return scenario ? scenario : "recv_fail";
}

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
    if (pack_pldm_header(&header, hdr) != PLDM_SUCCESS)
    {
        throw std::runtime_error("failed to pack PLDM header");
    }
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

    if (encode_platform_event_message_req(
            instanceId, PLDM_PLATFORM_EVENT_MESSAGE_FORMAT_VERSION, tid,
            eventClass, eventDataPtr, eventData.size(), msg, payloadLength) !=
        PLDM_SUCCESS)
    {
        throw std::runtime_error("failed to encode platform event request");
    }
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

void launchSigusr1AndHangupThread()
{
    std::thread([] {
        using namespace std::chrono_literals;

        sigset_t sigMask{};
        sigemptyset(&sigMask);
        sigaddset(&sigMask, SIGUSR1);
        pthread_sigmask(SIG_BLOCK, &sigMask, nullptr);

        for (int attempt = 0; attempt < 200; ++attempt)
        {
            if (fakeTransportState().writeFd >= 0)
            {
                break;
            }
            std::this_thread::sleep_for(5ms);
        }

        std::this_thread::sleep_for(500ms);
        kill(getpid(), SIGUSR1);
        std::this_thread::sleep_for(100ms);

        for (int attempt = 0; attempt < 200; ++attempt)
        {
            auto& state = fakeTransportState();
            if (state.writeFd >= 0)
            {
                close(state.writeFd);
                state.writeFd = -1;
                return;
            }
            std::this_thread::sleep_for(5ms);
        }
    }).detach();
}

// These helpers intentionally enumerate many scenario names that map to the
// same shape of state setup with different payload constants.
// NOLINTBEGIN(bugprone-branch-clone)
void configureFakeTransportFromEnv()
{
    resetFakeTransportState();
    auto& state = fakeTransportState();
    const std::string activeScenario = getActiveScenario();

    auto recvFail = FakeRecvAction{};
    auto recvSuccess = [](std::vector<uint8_t> payload) {
        return FakeRecvAction{PLDM_REQUESTER_SUCCESS, 8, std::move(payload)};
    };

    if (activeScenario == "enable_error_fail")
    {
        state.enableErrorQueueRc = -EIO;
        state.recvActions = {recvFail};
    }
    else if (activeScenario == "hangup")
    {
        state.closeWriteFdAfterConstruct = true;
    }
    else if (activeScenario == "unsupported_base")
    {
        state.recvActions = {
            recvSuccess(makeHeaderMessage(PLDM_REQUEST, 1, PLDM_BASE, 0x7E)),
            recvFail};
    }
    else if (activeScenario == "response_msg")
    {
        state.recvActions = {
            recvSuccess(makeHeaderMessage(PLDM_RESPONSE, 1, PLDM_BASE, 1)),
            recvFail};
    }
    else if (activeScenario == "fwup_response_msg")
    {
        state.recvActions = {
            recvSuccess(makeHeaderMessage(PLDM_RESPONSE, 1, PLDM_FWUP, 1)),
            recvFail};
    }
    else if (activeScenario == "send_fail")
    {
        state.sendRc = PLDM_REQUESTER_SEND_FAIL;
        state.recvActions = {
            recvSuccess(makeHeaderMessage(PLDM_REQUEST, 1, PLDM_BASE, 0x7E)),
            recvFail};
    }
    else if (activeScenario == "other_recv_error")
    {
        state.recvActions = {FakeRecvAction{PLDM_REQUESTER_SEND_FAIL, 8, {}},
                             recvFail};
    }
    else if (activeScenario == "invalid_header")
    {
        state.recvActions = {
            recvSuccess(std::vector<uint8_t>(sizeof(pldm_msg_hdr) - 1, 0)),
            recvFail};
    }
    else if (activeScenario == "fwup_request")
    {
        state.recvActions = {
            recvSuccess(makeHeaderMessage(PLDM_REQUEST, 1, PLDM_FWUP, 0x7E)),
            recvFail};
    }
    else if (activeScenario == "fwup_send_fail")
    {
        state.sendRc = PLDM_REQUESTER_SEND_FAIL;
        state.recvActions = {
            recvSuccess(makeHeaderMessage(PLDM_REQUEST, 1, PLDM_FWUP, 0x7E)),
            recvFail};
    }
    else if (activeScenario == "host_eid_unsupported_base")
    {
        state.recvActions = {
            recvSuccess(makeHeaderMessage(PLDM_REQUEST, 1, PLDM_BASE, 0x7E)),
            recvFail};
    }
    else if (activeScenario == "host_eid_fwup_request")
    {
        state.recvActions = {
            recvSuccess(makeHeaderMessage(PLDM_REQUEST, 1, PLDM_FWUP, 0x7E)),
            recvFail};
    }
    else if (activeScenario == "host_eid_fwup_send_fail")
    {
        state.sendRc = PLDM_REQUESTER_SEND_FAIL;
        state.recvActions = {
            recvSuccess(makeHeaderMessage(PLDM_REQUEST, 1, PLDM_FWUP, 0x7E)),
            recvFail};
    }
    else if (activeScenario == "host_eid_response_msg")
    {
        state.recvActions = {
            recvSuccess(makeHeaderMessage(PLDM_RESPONSE, 1, PLDM_BASE, 1)),
            recvFail};
    }
    else if (activeScenario == "host_eid_fwup_response_msg")
    {
        state.recvActions = {
            recvSuccess(makeHeaderMessage(PLDM_RESPONSE, 1, PLDM_FWUP, 1)),
            recvFail};
    }
    else if (activeScenario == "host_eid_sensor_event")
    {
        state.recvActions = {
            recvSuccess(makePlatformEventMessage(
                PLDM_SENSOR_EVENT,
                makeStateSensorEventData(1, PLDM_STATE_SENSOR_STATE, 0,
                                         PLDM_STATESET_HEALTH_STATE_CRITICAL,
                                         PLDM_STATESET_HEALTH_STATE_NORMAL))),
            recvFail};
    }
    else if (activeScenario == "host_eid_message_poll_event")
    {
        state.recvActions = {recvSuccess(makePlatformEventMessage(
                                 PLDM_MESSAGE_POLL_EVENT, {0x00})),
                             recvFail};
    }
    else if (activeScenario == "host_eid_cper_event")
    {
        state.recvActions = {recvSuccess(makePlatformEventMessage(
                                 PLDM_CPER_EVENT, {0x01, 0x00, 0x00, 0x00})),
                             recvFail};
    }
    else if (activeScenario == "host_eid_oem_cper_event")
    {
        state.recvActions = {
            recvSuccess(makePlatformEventMessage(PLDM_OEM_EVENT_CLASS_0xFA,
                                                 {0x01, 0x00, 0x00, 0x00})),
            recvFail};
    }
    else if (activeScenario == "host_eid_active_fw_version")
    {
        state.recvActions = {recvSuccess(makePlatformEventMessage(
                                 PLDM_OEM_EVENT_CLASS_0xFB, {0x00})),
                             recvFail};
    }
    else if (activeScenario == "host_eid_smbios_event")
    {
        state.recvActions = {recvSuccess(makePlatformEventMessage(
                                 PLDM_OEM_EVENT_CLASS_0xFC, {0x01})),
                             recvFail};
    }
    else if (activeScenario == "host_eid_inventory_json")
    {
        state.recvActions = {
            recvSuccess(makePlatformEventMessage(
                pldm::platform::PLDM_OEM_EVENT_CLASS_0xF3, {0x01, 0x00, 0x00})),
            recvFail};
    }
    else if (activeScenario == "host_eid_telemetry_management")
    {
        state.recvActions = {recvSuccess(makePlatformEventMessage(
                                 pldm::platform::PLDM_OEM_EVENT_CLASS_0xFD,
                                 {0x02, 0x01, 0x01, 0x00, 0x5A})),
                             recvFail};
    }
    else if (activeScenario == "host_eid_error_counter")
    {
        state.recvActions = {
            recvSuccess(makePlatformEventMessage(
                pldm::platform::PLDM_OEM_EVENT_CLASS_ERROR_COUNTER,
                {0x01, 0x00})),
            recvFail};
    }
    else if (activeScenario == "host_eid_pcie_telemetry")
    {
        state.recvActions = {
            recvSuccess(makePlatformEventMessage(
                pldm::platform::PLDM_OEM_EVENT_CLASS_PCIE_TELEMETRY,
                {0x02, 0x01, 0x01, 0x00, 0x5A})),
            recvFail};
    }
    else if (activeScenario == "host_eid_pcie_ltssm")
    {
        state.recvActions = {recvSuccess(makePlatformEventMessage(
                                 pldm::platform::PLDM_OEM_EVENT_CLASS_MFTDUMP,
                                 {0x02, 0x01, 0x01, 0x00, 0x5A})),
                             recvFail};
    }
    else if (activeScenario == "epollerr_with_input_fwup_request")
    {
        state.recvActions = {
            recvSuccess(makeHeaderMessage(PLDM_REQUEST, 1, PLDM_FWUP, 0x7E)),
            recvFail};
    }
    else if (activeScenario == "epollerr_with_input_fwup_send_fail")
    {
        state.sendRc = PLDM_REQUESTER_SEND_FAIL;
        state.recvActions = {
            recvSuccess(makeHeaderMessage(PLDM_REQUEST, 1, PLDM_FWUP, 0x7E)),
            recvFail};
    }
    else if (activeScenario == "sensor_event")
    {
        state.recvActions = {
            recvSuccess(makePlatformEventMessage(
                PLDM_SENSOR_EVENT,
                makeStateSensorEventData(1, PLDM_STATE_SENSOR_STATE, 0,
                                         PLDM_STATESET_HEALTH_STATE_CRITICAL,
                                         PLDM_STATESET_HEALTH_STATE_NORMAL))),
            recvFail};
    }
    else if (activeScenario == "message_poll_event")
    {
        state.recvActions = {recvSuccess(makePlatformEventMessage(
                                 PLDM_MESSAGE_POLL_EVENT, {0x00})),
                             recvFail};
    }
    else if (activeScenario == "cper_event")
    {
        state.recvActions = {recvSuccess(makePlatformEventMessage(
                                 PLDM_CPER_EVENT, {0x01, 0x00, 0x00, 0x00})),
                             recvFail};
    }
    else if (activeScenario == "oem_cper_event")
    {
        state.recvActions = {
            recvSuccess(makePlatformEventMessage(PLDM_OEM_EVENT_CLASS_0xFA,
                                                 {0x01, 0x00, 0x00, 0x00})),
            recvFail};
    }
    else if (activeScenario == "active_fw_version")
    {
        state.recvActions = {recvSuccess(makePlatformEventMessage(
                                 PLDM_OEM_EVENT_CLASS_0xFB, {0x00})),
                             recvFail};
    }
    else if (activeScenario == "smbios_event")
    {
        state.recvActions = {recvSuccess(makePlatformEventMessage(
                                 PLDM_OEM_EVENT_CLASS_0xFC, {0x01})),
                             recvFail};
    }
    else if (activeScenario == "inventory_json")
    {
        state.recvActions = {
            recvSuccess(makePlatformEventMessage(
                pldm::platform::PLDM_OEM_EVENT_CLASS_0xF3, {0x01, 0x00, 0x00})),
            recvFail};
    }
    else if (activeScenario == "telemetry_management")
    {
        state.recvActions = {recvSuccess(makePlatformEventMessage(
                                 pldm::platform::PLDM_OEM_EVENT_CLASS_0xFD,
                                 {0x02, 0x01, 0x01, 0x00, 0x5A})),
                             recvFail};
    }
    else if (activeScenario == "error_counter")
    {
        state.recvActions = {
            recvSuccess(makePlatformEventMessage(
                pldm::platform::PLDM_OEM_EVENT_CLASS_ERROR_COUNTER,
                {0x01, 0x00})),
            recvFail};
    }
    else if (activeScenario == "pcie_telemetry")
    {
        state.recvActions = {
            recvSuccess(makePlatformEventMessage(
                pldm::platform::PLDM_OEM_EVENT_CLASS_PCIE_TELEMETRY,
                {0x02, 0x01, 0x01, 0x00, 0x5A})),
            recvFail};
    }
    else if (activeScenario == "pcie_ltssm")
    {
        state.recvActions = {recvSuccess(makePlatformEventMessage(
                                 pldm::platform::PLDM_OEM_EVENT_CLASS_MFTDUMP,
                                 {0x02, 0x01, 0x01, 0x00, 0x5A})),
                             recvFail};
    }
    else if (activeScenario == "epollerr_followed_by_hangup")
    {
        state.recvActions = {recvFail};
    }
    else if (activeScenario == "sigusr1_then_hangup")
    {
        state.recvActions.clear();
    }
    else
    {
        state.recvActions = {recvFail};
    }
}

void configureCoverageHooksFromEnv()
{
    CoverageHooks::resetHooks();
    const std::string activeScenario = getActiveScenario();

    if (activeScenario == "host_eid" ||
        activeScenario == "host_eid_unsupported_base" ||
        activeScenario == "host_eid_fwup_request" ||
        activeScenario == "host_eid_fwup_send_fail" ||
        activeScenario == "host_eid_response_msg" ||
        activeScenario == "host_eid_fwup_response_msg" ||
        activeScenario == "host_eid_sensor_event" ||
        activeScenario == "host_eid_message_poll_event" ||
        activeScenario == "host_eid_cper_event" ||
        activeScenario == "host_eid_oem_cper_event" ||
        activeScenario == "host_eid_active_fw_version" ||
        activeScenario == "host_eid_smbios_event" ||
        activeScenario == "host_eid_inventory_json" ||
        activeScenario == "host_eid_telemetry_management" ||
        activeScenario == "host_eid_error_counter" ||
        activeScenario == "host_eid_pcie_telemetry" ||
        activeScenario == "host_eid_pcie_ltssm" ||
        activeScenario == "host_eid_request_name_fail")
    {
        CoverageHooks::setHostEid(8);
    }
    if (activeScenario == "request_name_fail" ||
        activeScenario == "host_eid_request_name_fail")
    {
        CoverageHooks::setForceRequestNameFailure();
    }
    else if (activeScenario == "pdr_init_fail")
    {
        CoverageHooks::setForcePdrInitFailure();
    }
    else if (activeScenario == "entity_init_fail")
    {
        CoverageHooks::setFailEntityInitCall(1);
    }
    else if (activeScenario == "bmc_entity_init_fail")
    {
        CoverageHooks::setFailEntityInitCall(2);
    }
    else if (activeScenario == "synthetic_no_readable")
    {
        CoverageHooks::setSyntheticEventLoop({makeSyntheticIoEvent(EPOLLOUT)});
    }
    else if (activeScenario == "synthetic_negative_fd")
    {
        CoverageHooks::setSyntheticEventLoop(
            {makeSyntheticIoEvent(EPOLLIN, -1)});
    }
    else if (activeScenario == "epollerr_no_input_fwup")
    {
        CoverageHooks::setReadMctpErrorQueueResult(0);
        CoverageHooks::setExtractedPldmType(PLDM_FWUP);
        CoverageHooks::setSyntheticEventLoop({makeSyntheticIoEvent(EPOLLERR)});
    }
    else if (activeScenario == "epollerr_no_input_non_fwup")
    {
        CoverageHooks::setReadMctpErrorQueueResult(0);
        CoverageHooks::setExtractedPldmType(PLDM_BASE);
        CoverageHooks::setSyntheticEventLoop({makeSyntheticIoEvent(EPOLLERR)});
    }
    else if (activeScenario == "epollerr_no_input_error")
    {
        CoverageHooks::setReadMctpErrorQueueResult(-EIO);
        CoverageHooks::setSyntheticEventLoop({makeSyntheticIoEvent(EPOLLERR)});
    }
    else if (activeScenario == "epollerr_no_input_positive")
    {
        CoverageHooks::setReadMctpErrorQueueResult(1);
        CoverageHooks::setSyntheticEventLoop({makeSyntheticIoEvent(EPOLLERR)});
    }
    else if (activeScenario == "epollerr_with_input_recv_fail")
    {
        CoverageHooks::setReadMctpErrorQueueResult(-EIO);
        CoverageHooks::setSyntheticEventLoop(
            {makeSyntheticIoEvent(EPOLLERR | EPOLLIN)});
    }
    else if (activeScenario == "epollerr_with_input_non_fwup")
    {
        CoverageHooks::setReadMctpErrorQueueResult(0);
        CoverageHooks::setExtractedPldmType(PLDM_BASE);
        CoverageHooks::setSyntheticEventLoop(
            {makeSyntheticIoEvent(EPOLLERR | EPOLLIN)});
    }
    else if (activeScenario == "epollerr_with_input_fwup_request")
    {
        CoverageHooks::setReadMctpErrorQueueResult(0);
        CoverageHooks::setExtractedPldmType(PLDM_FWUP);
        CoverageHooks::setSyntheticEventLoop(
            {makeSyntheticIoEvent(EPOLLERR | EPOLLIN)});
    }
    else if (activeScenario == "epollerr_with_input_fwup_send_fail")
    {
        CoverageHooks::setReadMctpErrorQueueResult(0);
        CoverageHooks::setExtractedPldmType(PLDM_FWUP);
        CoverageHooks::setSyntheticEventLoop(
            {makeSyntheticIoEvent(EPOLLERR | EPOLLIN)});
    }
    else if (activeScenario == "epollerr_negative_fd_with_input_error")
    {
        CoverageHooks::setReadMctpErrorQueueResult(-EIO);
        CoverageHooks::setSyntheticEventLoop(
            {makeSyntheticIoEvent(EPOLLERR | EPOLLIN, -1)});
    }
    else if (activeScenario == "epollerr_negative_fd_with_input_non_fwup")
    {
        CoverageHooks::setReadMctpErrorQueueResult(0);
        CoverageHooks::setExtractedPldmType(PLDM_BASE);
        CoverageHooks::setSyntheticEventLoop(
            {makeSyntheticIoEvent(EPOLLERR | EPOLLIN, -1)});
    }
    else if (activeScenario == "epollerr_negative_fd_with_input_positive")
    {
        CoverageHooks::setReadMctpErrorQueueResult(1);
        CoverageHooks::setSyntheticEventLoop(
            {makeSyntheticIoEvent(EPOLLERR | EPOLLIN, -1)});
    }
    else if (activeScenario == "epollerr_followed_by_hangup")
    {
        CoverageHooks::setReadMctpErrorQueueResult(0);
        CoverageHooks::setExtractedPldmType(PLDM_BASE);
        CoverageHooks::setSyntheticEventLoop(
            {makeSyntheticIoEvent(EPOLLERR | EPOLLHUP | EPOLLIN)});
    }
    else if (activeScenario == "synthetic_hangup")
    {
        CoverageHooks::setSyntheticEventLoop({makeSyntheticIoEvent(EPOLLHUP)});
    }
    else if (activeScenario == "synthetic_hangup_readable")
    {
        CoverageHooks::setSyntheticEventLoop(
            {makeSyntheticIoEvent(EPOLLHUP | EPOLLIN)});
    }
    else if (activeScenario == "event_loop_fail")
    {
        CoverageHooks::setSyntheticEventLoop({}, 1);
    }
}
// NOLINTEND(bugprone-branch-clone)

[[noreturn]] void terminateHandler()
{
    std::_Exit(EXIT_FAILURE);
}

} // namespace

extern "C" int __wrap_pldm_instance_db_init_default(
    struct pldm_instance_db** ctx)
{
    static uint64_t dbIndex = 0;
    static std::deque<std::string> dbPaths;
    auto root = pldm::test::ensureTempDir();
    dbPaths.emplace_back(
        (root / ("pldmd_main_helper_iid_" + std::to_string(::getpid()) + "_" +
                 std::to_string(dbIndex++)))
            .string());
    auto& dbPath = dbPaths.back();
    std::ofstream ofs(dbPath, std::ios::binary | std::ios::trunc);
    std::string data(256 * 32, '\0');
    ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
    return pldm_instance_db_init(ctx, dbPath.c_str());
}

struct CoverageHookInitializer
{
    CoverageHookInitializer()
    {
        configureCoverageHooksFromEnv();
        const auto activeScenario = getActiveScenario();
        if (activeScenario == "sigusr1_then_hangup")
        {
            launchSigusr1AndHangupThread();
        }
        if (activeScenario == "pdr_init_fail" ||
            activeScenario == "entity_init_fail" ||
            activeScenario == "bmc_entity_init_fail")
        {
            std::set_terminate(terminateHandler);
        }
    }
};

CoverageHookInitializer coverageHookInitializer{};

// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "pldmd/pldmd.cpp"

PldmTransport::PldmTransport([[maybe_unused]] bool listening) :
    PldmTransport(NoInit{})
{
    configureFakeTransportFromEnv();
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
        if (write(state.writeFd, &byte, sizeof(byte)) !=
            static_cast<ssize_t>(sizeof(byte)))
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

pldm_requester_rc_t PldmTransport::sendMsg(pldm_tid_t, const void*, size_t)
{
    auto& state = fakeTransportState();
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
    rx = nullptr;
    len = action.payload.size();

    if (action.rc == PLDM_REQUESTER_SUCCESS && !action.payload.empty())
    {
        rx = malloc(action.payload.size());
        if (rx == nullptr)
        {
            throw std::bad_alloc();
        }
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
