#pragma once

#include "libpldm/platform.h"

#include "common/types.hpp"
#include "platform-mc/numeric_effecter.hpp"
#include "platform-mc/oem_base.hpp"

#include <com/nvidia/StaticPowerHint/server.hpp>
#include <sdbusplus/server/object.hpp>
#include <sdeventplus/event.hpp>

#include <cmath>

namespace pldm
{
namespace platform_mc
{

using namespace sdbusplus;
using StaticPowerHintInft = sdbusplus::server::object_t<
    sdbusplus::com::nvidia::server::StaticPowerHint>;

class OemStaticPowerHintInft : public OemIntf, StaticPowerHintInft
{
  public:
    /** @brief Constructor to put object onto bus at a dbus path.
     *  @param[in] bus - Bus to attach to.
     *  @param[in] path - Path to attach at.
     *  @param[in] effecterPowerCpuClockFrequency - static power hint effecter.
     *  @param[in] effecterPowerTemperature - static power hint effecter.
     *  @param[in] effecterPowerWorkloadFactor - static power hint effecter.
     *  @param[in] effecterPowerEstimation - static power hint effecter.
     */
    OemStaticPowerHintInft(
        bus::bus& bus, const char* path,
        std::shared_ptr<NumericEffecter> effecterCpuClockFrequency,
        std::shared_ptr<NumericEffecter> effecterTemperature,
        std::shared_ptr<NumericEffecter> effecterWorkloadFactor,
        std::shared_ptr<NumericEffecter> effecterPowerEstimation,
        bool verbose = false) :
        StaticPowerHintInft(bus, path),
        effecterCpuClockFrequency(effecterCpuClockFrequency),
        effecterTemperature(effecterTemperature),
        effecterWorkloadFactor(effecterWorkloadFactor),
        effecterPowerEstimation(effecterPowerEstimation), verbose(verbose)
    {}

    virtual ~OemStaticPowerHintInft() = default;

    double maxCpuClockFrequency() const override
    {
        if (effecterCpuClockFrequency && effecterCpuClockFrequency->unitIntf)
        {
            return effecterCpuClockFrequency->unitIntf->pdrMaxSettable();
        }
        return 0;
    }

    double minCpuClockFrequency() const override
    {
        if (effecterCpuClockFrequency && effecterCpuClockFrequency->unitIntf)
        {
            return effecterCpuClockFrequency->unitIntf->pdrMinSettable();
        }
        return 0;
    }

    double maxTemperature() const override
    {
        if (effecterTemperature && effecterTemperature->unitIntf)
        {
            return effecterTemperature->unitIntf->pdrMaxSettable();
        }
        return 0;
    }

    double minTemperature() const override
    {
        if (effecterTemperature && effecterTemperature->unitIntf)
        {
            return effecterTemperature->unitIntf->pdrMinSettable();
        }
        return 0;
    }

    double maxWorkloadFactor() const override
    {
        if (effecterWorkloadFactor && effecterWorkloadFactor->unitIntf)
        {
            return effecterWorkloadFactor->unitIntf->pdrMaxSettable();
        }
        return 0;
    }

    double minWorkloadFactor() const override
    {
        if (effecterWorkloadFactor && effecterWorkloadFactor->unitIntf)
        {
            return effecterWorkloadFactor->unitIntf->pdrMinSettable();
        }
        return 0;
    }

    void estimatePower(double cpuClockFrequency, double workloadFactor,
                       double temperature) override
    {
        if (estimationTaskHandle)
        {
            if (!estimationTaskHandle.done())
            {
                throw sdbusplus::xyz::openbmc_project::Common::Error::
                    Unavailable();
            }
            estimationTaskHandle.destroy();
        }

        // check range
        if (cpuClockFrequency > maxCpuClockFrequency() ||
            cpuClockFrequency < minCpuClockFrequency())
        {
            throw sdbusplus::xyz::openbmc_project::Common::Error::
                InvalidArgument();
        }

        if (workloadFactor > maxWorkloadFactor() ||
            workloadFactor < minWorkloadFactor())
        {
            throw sdbusplus::xyz::openbmc_project::Common::Error::
                InvalidArgument();
        }

        if (temperature > maxTemperature() || temperature < minTemperature())
        {
            throw sdbusplus::xyz::openbmc_project::Common::Error::
                InvalidArgument();
        }

        // start task
        StaticPowerHintInft::cpuClockFrequency(cpuClockFrequency, true);
        StaticPowerHintInft::workloadFactor(workloadFactor, true);
        StaticPowerHintInft::temperature(temperature, true);
        StaticPowerHintInft::powerEstimate(0, true);
        StaticPowerHintInft::valid(false, true);
        auto co =
            estimationTask(cpuClockFrequency, workloadFactor, temperature);
        estimationTaskHandle = co.handle;
        if (estimationTaskHandle.done())
        {
            estimationTaskHandle = nullptr;
        }
    }

  private:
    requester::Coroutine estimationTask(double cpuClockFrequency,
                                        double workloadFactor,
                                        double temperature)
    {
        uint64_t t0 = 0;
        uint64_t t1 = 0;
        auto event = sdeventplus::Event::get_default();
        if (verbose)
        {
            sd_event_now(event.get(), CLOCK_MONOTONIC, &t0);
        }

        auto rc = co_await effecterCpuClockFrequency->setNumericEffecterValue(
            effecterCpuClockFrequency->baseToRaw(cpuClockFrequency));
        if (rc)
        {
            co_return PLDM_ERROR;
        }

        rc = co_await effecterWorkloadFactor->setNumericEffecterValue(
            effecterWorkloadFactor->baseToRaw(workloadFactor));
        if (rc)
        {
            co_return PLDM_ERROR;
        }

        rc = co_await effecterTemperature->setNumericEffecterValue(
            effecterTemperature->baseToRaw(temperature));
        if (rc)
        {
            co_return PLDM_ERROR;
        }

        rc = co_await effecterPowerEstimation->getNumericEffecterValue();
        if (rc)
        {
            co_return PLDM_ERROR;
        }

        if (verbose)
        {
            sd_event_now(event.get(), CLOCK_MONOTONIC, &t1);
            lg2::info("power estimate duration(us):{DELTA}", "DELTA", t1 - t0);
        }

        StaticPowerHintInft::valid(true, false);
        StaticPowerHintInft::powerEstimate(
            effecterPowerEstimation->rawToBase(
                effecterPowerEstimation->getValue()),
            false);
        co_return PLDM_SUCCESS;
    }

    std::shared_ptr<NumericEffecter> effecterCpuClockFrequency;
    std::shared_ptr<NumericEffecter> effecterTemperature;
    std::shared_ptr<NumericEffecter> effecterWorkloadFactor;
    std::shared_ptr<NumericEffecter> effecterPowerEstimation;

    std::coroutine_handle<> estimationTaskHandle;
    bool verbose;
};

} // namespace platform_mc
} // namespace pldm
