

#include "common/utils.hpp"
#include "fw-update/component_updater.hpp"
#include "fw-update/config.hpp"
#include "fw-update/device_inventory.hpp"
#include "fw-update/device_updater.hpp"
#include "fw-update/firmware_inventory.hpp"
#include "fw-update/inventory_manager.hpp"
#include "fw-update/manager.hpp"
#include "fw-update/other_device_update_manager.hpp"
#include "fw-update/package_parser.hpp"
#include "fw-update/package_signature.hpp"
#include "fw-update/update_manager.hpp"
#include "fw-update/watch.hpp"
#include "pldmd/invoker.hpp"
#include "requester/handler.hpp"
#include "requester/test/mock_mctp_discovery_handler_intf.hpp"
#include "requester/test/mock_request.hpp"
#include "test/test_instance_id.hpp"

#include <sdeventplus/event.hpp>

#include <chrono>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::_;
using namespace std::chrono;

TEST(MctpEndpointDiscoveryTest, SingleHandleMctpEndpoint)
{
    auto& bus = pldm::utils::DBusHandler::getBus();

    pldm::MockManager manager;
    pldm::responder::Invoker invoker;
    EXPECT_CALL(manager, handleMctpEndpoints(_)).Times(1);

    sdeventplus::Event event(sdeventplus::Event::get_default());
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler(
        nullptr, event, instanceIdDb, false, seconds(1), 2, milliseconds(100));
    pldm::fw_update::Manager fwUpdateManager(event, reqHandler, instanceIdDb,
                                             "", nullptr, false);

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});
    mctpDiscoveryHandler = nullptr;
}

TEST(MctpEndpointDiscoveryTest, MultipleHandleMctpEndpoints)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager1;
    pldm::MockManager manager2;

    EXPECT_CALL(manager1, handleMctpEndpoints(_)).Times(1);
    EXPECT_CALL(manager2, handleMctpEndpoints(_)).Times(1);

    pldm::responder::Invoker invoker;
    TestInstanceIdDb instanceIdDb;
    sdeventplus::Event event(sdeventplus::Event::get_default());
    pldm::requester::Handler<pldm::requester::Request> reqHandler(
        nullptr, event, instanceIdDb, false, seconds(1), 2, milliseconds(100));
    pldm::fw_update::Manager fwUpdateManager(event, reqHandler, instanceIdDb,
                                             "", nullptr, false);

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus,
        std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager1,
                                                               &manager2},
        "./static_eid_table.json");
}