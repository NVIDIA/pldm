#include "libpldm/base.h"

#include "common/instance_id.hpp"
#include "common/test/mocked_utils.hpp"
#include "fw-update/manager.hpp"
#include "requester/handler.hpp"
#include "requester/request.hpp"
#include "test/test_instance_id.hpp"

#include <sdbusplus/timer.hpp>
#include <sdeventplus/event.hpp>

#include <array>
#include <chrono>
#include <map>
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
#include "platform-mc/manager.hpp"

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
    {
        // The unit tests drive coroutines via stdexec::sync_wait without
        // running the sd-event loop, so the retry helpers' timer::Sleep()
        // backoff would never resume. Disable retry (single attempt) so the
        // getTID/setTID failure paths return immediately instead of hanging.
        terminusManager.mctpTidDiscoveryMaxAttempts = 1;
    }

    /** Make the terminus of knownInfo (TID 0x22) known, as if discovered
     *  before while it advertised @a types.
     */
    std::shared_ptr<platform_mc::Terminus> addKnownTerminus(uint8_t types)
    {
        UUID uuid("00000000-0000-0000-0000-000000000022");
        EXPECT_TRUE(terminusManager.mapTid(knownInfo, 0x22).has_value());
        termini[0x22] = std::make_shared<platform_mc::Terminus>(
            0x22, types, uuid, terminusManager);
        return termini[0x22];
    }

    /** Rediscover knownInfo, answering GetTID and, when @a types is given,
     *  GetPLDMTypes. Without @a types, GetPLDMTypes gets no response.
     */
    void rediscover(std::optional<uint8_t> types)
    {
        auto getTidResp = makeGetTidResp(0x22);
        ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(getTidResp));
        if (types)
        {
            auto typesResp = makeGetPldmTypesResp(*types);
            ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(typesResp));
        }
        auto rc =
            stdexec::sync_wait(terminusManager.initMctpTerminus(knownInfo));
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
        EXPECT_TRUE(terminusManager.responseMsgs.empty());
    }

    static constexpr uint8_t baseTypes = 1 << PLDM_BASE;
    static constexpr uint8_t platformTypes = baseTypes | (1 << PLDM_PLATFORM);
    const MctpInfo knownInfo = MctpInfo(
        22, "12345678-0000-0000-0000-000000000022",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);

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

TEST_F(TerminusManagerPrivateTest, knownTerminusRefreshesSupportedTypes)
{
    // First discovered without Type 2, as when booted into a recovery image.
    auto terminus = addKnownTerminus(baseTypes);

    // Type 2 gained: its PDRs are still to be fetched for the first time.
    terminus->initialized = true;
    terminus->initSensorList = false;
    ASSERT_NO_FATAL_FAILURE(rediscover(platformTypes));
    EXPECT_EQ(terminus, termini[0x22]);
    EXPECT_TRUE(terminus->doesSupport(PLDM_PLATFORM));
    EXPECT_FALSE(terminus->initialized);
    EXPECT_TRUE(terminus->initSensorList);

    // As after initTerminus() and the first polling round.
    terminus->initialized = true;
    terminus->initSensorList = false;

    // GetPLDMTypes fails: the cached types and the terminus state are kept.
    ASSERT_NO_FATAL_FAILURE(rediscover(std::nullopt));
    EXPECT_TRUE(terminus->doesSupport(PLDM_PLATFORM));
    EXPECT_TRUE(terminus->initialized);
    EXPECT_FALSE(terminus->initSensorList);

    // The same types again, or a type other than Type 2 changing, leave the
    // terminus as it is.
    ASSERT_NO_FATAL_FAILURE(rediscover(platformTypes));
    ASSERT_NO_FATAL_FAILURE(
        rediscover(static_cast<uint8_t>(platformTypes | (1 << PLDM_FRU))));
    EXPECT_EQ(terminus, termini[0x22]);
    EXPECT_TRUE(terminus->doesSupport(PLDM_FRU));
    EXPECT_TRUE(terminus->initialized);
    EXPECT_FALSE(terminus->initSensorList);
    EXPECT_TRUE(terminus->resumed);
    EXPECT_FALSE(terminus->stopPolling);
}

TEST_F(TerminusManagerPrivateTest, knownTerminusFollowsPlatformSupport)
{
    auto terminus = addKnownTerminus(platformTypes);
    // As after initTerminus() and the first polling round.
    terminus->initialized = true;
    terminus->initSensorList = false;

    // Type 2 dropped: polling stops and the terminus goes offline, keeping
    // its objects.
    ASSERT_NO_FATAL_FAILURE(rediscover(baseTypes));
    EXPECT_FALSE(terminus->doesSupport(PLDM_PLATFORM));
    EXPECT_TRUE(terminus->stopPolling);
    EXPECT_FALSE(terminus->resumed);
    EXPECT_TRUE(terminus->initialized);

    // Type 2 back with objects: keep them without fetching PDRs again.
    terminus->numericSensors.push_back(nullptr);
    terminus->initialized = true;
    terminus->initSensorList = false;
    ASSERT_NO_FATAL_FAILURE(rediscover(platformTypes));
    EXPECT_EQ(terminus, termini[0x22]);
    EXPECT_TRUE(terminus->doesSupport(PLDM_PLATFORM));
    EXPECT_TRUE(terminus->initialized);
    EXPECT_TRUE(terminus->initSensorList);
    // No Manager here to restart polling/resume after discovery.
    EXPECT_FALSE(terminus->resumed);
    EXPECT_TRUE(terminus->stopPolling);

    // Remove the placeholder before setOffline() touches sensor objects.
    terminus->numericSensors.clear();
    ASSERT_NO_FATAL_FAILURE(rediscover(baseTypes));
    EXPECT_FALSE(terminus->resumed);

    // Type 2 back without objects: initialize like a new terminus.
    terminus->initialized = true;
    terminus->initSensorList = false;
    ASSERT_NO_FATAL_FAILURE(rediscover(platformTypes));
    EXPECT_EQ(terminus, termini[0x22]);
    EXPECT_TRUE(terminus->doesSupport(PLDM_PLATFORM));
    EXPECT_FALSE(terminus->initialized);
    EXPECT_TRUE(terminus->initSensorList);
    EXPECT_TRUE(terminus->resumed);
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

namespace
{

MctpInfo namedMctpInfo(eid endpointEid, const std::string& name)
{
    return MctpInfo(endpointEid, "12345678-0000-0000-0000-0000000000ee",
                    "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB", 1, name,
                    "xyz.openbmc_project.MCTP.Binding.BindingTypes.USB",
                    std::nullopt);
}

/** @brief getSubtree answer: the subtree of @p subtrees for the interface
 *         asked for, none for any other. */
auto subtreeAnswer(
    std::map<std::string, pldm::utils::GetSubTreeResponse> subtrees)
{
    return [subtrees = std::move(subtrees)](
               const std::string&, int, const std::vector<std::string>& intfs) {
        if (intfs.empty())
        {
            return pldm::utils::GetSubTreeResponse{};
        }
        if (const std::map<std::string,
                           pldm::utils::GetSubTreeResponse>::const_iterator it =
                subtrees.find(intfs.front());
            it != subtrees.end())
        {
            return it->second;
        }
        return pldm::utils::GetSubTreeResponse{};
    };
}

} // namespace

TEST_F(TerminusManagerPrivateTest, loadTerminusConfigFromEmNoDevices)
{
    using ::testing::_;
    using ::testing::Throw;

    MockdBusHandler dbus;
    EXPECT_CALL(dbus, getSubtree(_, _, _))
        .WillRepeatedly(Throw(std::runtime_error("no devices")));
    const Configurations configurations{
        {"/cfg/HGX_CPU_0", namedMctpInfo(14, "HGX_CPU_0")}};
    terminusManager.loadTerminusConfigFromEm(configurations, dbus);
    EXPECT_TRUE(terminusManager.eidToTerminusConfigMap.empty());
    ::testing::Mock::VerifyAndClearExpectations(&dbus);

    // A failed GetSubTree reads as no devices, so the endpoint counts as
    // read: the next batch does not query D-Bus again for it.
    EXPECT_CALL(dbus, getSubtree(_, _, _)).Times(0);
    terminusManager.loadTerminusConfigFromEm(configurations, dbus);

    // Endpoints without a resolved name do not query D-Bus at all.
    EXPECT_CALL(dbus, getSubtree(_, _, _)).Times(0);
    MctpInfo unnamed = namedMctpInfo(15, "");
    std::get<4>(unnamed) = std::nullopt;
    terminusManager.loadTerminusConfigFromEm({{"/cfg/X", unnamed}}, dbus);
    EXPECT_TRUE(terminusManager.eidToTerminusConfigMap.empty());
}

// MctpDiscovery hands each batch of configured endpoints to the platform-mc
// Manager, which forwards it to TerminusManager::loadTerminusConfigFromEm()
// with pldmd's D-Bus. The endpoint names no device of any platform, so the
// read finds no config whatever the bus has; it still counts as read.
TEST_F(TerminusManagerPrivateTest, managerHandleConfigurationsReadsTheirConfig)
{
    pldm::fw_update::Manager fwUpdateManager(nullptr, event, reqHandler,
                                             instanceIdDb, "", false);
    platform_mc::Manager manager(event, reqHandler, instanceIdDb,
                                 fwUpdateManager);
    auto& tm = manager.getTerminusManager();

    MctpInfo unnamed = namedMctpInfo(0xF1, "");
    std::get<4>(unnamed) = std::nullopt;
    manager.handleConfigurations(
        {{"/cfg/UT_NoDevice_0", namedMctpInfo(0xF0, "UT_NoDevice_0")},
         {"/cfg/X", unnamed}});

    // The named endpoint is read; the unnamed one is not read at all.
    const std::set<std::pair<std::string, mctp_eid_t>> read{
        {"UT_NoDevice_0", 0xF0}};
    EXPECT_EQ(read, tm.emConfigEndpointsRead);
}

namespace
{

constexpr auto nvDeviceIntf = "xyz.openbmc_project.Configuration.NvDevice";
constexpr auto devicePrefixIntf =
    "xyz.openbmc_project.Configuration.DevicePrefix";

pldm::utils::GetSubTreeResponse subtreeOf(const std::vector<std::string>& paths,
                                          const std::string& intf)
{
    pldm::utils::GetSubTreeResponse subtree;
    for (const auto& path : paths)
    {
        subtree.emplace_back(
            path, std::vector<std::pair<std::string, std::vector<std::string>>>{
                      {"xyz.openbmc_project.EntityManager", {intf}}});
    }
    return subtree;
}

/** @brief getDbusPropertiesVariant answer from @p props, keyed by
 *         "<path>|<interface>"; throws for anything else, like GetAll of an
 *         interface the object does not have. */
auto propertiesAnswer(std::map<std::string, pldm::utils::PropertyMap> props)
{
    return [props = std::move(props)](const char*, const char* path,
                                      const char* intf) {
        auto it = props.find(std::string(path) + "|" + intf);
        if (it == props.end())
        {
            throw std::runtime_error("no such interface");
        }
        return it->second;
    };
}

} // namespace

TEST_F(TerminusManagerPrivateTest,
       loadTerminusConfigFromEmDerivesCpuFromNvDevice)
{
    using ::testing::_;
    using ::testing::Invoke;

    // Topology item and entity-file record of the same CPU device, a second
    // CPU of its module, a GPU of that module that gets no terminus config
    // and is no CPU to count, and the DevicePrefix record of the platform.
    const std::string topo0 = "/inventory/platform/HGX_CPU_0";
    const std::string entity0 = "/inventory/board/HGX_IRoT_CPU_0";
    const std::string topo1 = "/inventory/platform/HGX_CPU_1";
    const std::string gpu = "/inventory/platform/HGX_GPU_0";
    const std::string prefix = "/inventory/platform/Nvidia/DevicePrefix";
    const std::map<std::string, pldm::utils::PropertyMap> props{
        {topo0 + "|" + nvDeviceIntf,
         {{"Name", std::string("HGX_CPU_0")},
          {"Kind", std::string("cpu")},
          {"Instance", uint64_t{0}},
          {"EID", uint64_t{14}},
          {"ParentChassis", std::string("HGX_ProcessorModule_0")}}},
        {entity0 + "|" + nvDeviceIntf,
         {{"Name", std::string("HGX_CPU_0")},
          {"Kind", std::string("cpu")},
          {"InstanceNumber", uint64_t{0}},
          {"EID", uint64_t{14}}}},
        {topo1 + "|" + nvDeviceIntf,
         {{"Name", std::string("HGX_CPU_1")},
          {"Kind", std::string("cpu")},
          {"Instance", uint64_t{1}},
          {"EID", uint64_t{24}},
          {"ParentChassis", std::string("HGX_ProcessorModule_0")}}},
        {gpu + "|" + nvDeviceIntf,
         {{"Name", std::string("HGX_GPU_0")},
          {"Kind", std::string("gpu")},
          {"Instance", uint64_t{0}},
          {"EID", uint64_t{11}},
          {"ParentChassis", std::string("HGX_ProcessorModule_0")}}},
        {prefix + "|" + devicePrefixIntf,
         {{"Name", std::string("DevicePrefix")},
          {"Prefix", std::string("HGX_")}}}};

    MockdBusHandler dbus;
    EXPECT_CALL(dbus, getSubtree(_, _, _))
        .WillRepeatedly(Invoke(subtreeAnswer(
            {{nvDeviceIntf,
              subtreeOf({topo0, entity0, topo1, gpu}, nvDeviceIntf)},
             {devicePrefixIntf, subtreeOf({prefix}, devicePrefixIntf)}})));
    EXPECT_CALL(dbus, getDbusPropertiesVariant(_, _, _))
        .WillRepeatedly(Invoke(propertiesAnswer(props)));

    // HGX_CPU_0 is reachable over a second medium as EID 34
    terminusManager.loadTerminusConfigFromEm(
        {{"/cfg/HGX_CPU_0", namedMctpInfo(14, "HGX_CPU_0")},
         {"/cfg/HGX_CPU_0_pcie", namedMctpInfo(34, "HGX_CPU_0")},
         {"/cfg/HGX_CPU_1", namedMctpInfo(24, "HGX_CPU_1")},
         {"/cfg/HGX_GPU_0", namedMctpInfo(11, "HGX_GPU_0")}},
        dbus);

    ASSERT_EQ(3U, terminusManager.eidToTerminusConfigMap.size());
    for (auto eid : {14, 34})
    {
        const auto& c0 = terminusManager.eidToTerminusConfigMap.at(eid);
        EXPECT_EQ(0, std::get<0>(c0));
        EXPECT_EQ("ProcessorModule_0", std::get<1>(c0));
        EXPECT_EQ(std::optional<uint16_t>(0), std::get<2>(c0));
    }
    const auto& c1 = terminusManager.eidToTerminusConfigMap.at(24);
    EXPECT_EQ(1, std::get<0>(c1));
    EXPECT_EQ("ProcessorModule_0", std::get<1>(c1));
    EXPECT_EQ(std::optional<uint16_t>(1), std::get<2>(c1));
}

namespace
{

pldm::utils::PropertyMap cpuDevice(const std::string& name, uint64_t eid,
                                   uint64_t instance)
{
    return {{"Name", name},
            {"Kind", std::string("cpu")},
            {"Instance", instance},
            {"EID", eid},
            {"ParentChassis", "ProcessorModule_" + std::to_string(instance)}};
}

} // namespace

TEST_F(TerminusManagerPrivateTest,
       loadTerminusConfigFromEmReadsEachEndpointOnce)
{
    using ::testing::_;
    using ::testing::AtLeast;
    using ::testing::Invoke;

    const std::string cpu0 = "/inventory/platform/HGX_CPU_0";
    const std::string cpu1 = "/inventory/platform/HGX_CPU_1";
    std::map<std::string, pldm::utils::PropertyMap> props{
        {cpu0 + "|" + nvDeviceIntf, cpuDevice("HGX_CPU_0", 14, 0)},
        {cpu1 + "|" + nvDeviceIntf, cpuDevice("HGX_CPU_1", 24, 1)}};
    ::testing::NiceMock<MockdBusHandler> dbus;
    ON_CALL(dbus, getSubtree(_, _, _))
        .WillByDefault(Invoke(subtreeAnswer(
            {{nvDeviceIntf, subtreeOf({cpu0, cpu1}, nvDeviceIntf)}})));
    ON_CALL(dbus, getDbusPropertiesVariant(_, _, _))
        .WillByDefault(Invoke(
            [&props](const char* service, const char* path, const char* intf) {
                return propertiesAnswer(props)(service, path, intf);
            }));
    const auto config = [this](mctp_eid_t eid) {
        auto it = terminusManager.eidToTerminusConfigMap.find(eid);
        return it == terminusManager.eidToTerminusConfigMap.end()
                   ? std::string("<none>")
                   : std::get<1>(it->second);
    };

    // The first batch reads EM.
    const auto cpu0Usb = namedMctpInfo(14, "HGX_CPU_0");
    Configurations configurations{{"/cfg/HGX_CPU_0", cpu0Usb}};
    EXPECT_CALL(dbus, getSubtree(_, _, _)).Times(AtLeast(1));
    terminusManager.loadTerminusConfigFromEm(configurations, dbus);
    EXPECT_EQ("ProcessorModule_0", config(14));
    ::testing::Mock::VerifyAndClearExpectations(&dbus);

    // The same batch again does not, though HGX_CPU_0 has no terminus (its
    // SetTID failed), nor once it has one that failed to initialize.
    EXPECT_CALL(dbus, getSubtree(_, _, _)).Times(0);
    EXPECT_CALL(dbus, getDbusPropertiesVariant(_, _, _)).Times(0);
    terminusManager.loadTerminusConfigFromEm(configurations, dbus);
    ASSERT_TRUE(terminusManager.mapTid(cpu0Usb, 14).has_value());
    UUID uuid(std::get<1>(cpu0Usb));
    termini[14] = std::make_shared<platform_mc::Terminus>(
        14, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    terminusManager.loadTerminusConfigFromEm(configurations, dbus);
    ::testing::Mock::VerifyAndClearExpectations(&dbus);

    // A batch with a new name reads EM for that name only: the changed
    // NvDevice of HGX_CPU_0 is not read.
    props[cpu0 + "|" + nvDeviceIntf] = cpuDevice("HGX_CPU_0", 14, 5);
    configurations.emplace("/cfg/HGX_CPU_1", namedMctpInfo(24, "HGX_CPU_1"));
    EXPECT_CALL(dbus, getSubtree(_, _, _)).Times(AtLeast(1));
    terminusManager.loadTerminusConfigFromEm(configurations, dbus);
    EXPECT_EQ("ProcessorModule_0", config(14));
    EXPECT_EQ("ProcessorModule_1", config(24));
    ::testing::Mock::VerifyAndClearExpectations(&dbus);

    // So does a known name with a new EID: the device on a second medium.
    configurations.emplace("/cfg/HGX_CPU_0_pcie",
                           namedMctpInfo(34, "HGX_CPU_0"));
    EXPECT_CALL(dbus, getSubtree(_, _, _)).Times(AtLeast(1));
    terminusManager.loadTerminusConfigFromEm(configurations, dbus);
    EXPECT_EQ("ProcessorModule_5", config(34));
    EXPECT_EQ("ProcessorModule_0", config(14));
    ::testing::Mock::VerifyAndClearExpectations(&dbus);

    // force reads every named endpoint again.
    EXPECT_CALL(dbus, getSubtree(_, _, _)).Times(AtLeast(1));
    terminusManager.loadTerminusConfigFromEm(configurations, dbus,
                                             /*force=*/true);
    EXPECT_EQ("ProcessorModule_5", config(14));
}

namespace
{

/** @brief The config @p terminus has; an instance it does not have reads as
 *         0xFFFF. */
platform_mc::TerminusConfigEntry configOf(platform_mc::Terminus& terminus)
{
    return {terminus.getInstance().value_or(0xFFFF),
            std::string(terminus.getTerminusName().value_or("")),
            terminus.getCpuIndex()};
}

} // namespace

TEST_F(TerminusManagerPrivateTest, loadTerminusConfigFromEmAppliesLateConfigs)
{
    using ::testing::_;
    using ::testing::Invoke;

    // Two termini created before EM had their config: HGX_CPU_0 is being
    // initialized, HGX_CPU_1 is initialized with another config.
    const std::string cpu0 = "/inventory/platform/HGX_CPU_0";
    const std::string cpu1 = "/inventory/platform/HGX_CPU_1";
    const std::map<std::string, std::pair<tid_t, bool>> endpoints{
        {"HGX_CPU_0", {14, false}}, {"HGX_CPU_1", {24, true}}};
    Configurations configurations;
    for (const auto& [name, terminus] : endpoints)
    {
        const auto& [tid, initialized] = terminus;
        MctpInfo mctpInfo = namedMctpInfo(tid, name);
        configurations.emplace("/cfg/" + name, mctpInfo);
        ASSERT_TRUE(terminusManager.mapTid(mctpInfo, tid).has_value());
        UUID uuid(std::get<1>(mctpInfo));
        termini[tid] = std::make_shared<platform_mc::Terminus>(
            tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
        termini[tid]->initialized = initialized;
    }
    termini[24]->setTerminusName("Old_1");
    termini[24]->setInstance(5);
    termini[24]->setCpuIndex(6);

    // Both are CPU NvDevices.
    std::map<std::string, pldm::utils::PropertyMap> props{
        {cpu0 + "|" + nvDeviceIntf, cpuDevice("HGX_CPU_0", 14, 0)},
        {cpu1 + "|" + nvDeviceIntf, cpuDevice("HGX_CPU_1", 24, 1)}};
    ::testing::NiceMock<MockdBusHandler> dbus;
    ON_CALL(dbus, getSubtree(_, _, _))
        .WillByDefault(Invoke(subtreeAnswer(
            {{nvDeviceIntf, subtreeOf({cpu0, cpu1}, nvDeviceIntf)}})));
    ON_CALL(dbus, getDbusPropertiesVariant(_, _, _))
        .WillByDefault(Invoke(
            [&props](const char* service, const char* path, const char* intf) {
                return propertiesAnswer(props)(service, path, intf);
            }));

    terminusManager.loadTerminusConfigFromEm(configurations, dbus);

    // The terminus being initialized gets the NvDevice config. An
    // initialized terminus keeps its config until pldmd restarts; the new
    // one is stored for then.
    const platform_mc::TerminusConfigEntry fromNvDevice0{0, "ProcessorModule_0",
                                                         0};
    EXPECT_EQ(fromNvDevice0, terminusManager.eidToTerminusConfigMap.at(14));
    EXPECT_EQ(fromNvDevice0, configOf(*termini[14]));
    EXPECT_EQ(platform_mc::TerminusConfigEntry(1, "ProcessorModule_1", 0),
              terminusManager.eidToTerminusConfigMap.at(24));
    const platform_mc::TerminusConfigEntry old1{5, "Old_1", 6};
    EXPECT_EQ(old1, configOf(*termini[24]));

    // The NvDevice of HGX_CPU_0 is published again with another module. The
    // next batch has no new endpoint and does not read EM; the
    // InterfacesAdded signal of the NvDevice reloads the config of the
    // terminus it names, which is still being initialized.
    props[cpu0 + "|" + nvDeviceIntf] = cpuDevice("HGX_CPU_0", 14, 2);
    EXPECT_CALL(dbus, getSubtree(_, _, _)).Times(0);
    terminusManager.loadTerminusConfigFromEm(configurations, dbus);
    EXPECT_EQ(fromNvDevice0, configOf(*termini[14]));
    ::testing::Mock::VerifyAndClearExpectations(&dbus);

    EXPECT_CALL(dbus, getSubtree(_, _, _)).Times(::testing::AtLeast(1));
    terminusManager.reloadTerminusConfig(14, dbus);
    const platform_mc::TerminusConfigEntry late0{2, "ProcessorModule_2", 0};
    EXPECT_EQ(late0, terminusManager.eidToTerminusConfigMap.at(14));
    EXPECT_EQ(late0, configOf(*termini[14]));
    ::testing::Mock::VerifyAndClearExpectations(&dbus);

    // The initialized terminus is not reloaded.
    EXPECT_CALL(dbus, getSubtree(_, _, _)).Times(0);
    terminusManager.reloadTerminusConfig(24, dbus);
    EXPECT_EQ(old1, configOf(*termini[24]));
}

TEST_F(TerminusManagerPrivateTest, reloadTerminusConfigReadsTheEndpointDevice)
{
    using ::testing::_;
    using ::testing::Invoke;

    const std::string cpu0 = "/inventory/platform/HGX_CPU_0";
    bool published = false;
    ::testing::NiceMock<MockdBusHandler> dbus;
    ON_CALL(dbus, getSubtree(_, _, _))
        .WillByDefault(
            Invoke([&published, cpu0](const std::string& path, int depth,
                                      const std::vector<std::string>& intfs) {
                const std::vector<std::string> devices =
                    published ? std::vector{cpu0} : std::vector<std::string>{};
                return subtreeAnswer(
                    {{nvDeviceIntf,
                      subtreeOf(devices, nvDeviceIntf)}})(path, depth, intfs);
            }));
    ON_CALL(dbus, getDbusPropertiesVariant(_, _, _))
        .WillByDefault(Invoke(propertiesAnswer(
            {{cpu0 + "|" + nvDeviceIntf, cpuDevice("HGX_CPU_0", 14, 0)}})));

    // Nothing to read EM for: a TID without an endpoint, and an endpoint
    // without a name.
    MctpInfo unnamed = namedMctpInfo(15, "");
    std::get<4>(unnamed) = std::nullopt;
    ASSERT_TRUE(terminusManager.mapTid(unnamed, 15).has_value());
    EXPECT_CALL(dbus, getSubtree(_, _, _)).Times(0);
    terminusManager.reloadTerminusConfig(14, dbus);
    terminusManager.reloadTerminusConfig(15, dbus);
    EXPECT_TRUE(terminusManager.eidToTerminusConfigMap.empty());
    ::testing::Mock::VerifyAndClearExpectations(&dbus);

    // The terminus of HGX_CPU_0 is being initialized: its NvDevice applies,
    // also when the endpoint was read before, by the batch that created the
    // terminus and that met none.
    const MctpInfo mctpInfo = namedMctpInfo(14, "HGX_CPU_0");
    ASSERT_TRUE(terminusManager.mapTid(mctpInfo, 14).has_value());
    UUID uuid(std::get<1>(mctpInfo));
    termini[14] = std::make_shared<platform_mc::Terminus>(
        14, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    const Configurations configurations{{"/cfg/HGX_CPU_0", mctpInfo}};
    EXPECT_CALL(dbus, getSubtree(_, _, _)).Times(::testing::AtLeast(1));
    terminusManager.loadTerminusConfigFromEm(configurations, dbus);
    EXPECT_TRUE(terminusManager.eidToTerminusConfigMap.empty());
    ::testing::Mock::VerifyAndClearExpectations(&dbus);

    published = true;
    EXPECT_CALL(dbus, getSubtree(_, _, _)).Times(0);
    terminusManager.loadTerminusConfigFromEm(configurations, dbus);
    EXPECT_TRUE(terminusManager.eidToTerminusConfigMap.empty());
    ::testing::Mock::VerifyAndClearExpectations(&dbus);

    EXPECT_CALL(dbus, getSubtree(_, _, _)).Times(::testing::AtLeast(1));
    terminusManager.reloadTerminusConfig(14, dbus);
    const platform_mc::TerminusConfigEntry device{0, "ProcessorModule_0", 0};
    EXPECT_EQ(device, terminusManager.eidToTerminusConfigMap.at(14));
    EXPECT_EQ(device, configOf(*termini[14]));
    ::testing::Mock::VerifyAndClearExpectations(&dbus);

    // Once it is initialized, a reload leaves it and EM alone.
    termini[14]->initialized = true;
    EXPECT_CALL(dbus, getSubtree(_, _, _)).Times(0);
    terminusManager.reloadTerminusConfig(14, dbus);
    EXPECT_EQ(device, configOf(*termini[14]));
    ::testing::Mock::VerifyAndClearExpectations(&dbus);

    // A TID mapped to a second endpoint of the same device, whose terminus
    // is not created yet: the config is stored for its creation.
    ASSERT_TRUE(
        terminusManager.mapTid(namedMctpInfo(34, "HGX_CPU_0"), 34).has_value());
    ASSERT_FALSE(termini.contains(34));
    EXPECT_CALL(dbus, getSubtree(_, _, _)).Times(::testing::AtLeast(1));
    terminusManager.reloadTerminusConfig(34, dbus);
    EXPECT_EQ(device, terminusManager.eidToTerminusConfigMap.at(34));
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
