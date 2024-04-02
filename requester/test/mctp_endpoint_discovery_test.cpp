#include "config.h"

#include "requester/handler.hpp"
#include "common/utils.hpp"
#include "pldmd/socket_manager.hpp"
#include "requester/test/mock_mctp_discovery_handler_intf.hpp"
#include "pldmd/dbus_impl_requester.hpp"

#include "fw-update/manager.hpp"
#include "fw-update/component_updater.hpp"
#include "fw-update/device_updater.hpp"
#include "fw-update/other_device_update_manager.hpp"
#include "fw-update/update_manager.hpp"
#include "fw-update/config.hpp"
#include "fw-update/firmware_inventory.hpp"
#include "fw-update/package_parser.hpp"
#include "fw-update/watch.hpp"
#include "fw-update/device_inventory.hpp"
#include "fw-update/inventory_manager.hpp"
#include "fw-update/package_signature.hpp"

#include <sdeventplus/event.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::_;

TEST(MctpEndpointDiscoveryTest, SingleHandleMctpEndpoint)
{
    auto& bus = pldm::utils::DBusHandler::getBus();

    pldm::MockManager manager;
    pldm::responder::Invoker invoker;
    pldm::mctp_socket::Manager sockManager;
    EXPECT_CALL(manager, handleMctpEndpoints(_,_)).Times(1);

    sdeventplus::Event event(sdeventplus::Event::get_default());
    pldm::dbus_api::Requester requester(pldm::utils::DBusHandler::getBus(), "/xyz/openbmc_project/pldm");
    pldm::requester::Handler<pldm::requester::Request> reqHandler(event, requester, sockManager, false, std::chrono::seconds(1), 2,
               std::chrono::milliseconds(100));
    pldm::fw_update::Manager fwUpdateManager(event, reqHandler, requester, "", nullptr, false);
    pldm::mctp_socket::Handler handler(event,
                    reqHandler, invoker, fwUpdateManager, sockManager, false);


    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, handler,
        std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager},
        "./static_eid_table.json");
}

TEST(MctpEndpointDiscoveryTest, MultipleHandleMctpEndpoints)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager1;
    pldm::MockManager manager2;

    EXPECT_CALL(manager1, handleMctpEndpoints(_,_)).Times(1);
    EXPECT_CALL(manager2, handleMctpEndpoints(_,_)).Times(1);

    pldm::mctp_socket::Manager sockManager;
    pldm::responder::Invoker invoker;
    sdeventplus::Event event(sdeventplus::Event::get_default());
    pldm::dbus_api::Requester requester(pldm::utils::DBusHandler::getBus(),
                    "/xyz/openbmc_project/pldm");
    pldm::requester::Handler<pldm::requester::Request> reqHandler(event, requester, sockManager, false, std::chrono::seconds(1), 2,
               std::chrono::milliseconds(100));
    pldm::fw_update::Manager fwUpdateManager(event, reqHandler, requester, "", nullptr, false);
    pldm::mctp_socket::Handler handler(event,
                    reqHandler, invoker, fwUpdateManager, sockManager, false);

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, handler,
        std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager1,
                                                               &manager2},
        "./static_eid_table.json");
}