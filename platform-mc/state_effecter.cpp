#include "state_effecter.hpp"

#include "libpldm/platform.h"

#include "common/utils.hpp"
#include "platform-mc/terminus_manager.hpp"

#include <math.h>

#include <limits>
#include <regex>

namespace pldm
{
namespace platform_mc
{

StateEffecter::StateEffecter(const uint8_t tid, const bool effecterDisabled,
                             const uint16_t effecterId,
                             StateSetInfo effecterInfo,
                             std::string& effecterName,
                             std::string& associationPath,
                             TerminusManager& terminusManager) :
    tid(tid),
    effecterId(effecterId), effecterInfo(effecterInfo),
    terminusManager(terminusManager)
{
    path = "/xyz/openbmc_project/control/" + effecterName;
    path = std::regex_replace(path, std::regex("[^a-zA-Z0-9_/]+"), "_");

    auto& bus = pldm::utils::DBusHandler::getBus();
    availabilityIntf = std::make_unique<AvailabilityIntf>(bus, path.c_str());
    availabilityIntf->available(true);

    operationalStatusIntf =
        std::make_unique<OperationalStatusIntf>(bus, path.c_str());
    operationalStatusIntf->functional(!effecterDisabled);
    operationalStatusIntf->state(StateType::Starting);

    auto effecters = std::get<1>(effecterInfo);
    uint8_t idx = 0;
    for (auto& effecter : effecters)
    {
        auto stateSetId = std::get<0>(effecter);
        dbus::PathAssociation association = {"chassis", "all_controls",
                                             associationPath};
        std::string stateSetPath =
            path + "/Id_" + std::to_string(stateSets.size());
        auto stateSet = StateSetCreator::createEffecter(
            stateSetId, idx++, stateSetPath, association, this);
        if (stateSet != nullptr)
        {
            stateSets.emplace_back(std::move(stateSet));
        }
    }

    elapsedTime = 0;

    // TODO : Polling must be based on event support
    updateTime = 1000000;

    getStateEffecterStates().detach();
}

void StateEffecter::handleErrGetStateEffecterStates()
{
    availabilityIntf->available(false);
    operationalStatusIntf->functional(false);
    operationalStatusIntf->state(StateType::UnavailableOffline);
    for (auto& stateSet : stateSets)
    {
        stateSet->setDefaultValue();
    }
}

void StateEffecter::updateReading(uint8_t compEffecterIndex,
                                  pldm_effecter_oper_state effecterOperState,
                                  uint8_t pendingValue, uint8_t presentValue)
{
    bool available = true;
    bool functional = true;
    uint8_t value = 0;
    StateType state = StateType::Absent;

    switch (effecterOperState)
    {
        case EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING:
            available = true;
            functional = true;
            state = StateType::Deferring;
            value = pendingValue;
            break;
        case EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING:
            available = true;
            functional = true;
            state = StateType::Enabled;
            value = presentValue;
            break;
        case EFFECTER_OPER_STATE_DISABLED:
            available = true;
            functional = false;
            state = StateType::Disabled;
            break;
        case EFFECTER_OPER_STATE_INITIALIZING:
            available = false;
            functional = false;
            state = StateType::Starting;
            break;
        case EFFECTER_OPER_STATE_UNAVAILABLE:
        case EFFECTER_OPER_STATE_STATUSUNKNOWN:
        case EFFECTER_OPER_STATE_FAILED:
        case EFFECTER_OPER_STATE_SHUTTINGDOWN:
        case EFFECTER_OPER_STATE_INTEST:
        default:
            available = false;
            functional = false;
            state = StateType::UnavailableOffline;
            break;
    }
    availabilityIntf->available(available);
    operationalStatusIntf->functional(functional);
    operationalStatusIntf->state(state);

    if (compEffecterIndex < stateSets.size())
    {
        stateSets[compEffecterIndex]->setValue(value);
    }
    else
    {
        std::cerr << "State Effecter updateReading index out of range \n";
    }
}

requester::Coroutine StateEffecter::getStateEffecterStates()
{
    Request request(sizeof(pldm_msg_hdr) +
                    PLDM_GET_STATE_EFFECTER_STATES_REQ_BYTES);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    auto rc = encode_get_state_effecter_states_req(0, effecterId, requestMsg);
    if (rc)
    {
        std::cerr << "encode_get_state_effecter_states_req failed, TID="
                  << unsigned(tid) << ", RC=" << rc << "\n";
        co_return rc;
    }

    const pldm_msg* responseMsg = NULL;
    size_t payloadLen = 0;
    rc = co_await terminusManager.SendRecvPldmMsg(tid, request, &responseMsg,
                                                  &payloadLen);
    if (rc)
    {
        co_return rc;
    }

    uint8_t completionCode = PLDM_SUCCESS;
    uint8_t comp_effecter_count = 0;
    std::array<get_effecter_state_field, 8> stateField{};

    rc = decode_get_state_effecter_states_resp(
        responseMsg, payloadLen, &completionCode, &comp_effecter_count,
        stateField.data());
    if (rc)
    {
        std::cerr << "Failed to decode response of GetStateEffecterStates, TID="
                  << unsigned(tid) << ", RC=" << rc << "\n";
        handleErrGetStateEffecterStates();
        co_return rc;
    }

    if (completionCode != PLDM_SUCCESS)
    {
        std::cerr << "Failed to decode response of GetStateEffecterStates, TID="
                  << unsigned(tid) << ", CC=" << unsigned(completionCode)
                  << "\n";
        handleErrGetStateEffecterStates();
        co_return completionCode;
    }

    for (size_t i = 0; i < comp_effecter_count; i++)
    {
        updateReading(i,
                      static_cast<pldm_effecter_oper_state>(
                          stateField[i].effecter_op_state),
                      stateField[i].pending_state, stateField[i].present_state);
    }

    co_return completionCode;
}

requester::Coroutine StateEffecter::setStateEffecterStates(uint8_t cmpId,
                                                           uint8_t value)
{
    uint8_t cmpEffCnt = stateSets.size();

    if (cmpId >= cmpEffCnt)
    {
        std::cerr << "Request Message Error: cmpId size " << cmpId
                  << " is invalid, (Maximum " << cmpEffCnt << ") \n";
        co_return PLDM_ERROR_INVALID_DATA;
    }

    if (cmpEffCnt > PLDM_COMPOSITE_EFFECTER_MAX_COUNT ||
        cmpEffCnt < PLDM_COMPOSITE_EFFECTER_MIN_COUNT)
    {
        std::cerr << "Request Message Error: ComEffCnt size " << cmpEffCnt
                  << "is invalid\n";
        co_return PLDM_ERROR_INVALID_DATA;
    }

    std::vector<set_effecter_state_field> stateField(cmpEffCnt,
                                                     {PLDM_NO_CHANGE, 0});
    stateField[cmpId] = {PLDM_REQUEST_SET, value};

    Request request(sizeof(pldm_msg_hdr) + sizeof(effecterId) +
                    sizeof(cmpEffCnt) +
                    sizeof(set_effecter_state_field) * cmpEffCnt);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    auto rc = encode_set_state_effecter_states_req(
        0, effecterId, cmpEffCnt, stateField.data(), requestMsg);
    if (rc)
    {
        std::cerr << "encode_set_state_effecter_states_req failed, TID="
                  << unsigned(tid) << ", RC=" << rc << "\n";
        co_return rc;
    }

    const pldm_msg* responseMsg = NULL;
    size_t payloadLen = 0;
    rc = co_await terminusManager.SendRecvPldmMsg(tid, request, &responseMsg,
                                                  &payloadLen);
    if (rc)
    {
        co_return rc;
    }

    uint8_t completionCode = PLDM_SUCCESS;
    rc = decode_set_state_effecter_states_resp(responseMsg, payloadLen,
                                               &completionCode);
    if (rc != PLDM_SUCCESS || completionCode != PLDM_SUCCESS)
    {
        std::cerr << "Fail to decode response of setStateEffecterState, TID="
                  << unsigned(tid) << ", RC=" << rc
                  << ", CC=" << (int)completionCode << "\n";
        co_await getStateEffecterStates();
        co_return (rc == PLDM_SUCCESS) ? completionCode : rc;
    }

    co_await getStateEffecterStates();

    co_return completionCode;
}

} // namespace platform_mc
} // namespace pldm
