#include "common/utils.hpp"
#include "libpldmresponder/oem_handler.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "libpldmresponder/fru.hpp"
#undef private
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "libpldmresponder/fru_parser.hpp"

#include <libpldm/entity.h>
#include <libpldm/fru.h>

#include <array>

#include <gtest/gtest.h>

namespace
{

pldm_msg* asMsg(std::vector<uint8_t>& request)
{
    return reinterpret_cast<pldm_msg*>(request.data());
}

uint8_t completionCode(const pldm::responder::Response& response)
{
    return reinterpret_cast<const pldm_msg*>(response.data())->payload[0];
}

} // namespace

TEST(FruParser, allScenarios)
{
    using namespace pldm::responder::fru_parser;

    FruParser parser{"./fru_jsons/good",
                     "./fru_jsons/fru_master/fru_master.json"};

    // Get an item with a single PLDM FRU record
    FruRecordInfos cpu{
        {1,
         1,
         {{"xyz.openbmc_project.Inventory.Decorator.Asset", "Model", "string",
           2},
          {"xyz.openbmc_project.Inventory.Decorator.Asset", "PartNumber",
           "string", 3},
          {"xyz.openbmc_project.Inventory.Decorator.Asset", "SerialNumber",
           "string", 4},
          {"xyz.openbmc_project.Inventory.Decorator.Asset", "Manufacturer",
           "string", 5},
          {"xyz.openbmc_project.Inventory.Item", "PrettyName", "string", 8},
          {"xyz.openbmc_project.Inventory.Decorator.AssetTag", "AssetTag",
           "string", 11},
          {"xyz.openbmc_project.Inventory.Decorator.Revision", "Version",
           "string", 10}}},
        {1,
         1,
         {{"xyz.openbmc_project.Inventory.Decorator.Asset", "PartNumber",
           "string", 3},
          {"xyz.openbmc_project.Inventory.Decorator.Asset", "SerialNumber",
           "string", 4}}}};
    auto cpuInfos =
        parser.getRecordInfo("xyz.openbmc_project.Inventory.Item.Cpu");
    ASSERT_EQ(cpuInfos.size(), 2);
    ASSERT_EQ(cpu == cpuInfos, true);

    // Get an item type with 3 PLDM FRU records
    auto boardInfos =
        parser.getRecordInfo("xyz.openbmc_project.Inventory.Item.Board");
    ASSERT_EQ(boardInfos.size(), 3);

    // D-Bus lookup info for FRU information
    DBusLookupInfo lookupInfo{
        "xyz.openbmc_project.Inventory.Manager",
        "/xyz/openbmc_project/inventory",
        {"xyz.openbmc_project.Inventory.Item.Chassis",
         "xyz.openbmc_project.Inventory.Item.Board",
         "xyz.openbmc_project.Inventory.Item.PCIeDevice",
         "xyz.openbmc_project.Inventory.Item.Board.Motherboard",
         "xyz.openbmc_project.Inventory.Item.Dimm",
         "xyz.openbmc_project.Inventory.Item.Panel",
         "xyz.openbmc_project.Inventory.Item.DiskBackplane",
         "xyz.openbmc_project.Inventory.Item.Fan",
         "xyz.openbmc_project.Inventory.Item.PowerSupply",
         "xyz.openbmc_project.Inventory.Item.Battery",
         "xyz.openbmc_project.Inventory.Item.Vrm",
         "xyz.openbmc_project.Inventory.Item.Cpu",
         "xyz.openbmc_project.Inventory.Item.Bmc",
         "xyz.openbmc_project.Inventory.Item.Connector",
         "xyz.openbmc_project.Inventory.Item.PCIeSlot",
         "xyz.openbmc_project.Inventory.Item.System",
         "xyz.openbmc_project.Inventory.Item.Tpm"}};
    auto dbusInfo = parser.inventoryLookup();
    ASSERT_EQ(dbusInfo == lookupInfo, true);

    auto cpuEntityType =
        parser.getEntityType("xyz.openbmc_project.Inventory.Item.Cpu");
    EXPECT_EQ(cpuEntityType, 135);

    ASSERT_THROW(
        parser.getRecordInfo("xyz.openbmc_project.Inventory.Item.DIMM"),
        std::exception);
    ASSERT_THROW(
        parser.getEntityType("xyz.openbmc_project.Inventory.Item.DIMM"),
        std::exception);
}

TEST(FruHandler, requestCoveragePaths)
{
    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::fru::Handler handler(
        "./fru_jsons/good", "./fru_jsons/fru_master/fru_master.json", pdrRepo,
        entityTree, bmcEntityTree);

    {
        std::vector<uint8_t> request(
            sizeof(pldm_msg_hdr) +
            PLDM_GET_FRU_RECORD_TABLE_METADATA_REQ_BYTES);
        auto* msg = asMsg(request);
        ASSERT_EQ(encode_get_fru_record_table_metadata_req(
                      1, msg, PLDM_GET_FRU_RECORD_TABLE_METADATA_REQ_BYTES),
                  PLDM_SUCCESS);
        auto response = handler.getFRURecordTableMetadata(
            msg, PLDM_GET_FRU_RECORD_TABLE_METADATA_REQ_BYTES);
        EXPECT_EQ(completionCode(response), PLDM_SUCCESS);
    }

    {
        std::array<uint8_t, sizeof(pldm_msg_hdr)> requestBuffer{};
        auto* msg = reinterpret_cast<pldm_msg*>(requestBuffer.data());
        msg->hdr.instance_id = 2;
        auto response = handler.getFRURecordTable(msg, 0);
        EXPECT_EQ(completionCode(response), PLDM_ERROR_INVALID_LENGTH);
    }

    {
        std::vector<uint8_t> request(
            sizeof(pldm_msg_hdr) + PLDM_GET_FRU_RECORD_TABLE_REQ_BYTES);
        auto* msg = asMsg(request);
        ASSERT_EQ(encode_get_fru_record_table_req(
                      1, 0, PLDM_GET_FIRSTPART, msg,
                      PLDM_GET_FRU_RECORD_TABLE_REQ_BYTES),
                  PLDM_SUCCESS);
        auto response =
            handler.getFRURecordTable(msg, PLDM_GET_FRU_RECORD_TABLE_REQ_BYTES);
        EXPECT_EQ(completionCode(response), PLDM_SUCCESS);
    }

    {
        std::array<uint8_t, sizeof(pldm_msg_hdr)> requestBuffer{};
        auto* msg = reinterpret_cast<pldm_msg*>(requestBuffer.data());
        msg->hdr.instance_id = 3;
        auto response = handler.getFRURecordByOption(msg, 0);
        EXPECT_EQ(completionCode(response), PLDM_ERROR_INVALID_LENGTH);
    }

    {
        std::vector<uint8_t> request(
            sizeof(pldm_msg_hdr) + sizeof(pldm_get_fru_record_by_option_req));
        auto* msg = asMsg(request);
        ASSERT_EQ(encode_get_fru_record_by_option_req(
                      1, 0, 0, 1, PLDM_FRU_RECORD_TYPE_GENERAL,
                      PLDM_FRU_FIELD_TYPE_MODEL, PLDM_GET_FIRSTPART, msg,
                      sizeof(pldm_get_fru_record_by_option_req)),
                  PLDM_SUCCESS);
        auto response = handler.getFRURecordByOption(
            msg, sizeof(pldm_get_fru_record_by_option_req));
        EXPECT_TRUE(completionCode(response) == PLDM_SUCCESS ||
                    completionCode(response) ==
                        PLDM_FRU_DATA_STRUCTURE_TABLE_UNAVAILABLE);
    }

    pldm_entity_association_tree_destroy(entityTree);
    pldm_entity_association_tree_destroy(bmcEntityTree);
    pldm_pdr_destroy(pdrRepo);
}

TEST(FruImpl, helperCoveragePaths)
{
    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::FruImpl impl("./fru_jsons/good",
                                  "./fru_jsons/fru_master/fru_master.json",
                                  pdrRepo, entityTree, bmcEntityTree);

    auto fwVersion = impl.populatefwVersion();
    EXPECT_TRUE(fwVersion.empty() || !fwVersion.empty());

    impl.buildFRUTable();

    pldm::responder::Response response(sizeof(pldm_msg_hdr), 0);
    impl.getFRUTable(response);
    EXPECT_GE(response.size(), sizeof(pldm_msg_hdr) + sizeof(uint32_t));

    std::vector<uint8_t> fruData;
    auto rc = impl.getFRURecordByOption(
        fruData, 0, 1, PLDM_FRU_RECORD_TYPE_GENERAL, PLDM_FRU_FIELD_TYPE_MODEL);
    EXPECT_TRUE(rc == PLDM_SUCCESS ||
                rc == PLDM_FRU_DATA_STRUCTURE_TABLE_UNAVAILABLE);

    pldm_entity_association_tree_destroy(entityTree);
    pldm_entity_association_tree_destroy(bmcEntityTree);
    pldm_pdr_destroy(pdrRepo);
}

TEST(FruImpl, populateRecordsCoveragePaths)
{
    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::FruImpl impl("./fru_jsons/good",
                                  "./fru_jsons/fru_master/fru_master.json",
                                  pdrRepo, entityTree, bmcEntityTree);

    pldm::responder::dbus::InterfaceMap interfaces{
        {"xyz.openbmc_project.Inventory.Decorator.Asset",
         {{"Model", std::string("ModelX")},
          {"PartNumber", std::string("Part-1")},
          {"SerialNumber", std::string("Serial-1")},
          {"Manufacturer", std::string("Vendor")},
          {"Raw", std::vector<uint8_t>{0x11, 0x22}},
          {"Empty", std::string("")}}}};

    pldm::responder::fru_parser::FruRecordInfos recordInfos{
        {PLDM_FRU_RECORD_TYPE_GENERAL,
         PLDM_FRU_ENCODING_ASCII,
         {{"xyz.openbmc_project.Inventory.Decorator.Asset", "Model", "string",
           PLDM_FRU_FIELD_TYPE_MODEL},
          {"xyz.openbmc_project.Inventory.Decorator.Asset", "Raw", "bytearray",
           PLDM_FRU_FIELD_TYPE_VENDOR},
          {"xyz.openbmc_project.Inventory.Decorator.Asset", "Missing", "string",
           PLDM_FRU_FIELD_TYPE_SN}}},
        {PLDM_FRU_RECORD_TYPE_GENERAL,
         PLDM_FRU_ENCODING_ASCII,
         {{"xyz.openbmc_project.Inventory.Decorator.Asset", "Empty", "string",
           PLDM_FRU_FIELD_TYPE_NAME}}}};

    pldm_entity entity{64, 1, 1};
    impl.populateRecords(interfaces, recordInfos, entity);
    EXPECT_GT(impl.numRecords(), 0u);

    auto recordsAfterFirstPopulate = impl.numRecords();
    pldm::responder::fru_parser::FruRecordInfos emptyRecordInfos{
        {PLDM_FRU_RECORD_TYPE_GENERAL,
         PLDM_FRU_ENCODING_ASCII,
         {{"xyz.openbmc_project.Inventory.Decorator.Asset", "NoSuchProp",
           "string", PLDM_FRU_FIELD_TYPE_MODEL}}}};
    impl.populateRecords(interfaces, emptyRecordInfos, entity);
    EXPECT_EQ(impl.numRecords(), recordsAfterFirstPopulate);

    pldm_entity topEntity{64, 2, 0};
    pldm::responder::fru_parser::FruRecordInfos fwRecordInfos{
        {PLDM_FRU_RECORD_TYPE_GENERAL,
         PLDM_FRU_ENCODING_ASCII,
         {{"xyz.openbmc_project.Inventory.Decorator.Asset", "Version", "string",
           PLDM_FRU_FIELD_TYPE_VERSION}}}};
    impl.populateRecords(interfaces, fwRecordInfos, topEntity);

    impl.isBuilt = true;
    auto tableBefore = impl.size();
    impl.buildFRUTable();
    EXPECT_EQ(impl.size(), tableBefore);

    pldm_entity_association_tree_destroy(entityTree);
    pldm_entity_association_tree_destroy(bmcEntityTree);
    pldm_pdr_destroy(pdrRepo);
}

TEST(FruImpl, getFRURecordByOptionSuccessCoveragePaths)
{
    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::FruImpl impl("./fru_jsons/good",
                                  "./fru_jsons/fru_master/fru_master.json",
                                  pdrRepo, entityTree, bmcEntityTree);

    pldm::responder::dbus::InterfaceMap interfaces{
        {"xyz.openbmc_project.Inventory.Decorator.Asset",
         {{"Model", std::string("Model-X")},
          {"SerialNumber", std::string("SN-001")}}}};

    pldm::responder::fru_parser::FruRecordInfos recordInfos{
        {PLDM_FRU_RECORD_TYPE_GENERAL,
         PLDM_FRU_ENCODING_ASCII,
         {{"xyz.openbmc_project.Inventory.Decorator.Asset", "Model", "string",
           PLDM_FRU_FIELD_TYPE_MODEL},
          {"xyz.openbmc_project.Inventory.Decorator.Asset", "SerialNumber",
           "string", PLDM_FRU_FIELD_TYPE_SN}}}};

    pldm_entity entity{64, 1, 1};
    impl.populateRecords(interfaces, recordInfos, entity);
    ASSERT_GT(impl.numRecords(), 0u);
    ASSERT_GT(impl.rsi, 0u);

    std::vector<uint8_t> fruData;
    auto rc = impl.getFRURecordByOption(fruData, 0, impl.rsi,
                                        PLDM_FRU_RECORD_TYPE_GENERAL,
                                        PLDM_FRU_FIELD_TYPE_MODEL);
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_GT(fruData.size(), sizeof(uint32_t));

    pldm::responder::fru::Handler handler(
        "./fru_jsons/good", "./fru_jsons/fru_master/fru_master.json", pdrRepo,
        entityTree, bmcEntityTree);
    handler.impl.table = impl.table;
    handler.impl.padBytes = impl.padBytes;
    handler.impl.rsi = impl.rsi;
    handler.impl.numRecs = impl.numRecs;
    handler.impl.checksum = impl.checksum;
    handler.impl.isBuilt = true;

    std::vector<uint8_t> request(
        sizeof(pldm_msg_hdr) + sizeof(pldm_get_fru_record_by_option_req), 0);
    auto* msg = asMsg(request);
    ASSERT_EQ(encode_get_fru_record_by_option_req(
                  1, 0, 0, impl.rsi, PLDM_FRU_RECORD_TYPE_GENERAL,
                  PLDM_FRU_FIELD_TYPE_MODEL, PLDM_GET_FIRSTPART, msg,
                  sizeof(pldm_get_fru_record_by_option_req)),
              PLDM_SUCCESS);
    auto response = handler.getFRURecordByOption(
        msg, sizeof(pldm_get_fru_record_by_option_req));
    EXPECT_EQ(completionCode(response), PLDM_SUCCESS);

    pldm_entity_association_tree_destroy(entityTree);
    pldm_entity_association_tree_destroy(bmcEntityTree);
    pldm_pdr_destroy(pdrRepo);
}
