#pragma once

#include "common/mctp_error_handling.hpp"

#include <dlfcn.h>
#include <libpldm/base.h>
#include <libpldm/pdr.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace pldm::test::coverage
{

struct SyntheticIoEvent
{
    int fdOverride = std::numeric_limits<int>::min();
    uint32_t revents = 0;
};

struct HookState
{
    bool forceUnpackFailure = false;
    bool forcePackFailure = false;
    bool forceGetTidEncodeFailure = false;
    bool forceGetPldmTypesEncodeFailure = false;
    bool forcePdrInitFailure = false;
    int failEntityInitCall = 0;
    int entityInitCallCount = 0;
    bool overrideHostEid = false;
    uint8_t hostEid = 0;
    bool forceRequestNameFailure = false;
    bool overrideReadMctpErrorQueue = false;
    int readMctpErrorQueueRc = 0;
    pldm::transport::MctpError readMctpError{};
    bool overrideExtractPldmType = false;
    uint8_t extractedPldmType = 0;
    bool syntheticLoopEnabled = false;
    std::vector<SyntheticIoEvent> syntheticIoEvents{};
    int syntheticLoopReturnCode = 0;
    sd_event_io_handler_t capturedIoHandler = nullptr;
    void* capturedIoUserdata = nullptr;
    sd_event_source* capturedIoSource = nullptr;
    int capturedIoFd = -1;
};

inline HookState& hookState()
{
    static HookState state{};
    return state;
}

inline void resetHooks()
{
    hookState() = HookState{};
}

struct ScopedHookStateReset
{
    ScopedHookStateReset()
    {
        resetHooks();
    }

    ~ScopedHookStateReset()
    {
        resetHooks();
    }
};

template <typename Fn>
inline Fn resolveNextSymbol(const char* symbol)
{
    auto* rawFn = dlsym(RTLD_NEXT, symbol);
    if (rawFn == nullptr)
    {
        throw std::runtime_error(
            std::string("Failed to resolve symbol: ") + symbol);
    }
    return reinterpret_cast<Fn>(rawFn);
}

inline void setForceUnpackFailure(bool value = true)
{
    hookState().forceUnpackFailure = value;
}

inline void setForcePackFailure(bool value = true)
{
    hookState().forcePackFailure = value;
}

inline void setForceGetTidEncodeFailure(bool value = true)
{
    hookState().forceGetTidEncodeFailure = value;
}

inline void setForceGetPldmTypesEncodeFailure(bool value = true)
{
    hookState().forceGetPldmTypesEncodeFailure = value;
}

inline void setForcePdrInitFailure(bool value = true)
{
    hookState().forcePdrInitFailure = value;
}

inline void setFailEntityInitCall(int callNumber)
{
    auto& state = hookState();
    state.failEntityInitCall = callNumber;
    state.entityInitCallCount = 0;
}

inline void setHostEid(uint8_t eid)
{
    auto& state = hookState();
    state.overrideHostEid = true;
    state.hostEid = eid;
}

inline void setForceRequestNameFailure(bool value = true)
{
    hookState().forceRequestNameFailure = value;
}

inline void setReadMctpErrorQueueResult(
    int rc, const pldm::transport::MctpError& error = {})
{
    auto& state = hookState();
    state.overrideReadMctpErrorQueue = true;
    state.readMctpErrorQueueRc = rc;
    state.readMctpError = error;
}

inline void setExtractedPldmType(uint8_t pldmType)
{
    auto& state = hookState();
    state.overrideExtractPldmType = true;
    state.extractedPldmType = pldmType;
}

inline void setSyntheticEventLoop(const std::vector<SyntheticIoEvent>& events,
                                  int loopReturnCode = 0)
{
    auto& state = hookState();
    state.syntheticLoopEnabled = true;
    state.syntheticIoEvents = events;
    state.syntheticLoopReturnCode = loopReturnCode;
    state.capturedIoHandler = nullptr;
    state.capturedIoUserdata = nullptr;
    state.capturedIoSource = nullptr;
    state.capturedIoFd = -1;
}

} // namespace pldm::test::coverage

extern "C" uint8_t unpack_pldm_header(const struct pldm_msg_hdr* msg,
                                      struct pldm_header_info* hdr)
{
    auto& state = pldm::test::coverage::hookState();
    if (state.forceUnpackFailure)
    {
        return PLDM_ERROR_INVALID_DATA;
    }

    using Fn =
        uint8_t (*)(const struct pldm_msg_hdr*, struct pldm_header_info*);
    static auto realFn =
        pldm::test::coverage::resolveNextSymbol<Fn>("unpack_pldm_header");
    return realFn(msg, hdr);
}

extern "C" uint8_t pack_pldm_header(const struct pldm_header_info* header,
                                    struct pldm_msg_hdr* msg)
{
    auto& state = pldm::test::coverage::hookState();
    if (state.forcePackFailure)
    {
        return PLDM_ERROR_INVALID_DATA;
    }

    using Fn =
        uint8_t (*)(const struct pldm_header_info*, struct pldm_msg_hdr*);
    static auto realFn =
        pldm::test::coverage::resolveNextSymbol<Fn>("pack_pldm_header");
    return realFn(header, msg);
}

extern "C" int pack_pldm_header_errno(const struct pldm_header_info* header,
                                      struct pldm_msg_hdr* msg)
{
    auto& state = pldm::test::coverage::hookState();
    if (state.forcePackFailure)
    {
        return -EINVAL;
    }

    using Fn = int (*)(const struct pldm_header_info*, struct pldm_msg_hdr*);
    static auto realFn =
        pldm::test::coverage::resolveNextSymbol<Fn>("pack_pldm_header_errno");
    return realFn(header, msg);
}

extern "C" int encode_pldm_base_get_tid_resp(
    uint8_t instance_id, const struct pldm_base_get_tid_resp* resp,
    struct pldm_msg* msg, size_t* payload_length)
{
    auto& state = pldm::test::coverage::hookState();
    if (state.forceGetTidEncodeFailure)
    {
        return -EINVAL;
    }

    using Fn = int (*)(uint8_t, const struct pldm_base_get_tid_resp*,
                       struct pldm_msg*, size_t*);
    static auto realFn = pldm::test::coverage::resolveNextSymbol<Fn>(
        "encode_pldm_base_get_tid_resp");
    return realFn(instance_id, resp, msg, payload_length);
}

extern "C" int encode_pldm_base_get_pldm_types_resp(
    uint8_t instance_id, const struct pldm_base_get_pldm_types_resp* resp,
    struct pldm_msg* msg, size_t* payload_length)
{
    auto& state = pldm::test::coverage::hookState();
    if (state.forceGetPldmTypesEncodeFailure)
    {
        return -EINVAL;
    }

    using Fn = int (*)(uint8_t, const struct pldm_base_get_pldm_types_resp*,
                       struct pldm_msg*, size_t*);
    static auto realFn = pldm::test::coverage::resolveNextSymbol<Fn>(
        "encode_pldm_base_get_pldm_types_resp");
    return realFn(instance_id, resp, msg, payload_length);
}

extern "C" pldm_pdr* pldm_pdr_init()
{
    auto& state = pldm::test::coverage::hookState();
    if (state.forcePdrInitFailure)
    {
        return nullptr;
    }

    using Fn = pldm_pdr* (*)();
    static auto realFn =
        pldm::test::coverage::resolveNextSymbol<Fn>("pldm_pdr_init");
    return realFn();
}

extern "C" pldm_entity_association_tree* pldm_entity_association_tree_init()
{
    auto& state = pldm::test::coverage::hookState();
    ++state.entityInitCallCount;
    if (state.failEntityInitCall > 0 &&
        state.entityInitCallCount == state.failEntityInitCall)
    {
        return nullptr;
    }

    using Fn = pldm_entity_association_tree* (*)();
    static auto realFn = pldm::test::coverage::resolveNextSymbol<Fn>(
        "pldm_entity_association_tree_init");
    return realFn();
}

extern "C" int sd_event_add_io(sd_event* event, sd_event_source** source,
                               int fd, uint32_t events,
                               sd_event_io_handler_t callback, void* userdata)
{
    using Fn = int (*)(sd_event*, sd_event_source**, int, uint32_t,
                       sd_event_io_handler_t, void*);
    static auto realFn =
        pldm::test::coverage::resolveNextSymbol<Fn>("sd_event_add_io");

    auto rc = realFn(event, source, fd, events, callback, userdata);
    auto& state = pldm::test::coverage::hookState();
    if (state.syntheticLoopEnabled && rc >= 0)
    {
        state.capturedIoHandler = callback;
        state.capturedIoUserdata = userdata;
        state.capturedIoSource = (source != nullptr) ? *source : nullptr;
        state.capturedIoFd = fd;
    }
    return rc;
}

// NOLINTNEXTLINE(misc-include-cleaner)
extern "C" int sd_bus_request_name(sd_bus* bus, const char* name,
                                   uint64_t flags)
{
    auto& state = pldm::test::coverage::hookState();
    if (state.forceRequestNameFailure)
    {
        return -EEXIST;
    }

    using Fn = int (*)(sd_bus*, const char*, uint64_t);
    static auto realFn =
        pldm::test::coverage::resolveNextSymbol<Fn>("sd_bus_request_name");
    return realFn(bus, name, flags);
}

extern "C" int sd_event_loop(sd_event* event)
{
    using Fn = int (*)(sd_event*);
    static auto realFn =
        pldm::test::coverage::resolveNextSymbol<Fn>("sd_event_loop");

    auto& state = pldm::test::coverage::hookState();
    if (!state.syntheticLoopEnabled)
    {
        return realFn(event);
    }

    if (state.capturedIoHandler != nullptr)
    {
        for (const auto& syntheticEvent : state.syntheticIoEvents)
        {
            auto fd = syntheticEvent.fdOverride;
            if (fd == std::numeric_limits<int>::min())
            {
                fd = state.capturedIoFd;
            }
            state.capturedIoHandler(state.capturedIoSource, fd,
                                    syntheticEvent.revents,
                                    state.capturedIoUserdata);
        }
    }

    return state.syntheticLoopReturnCode;
}

namespace pldm::utils
{

uint8_t readHostEID()
{
    auto& state = pldm::test::coverage::hookState();
    if (state.overrideHostEid)
    {
        return state.hostEid;
    }

    using Fn = uint8_t (*)();
    static auto realFn = pldm::test::coverage::resolveNextSymbol<Fn>(
        "_ZN4pldm5utils11readHostEIDEv");
    return realFn();
}

} // namespace pldm::utils

namespace pldm::transport
{

int readMctpErrorQueue(int fd, MctpError& error)
{
    auto& state = pldm::test::coverage::hookState();
    if (state.overrideReadMctpErrorQueue)
    {
        error = state.readMctpError;
        return state.readMctpErrorQueueRc;
    }

    using Fn = int (*)(int, MctpError&);
    static auto realFn = pldm::test::coverage::resolveNextSymbol<Fn>(
        "_ZN4pldm9transport18readMctpErrorQueueEiR10mctp_error");
    return realFn(fd, error);
}

uint8_t extractPldmType(const MctpError& error)
{
    auto& state = pldm::test::coverage::hookState();
    if (state.overrideExtractPldmType)
    {
        return state.extractedPldmType;
    }

    using Fn = uint8_t (*)(const MctpError&);
    static auto realFn = pldm::test::coverage::resolveNextSymbol<Fn>(
        "_ZN4pldm9transport15extractPldmTypeERK10mctp_error");
    return realFn(error);
}

} // namespace pldm::transport
