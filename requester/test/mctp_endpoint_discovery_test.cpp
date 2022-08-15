#include "config.h"

#include "common/utils.hpp"
#include "requester/test/mock_mctp_discovery_handler_intf.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::_;

TEST(MctpEndpointDiscoveryTest, SingleHandleMctpEndpoint)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    EXPECT_CALL(manager, handleMctpEndpoints(_)).Times(1);

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, *(static_cast<pldm::mctp_socket::Handler*>(nullptr)),
        std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager},
        "./static_eid_table.json");
}

TEST(MctpEndpointDiscoveryTest, MultipleHandleMctpEndpoints)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager1;
    pldm::MockManager manager2;

    EXPECT_CALL(manager1, handleMctpEndpoints(_)).Times(1);
    EXPECT_CALL(manager2, handleMctpEndpoints(_)).Times(1);

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, *(static_cast<pldm::mctp_socket::Handler*>(nullptr)),
        std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager1,
                                                               &manager2},
        "./static_eid_table.json");
}