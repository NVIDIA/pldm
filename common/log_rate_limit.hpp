/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <chrono>
#include <unordered_map>

namespace pldm
{
namespace utils
{

/** @brief Rate-limit logging per key to avoid journal flooding when the same
 *         failure repeats (e.g. terminus timeout, sensor read failure).
 *
 *  Usage:
 *    if (rateLimiter.shouldLog(key))
 *    {
 *        lg2::error("...", ...);
 *        rateLimiter.recordLog(key);
 *    }
 *    // On success: rateLimiter.clear(key);
 *
 *  Logs at most once per key per interval. Call clear(key) when the
 *  operation succeeds so the next failure for that key is logged again.
 */
template <typename Key>
class LogRateLimiter
{
  public:
    /** @param interval Minimum time between logs per key (default 5 minutes) */
    explicit LogRateLimiter(std::chrono::seconds interval =
                                std::chrono::minutes(5)) : interval(interval)
    {}

    /** @return true if this key should be logged (first time or interval
     * elapsed) */
    bool shouldLog(const Key& key) const
    {
        auto it = lastLogTime.find(key);
        if (it == lastLogTime.end())
        {
            return true;
        }
        return std::chrono::steady_clock::now() - it->second >= interval;
    }

    /** Call after logging for key so subsequent logs are suppressed until
     * interval passes */
    void recordLog(const Key& key)
    {
        lastLogTime[key] = std::chrono::steady_clock::now();
    }

    /** Clear suppression for key (e.g. after success) only when outside the
     *  suppression window. This avoids clearing immediately after a failure
     *  when another request to the same key succeeds (e.g. one sensor fails
     *  then another on same EID succeeds), which would allow the next failure
     *  to log again before the interval has passed.
     */
    void clear(const Key& key)
    {
        auto it = lastLogTime.find(key);
        if (it == lastLogTime.end())
        {
            return;
        }
        if (std::chrono::steady_clock::now() - it->second >= interval)
        {
            lastLogTime.erase(it);
        }
    }

  private:
    std::chrono::seconds interval;
    std::unordered_map<Key, std::chrono::steady_clock::time_point> lastLogTime;
};

} // namespace utils
} // namespace pldm
