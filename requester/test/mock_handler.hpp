#pragma once

#include "common/types.hpp"
#include "common/utils.hpp"
#include "pldmd/socket_handler.hpp"
#include "requester/handler.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

class MockMctpSocketHandler : public pldm::mctp_socket::Handler
{
  public:
    MockMctpSocketHandler(
        sdeventplus::Event& event,
        pldm::requester::Handler<pldm::requester::Request>& handler,
        pldm::responder::Invoker& invoker, pldm::fw_update::Manager& fwManager,
        pldm::mctp_socket::Manager& manager, bool verbose) :
        Handler(event, handler, invoker, fwManager, manager, verbose)
    {}

    MOCK_METHOD(int, registerMctpEndpoint,
                (pldm::EID, int, int, const std::vector<uint8_t>&), (override));
    MOCK_METHOD(int, sendMsg, (pldm::EID, int, const uint8_t*, size_t),
                (const, override));
    MOCK_METHOD(void, handleReceivedMsg,
                (sdeventplus::source::IO&, int, uint32_t), (override));
};
