#include "platform-mc/numeric_effecter.hpp"

#include "libpldm/platform.h"

#include "common/utils.hpp"
#include "platform-mc/numeric_effecter_power_cap.hpp"
#include "platform-mc/terminus_manager.hpp"

#include <limits>
#include <regex>

namespace pldm
{
namespace platform_mc
{

NumericEffecter::NumericEffecter(
    const tid_t tid, const bool effecterDisabled,
    std::shared_ptr<pldm_numeric_effecter_value_pdr> pdr,
    std::string& effecerName, std::string& associationPath,
    TerminusManager& terminusManager) :
    tid(tid),
    effecterId(pdr->effecter_id),
    entityInfo(ContainerID(pdr->container_id), EntityType(pdr->entity_type),
               EntityInstance(pdr->entity_instance)),
    terminusManager(terminusManager), baseUnit(pdr->base_unit)
{
    std::string reverseAssociation;
    auto& bus = pldm::utils::DBusHandler::getBus();

    path = "/xyz/openbmc_project/control/";
    path += effecerName;
    path = std::regex_replace(path, std::regex("[^a-zA-Z0-9_/]+"), "_");

    switch (baseUnit)
    {
        case PLDM_SENSOR_UNIT_WATTS:
            reverseAssociation = "power_controls";
            unitIntf = std::make_unique<NumericEffecterWattInft>(*this, bus,
                                                                 path.c_str());
            break;
        case PLDM_SENSOR_UNIT_MINUTES:
            unitIntf = std::make_unique<NumericEffecterBaseUnit>(*this);
            break;
        default:
            throw std::runtime_error(
                "baseUnit(" + std::to_string(pdr->base_unit) +
                ") of Numeric Effecter is not of supported type");
            break;
    }

    associationDefinitionsIntf =
        std::make_unique<AssociationDefinitionsInft>(bus, path.c_str());
    associationDefinitionsIntf->associations(
        {{"chassis", reverseAssociation.c_str(), associationPath.c_str()}});

    double maxValue = std::numeric_limits<double>::quiet_NaN();
    double minValue = std::numeric_limits<double>::quiet_NaN();

    switch (pdr->effecter_data_size)
    {
        case PLDM_EFFECTER_DATA_SIZE_UINT8:
            maxValue = pdr->max_set_table.value_u8;
            minValue = pdr->min_set_table.value_u8;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT8:
            maxValue = pdr->max_set_table.value_s8;
            minValue = pdr->min_set_table.value_s8;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT16:
            maxValue = pdr->max_set_table.value_u16;
            minValue = pdr->min_set_table.value_u16;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT16:
            maxValue = pdr->max_set_table.value_s16;
            minValue = pdr->min_set_table.value_s16;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT32:
            maxValue = pdr->max_set_table.value_u32;
            minValue = pdr->min_set_table.value_u32;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT32:
            maxValue = pdr->max_set_table.value_s32;
            minValue = pdr->min_set_table.value_s32;
            break;
    }

    dataSize = pdr->effecter_data_size;
    resolution = pdr->resolution;
    offset = pdr->offset;
    unitModifier = pdr->unit_modifier;

    availabilityIntf = std::make_unique<AvailabilityIntf>(bus, path.c_str());
    if (availabilityIntf)
    {
        availabilityIntf->available(true);
    }

    operationalStatusIntf =
        std::make_unique<OperationalStatusIntf>(bus, path.c_str());
    if (operationalStatusIntf)
    {
        operationalStatusIntf->functional(!effecterDisabled);
    }

    if (unitIntf)
    {
        unitIntf->pdrMaxSettable(unitToBase(maxValue));
        unitIntf->pdrMinSettable(unitToBase(minValue));
    }

    getNumericEffecterValue().detach();
}

double NumericEffecter::rawToUnit(double value)
{
    double convertedValue = value;
    convertedValue *= std::isnan(resolution) ? 1 : resolution;
    convertedValue += std::isnan(offset) ? 0 : offset;

    return convertedValue;
}

double NumericEffecter::unitToRaw(double value)
{
    if (resolution == 0)
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double convertedValue = value;
    convertedValue -= std::isnan(offset) ? 0 : offset;
    convertedValue /= std::isnan(resolution) ? 1 : resolution;

    return convertedValue;
}

double NumericEffecter::unitToBase(double value)
{
    double convertedValue = value;
    convertedValue *= std::pow(10, unitModifier);

    return convertedValue;
}

double NumericEffecter::baseToUnit(double value)
{
    double convertedValue = value;
    convertedValue *= std::pow(10, -unitModifier);

    return convertedValue;
}

void NumericEffecter::updateValue(pldm_effecter_oper_state effecterOperState,
                                  double pendingValue, double presentValue)
{
    bool available = true;
    bool functional = true;
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
    if (availabilityIntf)
    {
        availabilityIntf->available(available);
    }

    if (operationalStatusIntf)
    {
        operationalStatusIntf->functional(functional);
        operationalStatusIntf->state(state);
    }

    if (unitIntf)
    {
        unitIntf->handleGetNumericEffecterValue(effecterOperState,
                                                rawToBase(pendingValue),
                                                rawToBase(presentValue));
    }

}

void NumericEffecter::handleErrGetNumericEffecterValue()
{
    if (availabilityIntf)
    {
        availabilityIntf->available(false);
    }

    if (operationalStatusIntf)
    {
        operationalStatusIntf->functional(false);
        operationalStatusIntf->state(StateType::UnavailableOffline);
    }

    if (unitIntf)
    {
        unitIntf->handleErrGetNumericEffecterValue();
    }
}

requester::Coroutine
    NumericEffecter::setNumericEffecterEnable(pldm_effecter_oper_state state)
{
    Request request(sizeof(pldm_msg_hdr) +
                    PLDM_SET_NUMERIC_EFFECTER_ENABLE_REQ_BYTES);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    auto rc = encode_set_numeric_effecter_enable_req(0, effecterId, state,
                                                     requestMsg);
    if (rc)
    {
        lg2::error(
            "encode_set_numeric_effecter_enable_req failed, tid={TID}, rc={RC}.",
            "TID", tid, "RC", rc);
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
    rc = decode_cc_only_resp(responseMsg, payloadLen, &completionCode);
    if (rc)
    {
        lg2::error(
            "Failed to decode response of SetEffecterEnable, tid={TID}, rc={RC}.",
            "TID", tid, "RC", rc);
        co_return rc;
    }

    if (completionCode != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to decode response of SetEffecterEnable, tid={TID}, rc={RC}, cc={CC}.",
            "TID", tid, "RC", rc, "CC", completionCode);
        co_await getNumericEffecterValue();
        co_return completionCode;
    }

    co_await getNumericEffecterValue();

    co_return completionCode;
}

requester::Coroutine
    NumericEffecter::setNumericEffecterValue(double effecterValue)
{
    Request request(sizeof(pldm_msg_hdr) +
                    PLDM_SET_NUMERIC_EFFECTER_VALUE_MAX_REQ_BYTES);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    union_effecter_data_size effecterValueRaw;
    size_t payloadLength;
    switch (dataSize)
    {
        case PLDM_EFFECTER_DATA_SIZE_UINT8:
            effecterValueRaw.value_u8 = static_cast<uint8_t>(effecterValue);
            payloadLength = PLDM_SET_NUMERIC_EFFECTER_VALUE_MIN_REQ_BYTES;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT8:
            effecterValueRaw.value_s8 = static_cast<int8_t>(effecterValue);
            payloadLength = PLDM_SET_NUMERIC_EFFECTER_VALUE_MIN_REQ_BYTES;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT16:
            effecterValueRaw.value_u16 = static_cast<uint16_t>(effecterValue);
            payloadLength = PLDM_SET_NUMERIC_EFFECTER_VALUE_MIN_REQ_BYTES + 1;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT16:
            effecterValueRaw.value_s16 = static_cast<int16_t>(effecterValue);
            payloadLength = PLDM_SET_NUMERIC_EFFECTER_VALUE_MIN_REQ_BYTES + 1;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT32:
            effecterValueRaw.value_u32 = static_cast<uint32_t>(effecterValue);
            payloadLength = PLDM_SET_NUMERIC_EFFECTER_VALUE_MIN_REQ_BYTES + 3;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT32:
        default:
            effecterValueRaw.value_s32 = static_cast<int32_t>(effecterValue);
            payloadLength = PLDM_SET_NUMERIC_EFFECTER_VALUE_MIN_REQ_BYTES + 3;
            break;
    }
    auto rc = encode_set_numeric_effecter_value_req(0, effecterId, dataSize,
                                                    &effecterValueRaw.value_u8,
                                                    requestMsg, payloadLength);
    if (rc)
    {
        lg2::error(
            "encode_set_numeric_effecter_value_req failed, tid={TID}, rc={RC}.",
            "TID", tid, "RC", rc);
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
    rc = decode_set_numeric_effecter_value_resp(responseMsg, payloadLen,
                                                &completionCode);
    if (rc)
    {
        lg2::error(
            "Failed to decode response of SetEffecterValue, tid={TID}, rc={RC}.",
            "TID", tid, "RC", rc);
        co_return rc;
    }

    if (completionCode != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to decode response of SetEffecterValue, tid={TID}, cc={CC}.",
            "TID", tid, "CC", completionCode);
        co_await getNumericEffecterValue();
        co_return completionCode;
    }

    co_await getNumericEffecterValue();

    co_return completionCode;
}

requester::Coroutine NumericEffecter::getNumericEffecterValue()
{
    Request request(sizeof(pldm_msg_hdr) +
                    PLDM_GET_NUMERIC_EFFECTER_VALUE_REQ_BYTES);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    auto rc = encode_get_numeric_effecter_value_req(0, effecterId, requestMsg);
    if (rc)
    {
        lg2::error(
            "encode_get_numeric_effecter_value_req failed, tid={TID}, rc={RC}.",
            "TID", tid, "RC", rc);
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
    uint8_t effecterDataSize = PLDM_EFFECTER_DATA_SIZE_SINT32;
    uint8_t effecterOperationalState = 0;
    union_effecter_data_size pendingValueRaw;
    union_effecter_data_size presentValueRaw;
    rc = decode_get_numeric_effecter_value_resp(
        responseMsg, payloadLen, &completionCode, &effecterDataSize,
        &effecterOperationalState, reinterpret_cast<uint8_t*>(&pendingValueRaw),
        reinterpret_cast<uint8_t*>(&presentValueRaw));
    if (rc)
    {
        lg2::error(
            "Failed to decode response of getNumericEffecterValue, tid={TID}, rc={RC}.",
            "TID", tid, "RC", rc);
        handleErrGetNumericEffecterValue();
        co_return rc;
    }

    if (completionCode != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to decode response of getNumericEffecterValue, tid={TID}, cc={CC}.",
            "TID", tid, "CC", completionCode);
        handleErrGetNumericEffecterValue();
        co_return completionCode;
    }

    double pendingValue;
    double presentValue;
    switch (effecterDataSize)
    {
        case PLDM_EFFECTER_DATA_SIZE_UINT8:
            pendingValue = static_cast<double>(pendingValueRaw.value_u8);
            presentValue = static_cast<double>(presentValueRaw.value_u8);
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT8:
            pendingValue = static_cast<double>(pendingValueRaw.value_s8);
            presentValue = static_cast<double>(presentValueRaw.value_s8);
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT16:
            pendingValue = static_cast<double>(pendingValueRaw.value_u16);
            presentValue = static_cast<double>(presentValueRaw.value_u16);
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT16:
            pendingValue = static_cast<double>(pendingValueRaw.value_s16);
            presentValue = static_cast<double>(presentValueRaw.value_s16);
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT32:
            pendingValue = static_cast<double>(pendingValueRaw.value_u32);
            presentValue = static_cast<double>(presentValueRaw.value_u32);
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT32:
            pendingValue = static_cast<double>(pendingValueRaw.value_s32);
            presentValue = static_cast<double>(presentValueRaw.value_s32);
            break;
        default:
            pendingValue = std::numeric_limits<double>::quiet_NaN();
            presentValue = std::numeric_limits<double>::quiet_NaN();
            break;
    }

    updateValue(static_cast<pldm_effecter_oper_state>(effecterOperationalState),
                pendingValue, presentValue);
    co_return completionCode;
}

} // namespace platform_mc
} // namespace pldm
