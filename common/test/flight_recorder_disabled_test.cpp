// Exercise the disabled-policy path separately from pldm_utils_test.
#ifdef FLIGHT_RECORDER_MAX_ENTRIES
#undef FLIGHT_RECORDER_MAX_ENTRIES
#endif
#define FLIGHT_RECORDER_MAX_ENTRIES 0

#include "common/flight_recorder.hpp"

#include <unistd.h>

#include <filesystem>

#include <gtest/gtest.h>

namespace
{

using namespace pldm::flightrecorder;

struct FlightRecorderAccess : FlightRecorder
{
    FlightRecorderAccess() = delete;

    // Base-member pointers allow access without downcasting the singleton.
    static constexpr auto indexMember = &FlightRecorderAccess::index;
    static constexpr auto tapeRecorderMember =
        &FlightRecorderAccess::tapeRecorder;
    static constexpr auto policyMember =
        &FlightRecorderAccess::flightRecorderPolicy;
};

TEST(FlightRecorderDisabled, SaveRecordIsNoOpWhenPolicyDisabled)
{
    auto& recorder = FlightRecorder::GetInstance();
    recorder.*FlightRecorderAccess::indexMember = 0;
    (recorder.*FlightRecorderAccess::tapeRecorderMember).clear();

    recorder.saveRecord(FlightRecorderData{0x01, 0x02, 0x03}, true);

    EXPECT_FALSE(recorder.*FlightRecorderAccess::policyMember);
    EXPECT_TRUE((recorder.*FlightRecorderAccess::tapeRecorderMember).empty());
    EXPECT_EQ(recorder.*FlightRecorderAccess::indexMember, 0);
}

TEST(FlightRecorderDisabled, PlayRecorderDoesNotCreateDumpFile)
{
    auto& recorder = FlightRecorder::GetInstance();
    unlink(flightRecorderDumpPath);

    recorder.playRecorder();

    EXPECT_FALSE(std::filesystem::exists(flightRecorderDumpPath));
}

} // namespace
