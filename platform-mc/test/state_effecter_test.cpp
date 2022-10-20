
#include "libpldm/entity.h"

#include "platform-mc/state_effecter.hpp"
#include "platform-mc/terminus.hpp"
#include "platform-mc/terminus_manager.hpp"

#include <gtest/gtest.h>

using namespace pldm::platform_mc;

class TestStateEffecter : public ::testing::Test
{
  public:
    TestStateEffecter() :
        bus(pldm::utils::DBusHandler::getBus()),
        event(sdeventplus::Event::get_default()),
        dbusImplRequester(bus, "/xyz/openbmc_project/pldm"),
        reqHandler(event, dbusImplRequester, sockManager, false, seconds(1), 2,
                   milliseconds(100)),
        terminusManager(event, reqHandler, dbusImplRequester, termini, 0x8,
                        nullptr)
    {}

    static const auto& getStateSets(const StateEffecter& stateEffecter)
    {
        return stateEffecter.stateSets;
    }

    sdbusplus::bus::bus& bus;
    sdeventplus::Event event;
    pldm::dbus_api::Requester dbusImplRequester;
    pldm::mctp_socket::Manager sockManager;
    pldm::requester::Handler<pldm::requester::Request> reqHandler;
    pldm::platform_mc::TerminusManager terminusManager;
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
};

TEST_F(TestStateEffecter, verifyStateEffecter)
{
    uint16_t sensorId = 0x0810;
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                     // record handle
        0x1,                     // PDRHeaderVersion
        PLDM_STATE_EFFECTER_PDR, // PDRType
        0x0,
        0x0, // recordChangeNumber
        0,
        0x13, // dataLength
        0,
        0, // PLDMTerminusHandle
        static_cast<uint8_t>(sensorId & 0xFF),
        static_cast<uint8_t>((sensorId >> 8) & 0xFF), // effecterID=0x0810
        PLDM_ENTITY_PROC_IO_MODULE,
        0, // entityType=Processor I/O Module (81)
        1,
        0, // entityInstanceNumber
        0x1,
        0x0, // containerID=1
        0x0,
        0x0,          // effecterSematicID
        PLDM_NO_INIT, // effecterInit
        false,        // effecterDescriptionPDR
        1,            // compositeSensorCount
        static_cast<uint8_t>(PLDM_STATESET_ID_LINKSTATE & 0xFF), //
        static_cast<uint8_t>((PLDM_STATESET_ID_LINKSTATE >> 8) &
                             0xFF), // stateSetID (33)
        0x1,                        // possibleStatesSize
        0x7                         // possibleStates
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    auto stateEffecterPdr = t1.stateEffecterPdrs[0];
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.stateEffecterPdrs.size());

    auto stateEffecter = t1.stateEffecters[0];
    EXPECT_EQ(sensorId, stateEffecter->effecterId);

    auto& stateSets = TestStateEffecter::getStateSets(*stateEffecter);
    EXPECT_EQ(PLDM_STATESET_ID_LINKSTATE, stateSets[0]->getStateSetId());

    // Should be true
    stateEffecter->updateReading(0,
                                 EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING,
                                 0,
                                 PLDM_STATESET_LINK_STATE_CONNECTED);
    EXPECT_EQ(true, stateSets[0]->getValue());

    // Test with invalid composite index
    stateEffecter->updateReading(4,
                                 EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING,
                                 0,
                                 PLDM_STATESET_LINK_STATE_DISCONNECTED);
    EXPECT_EQ(true, stateSets[0]->getValue());

    // Should be false
    stateEffecter->updateReading(0,
                                 EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING,
                                 0,
                                 PLDM_STATESET_LINK_STATE_DISCONNECTED);
    EXPECT_EQ(false, stateSets[0]->getValue());
}
