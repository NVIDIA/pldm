#include "common/instance_id.hpp"
#include "common/utils.hpp"
#include "libpldmresponder/base.hpp"
#include "test/test_instance_id.hpp"

#include <libpldm/base.h>

#include <sdeventplus/event.hpp>

#include <array>
#include <cstring>

#include <gtest/gtest.h>

using namespace pldm::responder;

class StubOemPlatformHandler : public pldm::responder::oem_platform::Handler
{
  public:
    StubOemPlatformHandler() :
        pldm::responder::oem_platform::Handler(
            static_cast<const pldm::utils::DBusHandler*>(nullptr))
    {}

    int getOemStateSensorReadingsHandler(
        pldm::pdr::EntityType, pldm::pdr::EntityInstance,
        pldm::pdr::ContainerID, pldm::pdr::StateSetId,
        pldm::pdr::CompositeCount, uint16_t,
        std::vector<get_sensor_state_field>&) override
    {
        return PLDM_SUCCESS;
    }

    int oemSetStateEffecterStatesHandler(uint16_t, uint16_t, uint16_t, uint8_t,
                                         std::vector<set_effecter_state_field>&,
                                         uint16_t) override
    {
        return PLDM_SUCCESS;
    }

    void buildOEMPDR(pldm::responder::pdr_utils::Repo&) override {}

    void checkAndDisableWatchDog() override
    {
        ++checkAndDisableWatchDogCount;
    }

    bool watchDogRunning() override
    {
        return false;
    }

    void resetWatchDogTimer() override {}

    void disableWatchDogTimer() override {}

    void countSetEventReceiver() override
    {
        ++countSetEventReceiverCalls;
    }

    int checkBMCState() override
    {
        return PLDM_SUCCESS;
    }

    size_t checkAndDisableWatchDogCount = 0;
    size_t countSetEventReceiverCalls = 0;
};

class TestBaseCommands : public testing::Test
{
  protected:
    TestBaseCommands() : event(sdeventplus::Event::get_default()) {}

    uint8_t mctpEid = 0;
    TestInstanceIdDb instanceIdDb;
    sdeventplus::Event event;
};

TEST_F(TestBaseCommands, testPLDMTypesGoodRequest)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr)> requestPayload{};
    auto request = reinterpret_cast<pldm_msg*>(requestPayload.data());
    // payload length will be 0 in this case
    size_t requestPayloadLength = 0;
    base::Handler handler(mctpEid, instanceIdDb, event, nullptr, nullptr);
    auto response = handler.getPLDMTypes(request, requestPayloadLength);
    // Need to support OEM type.
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    uint8_t* payload_ptr = responsePtr->payload;
    ASSERT_EQ(payload_ptr[0], 0);
    ASSERT_EQ(payload_ptr[1], 29); // 0b11101 see DSP0240 table11
    ASSERT_EQ(payload_ptr[2], 0);
}

TEST_F(TestBaseCommands, testGetPLDMCommandsGoodRequest)
{
    // Need to support OEM type commands.
    std::array<uint8_t, sizeof(pldm_msg_hdr) + PLDM_GET_COMMANDS_REQ_BYTES>
        requestPayload{};
    auto request = reinterpret_cast<pldm_msg*>(requestPayload.data());
    size_t requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);
    base::Handler handler(mctpEid, instanceIdDb, event, nullptr, nullptr);
    auto response = handler.getPLDMCommands(request, requestPayloadLength);
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    uint8_t* payload_ptr = responsePtr->payload;
    ASSERT_EQ(payload_ptr[0], 0);
    ASSERT_EQ(payload_ptr[1], 60); // 60 = 0b111100
    ASSERT_EQ(payload_ptr[2], 0);
}

TEST_F(TestBaseCommands, testGetPLDMCommandsBadRequest)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr) + PLDM_GET_COMMANDS_REQ_BYTES>
        requestPayload{};
    auto request = reinterpret_cast<pldm_msg*>(requestPayload.data());

    request->payload[0] = 0xFF;
    size_t requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);
    base::Handler handler(mctpEid, instanceIdDb, event, nullptr, nullptr);
    auto response = handler.getPLDMCommands(request, requestPayloadLength);
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    uint8_t* payload_ptr = responsePtr->payload;
    ASSERT_EQ(payload_ptr[0], PLDM_ERROR_INVALID_PLDM_TYPE);
}

TEST_F(TestBaseCommands, testGetPLDMVersionGoodRequest)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr) + PLDM_GET_VERSION_REQ_BYTES>
        requestPayload{};
    auto request = reinterpret_cast<pldm_msg*>(requestPayload.data());
    size_t requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);

    uint8_t pldmType = PLDM_BASE;
    uint32_t transferHandle = 0x0;
    uint8_t flag = PLDM_GET_FIRSTPART;
    uint8_t retFlag = PLDM_START_AND_END;
    ver32_t version = {0x00, 0xF0, 0xF0, 0xF1};

    auto rc =
        encode_get_version_req(0, transferHandle, flag, pldmType, request);

    ASSERT_EQ(0, rc);

    base::Handler handler(mctpEid, instanceIdDb, event, nullptr, nullptr);
    auto response = handler.getPLDMVersion(request, requestPayloadLength);
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());

    ASSERT_EQ(responsePtr->payload[0], 0);
    ASSERT_EQ(0, memcmp(responsePtr->payload + sizeof(responsePtr->payload[0]),
                        &transferHandle, sizeof(transferHandle)));
    ASSERT_EQ(0, memcmp(responsePtr->payload + sizeof(responsePtr->payload[0]) +
                            sizeof(transferHandle),
                        &retFlag, sizeof(flag)));
    ASSERT_EQ(0, memcmp(responsePtr->payload + sizeof(responsePtr->payload[0]) +
                            sizeof(transferHandle) + sizeof(flag),
                        &version, sizeof(version)));
}

TEST_F(TestBaseCommands, testGetPLDMVersionBadRequest)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr) + PLDM_GET_VERSION_REQ_BYTES>
        requestPayload{};
    auto request = reinterpret_cast<pldm_msg*>(requestPayload.data());
    size_t requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);

    uint8_t pldmType = 7;
    uint32_t transferHandle = 0x0;
    uint8_t flag = PLDM_GET_FIRSTPART;

    auto rc =
        encode_get_version_req(0, transferHandle, flag, pldmType, request);

    ASSERT_EQ(0, rc);

    base::Handler handler(mctpEid, instanceIdDb, event, nullptr, nullptr);
    auto response = handler.getPLDMVersion(request, requestPayloadLength - 1);
    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());

    ASSERT_EQ(responsePtr->payload[0], PLDM_ERROR_INVALID_LENGTH);

    request = reinterpret_cast<pldm_msg*>(requestPayload.data());
    requestPayloadLength = requestPayload.size() - sizeof(pldm_msg_hdr);

    rc = encode_get_version_req(0, transferHandle, flag, pldmType, request);

    ASSERT_EQ(0, rc);

    response = handler.getPLDMVersion(request, requestPayloadLength);
    responsePtr = reinterpret_cast<pldm_msg*>(response.data());

    ASSERT_EQ(responsePtr->payload[0], PLDM_ERROR_INVALID_PLDM_TYPE);
}

TEST_F(TestBaseCommands, testGetTIDGoodRequest)
{
    std::array<uint8_t, sizeof(pldm_msg_hdr)> requestPayload{};
    auto request = reinterpret_cast<pldm_msg*>(requestPayload.data());
    size_t requestPayloadLength = 0;

    base::Handler handler(mctpEid, instanceIdDb, event, nullptr, nullptr);
    auto response = handler.getTID(request, requestPayloadLength);

    auto responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    uint8_t* payload = responsePtr->payload;

    ASSERT_EQ(payload[0], 0);
    ASSERT_EQ(payload[1], 1);
}
