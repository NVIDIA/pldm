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
#include "test/test_tmp_utils.hpp"

#include <dlfcn.h>
#include <libpldm/entity.h>
#include <libpldm/fru.h>

#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <array>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

namespace
{

enum class FruEncodeHook
{
    none,
    metadataResp,
    tableResp,
    byOptionResp,
};

static FruEncodeHook fruEncodeHook = FruEncodeHook::none;

template <typename Fn>
Fn resolveFruSymbol(const char* symbol)
{
    return reinterpret_cast<Fn>(dlsym(RTLD_NEXT, symbol));
}

struct ScopedFruEncodeHook
{
    explicit ScopedFruEncodeHook(FruEncodeHook hook)
    {
        fruEncodeHook = hook;
    }

    ~ScopedFruEncodeHook()
    {
        fruEncodeHook = FruEncodeHook::none;
    }
};

extern "C" int encode_get_fru_record_table_metadata_resp(
    uint8_t instance_id, uint8_t completion_code,
    uint8_t fru_data_major_version, uint8_t fru_data_minor_version,
    uint32_t fru_table_maximum_size, uint32_t fru_table_length,
    uint16_t total_record_set_identifiers, uint16_t total_table_records,
    uint32_t checksum, struct pldm_msg* msg)
{
    if (fruEncodeHook == FruEncodeHook::metadataResp)
    {
        return PLDM_ERROR_INVALID_DATA;
    }

    using Fn = int (*)(uint8_t, uint8_t, uint8_t, uint8_t, uint32_t, uint32_t,
                       uint16_t, uint16_t, uint32_t, struct pldm_msg*);
    static auto realFn =
        resolveFruSymbol<Fn>("encode_get_fru_record_table_metadata_resp");
    return realFn(instance_id, completion_code, fru_data_major_version,
                  fru_data_minor_version, fru_table_maximum_size,
                  fru_table_length, total_record_set_identifiers,
                  total_table_records, checksum, msg);
}

extern "C" int encode_get_fru_record_table_resp(
    uint8_t instance_id, uint8_t completion_code,
    uint32_t next_data_transfer_handle, uint8_t transfer_flag,
    struct pldm_msg* msg)
{
    if (fruEncodeHook == FruEncodeHook::tableResp)
    {
        return PLDM_ERROR_INVALID_DATA;
    }

    using Fn = int (*)(uint8_t, uint8_t, uint32_t, uint8_t, struct pldm_msg*);
    static auto realFn =
        resolveFruSymbol<Fn>("encode_get_fru_record_table_resp");
    return realFn(instance_id, completion_code, next_data_transfer_handle,
                  transfer_flag, msg);
}

extern "C" int encode_get_fru_record_by_option_resp(
    uint8_t instance_id, uint8_t completion_code,
    uint32_t next_data_transfer_handle, uint8_t transfer_flag,
    const void* fru_structure_data, size_t data_size, struct pldm_msg* msg,
    size_t payload_length)
{
    if (fruEncodeHook == FruEncodeHook::byOptionResp)
    {
        return PLDM_ERROR_INVALID_DATA;
    }

    using Fn = int (*)(uint8_t, uint8_t, uint32_t, uint8_t, const void*, size_t,
                       struct pldm_msg*, size_t);
    static auto realFn =
        resolveFruSymbol<Fn>("encode_get_fru_record_by_option_resp");
    return realFn(instance_id, completion_code, next_data_transfer_handle,
                  transfer_flag, fru_structure_data, data_size, msg,
                  payload_length);
}

pldm_msg* asMsg(std::vector<uint8_t>& request)
{
    return reinterpret_cast<pldm_msg*>(request.data());
}

uint8_t completionCode(const pldm::responder::Response& response)
{
    return reinterpret_cast<const pldm_msg*>(response.data())->payload[0];
}

class FruDbusFixture
{
  public:
    enum class VersionMode
    {
        stringValue,
        uint32Value,
    };

    enum class EndpointsMode
    {
        singleEndpoint,
        emptyEndpoints,
        invalidEndpoint,
    };

    explicit FruDbusFixture(
        VersionMode versionMode = VersionMode::stringValue,
        EndpointsMode endpointsMode = EndpointsMode::singleEndpoint)
    {
        static constexpr auto mapperService =
            "xyz.openbmc_project.ObjectMapper";
        static constexpr auto mapperPath = "/xyz/openbmc_project/object_mapper";
        static constexpr auto mapperInterface =
            "xyz.openbmc_project.ObjectMapper";

        mapperConn = std::make_shared<sdbusplus::asio::connection>(
            io, sdbusplus::bus::new_bus());
        mapperConn->request_name(mapperService);
        mapperServer =
            std::make_unique<sdbusplus::asio::object_server>(mapperConn);

        mapperIface = mapperServer->add_interface(mapperPath, mapperInterface);
        mapperIface->register_method(
            "GetObject",
            [](const std::string& path, const std::vector<std::string>&) {
                std::map<std::string, std::vector<std::string>> response;
                if (path == "/xyz/openbmc_project/software/v1")
                {
                    response.emplace(
                        mapperService,
                        std::vector<std::string>{
                            "xyz.openbmc_project.Software.Version"});
                }
                return response;
            });
        mapperIface->initialize();

        functionalIface = mapperServer->add_interface(
            "/xyz/openbmc_project/software/functional",
            "xyz.openbmc_project.Association");
        if (endpointsMode == EndpointsMode::emptyEndpoints)
        {
            functionalIface->register_property("Endpoints",
                                               std::vector<std::string>{});
        }
        else if (endpointsMode == EndpointsMode::invalidEndpoint)
        {
            functionalIface->register_property(
                "Endpoints", std::vector<std::string>{
                                 "/xyz/openbmc_project/software/missing"});
        }
        else
        {
            functionalIface->register_property(
                "Endpoints",
                std::vector<std::string>{"/xyz/openbmc_project/software/v1"});
        }
        functionalIface->initialize();

        versionIface =
            mapperServer->add_interface("/xyz/openbmc_project/software/v1",
                                        "xyz.openbmc_project.Software.Version");
        if (versionMode == VersionMode::uint32Value)
        {
            versionIface->register_property("Version",
                                            static_cast<uint32_t>(7));
        }
        else
        {
            versionIface->register_property("Version", std::string("v1.2.3"));
        }
        versionIface->initialize();

        inventoryConn = std::make_shared<sdbusplus::asio::connection>(
            io, sdbusplus::bus::new_bus());
        inventoryConn->request_name("xyz.openbmc_project.Inventory.Manager");
        inventoryServer =
            std::make_unique<sdbusplus::asio::object_server>(inventoryConn);
        inventoryServer->add_manager("/xyz/openbmc_project/inventory");

        addInventoryInterface("/xyz/openbmc_project/inventory/board0",
                              "xyz.openbmc_project.Inventory.Item.Board", {});
        addInventoryInterface("/xyz/openbmc_project/inventory/board0",
                              "xyz.openbmc_project.Inventory.Decorator.Asset",
                              {{"Model", std::string("BoardModel")},
                               {"PartNumber", std::string("BoardPN")},
                               {"SerialNumber", std::string("BoardSN")},
                               {"Manufacturer", std::string("BoardVendor")}});
        addInventoryInterface("/xyz/openbmc_project/inventory/board0",
                              "xyz.openbmc_project.Inventory.Item",
                              {{"PrettyName", std::string("SystemBoard")}});
        addInventoryInterface(
            "/xyz/openbmc_project/inventory/board0",
            "xyz.openbmc_project.Inventory.Decorator.AssetTag",
            {{"AssetTag", std::string("Asset-1")}});
        addInventoryInterface(
            "/xyz/openbmc_project/inventory/board0",
            "xyz.openbmc_project.Inventory.Decorator.Revision",
            {{"Version", std::string("ignored")}});
        addInventoryInterface("/xyz/openbmc_project/inventory/board0",
                              "com.ibm.ipzvpd.VINI",
                              {{"RT", std::string("VINI-RT")},
                               {"B3", std::vector<uint8_t>{0x12, 0x34}}});

        addInventoryInterface("/xyz/openbmc_project/inventory/board0/cpu0",
                              "xyz.openbmc_project.Inventory.Item.Cpu", {});
        addInventoryInterface("/xyz/openbmc_project/inventory/board0/cpu0",
                              "xyz.openbmc_project.Inventory.Decorator.Asset",
                              {{"Model", std::string("CpuModel")},
                               {"PartNumber", std::string("CpuPN")},
                               {"SerialNumber", std::string("CpuSN")},
                               {"Manufacturer", std::string("CpuVendor")}});
        addInventoryInterface("/xyz/openbmc_project/inventory/board0/cpu0",
                              "xyz.openbmc_project.Inventory.Item",
                              {{"PrettyName", std::string("Cpu0")}});
        addInventoryInterface(
            "/xyz/openbmc_project/inventory/board0/cpu0",
            "xyz.openbmc_project.Inventory.Decorator.Revision",
            {{"Version", std::string("CpuRevA")}});
        addInventoryInterface("/xyz/openbmc_project/inventory/empty0",
                              "xyz.openbmc_project.Inventory.Item.CustomEmpty",
                              {});
        addInventoryInterface("/xyz/openbmc_project/inventory/empty0/child0",
                              "xyz.openbmc_project.Inventory.Item.CustomChild",
                              {});

        ioThread = std::thread([this] { io.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ~FruDbusFixture()
    {
        io.stop();
        if (ioThread.joinable())
        {
            ioThread.join();
        }
    }

  private:
    void addInventoryInterface(
        const std::string& path, const std::string& interface,
        const std::map<std::string, pldm::responder::dbus::Value>& properties)
    {
        auto iface = inventoryServer->add_interface(path, interface);
        for (const auto& [name, value] : properties)
        {
            std::visit(
                [&iface, &name](const auto& propertyValue) {
                    iface->register_property(name, propertyValue);
                },
                value);
        }
        iface->initialize();
        inventoryIfaces.push_back(iface);
    }

    boost::asio::io_context io;
    std::shared_ptr<sdbusplus::asio::connection> mapperConn;
    std::shared_ptr<sdbusplus::asio::connection> inventoryConn;
    std::unique_ptr<sdbusplus::asio::object_server> mapperServer;
    std::unique_ptr<sdbusplus::asio::object_server> inventoryServer;
    std::shared_ptr<sdbusplus::asio::dbus_interface> mapperIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> functionalIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> versionIface;
    std::vector<std::shared_ptr<sdbusplus::asio::dbus_interface>>
        inventoryIfaces;
    std::thread ioThread;
};

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

TEST(FruParser, missingMasterAndInvalidConfigCoverage)
{
    using namespace pldm::responder::fru_parser;

    auto dir = pldm::test::makeTempDir("FruParserCoverage.XXXXXX");

    FruParser missingMasterParser(dir.string(), dir / "missing.json");
    EXPECT_THROW(static_cast<void>(missingMasterParser.inventoryLookup()),
                 std::bad_optional_access);
    EXPECT_THROW(missingMasterParser.getRecordInfo(
                     "xyz.openbmc_project.Inventory.Item.Cpu"),
                 std::out_of_range);
    EXPECT_THROW(missingMasterParser.getEntityType(
                     "xyz.openbmc_project.Inventory.Item.Cpu"),
                 std::out_of_range);

    auto master = dir / "fru_master.json";
    {
        std::ofstream out(master);
        out << R"({"FruDBusLookupMap":{"xyz.openbmc_project.Inventory.Item.Cpu":135}})";
    }

    auto badConfig = dir / "bad.json";
    {
        std::ofstream out(badConfig);
        out << R"({"record_details":)";
    }

    EXPECT_ANY_THROW((FruParser(dir.string(), master)));
    fs::remove_all(dir);
}

TEST(FruParser, malformedEntryIsSkipped)
{
    using namespace pldm::responder::fru_parser;

    auto dir = pldm::test::makeTempDir("FruParserSkip.XXXXXX");

    auto master = dir / "fru_master.json";
    {
        std::ofstream out(master);
        out << R"({"FruDBusLookupMap":{"xyz.openbmc_project.Inventory.Item.Cpu":135}})";
    }

    auto malformedEntry = dir / "cpu.json";
    {
        std::ofstream out(malformedEntry);
        out << R"({
            "record_details": {
                "fru_record_type": 1,
                "fru_encoding_type": 1,
                "dbus_interface_name": "xyz.openbmc_project.Inventory.Item.Cpu"
            },
            "fru_fields": [
                {
                    "fru_field_type": 2,
                    "dbus": 5
                }
            ]
        })";
    }

    FruParser parser(dir.string(), master);
    auto cpuInfos =
        parser.getRecordInfo("xyz.openbmc_project.Inventory.Item.Cpu");
    EXPECT_EQ(cpuInfos.size(), 1u);

    fs::remove_all(dir);
}

TEST(FruParser, malformedMasterJsonDeath)
{
    auto dir = pldm::test::makeTempDir("FruParserDeath.XXXXXX");
    auto master = dir / "fru_master.json";
    {
        std::ofstream out(master);
        out << "{";
    }

    EXPECT_DEATH((pldm::responder::fru_parser::FruParser(dir.string(), master)),
                 "");
    std::filesystem::remove_all(dir);
}

TEST(FruParser, emptyLookupAndMissingDirCoverage)
{
    using namespace pldm::responder::fru_parser;

    auto dir = pldm::test::makeTempDir("FruParserEmptyLookup.XXXXXX");
    auto master = dir / "fru_master.json";
    {
        std::ofstream out(master);
        out << R"({"FruDBusLookupMap":{}})";
    }

    FruParser parser((dir / "missing").string(), master);
    auto lookup = parser.inventoryLookup();
    EXPECT_EQ(std::get<0>(lookup), "xyz.openbmc_project.Inventory.Manager");
    EXPECT_EQ(std::get<1>(lookup), "/xyz/openbmc_project/inventory");
    EXPECT_TRUE(std::get<2>(lookup).empty());

    fs::remove_all(dir);
}

TEST(FruParser, invalidLookupAndCustomInterfaceCoverage)
{
    using namespace pldm::responder::fru_parser;

    auto dir = pldm::test::makeTempDir("FruParserCustom.XXXXXX");
    auto master = dir / "fru_master.json";
    {
        std::ofstream out(master);
        out << R"({"FruDBusLookupMap":{"xyz.openbmc_project.Inventory.Item.Cpu":"bad"}})";
    }
    EXPECT_ANY_THROW((FruParser(dir.string(), master)));

    {
        std::ofstream out(master);
        out << R"({"FruDBusLookupMap":{"xyz.openbmc_project.Inventory.Item.Cpu":135}})";
    }
    {
        std::ofstream out(dir / "custom_empty.json");
        out << R"({
            "record_details": {
                "fru_record_type": 1,
                "fru_encoding_type": 1,
                "dbus_interface_name": "xyz.openbmc_project.Inventory.Item.Custom"
            },
            "fru_fields": []
        })";
    }
    {
        std::ofstream out(dir / "custom_full.json");
        out << R"({
            "record_details": {
                "fru_record_type": 2,
                "fru_encoding_type": 1,
                "dbus_interface_name": "xyz.openbmc_project.Inventory.Item.Custom"
            },
            "fru_fields": [
                {
                    "fru_field_type": 2,
                    "dbus": {
                        "interface": "xyz.openbmc_project.Inventory.Decorator.Asset",
                        "property_name": "Model",
                        "property_type": "string"
                    }
                }
            ]
        })";
    }

    FruParser parser(dir.string(), master);
    auto infos =
        parser.getRecordInfo("xyz.openbmc_project.Inventory.Item.Custom");
    ASSERT_EQ(infos.size(), 2u);
    EXPECT_TRUE(std::any_of(infos.begin(), infos.end(), [](const auto& info) {
        return std::get<2>(info).empty();
    }));
    EXPECT_TRUE(std::any_of(infos.begin(), infos.end(), [](const auto& info) {
        return std::get<2>(info).size() == 1;
    }));

    fs::remove_all(dir);
}

TEST(FruImpl, buildFRUTableSessionBusCoverage)
{
    FruDbusFixture dbusFixture;

    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::FruImpl impl("./fru_jsons/good",
                                  "./fru_jsons/fru_master/fru_master.json",
                                  pdrRepo, entityTree, bmcEntityTree);

    EXPECT_EQ(impl.populatefwVersion(), "v1.2.3");

    impl.buildFRUTable();

    EXPECT_GT(impl.size(), 0u);
    EXPECT_GT(impl.numRSI(), 0u);
    EXPECT_GT(impl.numRecords(), 0u);
    ASSERT_EQ(impl.getAssociateEntityMap().count(
                  "/xyz/openbmc_project/inventory/board0"),
              1u);
    ASSERT_EQ(impl.getAssociateEntityMap().count(
                  "/xyz/openbmc_project/inventory/board0/cpu0"),
              1u);

    pldm::responder::Response response(sizeof(pldm_msg_hdr), 0);
    impl.getFRUTable(response);
    EXPECT_GT(response.size(), sizeof(pldm_msg_hdr) + sizeof(uint32_t));

    pldm_entity_association_tree_destroy(entityTree);
    pldm_entity_association_tree_destroy(bmcEntityTree);
    pldm_pdr_destroy(pdrRepo);
}

TEST(FruImpl, buildFRUTableLookupFailureCoverage)
{
    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::FruImpl impl("./fru_jsons/good",
                                  "./fru_jsons/fru_master/fru_master.json",
                                  pdrRepo, entityTree, bmcEntityTree);

    impl.buildFRUTable();

    EXPECT_FALSE(impl.isBuilt);
    EXPECT_EQ(impl.size(), 0u);
    EXPECT_EQ(impl.numRecords(), 0u);
    EXPECT_TRUE(impl.getAssociateEntityMap().empty());

    pldm_entity_association_tree_destroy(entityTree);
    pldm_entity_association_tree_destroy(bmcEntityTree);
    pldm_pdr_destroy(pdrRepo);
}

TEST(FruImpl, buildFRUTableMissingMasterCoverage)
{
    auto dir = pldm::test::makeTempDir("FruBuildMissingMaster.XXXXXX");

    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::FruImpl impl(dir.string(), dir / "missing_master.json",
                                  pdrRepo, entityTree, bmcEntityTree);
    impl.buildFRUTable();

    EXPECT_FALSE(impl.isBuilt);
    EXPECT_EQ(impl.size(), 0u);
    EXPECT_EQ(impl.numRecords(), 0u);
    EXPECT_TRUE(impl.getAssociateEntityMap().empty());

    pldm_entity_association_tree_destroy(entityTree);
    pldm_entity_association_tree_destroy(bmcEntityTree);
    pldm_pdr_destroy(pdrRepo);
    std::filesystem::remove_all(dir);
}

TEST(FruImpl, buildFRUTableEmptyRecordInfosCoverage)
{
    FruDbusFixture dbusFixture(FruDbusFixture::VersionMode::uint32Value);

    auto dir = pldm::test::makeTempDir("FruBuildEmptyRecords.XXXXXX");
    auto master = dir / "fru_master.json";
    {
        std::ofstream out(master);
        out << R"({
            "FruDBusLookupMap": {
                "xyz.openbmc_project.Inventory.Item.CustomEmpty": 64,
                "xyz.openbmc_project.Inventory.Item.CustomChild": 67
            }
        })";
    }
    {
        std::ofstream out(dir / "custom_empty.json");
        out << R"({
            "record_details": {
                "fru_record_type": 1,
                "fru_encoding_type": 1,
                "dbus_interface_name": "xyz.openbmc_project.Inventory.Item.CustomEmpty"
            },
            "fru_fields": []
        })";
    }
    {
        std::ofstream out(dir / "custom_child.json");
        out << R"({
            "record_details": {
                "fru_record_type": 1,
                "fru_encoding_type": 1,
                "dbus_interface_name": "xyz.openbmc_project.Inventory.Item.CustomChild"
            },
            "fru_fields": []
        })";
    }

    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::FruImpl impl(dir.string(), master, pdrRepo, entityTree,
                                  bmcEntityTree);
    impl.buildFRUTable();

    EXPECT_TRUE(impl.isBuilt);
    EXPECT_EQ(impl.size(), 0u);
    EXPECT_EQ(impl.numRecords(), 0u);
    EXPECT_EQ(impl.getAssociateEntityMap().count(
                  "/xyz/openbmc_project/inventory/empty0"),
              1u);
    EXPECT_EQ(impl.getAssociateEntityMap().count(
                  "/xyz/openbmc_project/inventory/empty0/child0"),
              1u);

    pldm_entity_association_tree_destroy(entityTree);
    pldm_entity_association_tree_destroy(bmcEntityTree);
    pldm_pdr_destroy(pdrRepo);
    std::filesystem::remove_all(dir);
}

TEST(FruImpl, buildFRUTableDefaultRecordMapCoverage)
{
    FruDbusFixture dbusFixture(FruDbusFixture::VersionMode::uint32Value);

    auto dir = pldm::test::makeTempDir("FruBuildMissingConfig.XXXXXX");
    auto master = dir / "fru_master.json";
    {
        std::ofstream out(master);
        out << R"({
            "FruDBusLookupMap": {
                "xyz.openbmc_project.Inventory.Item.CustomEmpty": 64,
                "xyz.openbmc_project.Inventory.Item.CustomChild": 67
            }
        })";
    }
    {
        std::ofstream out(dir / "custom_child.json");
        out << R"({
            "record_details": {
                "fru_record_type": 1,
                "fru_encoding_type": 1,
                "dbus_interface_name": "xyz.openbmc_project.Inventory.Item.CustomChild"
            },
            "fru_fields": []
        })";
    }

    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::FruImpl impl(dir.string(), master, pdrRepo, entityTree,
                                  bmcEntityTree);
    impl.buildFRUTable();

    EXPECT_TRUE(impl.isBuilt);
    EXPECT_EQ(impl.size(), 0u);
    EXPECT_EQ(impl.numRecords(), 0u);
    EXPECT_EQ(impl.getAssociateEntityMap().count(
                  "/xyz/openbmc_project/inventory/empty0"),
              1u);
    EXPECT_EQ(impl.getAssociateEntityMap().count(
                  "/xyz/openbmc_project/inventory/empty0/child0"),
              1u);

    pldm_entity_association_tree_destroy(entityTree);
    pldm_entity_association_tree_destroy(bmcEntityTree);
    pldm_pdr_destroy(pdrRepo);
    std::filesystem::remove_all(dir);
}

TEST(FruImpl, buildFRUTableMissingRecordInfoCoverage)
{
    FruDbusFixture dbusFixture;

    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::FruImpl impl("./fru_jsons/good",
                                  "./fru_jsons/fru_master/fru_master.json",
                                  pdrRepo, entityTree, bmcEntityTree);
    impl.parser.recordMap.erase("xyz.openbmc_project.Inventory.Item.Board");
    impl.buildFRUTable();

    EXPECT_TRUE(impl.isBuilt);
    EXPECT_FALSE(impl.getAssociateEntityMap().contains(
        "/xyz/openbmc_project/inventory/board0"));
    EXPECT_TRUE(impl.getAssociateEntityMap().contains(
        "/xyz/openbmc_project/inventory/board0/cpu0"));

    pldm_entity_association_tree_destroy(entityTree);
    pldm_entity_association_tree_destroy(bmcEntityTree);
    pldm_pdr_destroy(pdrRepo);
}

TEST(FruImpl, buildFRUTableReturnsImmediatelyWhenAlreadyBuilt)
{
    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::FruImpl impl("./fru_jsons/good",
                                  "./fru_jsons/fru_master/fru_master.json",
                                  pdrRepo, entityTree, bmcEntityTree);
    impl.isBuilt = true;
    impl.table = {0x11, 0x22, 0x33};
    impl.rsi = 7;
    impl.numRecs = 9;
    impl.padBytes = 2;
    impl.checksum = 0x78563412;

    impl.buildFRUTable();

    EXPECT_TRUE(impl.isBuilt);
    EXPECT_EQ(impl.table, (std::vector<uint8_t>{0x11, 0x22, 0x33}));
    EXPECT_EQ(impl.rsi, 7);
    EXPECT_EQ(impl.numRecs, 9);
    EXPECT_EQ(impl.padBytes, 2);
    EXPECT_EQ(impl.checksum, 0x78563412u);

    pldm_entity_association_tree_destroy(entityTree);
    pldm_entity_association_tree_destroy(bmcEntityTree);
    pldm_pdr_destroy(pdrRepo);
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

TEST(FruHandler, requestEncodeFailureCoverage)
{
    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();
    pldm::responder::fru::Handler handler(
        "./fru_jsons/good", "./fru_jsons/fru_master/fru_master.json", pdrRepo,
        entityTree, bmcEntityTree);

    std::vector<uint8_t> metadataRequest(
        sizeof(pldm_msg_hdr) + PLDM_GET_FRU_RECORD_TABLE_METADATA_REQ_BYTES, 0);
    auto* metadataMsg = asMsg(metadataRequest);
    ASSERT_EQ(encode_get_fru_record_table_metadata_req(
                  1, metadataMsg, PLDM_GET_FRU_RECORD_TABLE_METADATA_REQ_BYTES),
              PLDM_SUCCESS);
    {
        ScopedFruEncodeHook hook(FruEncodeHook::metadataResp);
        auto response = handler.getFRURecordTableMetadata(
            metadataMsg, PLDM_GET_FRU_RECORD_TABLE_METADATA_REQ_BYTES);
        EXPECT_EQ(completionCode(response), PLDM_ERROR_INVALID_DATA);
    }

    std::vector<uint8_t> tableRequest(
        sizeof(pldm_msg_hdr) + PLDM_GET_FRU_RECORD_TABLE_REQ_BYTES, 0);
    auto* tableMsg = asMsg(tableRequest);
    ASSERT_EQ(
        encode_get_fru_record_table_req(1, 0, PLDM_GET_FIRSTPART, tableMsg,
                                        PLDM_GET_FRU_RECORD_TABLE_REQ_BYTES),
        PLDM_SUCCESS);
    {
        ScopedFruEncodeHook hook(FruEncodeHook::tableResp);
        auto response = handler.getFRURecordTable(
            tableMsg, PLDM_GET_FRU_RECORD_TABLE_REQ_BYTES);
        EXPECT_EQ(completionCode(response), PLDM_ERROR_INVALID_DATA);
    }

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

    handler.impl.table = impl.table;
    handler.impl.padBytes = impl.padBytes;
    handler.impl.rsi = impl.rsi;
    handler.impl.numRecs = impl.numRecs;
    handler.impl.checksum = impl.checksum;
    handler.impl.isBuilt = true;

    std::vector<uint8_t> byOptionRequest(
        sizeof(pldm_msg_hdr) + sizeof(pldm_get_fru_record_by_option_req), 0);
    auto* byOptionMsg = asMsg(byOptionRequest);
    ASSERT_EQ(encode_get_fru_record_by_option_req(
                  1, 0, 0, impl.rsi, PLDM_FRU_RECORD_TYPE_GENERAL,
                  PLDM_FRU_FIELD_TYPE_MODEL, PLDM_GET_FIRSTPART, byOptionMsg,
                  sizeof(pldm_get_fru_record_by_option_req)),
              PLDM_SUCCESS);
    {
        ScopedFruEncodeHook hook(FruEncodeHook::byOptionResp);
        auto response = handler.getFRURecordByOption(
            byOptionMsg, sizeof(pldm_get_fru_record_by_option_req));
        EXPECT_EQ(completionCode(response), PLDM_ERROR_INVALID_DATA);
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

TEST(FruImpl, populatefwVersionWrongPropertyTypeCoverage)
{
    FruDbusFixture dbusFixture(FruDbusFixture::VersionMode::uint32Value);

    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::FruImpl impl("./fru_jsons/good",
                                  "./fru_jsons/fru_master/fru_master.json",
                                  pdrRepo, entityTree, bmcEntityTree);

    EXPECT_TRUE(impl.populatefwVersion().empty());

    pldm::responder::dbus::InterfaceMap interfaces{};
    pldm::responder::fru_parser::FruRecordInfos recordInfos{
        {PLDM_FRU_RECORD_TYPE_GENERAL,
         PLDM_FRU_ENCODING_ASCII,
         {{"xyz.openbmc_project.Inventory.Decorator.Revision", "Version",
           "string", PLDM_FRU_FIELD_TYPE_VERSION}}}};

    pldm_entity entity{64, 1, 0};
    impl.populateRecords(interfaces, recordInfos, entity);
    EXPECT_EQ(impl.numRecords(), 0u);
    EXPECT_TRUE(impl.table.empty());

    pldm_entity_association_tree_destroy(entityTree);
    pldm_entity_association_tree_destroy(bmcEntityTree);
    pldm_pdr_destroy(pdrRepo);
}

TEST(FruImpl, populatefwVersionMissingVersionEndpointCoverage)
{
    FruDbusFixture dbusFixture(FruDbusFixture::VersionMode::stringValue,
                               FruDbusFixture::EndpointsMode::invalidEndpoint);

    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::FruImpl impl("./fru_jsons/good",
                                  "./fru_jsons/fru_master/fru_master.json",
                                  pdrRepo, entityTree, bmcEntityTree);

    EXPECT_TRUE(impl.populatefwVersion().empty());

    pldm_entity_association_tree_destroy(entityTree);
    pldm_entity_association_tree_destroy(bmcEntityTree);
    pldm_pdr_destroy(pdrRepo);
}

TEST(FruImpl, populatefwVersionEmptyEndpointsCoverage)
{
    FruDbusFixture dbusFixture(FruDbusFixture::VersionMode::stringValue,
                               FruDbusFixture::EndpointsMode::emptyEndpoints);

    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::FruImpl impl("./fru_jsons/good",
                                  "./fru_jsons/fru_master/fru_master.json",
                                  pdrRepo, entityTree, bmcEntityTree);
    EXPECT_TRUE(impl.populatefwVersion().empty());

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

TEST(FruImpl, populateRecordsByteArrayWrongVariantThrows)
{
    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::FruImpl impl("./fru_jsons/good",
                                  "./fru_jsons/fru_master/fru_master.json",
                                  pdrRepo, entityTree, bmcEntityTree);

    pldm::responder::dbus::InterfaceMap interfaces{
        {"xyz.openbmc_project.Inventory.Decorator.Asset",
         {{"Raw", std::string("not-a-byte-array")}}}};
    pldm::responder::fru_parser::FruRecordInfos recordInfos{
        {PLDM_FRU_RECORD_TYPE_GENERAL,
         PLDM_FRU_ENCODING_ASCII,
         {{"xyz.openbmc_project.Inventory.Decorator.Asset", "Raw", "bytearray",
           PLDM_FRU_FIELD_TYPE_VENDOR}}}};

    pldm_entity entity{64, 7, 1};
    EXPECT_THROW((impl.populateRecords(interfaces, recordInfos, entity)),
                 std::bad_variant_access);
    EXPECT_TRUE(impl.table.empty());
    EXPECT_EQ(impl.numRecords(), 0u);

    pldm_entity_association_tree_destroy(entityTree);
    pldm_entity_association_tree_destroy(bmcEntityTree);
    pldm_pdr_destroy(pdrRepo);
}

TEST(FruImpl, populateRecordsEmptyByteArrayCoverage)
{
    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::FruImpl impl("./fru_jsons/good",
                                  "./fru_jsons/fru_master/fru_master.json",
                                  pdrRepo, entityTree, bmcEntityTree);

    pldm::responder::dbus::InterfaceMap interfaces{
        {"xyz.openbmc_project.Inventory.Decorator.Asset",
         {{"Model", std::string("ModelY")}, {"Raw", std::vector<uint8_t>{}}}}};

    pldm::responder::fru_parser::FruRecordInfos recordInfos{
        {PLDM_FRU_RECORD_TYPE_GENERAL,
         PLDM_FRU_ENCODING_ASCII,
         {{"xyz.openbmc_project.Inventory.Decorator.Asset", "Raw", "bytearray",
           PLDM_FRU_FIELD_TYPE_VENDOR},
          {"xyz.openbmc_project.Inventory.Decorator.Asset", "Model", "string",
           PLDM_FRU_FIELD_TYPE_MODEL}}}};

    pldm_entity entity{64, 4, 1};
    impl.populateRecords(interfaces, recordInfos, entity);

    EXPECT_EQ(impl.numRecords(), 1u);
    EXPECT_FALSE(impl.table.empty());

    pldm_entity_association_tree_destroy(entityTree);
    pldm_entity_association_tree_destroy(bmcEntityTree);
    pldm_pdr_destroy(pdrRepo);
}

TEST(FruImpl, populateRecordsStringWrongVariantThrows)
{
    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::FruImpl impl("./fru_jsons/good",
                                  "./fru_jsons/fru_master/fru_master.json",
                                  pdrRepo, entityTree, bmcEntityTree);

    pldm::responder::dbus::InterfaceMap interfaces{
        {"xyz.openbmc_project.Inventory.Decorator.Asset",
         {{"Model", std::vector<uint8_t>{0x01, 0x02}}}}};
    pldm::responder::fru_parser::FruRecordInfos recordInfos{
        {PLDM_FRU_RECORD_TYPE_GENERAL,
         PLDM_FRU_ENCODING_ASCII,
         {{"xyz.openbmc_project.Inventory.Decorator.Asset", "Model", "string",
           PLDM_FRU_FIELD_TYPE_MODEL}}}};

    pldm_entity entity{64, 8, 1};
    EXPECT_THROW((impl.populateRecords(interfaces, recordInfos, entity)),
                 std::bad_variant_access);
    EXPECT_TRUE(impl.table.empty());
    EXPECT_EQ(impl.numRecords(), 0u);

    pldm_entity_association_tree_destroy(entityTree);
    pldm_entity_association_tree_destroy(bmcEntityTree);
    pldm_pdr_destroy(pdrRepo);
}

TEST(FruImpl, populateRecordsTopLevelVersionUsesFirmwareVersionCoverage)
{
    FruDbusFixture dbusFixture;

    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::FruImpl impl("./fru_jsons/good",
                                  "./fru_jsons/fru_master/fru_master.json",
                                  pdrRepo, entityTree, bmcEntityTree);

    pldm::responder::dbus::InterfaceMap interfaces{};
    pldm::responder::fru_parser::FruRecordInfos recordInfos{
        {PLDM_FRU_RECORD_TYPE_GENERAL,
         PLDM_FRU_ENCODING_ASCII,
         {{"xyz.openbmc_project.Inventory.Decorator.Revision", "Version",
           "string", PLDM_FRU_FIELD_TYPE_VERSION}}}};

    pldm_entity entity{64, 1, 0};
    impl.populateRecords(interfaces, recordInfos, entity);

    EXPECT_EQ(impl.numRecords(), 1u);
    EXPECT_FALSE(impl.table.empty());

    pldm_entity_association_tree_destroy(entityTree);
    pldm_entity_association_tree_destroy(bmcEntityTree);
    pldm_pdr_destroy(pdrRepo);
}

TEST(FruImpl, populateRecordsUnsupportedTypeAndEmptyFieldsCoverage)
{
    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::FruImpl impl("./fru_jsons/good",
                                  "./fru_jsons/fru_master/fru_master.json",
                                  pdrRepo, entityTree, bmcEntityTree);

    pldm::responder::dbus::InterfaceMap interfaces{
        {"xyz.openbmc_project.Inventory.Decorator.Asset",
         {{"Opaque", static_cast<uint8_t>(7)}}}};
    pldm::responder::fru_parser::FruRecordInfos recordInfos{
        {PLDM_FRU_RECORD_TYPE_GENERAL, PLDM_FRU_ENCODING_ASCII, {}},
        {PLDM_FRU_RECORD_TYPE_GENERAL,
         PLDM_FRU_ENCODING_ASCII,
         {{"xyz.openbmc_project.Inventory.Decorator.Asset", "Opaque", "uint8_t",
           PLDM_FRU_FIELD_TYPE_VENDOR}}}};

    pldm_entity entity{64, 3, 1};
    impl.populateRecords(interfaces, recordInfos, entity);
    EXPECT_EQ(impl.numRecords(), 0u);
    EXPECT_TRUE(impl.table.empty());

    pldm_entity_association_tree_destroy(entityTree);
    pldm_entity_association_tree_destroy(bmcEntityTree);
    pldm_pdr_destroy(pdrRepo);
}

TEST(FruImpl, populateRecordsSkipsEmptyStringAndByteArrayFields)
{
    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::FruImpl impl("./fru_jsons/good",
                                  "./fru_jsons/fru_master/fru_master.json",
                                  pdrRepo, entityTree, bmcEntityTree);

    pldm::responder::dbus::InterfaceMap interfaces{
        {"xyz.openbmc_project.Inventory.Decorator.Asset",
         {{"Model", std::string{}}, {"Raw", std::vector<uint8_t>{}}}}};
    pldm::responder::fru_parser::FruRecordInfos recordInfos{
        {PLDM_FRU_RECORD_TYPE_GENERAL,
         PLDM_FRU_ENCODING_ASCII,
         {{"xyz.openbmc_project.Inventory.Decorator.Asset", "Model", "string",
           PLDM_FRU_FIELD_TYPE_MODEL},
          {"xyz.openbmc_project.Inventory.Decorator.Asset", "Raw", "bytearray",
           PLDM_FRU_FIELD_TYPE_VENDOR}}}};

    impl.populateRecords(interfaces, recordInfos, pldm_entity{64, 11, 1});

    EXPECT_TRUE(impl.table.empty());
    EXPECT_EQ(impl.numRecords(), 0u);

    pldm_entity_association_tree_destroy(entityTree);
    pldm_entity_association_tree_destroy(bmcEntityTree);
    pldm_pdr_destroy(pdrRepo);
}

TEST(FruImpl, populateRecordsSkipsMissingFieldsBeforeEncodingValidRecord)
{
    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::FruImpl impl("./fru_jsons/good",
                                  "./fru_jsons/fru_master/fru_master.json",
                                  pdrRepo, entityTree, bmcEntityTree);

    pldm::responder::dbus::InterfaceMap interfaces{
        {"xyz.openbmc_project.Inventory.Decorator.Asset",
         {{"Model", std::string("Model-Y")}}}};
    pldm::responder::fru_parser::FruRecordInfos recordInfos{
        {PLDM_FRU_RECORD_TYPE_GENERAL,
         PLDM_FRU_ENCODING_ASCII,
         {{"xyz.openbmc_project.Inventory.Decorator.Asset", "Missing", "string",
           PLDM_FRU_FIELD_TYPE_VENDOR},
          {"xyz.openbmc_project.Inventory.Decorator.Asset", "Model", "string",
           PLDM_FRU_FIELD_TYPE_MODEL}}}};

    impl.populateRecords(interfaces, recordInfos, pldm_entity{64, 12, 1});

    EXPECT_EQ(impl.numRecords(), 1u);
    EXPECT_FALSE(impl.table.empty());

    pldm_entity_association_tree_destroy(entityTree);
    pldm_entity_association_tree_destroy(bmcEntityTree);
    pldm_pdr_destroy(pdrRepo);
}

TEST(FruImpl, populateRecordsLongFieldValuesCoverage)
{
    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::FruImpl impl("./fru_jsons/good",
                                  "./fru_jsons/fru_master/fru_master.json",
                                  pdrRepo, entityTree, bmcEntityTree);

    pldm::responder::dbus::InterfaceMap interfaces{
        {"xyz.openbmc_project.Inventory.Decorator.Asset",
         {{"Model", std::string(96, 'M')},
          {"Raw", std::vector<uint8_t>(48, 0x5A)}}}};
    pldm::responder::fru_parser::FruRecordInfos recordInfos{
        {PLDM_FRU_RECORD_TYPE_GENERAL,
         PLDM_FRU_ENCODING_ASCII,
         {{"xyz.openbmc_project.Inventory.Decorator.Asset", "Model", "string",
           PLDM_FRU_FIELD_TYPE_MODEL},
          {"xyz.openbmc_project.Inventory.Decorator.Asset", "Raw", "bytearray",
           PLDM_FRU_FIELD_TYPE_VENDOR}}}};

    pldm_entity entity{64, 9, 1};
    impl.populateRecords(interfaces, recordInfos, entity);

    EXPECT_EQ(impl.numRecords(), 1u);
    EXPECT_FALSE(impl.table.empty());

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

TEST(FruHandler, getFRURecordByOptionUnavailableCoverage)
{
    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::FruImpl impl("./fru_jsons/good",
                                  "./fru_jsons/fru_master/fru_master.json",
                                  pdrRepo, entityTree, bmcEntityTree);
    pldm::responder::dbus::InterfaceMap interfaces{
        {"xyz.openbmc_project.Inventory.Decorator.Asset",
         {{"Model", std::string("Model-X")}}}};
    pldm::responder::fru_parser::FruRecordInfos recordInfos{
        {PLDM_FRU_RECORD_TYPE_GENERAL,
         PLDM_FRU_ENCODING_ASCII,
         {{"xyz.openbmc_project.Inventory.Decorator.Asset", "Model", "string",
           PLDM_FRU_FIELD_TYPE_MODEL}}}};

    pldm_entity entity{64, 1, 1};
    impl.populateRecords(interfaces, recordInfos, entity);
    ASSERT_GT(impl.numRecords(), 0u);

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
                  1, 0, 0, impl.rsi + 1, PLDM_FRU_RECORD_TYPE_GENERAL,
                  PLDM_FRU_FIELD_TYPE_MODEL, PLDM_GET_FIRSTPART, msg,
                  sizeof(pldm_get_fru_record_by_option_req)),
              PLDM_SUCCESS);
    auto response = handler.getFRURecordByOption(
        msg, sizeof(pldm_get_fru_record_by_option_req));
    EXPECT_EQ(completionCode(response),
              PLDM_FRU_DATA_STRUCTURE_TABLE_UNAVAILABLE);

    pldm_entity_association_tree_destroy(entityTree);
    pldm_entity_association_tree_destroy(bmcEntityTree);
    pldm_pdr_destroy(pdrRepo);
}

TEST(FruImpl, populateRecordsReusesRecordSetIdentifierAcrossMultipleRecords)
{
    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::FruImpl impl("./fru_jsons/good",
                                  "./fru_jsons/fru_master/fru_master.json",
                                  pdrRepo, entityTree, bmcEntityTree);

    pldm::responder::dbus::InterfaceMap interfaces{
        {"xyz.openbmc_project.Inventory.Decorator.Asset",
         {{"Model", std::string("Model-Z")},
          {"SerialNumber", std::string("SN-Z")},
          {"Manufacturer", std::string("Vendor-Z")}}}};
    pldm::responder::fru_parser::FruRecordInfos recordInfos{
        {PLDM_FRU_RECORD_TYPE_GENERAL,
         PLDM_FRU_ENCODING_ASCII,
         {{"xyz.openbmc_project.Inventory.Decorator.Asset", "Model", "string",
           PLDM_FRU_FIELD_TYPE_MODEL}}},
        {PLDM_FRU_RECORD_TYPE_GENERAL,
         PLDM_FRU_ENCODING_ASCII,
         {{"xyz.openbmc_project.Inventory.Decorator.Asset", "SerialNumber",
           "string", PLDM_FRU_FIELD_TYPE_SN},
          {"xyz.openbmc_project.Inventory.Decorator.Asset", "Manufacturer",
           "string", PLDM_FRU_FIELD_TYPE_MANUFAC}}},
    };

    pldm_entity entity{64, 10, 1};
    impl.populateRecords(interfaces, recordInfos, entity);

    EXPECT_EQ(impl.numRecords(), 2u);
    EXPECT_GT(impl.numRSI(), 0u);
    EXPECT_FALSE(impl.table.empty());

    std::vector<uint8_t> fruData;
    auto rc = impl.getFRURecordByOption(fruData, 0, impl.numRSI(),
                                        PLDM_FRU_RECORD_TYPE_GENERAL,
                                        PLDM_FRU_FIELD_TYPE_SN);
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_GT(fruData.size(), sizeof(uint32_t));

    pldm_entity_association_tree_destroy(entityTree);
    pldm_entity_association_tree_destroy(bmcEntityTree);
    pldm_pdr_destroy(pdrRepo);
}

TEST(FruImpl, getFRUTableAppendsChecksumToExistingResponseCoverage)
{
    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::FruImpl impl("./fru_jsons/good",
                                  "./fru_jsons/fru_master/fru_master.json",
                                  pdrRepo, entityTree, bmcEntityTree);
    impl.table = {0x11, 0x22, 0x33, 0x44};
    impl.checksum = 0x78563412;

    pldm::responder::Response response(sizeof(pldm_msg_hdr) + 2, 0xAA);
    const auto headerSize = response.size();
    impl.getFRUTable(response);

    ASSERT_EQ(response.size(),
              headerSize + impl.table.size() + sizeof(impl.checksum));
    EXPECT_EQ(response[headerSize], 0x11);
    EXPECT_EQ(response[headerSize + 1], 0x22);
    EXPECT_EQ(response[headerSize + 2], 0x33);
    EXPECT_EQ(response[headerSize + 3], 0x44);
    EXPECT_EQ(response[headerSize + impl.table.size()], 0x12);
    EXPECT_EQ(response[headerSize + impl.table.size() + 1], 0x34);
    EXPECT_EQ(response[headerSize + impl.table.size() + 2], 0x56);
    EXPECT_EQ(response[headerSize + impl.table.size() + 3], 0x78);

    pldm_entity_association_tree_destroy(entityTree);
    pldm_entity_association_tree_destroy(bmcEntityTree);
    pldm_pdr_destroy(pdrRepo);
}

TEST(FruImpl, getFRURecordByOptionReturnsUnavailableForPrebuiltEmptyTable)
{
    auto* pdrRepo = pldm_pdr_init();
    auto* entityTree = pldm_entity_association_tree_init();
    auto* bmcEntityTree = pldm_entity_association_tree_init();

    pldm::responder::FruImpl impl("./fru_jsons/good",
                                  "./fru_jsons/fru_master/fru_master.json",
                                  pdrRepo, entityTree, bmcEntityTree);
    impl.isBuilt = true;
    impl.table.clear();
    impl.padBytes = 0;

    std::vector<uint8_t> fruData;
    EXPECT_EQ(impl.getFRURecordByOption(fruData, 0, 1,
                                        PLDM_FRU_RECORD_TYPE_GENERAL,
                                        PLDM_FRU_FIELD_TYPE_MODEL),
              PLDM_FRU_DATA_STRUCTURE_TABLE_UNAVAILABLE);

    pldm_entity_association_tree_destroy(entityTree);
    pldm_entity_association_tree_destroy(bmcEntityTree);
    pldm_pdr_destroy(pdrRepo);
}
