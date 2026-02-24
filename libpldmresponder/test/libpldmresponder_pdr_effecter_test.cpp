#include "common/test/mocked_utils.hpp"
#include "libpldmresponder/event_parser.hpp"
#include "libpldmresponder/pdr_numeric_effecter.hpp"
#include "libpldmresponder/pdr_state_effecter.hpp"
#include "libpldmresponder/pdr_state_sensor.hpp"
#include "libpldmresponder/pdr_utils.hpp"
#include "libpldmresponder/platform.hpp"

#include <libpldm/platform.h>

#include <sdbusplus/test/sdbus_mock.hpp>
#include <sdeventplus/event.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <gtest/gtest.h>

using namespace pldm::responder;
using namespace pldm::responder::platform;
using namespace pldm::responder::pdr;
using namespace pldm::responder::pdr_utils;
using namespace pldm::utils;

using ::testing::_;
using ::testing::AtLeast;
using ::testing::Return;
using ::testing::StrEq;
using ::testing::Throw;

namespace
{

class TemplateTestHandler
{
  public:
    uint16_t getNextEffecterId()
    {
        return ++nextEffecterId;
    }

    uint16_t getNextSensorId()
    {
        return ++nextSensorId;
    }

    void addDbusObjMaps(uint16_t id,
                        std::tuple<DbusMappings, DbusValMaps> dbusObj,
                        TypeId typeId = TypeId::PLDM_EFFECTER_ID)
    {
        if (typeId == TypeId::PLDM_SENSOR_ID)
        {
            sensorDbusObjMaps.emplace(id, std::move(dbusObj));
            return;
        }
        effecterDbusObjMaps.emplace(id, std::move(dbusObj));
    }

    const AssociatedEntityMap& getAssociateEntityMap() const
    {
        return associatedEntityMap;
    }

    AssociatedEntityMap associatedEntityMap{};
    std::map<uint16_t, std::tuple<DbusMappings, DbusValMaps>>
        effecterDbusObjMaps{};
    std::map<uint16_t, std::tuple<DbusMappings, DbusValMaps>>
        sensorDbusObjMaps{};

  private:
    uint16_t nextEffecterId{};
    uint16_t nextSensorId{};
};

} // namespace

TEST(GeneratePDRByStateEffecter, testGoodJson)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(StrEq("/foo/bar"), _))
        .Times(5)
        .WillRepeatedly(Return("foo.bar"));

    auto inPDRRepo = pldm_pdr_init();
    auto outPDRRepo = pldm_pdr_init();
    Repo outRepo(outPDRRepo);
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_effecter/good", inPDRRepo,
                    nullptr, nullptr, nullptr, nullptr, event);
    Repo inRepo(inPDRRepo);
    getRepoByType(inRepo, outRepo, PLDM_STATE_EFFECTER_PDR);

    // 2 entries
    ASSERT_EQ(outRepo.getRecordCount(), 2);

    // Check first PDR
    pdr_utils::PdrEntry e;
    auto record2 = pdr::getRecordByHandle(outRepo, 2, e);
    ASSERT_NE(record2, nullptr);
    pldm_state_effecter_pdr* pdr =
        reinterpret_cast<pldm_state_effecter_pdr*>(e.data);

    ASSERT_EQ(pdr->hdr.record_handle, 2);
    ASSERT_EQ(pdr->hdr.version, 1);
    ASSERT_EQ(pdr->hdr.type, PLDM_STATE_EFFECTER_PDR);
    ASSERT_EQ(pdr->hdr.record_change_num, 0);
    ASSERT_EQ(pdr->hdr.length, 23);

    ASSERT_EQ(pdr->terminus_handle, TERMINUS_HANDLE);
    ASSERT_EQ(pdr->effecter_id, 1);
    ASSERT_EQ(pdr->entity_type, 33);
    ASSERT_EQ(pdr->entity_instance, 0);
    ASSERT_EQ(pdr->container_id, 0);
    ASSERT_EQ(pdr->effecter_semantic_id, 0);
    ASSERT_EQ(pdr->effecter_init, PLDM_NO_INIT);
    ASSERT_EQ(pdr->has_description_pdr, false);
    ASSERT_EQ(pdr->composite_effecter_count, 2);
    state_effecter_possible_states* states =
        reinterpret_cast<state_effecter_possible_states*>(pdr->possible_states);
    ASSERT_EQ(states->state_set_id, 196);
    ASSERT_EQ(states->possible_states_size, 1);
    bitfield8_t bf1{};
    bf1.byte = 2;
    ASSERT_EQ(states->states[0].byte, bf1.byte);

    const auto& [dbusMappings1, dbusValMaps1] =
        handler.getDbusObjMaps(pdr->effecter_id);
    ASSERT_EQ(dbusMappings1[0].objectPath, "/foo/bar");

    // Check second PDR
    auto record3 = pdr::getRecordByHandle(outRepo, 3, e);
    ASSERT_NE(record3, nullptr);
    pdr = reinterpret_cast<pldm_state_effecter_pdr*>(e.data);

    ASSERT_EQ(pdr->hdr.record_handle, 3);
    ASSERT_EQ(pdr->hdr.version, 1);
    ASSERT_EQ(pdr->hdr.type, PLDM_STATE_EFFECTER_PDR);
    ASSERT_EQ(pdr->hdr.record_change_num, 0);
    ASSERT_EQ(pdr->hdr.length, 24);

    ASSERT_EQ(pdr->terminus_handle, TERMINUS_HANDLE);
    ASSERT_EQ(pdr->effecter_id, 2);
    ASSERT_EQ(pdr->entity_type, 100);
    ASSERT_EQ(pdr->entity_instance, 0);
    ASSERT_EQ(pdr->container_id, 0);
    ASSERT_EQ(pdr->effecter_semantic_id, 0);
    ASSERT_EQ(pdr->effecter_init, PLDM_NO_INIT);
    ASSERT_EQ(pdr->has_description_pdr, false);
    ASSERT_EQ(pdr->composite_effecter_count, 2);
    states =
        reinterpret_cast<state_effecter_possible_states*>(pdr->possible_states);
    ASSERT_EQ(states->state_set_id, 197);
    ASSERT_EQ(states->possible_states_size, 1);
    bf1.byte = 2;
    ASSERT_EQ(states->states[0].byte, bf1.byte);
    states = reinterpret_cast<state_effecter_possible_states*>(
        pdr->possible_states + sizeof(state_effecter_possible_states));
    ASSERT_EQ(states->state_set_id, 198);
    ASSERT_EQ(states->possible_states_size, 2);
    bitfield8_t bf2[2];
    bf2[0].byte = 38;
    bf2[1].byte = 128;
    ASSERT_EQ(states->states[0].byte, bf2[0].byte);
    ASSERT_EQ(states->states[1].byte, bf2[1].byte);

    const auto& [dbusMappings2, dbusValMaps2] =
        handler.getDbusObjMaps(pdr->effecter_id);
    ASSERT_EQ(dbusMappings2[0].objectPath, "/foo/bar");
    ASSERT_EQ(dbusMappings2[1].objectPath, "/foo/bar");

    ASSERT_THROW(handler.getDbusObjMaps(0xDEAD), std::exception);

    pldm_pdr_destroy(inPDRRepo);
    pldm_pdr_destroy(outPDRRepo);
}

TEST(GeneratePDRByNumericEffecter, testGoodJson)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(StrEq("/foo/bar"), _))
        .Times(5)
        .WillRepeatedly(Return("foo.bar"));

    auto inPDRRepo = pldm_pdr_init();
    auto outPDRRepo = pldm_pdr_init();
    Repo outRepo(outPDRRepo);
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_effecter/good", inPDRRepo,
                    nullptr, nullptr, nullptr, nullptr, event);
    Repo inRepo(inPDRRepo);
    getRepoByType(inRepo, outRepo, PLDM_NUMERIC_EFFECTER_PDR);

    // 1 entries
    ASSERT_EQ(outRepo.getRecordCount(), 1);

    // Check first PDR
    pdr_utils::PdrEntry e;
    auto record = pdr::getRecordByHandle(outRepo, 4, e);
    ASSERT_NE(record, nullptr);

    pldm_numeric_effecter_value_pdr* pdr =
        reinterpret_cast<pldm_numeric_effecter_value_pdr*>(e.data);
    EXPECT_EQ(pdr->hdr.record_handle, 4);
    EXPECT_EQ(pdr->hdr.version, 1);
    EXPECT_EQ(pdr->hdr.type, PLDM_NUMERIC_EFFECTER_PDR);
    EXPECT_EQ(pdr->hdr.record_change_num, 0);
    EXPECT_EQ(pdr->hdr.length,
              sizeof(pldm_numeric_effecter_value_pdr) - sizeof(pldm_pdr_hdr));

    EXPECT_EQ(pdr->effecter_id, 3);
    EXPECT_EQ(pdr->effecter_data_size, 4);

    const auto& [dbusMappings, dbusValMaps] =
        handler.getDbusObjMaps(pdr->effecter_id);
    EXPECT_EQ(dbusMappings[0].objectPath, "/foo/bar");
    EXPECT_EQ(dbusMappings[0].interface, "xyz.openbmc_project.Foo.Bar");
    EXPECT_EQ(dbusMappings[0].propertyName, "propertyName");
    EXPECT_EQ(dbusMappings[0].propertyType, "uint64_t");

    pldm_pdr_destroy(inPDRRepo);
    pldm_pdr_destroy(outPDRRepo);
}

TEST(GeneratePDR, testMalformedJson)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(StrEq("/foo/bar"), _))
        .Times(5)
        .WillRepeatedly(Return("foo.bar"));

    auto inPDRRepo = pldm_pdr_init();
    auto outPDRRepo = pldm_pdr_init();
    Repo outRepo(outPDRRepo);
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_effecter/good", inPDRRepo,
                    nullptr, nullptr, nullptr, nullptr, event);
    Repo inRepo(inPDRRepo);
    getRepoByType(inRepo, outRepo, PLDM_STATE_EFFECTER_PDR);

    ASSERT_EQ(outRepo.getRecordCount(), 2);
    ASSERT_THROW(pdr_utils::readJson("./pdr_jsons/state_effecter/malformed"),
                 std::exception);

    pldm_pdr_destroy(inPDRRepo);
    pldm_pdr_destroy(outPDRRepo);
}

TEST(ReadJson, coveragePaths)
{
    namespace fs = std::filesystem;

    EXPECT_THROW(pdr_utils::readJson("/tmp/pldm_readjson_missing_path_123456"),
                 std::exception);

    const auto uniqueId =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto lockedJsonPath =
        fs::current_path() / fs::path("pldm_readjson_open_fail_" +
                                      std::to_string(uniqueId) + ".json");
    std::ofstream(lockedJsonPath) << "{}";

    std::error_code ec{};
    fs::permissions(lockedJsonPath, fs::perms::none, fs::perm_options::replace,
                    ec);
    ASSERT_FALSE(ec);

    auto parsed = pdr_utils::readJson(lockedJsonPath.string());
    EXPECT_TRUE(parsed.empty());

    fs::permissions(lockedJsonPath,
                    fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
    fs::remove(lockedJsonPath, ec);
}

TEST(findStateEffecterId, goodJson)
{
    MockdBusHandler mockedUtils;
    EXPECT_CALL(mockedUtils, getService(StrEq("/foo/bar"), _))
        .Times(5)
        .WillRepeatedly(Return("foo.bar"));

    auto inPDRRepo = pldm_pdr_init();
    auto event = sdeventplus::Event::get_default();
    Handler handler(&mockedUtils, "./pdr_jsons/state_effecter/good", inPDRRepo,
                    nullptr, nullptr, nullptr, nullptr, event);
    uint16_t entityType = 33;
    uint16_t entityInstance = 0;
    uint16_t containerId = 0;
    uint16_t stateSetId = 196;
    auto effecterId = findStateEffecterId(inPDRRepo, entityType, entityInstance,
                                          containerId, stateSetId, true);
    ASSERT_EQ(effecterId, 1);
    stateSetId = 300;
    effecterId = findStateEffecterId(inPDRRepo, entityType, entityInstance,
                                     containerId, stateSetId, true);
    ASSERT_EQ(effecterId, PLDM_INVALID_EFFECTER_ID);
    pldm_pdr_destroy(inPDRRepo);
}

TEST(GeneratePDRTemplates, NumericEffecterSwitchAndCatchCoverage)
{
    MockdBusHandler mockedUtils;
    ON_CALL(mockedUtils, getService(StrEq("/foo/bar"), _))
        .WillByDefault(Return("foo.bar"));
    ON_CALL(mockedUtils, getService(StrEq("/bad/path"), _))
        .WillByDefault(Throw(std::runtime_error("missing path")));

    auto* pdrRepoRaw = pldm_pdr_init();
    Repo repo(pdrRepoRaw);
    TemplateTestHandler handler;
    handler.associatedEntityMap.emplace(
        "/xyz/openbmc_project/inventory/system/chassis", pldm_entity{33, 1, 0});

    const Json json = {
        {"entries",
         {
             {{"entity_path", "/xyz/openbmc_project/inventory/system/chassis"},
              {"effecter_data_size", PLDM_EFFECTER_DATA_SIZE_UINT8},
              {"range_field_format", PLDM_RANGE_FIELD_FORMAT_UINT8},
              {"nominal_value", 9},
              {"normal_max", 10},
              {"normal_min", 2},
              {"rated_max", 20},
              {"rated_min", 1},
              {"dbus",
               {{"path", "/foo/bar"},
                {"interface", "xyz.openbmc_project.Foo.Bar"},
                {"property_name", "property0"},
                {"property_type", "uint8_t"}}}},
             {{"entity_path", 5},
              {"type", 200},
              {"instance", 2},
              {"container", 3},
              {"effecter_data_size", PLDM_EFFECTER_DATA_SIZE_SINT8},
              {"range_field_format", PLDM_RANGE_FIELD_FORMAT_SINT8},
              {"nominal_value", -8},
              {"normal_max", 5},
              {"normal_min", -4},
              {"rated_max", 7},
              {"rated_min", -7},
              {"dbus",
               {{"path", "/foo/bar"},
                {"interface", "xyz.openbmc_project.Foo.Bar"},
                {"property_name", "property1"},
                {"property_type", "int16_t"}}}},
             {{"type", 44},
              {"instance", 5},
              {"container", 7},
              {"effecter_data_size", PLDM_EFFECTER_DATA_SIZE_UINT16},
              {"range_field_format", PLDM_RANGE_FIELD_FORMAT_UINT16},
              {"nominal_value", 100},
              {"normal_max", 120},
              {"normal_min", 50},
              {"rated_max", 200},
              {"rated_min", 20},
              {"dbus",
               {{"path", "/foo/bar"},
                {"interface", "xyz.openbmc_project.Foo.Bar"},
                {"property_name", "property2"},
                {"property_type", "uint16_t"}}}},
             {{"type", 45},
              {"instance", 6},
              {"container", 8},
              {"effecter_data_size", PLDM_EFFECTER_DATA_SIZE_SINT16},
              {"range_field_format", PLDM_RANGE_FIELD_FORMAT_SINT16},
              {"nominal_value", -20},
              {"normal_max", 20},
              {"normal_min", -20},
              {"rated_max", 40},
              {"rated_min", -40},
              {"dbus",
               {{"path", "/foo/bar"},
                {"interface", "xyz.openbmc_project.Foo.Bar"},
                {"property_name", "property3"},
                {"property_type", "int32_t"}}}},
             {{"type", 46},
              {"instance", 7},
              {"container", 9},
              {"effecter_data_size", PLDM_EFFECTER_DATA_SIZE_UINT32},
              {"range_field_format", PLDM_RANGE_FIELD_FORMAT_UINT32},
              {"nominal_value", 1000},
              {"normal_max", 1200},
              {"normal_min", 500},
              {"rated_max", 3000},
              {"rated_min", 400},
              {"dbus",
               {{"path", "/foo/bar"},
                {"interface", "xyz.openbmc_project.Foo.Bar"},
                {"property_name", "property4"},
                {"property_type", "uint64_t"}}}},
             {{"type", 47},
              {"instance", 8},
              {"container", 10},
              {"effecter_data_size", PLDM_EFFECTER_DATA_SIZE_SINT32},
              {"range_field_format", PLDM_RANGE_FIELD_FORMAT_SINT32},
              {"nominal_value", -1000},
              {"normal_max", 1200},
              {"normal_min", -500},
              {"rated_max", 3000},
              {"rated_min", -400},
              {"dbus",
               {{"path", "/foo/bar"},
                {"interface", "xyz.openbmc_project.Foo.Bar"},
                {"property_name", "property5"},
                {"property_type", "double"}}}},
             {{"type", 48},
              {"instance", 9},
              {"container", 11},
              {"effecter_data_size", PLDM_EFFECTER_DATA_SIZE_UINT8},
              {"range_field_format", PLDM_RANGE_FIELD_FORMAT_REAL32},
              {"nominal_value", 3.5},
              {"normal_max", 4.0},
              {"normal_min", 2.0},
              {"rated_max", 5.0},
              {"rated_min", 1.0},
              {"dbus",
               {{"path", "/bad/path"},
                {"interface", "xyz.openbmc_project.Bad"},
                {"property_name", "property6"},
                {"property_type", "uint8_t"}}}},
         }}};

    pdr_numeric_effecter::generateNumericEffecterPDR(mockedUtils, json, handler,
                                                     repo);
    EXPECT_EQ(repo.getRecordCount(), 7u);
    EXPECT_EQ(handler.effecterDbusObjMaps.size(), 7u);

    PdrEntry entry{};
    auto* first = repo.getFirstRecord(entry);
    ASSERT_NE(first, nullptr);
    auto* firstPdr =
        reinterpret_cast<const pldm_numeric_effecter_value_pdr*>(entry.data);
    EXPECT_EQ(firstPdr->entity_type, 33);
    EXPECT_EQ(firstPdr->entity_instance, 1);

    pldm_pdr_destroy(pdrRepoRaw);
}

TEST(GeneratePDRTemplates, StateEffecterSensorAndMappingCoverage)
{
    MockdBusHandler mockedUtils;
    ON_CALL(mockedUtils, getService(StrEq("/foo/bar"), _))
        .WillByDefault(Return("foo.bar"));
    ON_CALL(mockedUtils, getService(StrEq("/bad/path"), _))
        .WillByDefault(Throw(std::runtime_error("missing path")));

    TemplateTestHandler handler;
    handler.associatedEntityMap.emplace(
        "/xyz/openbmc_project/inventory/system/chassis", pldm_entity{64, 2, 1});

    auto* stateEffecterRepoRaw = pldm_pdr_init();
    Repo stateEffecterRepo(stateEffecterRepoRaw);
    const Json stateEffecterJson = {
        {"entries",
         {
             {{"entity_path", "/xyz/openbmc_project/inventory/system/chassis"},
              {"effecters",
               {{{"set", {{"id", 196}, {"size", 1}, {"states", {1, 3}}}},
                 {"dbus",
                  {{"path", "/foo/bar"},
                   {"interface", "xyz.openbmc_project.Example"},
                   {"property_name", "State0"},
                   {"property_type", "string"},
                   {"property_values", {"A", "B"}}}}}}}},
             {{"entity_path", 11},
              {"type", 70},
              {"instance", 9},
              {"container", 4},
              {"effecters",
               {{{"set", {{"id", 197}, {"size", 2}, {"states", {1, 2, 9}}}},
                 {"dbus",
                  {{"path", "/bad/path"},
                   {"interface", "xyz.openbmc_project.Example"},
                   {"property_name", "State1"},
                   {"property_type", "uint8_t"},
                   {"property_values", {7, 8, 9}}}}}}}},
         }}};
    pdr_state_effecter::generateStateEffecterPDR(mockedUtils, stateEffecterJson,
                                                 handler, stateEffecterRepo);
    EXPECT_EQ(stateEffecterRepo.getRecordCount(), 2u);

    auto* stateSensorRepoRaw = pldm_pdr_init();
    Repo stateSensorRepo(stateSensorRepoRaw);
    const Json stateSensorJson = {
        {"entries",
         {{{"entity_path", "/xyz/openbmc_project/inventory/system/chassis"},
           {"sensors",
            {{{"set", {{"id", 128}, {"size", 1}, {"states", {1, 2}}}},
              {"dbus",
               {{"path", "/foo/bar"},
                {"interface", "xyz.openbmc_project.Example"},
                {"property_name", "Sensor0"},
                {"property_type", "bool"},
                {"property_values", {true, false}}}}}}}}}}};
    pdr_state_sensor::generateStateSensorPDR(mockedUtils, stateSensorJson,
                                             handler, stateSensorRepo);
    EXPECT_EQ(stateSensorRepo.getRecordCount(), 1u);
    EXPECT_EQ(handler.sensorDbusObjMaps.size(), 1u);

    const auto u8Map =
        populateMapping("uint8_t", Json::array({1, 2}), PossibleValues{9, 10});
    EXPECT_EQ(u8Map.size(), 2u);
    const auto strMap = populateMapping("string", Json::array({"x", "y"}),
                                        PossibleValues{1, 2});
    EXPECT_EQ(strMap.size(), 2u);
    const auto mismatchMap =
        populateMapping("uint8_t", Json::array({1}), PossibleValues{1, 2});
    EXPECT_TRUE(mismatchMap.empty());
    const auto unknownMap = populateMapping("unsupported", Json::array({1, 2}),
                                            PossibleValues{1, 2});
    EXPECT_TRUE(unknownMap.empty());

    pldm_pdr_destroy(stateEffecterRepoRaw);
    pldm_pdr_destroy(stateSensorRepoRaw);
}

TEST(GeneratePDRTemplates, StateEffecterSensorMalformedCoverage)
{
    MockdBusHandler mockedUtils;
    TemplateTestHandler handler;
    auto* repoRaw = pldm_pdr_init();
    Repo repo(repoRaw);

    const Json badStateEffecter = {
        {"entries",
         Json::array({Json{
             {"effecters", Json::array({Json{
                               {"set", Json{{"id", 196}, {"size", 0}}}}})}}})}};
    EXPECT_THROW(pdr_state_effecter::generateStateEffecterPDR(
                     mockedUtils, badStateEffecter, handler, repo),
                 std::exception);

    const Json badStateSensor = {
        {"entries",
         Json::array({Json{
             {"sensors", Json::array({Json{
                             {"set", Json{{"id", 128}, {"size", 0}}}}})}}})}};
    EXPECT_THROW(pdr_state_sensor::generateStateSensorPDR(
                     mockedUtils, badStateSensor, handler, repo),
                 std::exception);

    Json tooManySensors = {{"entries", Json::array({Json::object()})}};
    auto& sensors = tooManySensors["entries"][0]["sensors"];
    sensors = Json::array();
    for (int i = 0; i < 9; ++i)
    {
        sensors.push_back(
            {{"set", {{"id", 128}, {"size", 1}, {"states", {1}}}},
             {"dbus",
              {{"path", "/foo/bar"},
               {"interface", "xyz.openbmc_project.Example"},
               {"property_name", "Sensor"},
               {"property_type", "uint8_t"},
               {"property_values", {1}}}}});
    }
    EXPECT_THROW(pdr_state_sensor::generateStateSensorPDR(
                     mockedUtils, tooManySensors, handler, repo),
                 std::runtime_error);

    pldm_pdr_destroy(repoRaw);
}
