#pragma once

#include "requester/request.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace pldm
{

namespace requester
{

class MockRequest : public RequestRetryTimer
{
  public:
    MockRequest(int /*fd*/, mctp_eid_t /*eid*/, sdeventplus::Event& event,
                const pldm::mctp_socket::Handler* /* handler */,
                pldm::Request&& /*requestMsg*/, uint8_t numRetries,
                std::chrono::milliseconds responseTimeOut, bool /*verbose*/) :
        RequestRetryTimer(event, numRetries, responseTimeOut)
    {}

    MOCK_METHOD(int, send, (), (const, override));
};

} // namespace requester

} // namespace pldm
