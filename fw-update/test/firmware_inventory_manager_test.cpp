#include "common/test/mocked_utils.hpp"
#include "common/types.hpp"
#include "common/utils.hpp"
#include "fw-update/aggregate_update_manager.hpp"
#include "fw-update/firmware_inventory.hpp"
#include "fw-update/firmware_inventory_manager.hpp"
#include "requester/handler.hpp"
#include "requester/request.hpp"
#include "test/test_instance_id.hpp"

#include <sdeventplus/event.hpp>
#include <xyz/openbmc_project/Common/error.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <tuple>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pldm;
using namespace std::chrono;
using namespace pldm::fw_update;

// Helper class for testing: inherits FirmwareInventory and exposes protected
class FirmwareInventoryTestInstance : public pldm::fw_update::FirmwareInventory
{
  public:
    using FirmwareInventory::FirmwareInventory;

    const std::string& getSoftwarePath() const
    {
        return this->softwarePath;
    }
    const SoftwareAssociationDefinitions& getAssociation() const
    {
        return this->association;
    }
    const SoftwareVersion& getVersion() const
    {
        return this->version;
    }
};

class FirmwareInventoryManagerTest : public FirmwareInventoryManager
{
  public:
    FirmwareInventoryManagerTest(const pldm::utils::DBusHandler* handler,
                                 const Configurations& config,
                                 AggregateUpdateManager& updateManager) :
        FirmwareInventoryManager(handler, config, updateManager)
    {}

    SoftwareMap& getSoftwareMap()
    {
        return softwareMap;
    }
};

TEST(GetBoardPath_WithMockHandler, ReturnsExpectedBoardPath)
{
    MockdBusHandler mockHandler;
    InventoryPath inventoryPath =
        "/xyz/openbmc_project/inventory/system/board/PLDM_Device";
    pldm::utils::GetAncestorsResponse fakeResponse = {{inventoryPath, {}}};
    EXPECT_CALL(mockHandler, getAncestors)
        .WillOnce(::testing::Return(fakeResponse));

    Configurations configurations;
    std::string boardInventoryPath =
        "/xyz/openbmc_project/inventory/system/board/PLDM_Device";
    pldm::eid endpointId = 1;
    pldm::UUID endpointUuid = "uuid";
    pldm::MctpMedium endpointMedium = "medium";
    pldm::NetworkId endpointNetId = 0;
    pldm::MctpInfoName endpointName = "BMC";
    pldm::MctpBinding endpointBinding = "binding";
    pldm::LocalEid endpointLocalEid = std::nullopt;
    pldm::MctpInfo endpointInfo =
        std::make_tuple(endpointId, endpointUuid, endpointMedium, endpointNetId,
                        endpointName, endpointBinding, endpointLocalEid);
    configurations[boardInventoryPath] = endpointInfo;

    Event event(sdeventplus::Event::get_default());
    TestInstanceIdDb instanceIdDb;
    requester::Handler<requester::Request> handler(
        nullptr, event, instanceIdDb, false, seconds(1), 2, milliseconds(100));

    DescriptorMap descriptorMap{};
    ComponentInfoMap componentInfoMap{};

    AggregateUpdateManager updateManager(event, handler, instanceIdDb,
                                         descriptorMap, componentInfoMap);

    FirmwareInventoryManagerTest inventoryManager(&mockHandler, configurations,
                                                  updateManager);

    SoftwareIdentifier softwareIdentifier{endpointId, 100};
    SoftwareName softwareName{"TestDevice"};
    std::string firmwareVersion{"1.0.0"};
    Descriptors firmwareDescriptors;
    ComponentInfo firmwareComponentInfo;

    inventoryManager.createFirmwareEntry(
        softwareIdentifier, softwareName, firmwareVersion, firmwareDescriptors,
        firmwareComponentInfo);
    ASSERT_TRUE(inventoryManager.getSoftwareMap().contains(softwareIdentifier));

    auto inventoryIt =
        inventoryManager.getSoftwareMap().find(softwareIdentifier);
    ASSERT_NE(inventoryIt, inventoryManager.getSoftwareMap().end());
    const auto* inventory =
        static_cast<FirmwareInventoryTestInstance*>(inventoryIt->second.get());
    ASSERT_NE(inventory, nullptr);
    EXPECT_NE(inventory->getSoftwarePath().find(
                  "/xyz/openbmc_project/software/PLDM_Device_TestDevice_"),
              std::string::npos);
    EXPECT_EQ(inventory->getVersion().version(), firmwareVersion);
}

// Fixture providing the boilerplate needed to drive FirmwareInventoryManager.
class FirmwareInventoryManagerCoverageTest : public ::testing::Test
{
  protected:
    void addConfig(const std::string& path, pldm::eid id)
    {
        pldm::MctpInfo info = std::make_tuple(
            id, pldm::UUID{"uuid"}, pldm::MctpMedium{"medium"},
            pldm::NetworkId{0}, pldm::MctpInfoName{"BMC"},
            pldm::MctpBinding{"binding"}, pldm::LocalEid{std::nullopt});
        configurations[path] = info;
    }

    MockdBusHandler mockHandler;
    Configurations configurations;
    Event event{sdeventplus::Event::get_default()};
    TestInstanceIdDb instanceIdDb;
    requester::Handler<requester::Request> handler{
        nullptr, event, instanceIdDb, false, seconds(1), 2, milliseconds(100)};
    DescriptorMap descriptorMap{};
    ComponentInfoMap componentInfoMap{};
    AggregateUpdateManager updateManager{event, handler, instanceIdDb,
                                         descriptorMap, componentInfoMap};
};

// deleteFirmwareEntry() removes every software object for an EID.
TEST_F(FirmwareInventoryManagerCoverageTest, DeleteFirmwareEntryRemovesEntries)
{
    const std::string boardPath =
        "/xyz/openbmc_project/inventory/system/board/PLDM_Device";
    pldm::eid endpointId = 1;
    addConfig(boardPath, endpointId);

    pldm::utils::GetAncestorsResponse fakeResponse = {{boardPath, {}}};
    EXPECT_CALL(mockHandler, getAncestors)
        .WillOnce(::testing::Return(fakeResponse));

    FirmwareInventoryManagerTest mgr(&mockHandler, configurations,
                                     updateManager);
    SoftwareIdentifier id{endpointId, 100};
    mgr.createFirmwareEntry(id, SoftwareName{"TestDevice"}, "1.0.0",
                            Descriptors{}, ComponentInfo{});
    ASSERT_TRUE(mgr.getSoftwareMap().contains(id));

    mgr.deleteFirmwareEntry(endpointId);
    EXPECT_FALSE(mgr.getSoftwareMap().contains(id));
    EXPECT_TRUE(mgr.getSoftwareMap().empty());
}

// No inventory path for the EID: createFirmwareEntry() falls back to the
// default placeholder board path and never queries ancestors.
TEST_F(FirmwareInventoryManagerCoverageTest,
       CreateFirmwareEntryUsesDefaultPathWhenNoInventoryPath)
{
    // configurations left empty -> getInventoryPath() returns nullopt.
    EXPECT_CALL(mockHandler, getAncestors).Times(0);

    FirmwareInventoryManagerTest mgr(&mockHandler, configurations,
                                     updateManager);
    SoftwareIdentifier id{99, 100};
    mgr.createFirmwareEntry(id, SoftwareName{"NoConfigDev"}, "1.0.0",
                            Descriptors{}, ComponentInfo{});
    ASSERT_TRUE(mgr.getSoftwareMap().contains(id));

    const auto* inventory = static_cast<FirmwareInventoryTestInstance*>(
        mgr.getSoftwareMap().find(id)->second.get());
    ASSERT_NE(inventory, nullptr);
    EXPECT_NE(inventory->getSoftwarePath().find(
                  "/xyz/openbmc_project/software/PLDM_Device_NoConfigDev_"),
              std::string::npos);
}

// getBoardPath() returns nullopt when the ObjectMapper query throws.
TEST_F(FirmwareInventoryManagerCoverageTest, GetBoardPathReturnsNulloptOnThrow)
{
    EXPECT_CALL(mockHandler, getAncestors)
        .WillOnce(::testing::Throw(
            sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure{}));

    auto result = pldm::fw_update::getBoardPath(
        mockHandler, InventoryPath{"/xyz/openbmc_project/inventory/foo"});
    EXPECT_FALSE(result.has_value());
}

// getBoardPath() returns nullopt when the ObjectMapper query is empty.
TEST_F(FirmwareInventoryManagerCoverageTest, GetBoardPathReturnsNulloptOnEmpty)
{
    EXPECT_CALL(mockHandler, getAncestors)
        .WillOnce(::testing::Return(pldm::utils::GetAncestorsResponse{}));

    auto result = pldm::fw_update::getBoardPath(
        mockHandler, InventoryPath{"/xyz/openbmc_project/inventory/foo"});
    EXPECT_FALSE(result.has_value());
}
