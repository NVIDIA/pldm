#include "bios.hpp"

#include "libpldmresponder/utils.hpp"
#include "xyz/openbmc_project/Common/error.hpp"

#include <array>
#include <chrono>
#include <ctime>
#include <iostream>
#include <phosphor-logging/elog-errors.hpp>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace pldm
{

using namespace phosphor::logging;

using EpochTimeUS = uint64_t;

constexpr auto dbusProperties = "org.freedesktop.DBus.Properties";

namespace responder
{

namespace utils
{

void epochToBCDTime(uint64_t timeSec, uint8_t& seconds, uint8_t& minutes,
                    uint8_t& hours, uint8_t& day, uint8_t& month,
                    uint16_t& year)
{
    auto t = time_t(timeSec);
    auto time = localtime(&t);

    seconds = decimalToBcd(time->tm_sec);
    minutes = decimalToBcd(time->tm_min);
    hours = decimalToBcd(time->tm_hour);
    day = decimalToBcd(time->tm_mday);
    month =
        decimalToBcd(time->tm_mon + 1); // The number of months in the range
                                        // 0 to 11.PLDM expects range 1 to 12
    year = decimalToBcd(time->tm_year + 1900); // The number of years since 1900
}

} // namespace utils

void getDateTime(const pldm_msg_payload* request, pldm_msg* response)
{
    uint8_t seconds = 0;
    uint8_t minutes = 0;
    uint8_t hours = 0;
    uint8_t day = 0;
    uint8_t month = 0;
    uint16_t year = 0;

    constexpr auto timeInterface = "xyz.openbmc_project.Time.EpochTime";
    constexpr auto bmcTimePath = "/xyz/openbmc_project/time/bmc";
    std::variant<EpochTimeUS> value;

    auto bus = sdbusplus::bus::new_default();
    try
    {
        auto service = getService(bus, bmcTimePath, timeInterface);

        auto method = bus.new_method_call(service.c_str(), bmcTimePath,
                                          dbusProperties, "Get");
        method.append(timeInterface, "Elapsed");

        auto reply = bus.call(method);
        reply.read(value);
    }

    catch (std::exception& e)
    {
        log<level::ERR>("Error getting time", entry("PATH=%s", bmcTimePath),
                        entry("TIME INTERACE=%s", timeInterface));

        encode_get_date_time_resp(0, PLDM_ERROR, seconds, minutes, hours, day,
                                  month, year, response);
        return;
    }

    uint64_t timeUsec = std::get<EpochTimeUS>(value);

    uint64_t timeSec = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::microseconds(timeUsec))
                           .count();

    utils::epochToBCDTime(timeSec, seconds, minutes, hours, day, month, year);

    encode_get_date_time_resp(0, PLDM_SUCCESS, seconds, minutes, hours, day,
                              month, year, response);
}

} // namespace responder
} // namespace pldm