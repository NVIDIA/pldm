#include "libpldm/base.h"

#include "common/instance_id.hpp"
#include "requester/handler.hpp"
#include "requester/request.hpp"
#include "test/test_instance_id.hpp"

#include <sdbusplus/timer.hpp>
#include <sdeventplus/event.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "mock_terminus_manager.hpp"
#undef private
#ifdef __clang__
#pragma clang diagnostic pop
#endif

using namespace std::chrono;
using namespace pldm;

namespace
{

std::vector<uint8_t> makeGetTidResp(uint8_t tid,
                                    uint8_t completionCode = PLDM_SUCCESS)
{
    return {0x00, PLDM_BASE, PLDM_GET_TID, completionCode, tid};
}

std::vector<uint8_t> makeSetTidResp(uint8_t completionCode = PLDM_SUCCESS)
{
    return {0x00, PLDM_BASE, PLDM_SET_TID, completionCode};
}

std::vector<uint8_t> makeGetPldmTypesResp(uint8_t supportedTypeBitmap0,
                                          uint8_t completionCode = PLDM_SUCCESS)
{
    return {0x00,
            PLDM_BASE,
            PLDM_GET_PLDM_TYPES,
            completionCode,
            supportedTypeBitmap0,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00};
}

std::vector<uint8_t> makeGetTerminusUidResp(
    const std::array<uint8_t, 16>& uid, uint8_t completionCode = PLDM_SUCCESS)
{
    std::vector<uint8_t> response(
        sizeof(pldm_msg_hdr) + PLDM_GET_TERMINUS_UID_RESP_BYTES, 0);
    auto* responseMsg = reinterpret_cast<pldm_msg*>(response.data());
    responseMsg->hdr.type = PLDM_PLATFORM;
    responseMsg->hdr.command = PLDM_GET_TERMINUS_UID;
    responseMsg->payload[0] = completionCode;
    memcpy(responseMsg->payload + 1, uid.data(), uid.size());
    return response;
}

class TerminusManagerPrivateTest : public testing::Test
{
  protected:
    TerminusManagerPrivateTest() :
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        terminusManager(event, reqHandler, instanceIdDb, termini, 0x08, nullptr)
    {}

    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    requester::Handler<requester::Request> reqHandler;
    platform_mc::MockTerminusManager terminusManager;
    std::map<tid_t, std::shared_ptr<platform_mc::Terminus>> termini;
};

TEST_F(TerminusManagerPrivateTest, privateProtocolDecodeCoverage)
{
    tid_t tid = 0;

    std::vector<uint8_t> shortGetTidResp{0x00, PLDM_BASE, PLDM_GET_TID,
                                         PLDM_SUCCESS};
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(shortGetTidResp));
    auto shortTidRc =
        stdexec::sync_wait(terminusManager.getTidOverMctp(12, tid));
    ASSERT_TRUE(shortTidRc.has_value());
    EXPECT_NE(PLDM_SUCCESS, std::get<0>(*shortTidRc));

    auto ccGetTidResp = makeGetTidResp(0x22, PLDM_ERROR);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(ccGetTidResp));
    auto ccTidRc = stdexec::sync_wait(terminusManager.getTidOverMctp(12, tid));
    ASSERT_TRUE(ccTidRc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*ccTidRc));

    auto okGetTidResp = makeGetTidResp(0x22);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(okGetTidResp));
    auto okTidRc = stdexec::sync_wait(terminusManager.getTidOverMctp(12, tid));
    ASSERT_TRUE(okTidRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*okTidRc));
    EXPECT_EQ(0x22, tid);

    const MctpInfo mctpInfo(
        12, "12345678-0000-0000-0000-000000000022",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
    ASSERT_TRUE(terminusManager.mapTid(mctpInfo, tid).has_value());

    std::vector<uint8_t> shortSetTidResp{0x00, PLDM_BASE, PLDM_SET_TID,
                                         PLDM_SUCCESS, 0x00};
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(shortSetTidResp));
    auto shortSetRc =
        stdexec::sync_wait(terminusManager.setTidOverMctp(12, tid));
    ASSERT_TRUE(shortSetRc.has_value());
    EXPECT_NE(PLDM_SUCCESS, std::get<0>(*shortSetRc));

    auto ccSetTidResp = makeSetTidResp(PLDM_ERROR);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(ccSetTidResp));
    auto ccSetRc = stdexec::sync_wait(terminusManager.setTidOverMctp(12, tid));
    ASSERT_TRUE(ccSetRc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*ccSetRc));

    auto okSetTidResp = makeSetTidResp();
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(okSetTidResp));
    auto okSetRc = stdexec::sync_wait(terminusManager.setTidOverMctp(12, tid));
    ASSERT_TRUE(okSetRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*okSetRc));

    uint64_t supportedTypes = 0;
    std::vector<uint8_t> shortTypesResp{0x00, PLDM_BASE, PLDM_GET_PLDM_TYPES,
                                        PLDM_SUCCESS};
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(shortTypesResp));
    auto shortTypesRc =
        stdexec::sync_wait(terminusManager.getPLDMTypes(tid, supportedTypes));
    ASSERT_TRUE(shortTypesRc.has_value());
    EXPECT_NE(PLDM_SUCCESS, std::get<0>(*shortTypesRc));

    auto ccTypesResp = makeGetPldmTypesResp(
        static_cast<uint8_t>(1 << PLDM_PLATFORM), PLDM_ERROR);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(ccTypesResp));
    auto ccTypesRc =
        stdexec::sync_wait(terminusManager.getPLDMTypes(tid, supportedTypes));
    ASSERT_TRUE(ccTypesRc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*ccTypesRc));

    auto okTypesResp = makeGetPldmTypesResp(
        static_cast<uint8_t>((1 << PLDM_BASE) | (1 << PLDM_PLATFORM)));
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(okTypesResp));
    auto okTypesRc =
        stdexec::sync_wait(terminusManager.getPLDMTypes(tid, supportedTypes));
    ASSERT_TRUE(okTypesRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*okTypesRc));
    EXPECT_TRUE((supportedTypes & (1 << PLDM_BASE)) != 0);
    EXPECT_TRUE((supportedTypes & (1 << PLDM_PLATFORM)) != 0);

    const std::array<uint8_t, 16> uidBytes{
        0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    UUID uuid{"00000000-0000-0000-0000-000000000000"};
    auto invalidLengthUidResp = makeGetTerminusUidResp(uidBytes);
    invalidLengthUidResp.push_back(0x00);
    ASSERT_EQ(PLDM_SUCCESS,
              terminusManager.enqueueResponse(invalidLengthUidResp));
    auto invalidLengthUidRc =
        stdexec::sync_wait(terminusManager.getTerminusUID(tid, uuid));
    ASSERT_TRUE(invalidLengthUidRc.has_value());
    EXPECT_NE(PLDM_SUCCESS, std::get<0>(*invalidLengthUidRc));

    auto unsupportedUidResp =
        makeGetTerminusUidResp(uidBytes, PLDM_ERROR_UNSUPPORTED_PLDM_CMD);
    ASSERT_EQ(PLDM_SUCCESS,
              terminusManager.enqueueResponse(unsupportedUidResp));
    auto unsupportedUidRc =
        stdexec::sync_wait(terminusManager.getTerminusUID(tid, uuid));
    ASSERT_TRUE(unsupportedUidRc.has_value());
    EXPECT_EQ(PLDM_ERROR_UNSUPPORTED_PLDM_CMD, std::get<0>(*unsupportedUidRc));

    auto okUidResp = makeGetTerminusUidResp(uidBytes);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(okUidResp));
    auto okUidRc =
        stdexec::sync_wait(terminusManager.getTerminusUID(tid, uuid));
    ASSERT_TRUE(okUidRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*okUidRc));
    EXPECT_EQ("12345678-9abc-def0-1122-334455667788", uuid);
}

TEST_F(TerminusManagerPrivateTest, initMctpTerminusInternalCoverage)
{
    const MctpInfo reservedTidInfo(
        21, "12345678-0000-0000-0000-000000000021",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);

    auto reservedGetTidResp = makeGetTidResp(PLDM_TID_RESERVED);
    ASSERT_EQ(PLDM_SUCCESS,
              terminusManager.enqueueResponse(reservedGetTidResp));
    auto initReservedRc =
        stdexec::sync_wait(terminusManager.initMctpTerminus(reservedTidInfo));
    ASSERT_TRUE(initReservedRc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*initReservedRc));
    EXPECT_EQ(std::nullopt, terminusManager.toTid(reservedTidInfo));
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.clearQueuedResponses());

    const MctpInfo failureInfo(
        24, "12345678-0000-0000-0000-000000000024",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);

    auto failureGetTidResp = makeGetTidResp(0);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(failureGetTidResp));
    auto failureSetTidResp = makeSetTidResp(PLDM_ERROR);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(failureSetTidResp));
    auto initFailRc =
        stdexec::sync_wait(terminusManager.initMctpTerminus(failureInfo));
    ASSERT_TRUE(initFailRc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*initFailRc));
    EXPECT_EQ(std::nullopt, terminusManager.toTid(failureInfo));
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.clearQueuedResponses());

    const MctpInfo existingInfo(
        22, "12345678-0000-0000-0000-000000000022",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
    std::string existingUuid("00000000-0000-0000-0000-000000000022");
    ASSERT_TRUE(terminusManager.mapTid(existingInfo, 0x22).has_value());
    termini[0x22] = std::make_shared<platform_mc::Terminus>(
        0x22, 1 << PLDM_BASE, existingUuid, terminusManager);

    auto existingGetTidResp = makeGetTidResp(0x22);
    ASSERT_EQ(PLDM_SUCCESS,
              terminusManager.enqueueResponse(existingGetTidResp));
    auto existingSetTidResp = makeSetTidResp();
    ASSERT_EQ(PLDM_SUCCESS,
              terminusManager.enqueueResponse(existingSetTidResp));
    auto initExistingRc =
        stdexec::sync_wait(terminusManager.initMctpTerminus(existingInfo));
    ASSERT_TRUE(initExistingRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*initExistingRc));
    EXPECT_EQ(existingUuid, termini[0x22]->getUuid());
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.clearQueuedResponses());

    const MctpInfo configuredInfo(
        23, "12345678-0000-0000-0000-000000000023",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
    terminusManager.eidToTerminusConfigMap[23] = std::make_tuple(
        7, std::string("ProcessorModule_7"), std::optional<uint16_t>{});

    auto configuredGetTidResp = makeGetTidResp(0);
    ASSERT_EQ(PLDM_SUCCESS,
              terminusManager.enqueueResponse(configuredGetTidResp));
    auto configuredSetTidResp = makeSetTidResp();
    ASSERT_EQ(PLDM_SUCCESS,
              terminusManager.enqueueResponse(configuredSetTidResp));
    auto configuredTypesResp =
        makeGetPldmTypesResp(static_cast<uint8_t>(1 << PLDM_BASE));
    ASSERT_EQ(PLDM_SUCCESS,
              terminusManager.enqueueResponse(configuredTypesResp));
    auto initConfiguredRc =
        stdexec::sync_wait(terminusManager.initMctpTerminus(configuredInfo));
    ASSERT_TRUE(initConfiguredRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*initConfiguredRc));
    ASSERT_NE(termini.end(), termini.find(23));
    ASSERT_NE(nullptr, termini[23]);
    EXPECT_FALSE(termini[23]->doesSupport(PLDM_PLATFORM));
    ASSERT_TRUE(termini[23]->getTerminusName().has_value());
    EXPECT_EQ("ProcessorModule_7", termini[23]->getTerminusName().value());
    ASSERT_TRUE(termini[23]->getInstance().has_value());
    EXPECT_EQ(7, termini[23]->getInstance().value());
    EXPECT_EQ("12345678-0000-0000-0000-000000000023", termini[23]->getUuid());
}

TEST_F(TerminusManagerPrivateTest, mappingLookupAdditionalCoverage)
{
    constexpr tid_t tid = 0x31;
    const MctpInfo mctpInfo(
        0x31, "12345678-0000-0000-0000-000000000031",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);

    ASSERT_TRUE(terminusManager.mapTid(mctpInfo, tid).has_value());

    auto mappedTid = terminusManager.toTid(mctpInfo);
    ASSERT_TRUE(mappedTid.has_value());
    EXPECT_EQ(tid, mappedTid.value());

    terminusManager.transportLayerTable[tid] =
        static_cast<SupportedTransportLayer>(1);
    EXPECT_EQ(std::nullopt, terminusManager.toMctpInfo(tid));
}

TEST_F(TerminusManagerPrivateTest, discoverMctpTerminusStateCoverage)
{
    auto& [scope,
           rcOpt] = terminusManager.discoverMctpTerminusTaskHandle.emplace();
    (void)scope;
    EXPECT_FALSE(rcOpt.has_value());

    const MctpInfos queuedInfos{MctpInfo(
        0x32, "12345678-0000-0000-0000-000000000032",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt)};
    terminusManager.discoverMctpTerminus(queuedInfos);
    EXPECT_EQ(1u, terminusManager.queuedMctpInfos.size());
    EXPECT_TRUE(terminusManager.discoverMctpTerminusTaskHandle.has_value());
    EXPECT_FALSE(
        terminusManager.discoverMctpTerminusTaskHandle->second.has_value());

    terminusManager.discoverMctpTerminusTaskHandle.reset();
    terminusManager.queuedMctpInfos = {};
    terminusManager.queuedMctpInfos.emplace(MctpInfos{});

    auto taskRc =
        stdexec::sync_wait(terminusManager.discoverMctpTerminusTask());
    ASSERT_TRUE(taskRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*taskRc));
    EXPECT_TRUE(terminusManager.queuedMctpInfos.empty());
}

TEST_F(TerminusManagerPrivateTest, sendRecvPldmMsgErrorPathsCoverage)
{
    const pldm_msg* responseMsg = nullptr;
    size_t responseLen = 0;
    Request request(sizeof(pldm_msg_hdr));

    auto unmappedRc = stdexec::sync_wait(terminusManager.SendRecvPldmMsg(
        0x33, request, &responseMsg, &responseLen));
    ASSERT_TRUE(unmappedRc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*unmappedRc));

    terminusManager.tidPool[0x34] = true;
    terminusManager.transportLayerTable[0x34] = SupportedTransportLayer::MCTP;

    auto missingInfoRc = stdexec::sync_wait(terminusManager.SendRecvPldmMsg(
        0x34, request, &responseMsg, &responseLen));
    ASSERT_TRUE(missingInfoRc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*missingInfoRc));
}

TEST_F(TerminusManagerPrivateTest, loadStaticTerminusConfigValidationCoverage)
{
    namespace fs = std::filesystem;

    const auto configPath =
        fs::temp_directory_path() / "terminus_manager_private_validation.json";
    {
        std::ofstream out(configPath);
        out << R"({
  "PLDMTermini": [
    {"EID": 49, "Name": "CPU49", "TerminusName": "", "Instance": 0},
    {"EID": 255, "Name": "BadEid", "TerminusName": "ProcessorModule_bad_eid", "Instance": 1},
    {"EID": 50, "Name": "", "TerminusName": "", "Instance": 1},
    {"EID": 51, "Name": "BadInstance", "TerminusName": "ProcessorModule_bad_inst", "Instance": -1}
  ]
})";
    }

    terminusManager.loadStaticTerminusConfig(configPath.string());
    EXPECT_TRUE(terminusManager.eidToTerminusConfigMap.empty());

    fs::remove(configPath);
}

TEST_F(TerminusManagerPrivateTest, loadStaticTerminusConfigEmptyTerminusName)
{
    namespace fs = std::filesystem;

    const auto configPath =
        fs::temp_directory_path() / "terminus_manager_private_empty_name.json";
    {
        std::ofstream out(configPath);
        out << R"({
  "PLDMTermini": [
    {"EID": 52, "Name": "CPU52", "TerminusName": "", "Instance": 7}
  ]
})";
    }

    terminusManager.loadStaticTerminusConfig(configPath.string());
    EXPECT_TRUE(terminusManager.eidToTerminusConfigMap.empty());

    fs::remove(configPath);
}

TEST_F(TerminusManagerPrivateTest, discoverMctpTerminusCompletedHandleCoverage)
{
    auto& [scope,
           rcOpt] = terminusManager.discoverMctpTerminusTaskHandle.emplace();
    (void)scope;
    rcOpt.emplace(PLDM_SUCCESS);

    ASSERT_EQ(PLDM_SUCCESS, terminusManager.clearQueuedResponses());

    terminusManager.discoverMctpTerminus(MctpInfos{});

    ASSERT_TRUE(terminusManager.discoverMctpTerminusTaskHandle.has_value());
    EXPECT_TRUE(
        terminusManager.discoverMctpTerminusTaskHandle->second.has_value());
    EXPECT_EQ(PLDM_SUCCESS,
              terminusManager.discoverMctpTerminusTaskHandle->second.value());
    EXPECT_TRUE(terminusManager.queuedMctpInfos.empty());
}

TEST_F(TerminusManagerPrivateTest, mappingPredicateShortCircuitCoverage)
{
    const MctpInfo mappedInfo(
        0x41, "12345678-0000-0000-0000-000000000041",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
    ASSERT_TRUE(terminusManager.mapTid(mappedInfo, 0x41).has_value());

    const MctpInfo differentMedium(
        0x41, "12345678-0000-0000-0000-000000000041",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.USB", std::nullopt);
    EXPECT_EQ(std::nullopt, terminusManager.toTid(differentMedium));

    const MctpInfo busyTidInfo(
        0x41, "12345678-0000-0000-0000-000000000142",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SPI", 2, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.SPI", std::nullopt);
    EXPECT_EQ(std::nullopt, terminusManager.mapTid(busyTidInfo));
}

TEST_F(TerminusManagerPrivateTest, mappingPredicateNetworkMismatchCoverage)
{
    const MctpInfo mappedInfo(
        0x45, "12345678-0000-0000-0000-000000000045",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
    ASSERT_TRUE(terminusManager.mapTid(mappedInfo, 0x45).has_value());

    const MctpInfo differentNetwork(
        0x45, "12345678-0000-0000-0000-000000000045",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 2, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
    EXPECT_EQ(std::nullopt, terminusManager.mapTid(differentNetwork));
}

TEST_F(TerminusManagerPrivateTest, mappingPredicateBindingMismatchCoverage)
{
    const MctpInfo mappedInfo(
        0x46, "12345678-0000-0000-0000-000000000046",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
    ASSERT_TRUE(terminusManager.mapTid(mappedInfo, 0x46).has_value());

    const MctpInfo differentBinding(
        0x46, "12345678-0000-0000-0000-000000000046",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.Serial", std::nullopt);
    EXPECT_EQ(std::nullopt, terminusManager.mapTid(differentBinding));
}

TEST_F(TerminusManagerPrivateTest, preferredBindingUpgradeCoverage)
{
    const MctpInfo slowerBinding(
        0x47, "12345678-0000-0000-0000-000000000047",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.Serial", std::nullopt);
    const MctpInfo fasterBinding(
        0x48, "12345678-0000-0000-0000-000000000047",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);

    auto tid = terminusManager.mapTid(slowerBinding);
    ASSERT_TRUE(tid.has_value());

    auto upgradedTid = terminusManager.mapTid(fasterBinding);
    ASSERT_TRUE(upgradedTid.has_value());
    EXPECT_EQ(tid, upgradedTid);

    auto mappedInfo = terminusManager.toMctpInfo(tid.value());
    ASSERT_TRUE(mappedInfo.has_value());
    EXPECT_EQ(fasterBinding, mappedInfo.value());
}

TEST_F(TerminusManagerPrivateTest, unmapTidMissingEntryCoverage)
{
    constexpr tid_t tid = 0x42;
    terminusManager.tidPool[tid] = true;

    EXPECT_NO_THROW(terminusManager.unmapTid(tid));
    EXPECT_FALSE(terminusManager.tidPool[tid]);
    EXPECT_EQ(terminusManager.transportLayerTable.end(),
              terminusManager.transportLayerTable.find(tid));
    EXPECT_EQ(terminusManager.mctpInfoTable.end(),
              terminusManager.mctpInfoTable.find(tid));
}

TEST_F(TerminusManagerPrivateTest, unmapTidZeroEarlyReturnCoverage)
{
    constexpr tid_t tid = 0;
    terminusManager.tidPool[tid] = true;
    terminusManager.transportLayerTable[tid] = SupportedTransportLayer::MCTP;
    terminusManager.mctpInfoTable.emplace(
        tid, MctpInfo(0x50, "12345678-0000-0000-0000-000000000050",
                      "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1,
                      std::nullopt,
                      "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe",
                      std::nullopt));

    EXPECT_NO_THROW(terminusManager.unmapTid(tid));
    EXPECT_TRUE(terminusManager.tidPool[tid]);
    EXPECT_NE(terminusManager.transportLayerTable.end(),
              terminusManager.transportLayerTable.find(tid));
    EXPECT_NE(terminusManager.mctpInfoTable.end(),
              terminusManager.mctpInfoTable.find(tid));
}

TEST_F(TerminusManagerPrivateTest, unmapTidReservedEarlyReturnCoverage)
{
    constexpr tid_t tid = PLDM_TID_RESERVED;
    terminusManager.tidPool[tid] = true;
    terminusManager.transportLayerTable[tid] = SupportedTransportLayer::MCTP;
    terminusManager.mctpInfoTable.emplace(
        tid, MctpInfo(0x51, "12345678-0000-0000-0000-000000000051",
                      "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1,
                      std::nullopt,
                      "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe",
                      std::nullopt));

    EXPECT_NO_THROW(terminusManager.unmapTid(tid));
    EXPECT_TRUE(terminusManager.tidPool[tid]);
    EXPECT_NE(terminusManager.transportLayerTable.end(),
              terminusManager.transportLayerTable.find(tid));
    EXPECT_NE(terminusManager.mctpInfoTable.end(),
              terminusManager.mctpInfoTable.find(tid));
}

} // namespace
