#pragma once

#include "config.h"

#include "common/types.hpp"
#include "common/utils.hpp"

#include <systemd/sd-event.h>

#include <functional>
#include <string>

namespace pldm
{

namespace fw_update
{

class UpdateManager;

/** @class Watch
 *
 *  @brief Adds inotify watch on software image upload directory
 *
 *  The inotify watch is hooked up with sd-event, so that on call back,
 *  appropriate actions related to a software image upload can be taken.
 */
class Watch
{
  public:
    /** @brief ctor - hook inotify watch with sd-event
     *
     *  @param[in] loop - sd-event object
     *  @param[in] imageCallbackImmediate - The callback function for processing
     * the immdidate update image
     *  @param[in] imageCallbackSplitStage - The callback function for
     * processing the split-stage update image
     *  @param[in] updateManager
     */
    Watch(sd_event* loop,
          std::function<int(std::string&)> imageCallbackImmediate,
          std::function<int(std::string&)> imageCallbackSplitStage,
          UpdateManager* updateManager);

    Watch(const Watch&) = delete;
    Watch& operator=(const Watch&) = delete;
    Watch(Watch&&) = delete;
    Watch& operator=(Watch&&) = delete;

    /** @brief dtor - remove inotify watch and close fd's
     */
    ~Watch();
    /**
     * @brief initialize file watchers for immediate update
     *
     */
    void initImmediateUpdateWatch();

    /**
     * @brief initialize file watchers for split-stage update
     *
     */
    void initStagedUpdateWatch();

    /* time stamp for immediate and split update to handle duplicate events */
    uint64_t stateChangeTimeImmediate = 0;
    uint64_t stateChangeTimeSplitStage = 0;

  private:
    /** @brief sd-event callback for immediate update
     *
     *  @param[in] s - event source, floating (unused) in our case
     *  @param[in] fd - inotify fd
     *  @param[in] revents - events that matched for fd
     *  @param[in] userdata - pointer to Watch object
     *  @returns 0 on success, -1 on fail
     */
    static int callbackImmediate(sd_event_source* s, int fd, uint32_t revents,
                                 void* userdata);

    /** @brief sd-event callback for split-stage update
     *
     *  @param[in] s - event source, floating (unused) in our case
     *  @param[in] fd - inotify fd
     *  @param[in] revents - events that matched for fd
     *  @param[in] userdata - pointer to Watch object
     *  @returns 0 on success, -1 on fail
     */
    static int callbackSplitStaged(sd_event_source* s, int fd, uint32_t revents,
                                   void* userdata);

    /** @brief image upload directory watch descriptor Immediate update */
    int wdImmediate = -1;

    /** @brief image upload directory watch descriptor staged update */
    int wdSplitStage = -1;

    /** @brief inotify file descriptor */
    int fdImmediate = -1;
    int fdSplitStage = -1;

    /** @brief The callback function for processing the immediate update. */
    std::function<int(std::string&)> imageCallbackImmediate;

    /** @brief The callback function for processing the split stage and update
     * image. */
    std::function<int(std::string&)> imageCallbackSplitStage;

    sd_event* loop;
    /* UpdateManager object to process events related to mount points */
    UpdateManager* updateManager;

    /**
     * @brief add file watch event listener for immediate update
     *
     */
    void addFileEventWatchImmediate();

    /**
     * @brief add file watch event listener for staged update
     *
     */
    void addFileEventWatchStaged();

    /**
     * @brief checks if systemd service is completed
     *
     * @param[in] serviceName
     * @return true - if service is completed
     * @return false - if service is running or failed
     */
    bool isServiceCompleted(const std::string& serviceName);
    /**
     * @brief subscribe for service stage change events
     *
     * @param[in] serviceName
     * @param[in] imagePath
     */
    void subscribeToServiceStateChange(const std::string& serviceName,
                                       const std::string& imagePath);
    std::unique_ptr<sdbusplus::bus::match_t> immediateUpdateEvent;
    std::unique_ptr<sdbusplus::bus::match_t> stagedUpdateEvent;
};

} // namespace fw_update
} // namespace pldm
