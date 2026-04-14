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

    static auto& ref()
    {
        return static_cast<FlightRecorderAccess&>(
            FlightRecorder::GetInstance());
    }

    void reset()
    {
        index = 0;
        tapeRecorder.clear();
    }

    bool policyEnabled() const
    {
        return flightRecorderPolicy;
    }

    bool empty() const
    {
        return tapeRecorder.empty();
    }

    int currentIndex() const
    {
        return index;
    }
};

TEST(FlightRecorderDisabled, SaveRecordIsNoOpWhenPolicyDisabled)
{
    auto& recorder = FlightRecorderAccess::ref();
    recorder.reset();

    recorder.saveRecord(FlightRecorderData{0x01, 0x02, 0x03}, true);

    EXPECT_FALSE(recorder.policyEnabled());
    EXPECT_TRUE(recorder.empty());
    EXPECT_EQ(recorder.currentIndex(), 0);
}

TEST(FlightRecorderDisabled, PlayRecorderDoesNotCreateDumpFile)
{
    auto& recorder = FlightRecorderAccess::ref();
    unlink(flightRecorderDumpPath);

    recorder.playRecorder();

    EXPECT_FALSE(std::filesystem::exists(flightRecorderDumpPath));
}

} // namespace
