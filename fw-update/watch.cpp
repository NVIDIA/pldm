#include "watch.hpp"

#include <sys/inotify.h>
#include <unistd.h>

#include <phosphor-logging/lg2.hpp>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace pldm
{

namespace fw_update
{

// using namespace phosphor::logging;
using namespace std::string_literals;
namespace fs = std::filesystem;

Watch::Watch(sd_event* loop,
             std::function<int(std::string&)> imageCallbackImmediate,
             std::function<int(std::string&)> imageCallbackSplitStage) :
    imageCallbackImmediate(imageCallbackImmediate),
    imageCallbackSplitStage(imageCallbackSplitStage)
{
    initializeImmediateUpdateWatch(loop);
    initializeStagedUpdateWatch(loop);
}

Watch::~Watch()
{
    if (-1 != fdImmediate)
    {
        if (-1 != wd)
        {
            inotify_rm_watch(fdImmediate, wd);
        }
        close(fdImmediate);
    }

    if (-1 != fdSplitStage)
    {
        if (-1 != wd)
        {
            inotify_rm_watch(fdSplitStage, wd);
        }
        close(fdSplitStage);
    }
}

int Watch::callbackImmediate(sd_event_source* /* s */, int fd, uint32_t revents,
                             void* userdata)
{
    if (!(revents & EPOLLIN))
    {
        return 0;
    }

    constexpr auto maxBytes = 1024;
    uint8_t buffer[maxBytes];
    auto bytes = read(fd, buffer, maxBytes);
    if (0 > bytes)
    {
        auto error = errno;
        throw std::runtime_error("failed to read inotify event, errno="s +
                                 std::strerror(error));
    }

    auto offset = 0;
    while (offset < bytes)
    {
        auto event = reinterpret_cast<inotify_event*>(&buffer[offset]);
        if ((event->mask & IN_CLOSE_WRITE) && !(event->mask & IN_ISDIR))
        {
            auto tarballPath =
                std::string{FIRMWARE_PACKAGE_STAGING_DIR} + '/' + event->name;
            auto rc = static_cast<Watch*>(userdata)->imageCallbackImmediate(
                tarballPath);
            if (rc < 0)
            {
                // log<level::ERR>("Error processing image",
                //                 entry("IMAGE=%s", tarballPath.c_str()));
            }
        }

        offset += offsetof(inotify_event, name) + event->len;
        if (0 >= offset) // check to ensure tainted input from buffer
        {
            break;
        }
    }

    return 0;
}

int Watch::callbackSplitStaged(sd_event_source* /* s */, int fd,
                               uint32_t revents, void* userdata)
{
    if (!(revents & EPOLLIN))
    {
        return 0;
    }

    constexpr auto maxBytes = 1024;
    uint8_t buffer[maxBytes];
    auto bytes = read(fd, buffer, maxBytes);
    if (0 > bytes)
    {
        auto error = errno;
        throw std::runtime_error("failed to read inotify event, errno="s +
                                 std::strerror(error));
    }

    auto offset = 0;
    while (offset < bytes)
    {
        auto event = reinterpret_cast<inotify_event*>(&buffer[offset]);
        if ((event->mask & IN_CLOSE_WRITE) && !(event->mask & IN_ISDIR))
        {
            auto tarballPath = std::string{FIRMWARE_PACKAGE_SPLIT_STAGING_DIR} +
                               '/' + event->name;
            auto rc = static_cast<Watch*>(userdata)->imageCallbackSplitStage(
                tarballPath);
            if (rc < 0)
            {
                // log<level::ERR>("Error processing image",
                //                 entry("IMAGE=%s", tarballPath.c_str()));
            }
        }

        offset += offsetof(inotify_event, name) + event->len;
        if (0 >= offset) // check to ensure tainted input from buffer
        {
            break;
        }
    }

    return 0;
}

void Watch::initializeImmediateUpdateWatch(sd_event* loop)
{
    fs::path imgDirPath(FIRMWARE_PACKAGE_STAGING_DIR);
    if (!fs::is_directory(imgDirPath))
    {
        fs::create_directories(imgDirPath);
    }

    fdImmediate = inotify_init1(IN_NONBLOCK);
    if (-1 == fdImmediate)
    {
        // Store a copy of errno, because the string creation below will
        // invalidate errno due to one more system calls.
        auto error = errno;
        throw std::runtime_error("inotify_init1 failed, errno="s +
                                 std::strerror(error));
    }

    wd = inotify_add_watch(fdImmediate, FIRMWARE_PACKAGE_STAGING_DIR,
                           IN_CLOSE_WRITE);
    if (-1 == wd)
    {
        auto error = errno;
        close(fdImmediate);
        throw std::runtime_error("inotify_add_watch failed, errno="s +
                                 std::strerror(error));
    }

    auto rc = sd_event_add_io(loop, nullptr, fdImmediate, EPOLLIN,
                              callbackImmediate, this);
    if (0 > rc)
    {
        throw std::runtime_error("failed to add to event loop, rc="s +
                                 std::strerror(-rc));
    }
}

void Watch::initializeStagedUpdateWatch(sd_event* loop)
{
    fs::path imgSplitStageDirPath(FIRMWARE_PACKAGE_SPLIT_STAGING_DIR);

    if (fs::is_directory(imgSplitStageDirPath))
    {
        fdSplitStage = inotify_init1(IN_NONBLOCK);
        if (-1 == fdSplitStage)
        {
            // Store a copy of errno, because the string creation below will
            // invalidate errno due to one more system calls.
            auto error = errno;
            throw std::runtime_error("inotify_init1 failed, errno="s +
                                     std::strerror(error));
        }

        wd = inotify_add_watch(fdSplitStage, FIRMWARE_PACKAGE_SPLIT_STAGING_DIR,
                               IN_CLOSE_WRITE);
        if (-1 == wd)
        {
            auto error = errno;
            close(fdSplitStage);
            throw std::runtime_error("inotify_add_watch failed, errno="s +
                                     std::strerror(error));
        }

        auto rc = sd_event_add_io(loop, nullptr, fdSplitStage, EPOLLIN,
                             callbackSplitStaged, this);
        if (0 > rc)
        {
            throw std::runtime_error("failed to add to event loop, rc="s +
                                     std::strerror(-rc));
        }
    }
    else
    {
        if (!imgSplitStageDirPath.empty())
        {
            lg2::error("Invalid path for staged directory: {STAGE_DIR_PATH}",
                       "STAGE_DIR_PATH", imgSplitStageDirPath);
        }
    }
}
} // namespace fw_update
} // namespace pldm
