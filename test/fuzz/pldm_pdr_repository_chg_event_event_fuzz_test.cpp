/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION &
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
#include "libpldm/firmware_update.h"

#include "common/utils.hpp"
#include "common_utils.hpp"
#include "pldmd/dbus_impl_requester.hpp"
#include "requester/handler.hpp"

#include <cstring> // Add for memcpy and memcmp
#include <vector>

using namespace pldm;

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    // Input validation
    const uint8_t eventDataFormat = getUint8t();
    const uint8_t numberOfChangeRecords = getUint8t();
    if (numberOfChangeRecords == 0)
    {
        return -1;
    }

    // First record
    uint8_t eventDataOperation1 = PLDM_RECORDS_MODIFIED % (getUint8t() + 1);
    const uint8_t numberOfChangeEntries1 = getUint8t();
    if (numberOfChangeEntries1 == 0)
    {
        return -1;
    }

    std::vector<uint32_t> changeRecordArr1(numberOfChangeEntries1);
    for (uint8_t i = 0; i < numberOfChangeEntries1; ++i)
    { // Changed int to uint8_t
        getFromCin(&changeRecordArr1[i], sizeof(uint32_t));
    }

    // Second record
    uint8_t eventDataOperation2 = PLDM_RECORDS_MODIFIED % (getUint8t() + 1);
    const uint8_t numberOfChangeEntries2 = getUint8t();
    if (numberOfChangeEntries2 == 0)
    {
        return -1;
    }

    std::vector<uint32_t> changeRecordArr2(numberOfChangeEntries2);
    for (uint8_t i = 0; i < numberOfChangeEntries2; ++i)
    { // Changed int to uint8_t
        getFromCin(&changeRecordArr2[i], sizeof(uint32_t));
    }

    // Calculate size and allocate buffer
    auto sizeEventData =
        PLDM_PDR_REPOSITORY_CHG_EVENT_MIN_LENGTH +
        PLDM_PDR_REPOSITORY_CHANGE_RECORD_MIN_LENGTH * numberOfChangeRecords +
        (numberOfChangeEntries1 + numberOfChangeEntries2) * sizeof(uint32_t);
    std::vector<uint8_t> eventDataArr(sizeEventData);

    // Set up event data
    auto eventData =
        reinterpret_cast<struct pldm_pdr_repository_chg_event_data*>(
            eventDataArr.data());
    eventData->event_data_format = eventDataFormat;
    eventData->number_of_change_records = numberOfChangeRecords;

    // Set up first change record
    auto changeRecord1 =
        reinterpret_cast<struct pldm_pdr_repository_change_record_data*>(
            eventData->change_records);
    changeRecord1->event_data_operation = eventDataOperation1;
    changeRecord1->number_of_change_entries = numberOfChangeEntries1;
    if (changeRecordArr1.size() > 0)
    {
        std::memcpy(changeRecord1->change_entry, changeRecordArr1.data(),
                    changeRecordArr1.size() * sizeof(uint32_t));
    }

    // Set up second change record
    auto changeRecord2 =
        reinterpret_cast<struct pldm_pdr_repository_change_record_data*>(
            eventData->change_records +
            PLDM_PDR_REPOSITORY_CHANGE_RECORD_MIN_LENGTH +
            (changeRecordArr1.size() * sizeof(uint32_t)));
    changeRecord2->event_data_operation = eventDataOperation2;
    changeRecord2->number_of_change_entries = numberOfChangeEntries2;
    if (changeRecordArr2.size() > 0)
    {
        std::memcpy(changeRecord2->change_entry, changeRecordArr2.data(),
                    changeRecordArr2.size() * sizeof(uint32_t));
    }

    // Decode and verify
    uint8_t retEventDataFormat{};
    uint8_t retNumberOfChangeRecords{};
    size_t retChangeRecordDataOffset{0};

    auto rc = decode_pldm_pdr_repository_chg_event_data(
        reinterpret_cast<const uint8_t*>(eventData), eventDataArr.size(),
        &retEventDataFormat, &retNumberOfChangeRecords,
        &retChangeRecordDataOffset);
    if (rc != 0)
    {
        return rc;
    }

    // Verify first record
    const uint8_t* changeRecordData =
        reinterpret_cast<const uint8_t*>(changeRecord1);
    size_t changeRecordDataSize =
        eventDataArr.size() - PLDM_PDR_REPOSITORY_CHG_EVENT_MIN_LENGTH;
    uint8_t retEventDataOperation{};
    uint8_t retNumberOfChangeEntries{};
    size_t retChangeEntryDataOffset{};

    rc = decode_pldm_pdr_repository_change_record_data(
        changeRecordData, changeRecordDataSize, &retEventDataOperation,
        &retNumberOfChangeEntries, &retChangeEntryDataOffset);
    if (rc != 0)
    {
        return rc;
    }

    // Verify data matches
    changeRecordData += retChangeEntryDataOffset;
    if (std::memcmp(changeRecordData, changeRecordArr1.data(),
                    sizeof(uint32_t) * retNumberOfChangeEntries) != 0)
    {
        return -1;
    }

    // Verify second record
    changeRecordData += sizeof(uint32_t) * retNumberOfChangeEntries;
    changeRecordDataSize =
        changeRecordDataSize - (sizeof(uint32_t) * retNumberOfChangeEntries +
                                PLDM_PDR_REPOSITORY_CHANGE_RECORD_MIN_LENGTH);

    rc = decode_pldm_pdr_repository_change_record_data(
        changeRecordData, changeRecordDataSize, &retEventDataOperation,
        &retNumberOfChangeEntries, &retChangeEntryDataOffset);

    return rc;
}
