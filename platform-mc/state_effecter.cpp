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

StateEffecter::StateEffecter(
    const uint8_t tid, const bool effecterDisabled, const uint16_t effecterId,
    StateSetInfo effecterInfo,
    std::vector<std::vector<std::pair<std::string, std::string>>>*
        effecterNames,
    std::string& associationPath, TerminusManager& terminusManager) :
    tid(tid),
    effecterId(effecterId), effecterInfo(effecterInfo),
    terminusManager(terminusManager)
{
    path = "/xyz/openbmc_project/control/PLDM_Effecter_" +
           std::to_string(effecterId) + "_" + std::to_string(tid);

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

        std::string compositeEffecterName = "Id";
        auto compositeEffecterId = stateSets.size();
        // pick first en langTag sensor aux name
        if (effecterNames && effecterNames->size() > compositeEffecterId)
        {
            for (const auto& [tag, name] :
                 effecterNames->at(compositeEffecterId))
            {
                if (tag == "en")
                {
                    compositeEffecterName = name;
                }
            }
        }

        std::string objPath = path + "/" + compositeEffecterName + "_" +
                              std::to_string(stateSets.size());
        objPath =
            std::regex_replace(objPath, std::regex("[^a-zA-Z0-9_/]+"), "_");

        auto stateSet = StateSetCreator::createEffecter(
            stateSetId, idx++, objPath, association, this);
        if (stateSet != nullptr)
        {
            stateSets.emplace_back(std::move(stateSet));
        }
    }

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
        lg2::error(
            "State Effecter id:{EFFECTERID} updateReading index out of range.",
            "EFFECTERID", effecterId);
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
        lg2::error(
            "encode_get_state_effecter_states_req failed, tid={TID}, rc={RC}.",
            "TID", tid, "RC", rc);
        co_return rc;
    }

    const pldm_msg* responseMsg = NULL;
    size_t payloadLen = 0;
    rc = co_await terminusManager.SendRecvPldmMsg(tid, request, &responseMsg,
                                                  &payloadLen);
    if (rc)
    {
        lg2::error("getStateEffecterStates failed, tid={TID}, rc={RC}.", "TID",
                   tid, "RC", rc);
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
        lg2::error(
            "Failed to decode response of GetStateEffecterStates, tid={TID}, rc={RC}.",
            "TID", tid, "RC", rc);
        handleErrGetStateEffecterStates();
        co_return rc;
    }

    if (completionCode != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to decode response of GetStateEffecterStates, tid={TID}, rc={RC}, cc={CC}.",
            "TID", tid, "RC", rc, "CC", completionCode);
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
        lg2::error(
            "Request Message Error: cmpId size {CMPID} is invalid, (Maximum {CMPEFFCNT})",
            "CMPID", cmpId, "CMPEFFCNT", cmpEffCnt);
        co_return PLDM_ERROR_INVALID_DATA;
    }

    if (cmpEffCnt > PLDM_COMPOSITE_EFFECTER_MAX_COUNT ||
        cmpEffCnt < PLDM_COMPOSITE_EFFECTER_MIN_COUNT)
    {
        lg2::error(
            "Request Message Error: ComEffCnt size  {CMPEFFCNT} is invalid",
            "CMPEFFCNT", cmpEffCnt);
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
        lg2::error(
            "encode_set_state_effecter_states_req failed, tid={TID}, rc={RC}.",
            "TID", tid, "RC", rc);
        co_return rc;
    }

    const pldm_msg* responseMsg = NULL;
    size_t payloadLen = 0;
    rc = co_await terminusManager.SendRecvPldmMsg(tid, request, &responseMsg,
                                                  &payloadLen);
    if (rc)
    {
        lg2::error("setStateEffecterStates failed, tid={TID}, rc={RC}.", "TID",
                   tid, "RC", rc);
        co_return rc;
    }

    uint8_t completionCode = PLDM_SUCCESS;
    rc = decode_set_state_effecter_states_resp(responseMsg, payloadLen,
                                               &completionCode);
    if (rc != PLDM_SUCCESS || completionCode != PLDM_SUCCESS)
    {
        lg2::error(
            "Fail to decode response of setStateEffecterState, tid={TID}, rc={RC} cc={CC}.",
            "TID", tid, "RC", rc, "CC", completionCode);
        co_await getStateEffecterStates();
        co_return (rc == PLDM_SUCCESS) ? completionCode : rc;
    }

    co_await getStateEffecterStates();

    co_return completionCode;
}

} // namespace platform_mc
} // namespace pldm
