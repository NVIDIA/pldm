#pragma once

#include "libpldm/platform.h"

#include <cmath>
#include <limits>

namespace pldm
{
namespace platform_mc
{

class NumericEffecter;

/**
 * @brief NumericEffecterBaseUnit
 *
 * This class provides common APIs for all numeric effecter DBus interfaces,
 * all APIs are virtual functions that can be overridden by individual
 * interfaces.
 */
class NumericEffecterBaseUnit
{
  public:
    constexpr static pldm_effecter_oper_state EFFECTER_OPER_NO_REQ =
        EFFECTER_OPER_STATE_STATUSUNKNOWN;

    NumericEffecterBaseUnit(NumericEffecter& effecter) : effecter(effecter)
    {}

    virtual ~NumericEffecterBaseUnit() = default;

    virtual void pdrMaxSettable([[maybe_unused]] double maxValue)
    {}

    virtual void pdrMinSettable([[maybe_unused]] double minValue)
    {}

    virtual void handleGetNumericEffecterValue(
        [[maybe_unused]] pldm_effecter_oper_state effecterOperState,
        [[maybe_unused]] double pendingValue,
        [[maybe_unused]] double presentValue)
    {}

    virtual void handleErrGetNumericEffecterValue()
    {
        handleGetNumericEffecterValue(EFFECTER_OPER_STATE_FAILED, 0.0, 0.0);
    }

  protected:
    /** @brief Reference to associated NumericEffecter */
    NumericEffecter& effecter;
};

} // namespace platform_mc
} // namespace pldm
