#include "config.h"

#include "libpldm/firmware_update.h"

#include "common/test/mocked_utils.hpp"
#include "common/utils.hpp"
#include "fw-update/manager.hpp"
#include "pldmd/invoker.hpp"
#include "requester/test/mock_mctp_discovery_handler_intf.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::_;
using namespace pldm;
using namespace std::chrono;
using namespace pldm::fw_update;
class MctpEndpointDiscoveryTest : public testing::Test
{
  protected:
    MctpEndpointDiscoveryTest() :
        event(sdeventplus::Event::get_default()),
        dbusImplRequester(pldm::utils::DBusHandler::getBus(),
                          "/xyz/openbmc_project/pldm"),
        reqHandler(event, dbusImplRequester, sockManager, false, seconds(1), 2,
                   milliseconds(100)),
        fwManager(event, reqHandler, dbusImplRequester, FW_UPDATE_CONFIG_JSON,
                  &dbusHandler, false),
        sockHandler(event, reqHandler, invoker, fwManager, sockManager, false),
        inventoryManager(reqHandler, dbusImplRequester, nullptr,
                         outDescriptorMap, outComponentInfoMap)
    {}

    sdeventplus::Event event;
    pldm::dbus_api::Requester dbusImplRequester;
    MockdBusHandler dbusHandler;
    pldm::mctp_socket::Manager sockManager;
    requester::Handler<requester::Request> reqHandler;
    fw_update::Manager fwManager;
    pldm::responder::Invoker invoker{};
    pldm::mctp_socket::Handler sockHandler;
    InventoryManager inventoryManager;
    DescriptorMap outDescriptorMap{};
    ComponentInfoMap outComponentInfoMap{};
};

TEST_F(MctpEndpointDiscoveryTest, SinglehandleMCTPEndpoint)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    EXPECT_CALL(manager, handleMCTPEndpoints(_)).Times(1);

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, sockHandler,
        std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager},
        "./static_eid_table.json");
}

TEST_F(MctpEndpointDiscoveryTest, MultipleHandleMCTPEndpoints)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager1;
    pldm::MockManager manager2;

    EXPECT_CALL(manager1, handleMCTPEndpoints(_)).Times(1);
    EXPECT_CALL(manager2, handleMCTPEndpoints(_)).Times(1);

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, sockHandler,
        std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager1,
                                                               &manager2},
        "./static_eid_table.json");
}