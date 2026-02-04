/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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

#include <libpldm/pldm.h>
#include <sys/socket.h>

#include <cstdint>
#include <string>
#include <vector>

// TODO: Remove fallback definitions when sysroot kernel headers are updated

// Socket level and options
#define SOL_MCTP 285
#define MCTP_OPT_ENABLE_ERRQUEUE 2

// Error queue control message
#define MCTP_RECVERR 1

// Direction values
#define MCTP_DIR_TX 0
#define MCTP_DIR_RX 1

// Message types
#define MCTP_MSG_TYPE_PLDM 0x01

#define MCTP_BINDING_UNKNOWN                                                   \
    255 /* Used for sync API calls where binding is not applicable */

// Payload size
#define MCTP_ERROR_PAYLOAD_SIZE 32 /* Capture first 32 bytes of payload */

struct mctp_error
{
    uint32_t error_code;   // errno value
    uint8_t direction;     // MCTP_DIR_TX or MCTP_DIR_RX
    uint8_t binding;       // MCTP_BINDING_* (USB=1, I2C=2, PCIe=3)
    uint16_t reserved1;    // Padding/reserved
    uint8_t src_eid;       // Source endpoint ID
    uint8_t dest_eid;      // Destination endpoint ID
    uint8_t tag;           // MCTP tag (0-7)
    uint8_t msg_type;      // MCTP message type (0x01 for PLDM)
    uint64_t timestamp_ns; // Placeholder for timestamp
    uint16_t payload_len;  // Length of payload data
    uint16_t reserved2;    // Padding/reserved
    uint8_t payload[MCTP_ERROR_PAYLOAD_SIZE]; // Failed packet payload
    uint32_t reserved3[2];                    // Reserved for future use
} __attribute__((packed));

namespace pldm
{
namespace transport
{

// Use mctp_error structure (from kernel or fallback definition above)
using MctpError = struct mctp_error;

/**
 * @brief Read MCTP transport error from socket error queue
 *
 * This function reads transport-level error information from
 * the AF_MCTP socket's error queue using MSG_ERRQUEUE.
 *
 * @param[in] fd - Socket file descriptor
 * @param[out] error - Output MctpError structure to be filled
 * @return 0 on success, negative errno on failure
 *         -EAGAIN/-EWOULDBLOCK if no error in queue
 *         -ENOENT if no MCTP_RECVERR control message found
 */
int readMctpErrorQueue(int fd, MctpError& error);

/**
 * @brief Extract PLDM type from MCTP error payload
 *
 * Parses the PLDM header from the error payload to extract the PLDM type.
 * Returns 0xFF if not a PLDM message or payload is insufficient.
 *
 * @param[in] error - MctpError structure containing payload
 * @return PLDM type (0-63), or 0xFF if not PLDM/unknown
 */
uint8_t extractPldmType(const MctpError& error);

/**
 * @brief Create MctpError structure for immediate sendMsg failures
 *
 * Constructs an MctpError structure when sendMsg() fails immediately
 * (before queuing). This allows storing the error for later correlation
 * with timeout handlers, preventing error flooding.
 *
 * @param[in] destEid - Destination endpoint ID
 * @param[in] errorCode - errno value from sendMsg failure
 * @param[in] binding - MCTP binding type
 * @param[in] payload - Request/response payload that failed to send
 * @return MctpError structure ready for storage
 */
MctpError createMctpErrorObject(mctp_eid_t destEid, int errorCode,
                                uint8_t binding,
                                const std::vector<uint8_t>& payload);

/**
 * @brief Create a Redfish event for MCTP transport errors
 *
 * This function creates Redfish event log entries for MCTP socket transport
 * failures. It handles both synchronous API failures and asynchronous errors
 * from the MCTP error queue.
 *
 * The function automatically:
 * - Retrieves the device/component name from EID
 * - Maps error codes to appropriate Redfish message registry entries
 * - Handles device-specific error logging for certain error types
 *
 * Transport error categories:
 * - Synchronous failures: Errors returned directly from sendMsg API
 *   (e.g., EHOSTUNREACH, EINVAL, send/recv failures)
 * - Asynchronous failures: Errors from MCTP error queue via MSG_ERRQUEUE
 *   (e.g., ETIMEDOUT after packet transmission, link down notifications)
 *
 * @param[in] eid - Endpoint ID involved in the error
 * @param[in] commandName - Name of PLDM command (e.g., "RequestUpdate",
 * "GetPDR")
 * @param[in] errorCode - MCTP transport error code (errno value)
 * @param[in] binding - MCTP binding type:
 * @param[in] direction - MCTP direction:
 *                        - 0: TX (MCTP_DIR_TX)
 *                        - 1: RX (MCTP_DIR_RX)
 * @param[in] logNamespace - Namespace for the Redfish event (default:
 * "FWUpdate")
 */
void createMctpTransportRedfishEvent(
    mctp_eid_t eid, const std::string& commandName, uint32_t errorCode,
    uint8_t binding, uint8_t direction,
    const std::string& logNamespace = "FWUpdate");

} // namespace transport
} // namespace pldm
