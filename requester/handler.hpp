#pragma once

#include "common/instance_id.hpp"
#include "common/log_rate_limit.hpp"
#include "common/mctp_error_handling.hpp"
#include "common/transport.hpp"
#include "common/types.hpp"
#include "request.hpp"

#include <libpldm/base.h>
#include <sys/socket.h>

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/async.hpp>
#include <sdbusplus/timer.hpp>
#include <sdeventplus/event.hpp>
#include <sdeventplus/source/event.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <tuple>
#include <unordered_map>
#include <vector>

PHOSPHOR_LOG2_USING;

namespace pldm
{
namespace requester
{
/** @struct RequestKey
 *
 *  RequestKey uniquely identifies the PLDM request message to match it with the
 *  response and a combination of MCTP endpoint ID, PLDM instance ID, PLDM type
 *  and PLDM command is the key.
 */
struct RequestKey
{
    mctp_eid_t eid;     //!< MCTP endpoint ID
    uint8_t instanceId; //!< PLDM instance ID
    uint8_t type;       //!< PLDM type
    uint8_t command;    //!< PLDM command

    bool operator==(const RequestKey& e) const
    {
        return ((eid == e.eid) && (instanceId == e.instanceId) &&
                (type == e.type) && (command == e.command));
    }
};

/** @struct RequestKeyHasher
 *
 *  This is a simple hash function, since the instance ID generator API
 *  generates unique instance IDs for MCTP endpoint ID.
 */
struct RequestKeyHasher
{
    std::size_t operator()(const RequestKey& key) const
    {
        return (key.eid << 24 | key.instanceId << 16 | key.type << 8 |
                key.command);
    }
};

using ResponseHandler = std::function<void(
    mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen)>;

/** @brief The response from SendRecvMsg with coroutine API
 *
 *  The response when registers PLDM request message using the SendRecvMsg
 *  with coroutine API.
 *  Responded tuple includes <CompleteCode, ResponseMgs, ResponseMsgLength>
 *  Value: [PLDM_ERROR, _, _] if registerRequest fails.
 *         [PLDM_ERROR_NOT_READY, nullptr, 0] if timed out.
 *         [PLDM_SUCCESS, ResponseMsg, ResponseMsgLength] if succeeded
 */
using SendRecvCoResp = std::tuple<int, const pldm_msg*, size_t>;

/** @struct RegisteredRequest
 *
 *  This struct is used to store the registered request to one endpoint.
 */
struct RegisteredRequest
{
    RequestKey key;                  //!< Responder MCTP endpoint ID
    std::vector<uint8_t> reqMsg;     //!< Request messages queue
    ResponseHandler responseHandler; //!< Waiting for response flag
};

/** @struct EndpointMessageQueue
 *
 *  This struct is used to save the list of request messages of one endpoint and
 *  the existing of the request message to the endpoint with its' EID.
 */
struct EndpointMessageQueue
{
    mctp_eid_t eid; //!< Responder MCTP endpoint ID
    std::deque<std::shared_ptr<RegisteredRequest>> requestQueue; //!< Queue
    bool activeRequest; //!< Waiting for response flag

    bool operator==(const mctp_eid_t& mctpEid) const
    {
        return (eid == mctpEid);
    }
};

/** @struct TransportErrorInfo
 *
 *  This struct stores MCTP transport error information for an endpoint.
 *  Used to distinguish between timeout (no response) and transport failure
 *  (e.g., device unreachable, link down).
 */
struct TransportErrorInfo
{
    uint32_t errorCode;   //!< errno from MCTP (ETIMEDOUT, EHOSTUNREACH, etc.)
    uint8_t binding;      //!< MCTP binding type (USB, I2C, PCIe)
    uint8_t direction;    //!< TX (0) or RX (1)
    uint8_t srcEid;       //!< Source EID
    uint8_t destEid;      //!< Destination EID
    uint64_t timestampNs; //!< When error occurred (nanoseconds)
    uint8_t pldmType;     //!< PLDM Type from request (0xFF if not PLDM/unknown)
    std::vector<uint8_t> requestPayload; //!< Request payload bytes from error
};

/** @class Handler
 *
 *  This class handles the lifecycle of the PLDM request message based on the
 *  instance ID expiration interval, number of request retries and the timeout
 *  waiting for a response. The registered response handlers are invoked with
 *  response once the PLDM responder sends the response. If no response is
 *  received within the instance ID expiration interval or any other failure the
 *  response handler is invoked with the empty response.
 *
 * @tparam RequestInterface - Request class type
 */
template <class RequestInterface>
class Handler
{
  public:
    Handler() = delete;
    Handler(const Handler&) = delete;
    Handler(Handler&&) = delete;
    Handler& operator=(const Handler&) = delete;
    Handler& operator=(Handler&&) = delete;
    ~Handler() = default;

    /** @brief Constructor
     *
     *  @param[in] pldmTransport - PLDM transport object
     *  @param[in] event - reference to PLDM daemon's main event loop
     *  @param[in] instanceIdDb - reference to an InstanceIdDb
     *  @param[in] verbose - verbose tracing flag
     *  @param[in] instanceIdExpiryInterval - instance ID expiration interval
     *  @param[in] numRetries - number of request retries
     *  @param[in] responseTimeOut - time to wait between each retry
     */
    explicit Handler(
        PldmTransport* pldmTransport, sdeventplus::Event& event,
        pldm::InstanceIdDb& instanceIdDb, bool verbose,
        std::chrono::seconds instanceIdExpiryInterval = std::chrono::seconds(5),
        uint8_t numRetries = 2,
        std::chrono::milliseconds responseTimeOut =
            std::chrono::milliseconds(RESPONSE_TIME_OUT)) :
        pldmTransport(pldmTransport), event(event), instanceIdDb(instanceIdDb),
        verbose(verbose), instanceIdExpiryInterval(instanceIdExpiryInterval),
        numRetries(numRetries), responseTimeOut(responseTimeOut)
    {}

    void instanceIdExpiryCallBack(RequestKey key)
    {
        auto eid = key.eid;
        if (this->handlers.contains(key))
        {
            bool shouldLog = eidTimeoutLogLimiter.shouldLog(eid);
            if (shouldLog)
            {
                eidTimeoutLogLimiter.recordLog(eid);
                info(
                    "Instance ID expiry for EID '{EID}' using InstanceID '{INSTANCEID}'",
                    "EID", key.eid, "INSTANCEID", key.instanceId);
            }
            auto& [request, responseHandler,
                   timerInstance] = this->handlers[key];
            request->stop();
            auto rc = timerInstance->stop();
            if (rc)
            {
                error(
                    "Failed to stop the instance ID expiry timer, response code '{RC}'",
                    "RC", rc);
            }

            if (shouldLog)
            {
                auto transportErrorIt = transportErrors.find(eid);
                if (transportErrorIt != transportErrors.end())
                {
                    const auto& errInfo = transportErrorIt->second;
                    error(
                        "Request to EID {EID} failed due to MCTP transport error "
                        "(errno={ERRNO})",
                        "EID", eid, "ERRNO", errInfo.errorCode);
                }
                else
                {
                    error(
                        "Request to EID {EID} timed out - no response received",
                        "EID", eid);
                }
            }

            // Call response handler with an empty response to indicate failure
            // The handler can check transportErrors to distinguish timeout vs
            // transport error
            responseHandler(eid, nullptr, 0);
            this->removeRequestContainer.emplace(
                key,
                std::make_unique<sdeventplus::source::Defer>(
                    event, std::bind(&Handler::removeRequestEntry, this, key)));
            endpointMessageQueues[eid]->activeRequest = false;

            /* try to send new request if the endpoint is free */
            pollEndpointQueue(eid);
        }
        else
        {
            // This condition is not possible, if a response is received
            // before the instance ID expiry, then the response handler
            // is executed and the entry will be removed.
            assert(false);
        }
    }

    /** @brief Send the remaining PLDM request messages in endpoint queue
     *
     *  @param[in] eid - endpoint ID of the remote MCTP endpoint
     */
    int pollEndpointQueue(mctp_eid_t eid)
    {
        if (endpointMessageQueues[eid]->activeRequest ||
            endpointMessageQueues[eid]->requestQueue.empty())
        {
            return PLDM_SUCCESS;
        }

        endpointMessageQueues[eid]->activeRequest = true;
        auto requestMsg = endpointMessageQueues[eid]->requestQueue.front();
        endpointMessageQueues[eid]->requestQueue.pop_front();

        // Clear any stale transport error from previous operations to this EID
        // This prevents old errors from affecting new requests
        clearTransportError(eid);

        auto request = std::make_unique<RequestInterface>(
            pldmTransport, requestMsg->key.eid, event,
            std::move(requestMsg->reqMsg), numRetries, responseTimeOut,
            verbose);
        auto timer = std::make_unique<sdbusplus::Timer>(
            event.get(), std::bind(&Handler::instanceIdExpiryCallBack, this,
                                   requestMsg->key));

        auto rc = request->start();
        if (rc)
        {
            instanceIdDb.free(requestMsg->key.eid, requestMsg->key.instanceId);
            error(
                "Failure to send the PLDM request message for polling endpoint "
                "queue, response code '{RC}'",
                "RC", rc);
            endpointMessageQueues[eid]->activeRequest = false;
            requestMsg->responseHandler(eid, nullptr, 0);
            return rc;
        }

        try
        {
            timer->start(duration_cast<std::chrono::microseconds>(
                instanceIdExpiryInterval));
        }
        catch (const std::runtime_error& e)
        {
            instanceIdDb.free(requestMsg->key.eid, requestMsg->key.instanceId);
            error(
                "Failed to start the instance ID expiry timer, error - {ERROR}",
                "ERROR", e);
            endpointMessageQueues[eid]->activeRequest = false;
            requestMsg->responseHandler(eid, nullptr, 0);
            return PLDM_ERROR;
        }

        handlers.emplace(requestMsg->key,
                         std::make_tuple(std::move(request),
                                         std::move(requestMsg->responseHandler),
                                         std::move(timer)));
        return PLDM_SUCCESS;
    }

    /** @brief Register a PLDM request message
     *
     *  @param[in] eid - endpoint ID of the remote MCTP endpoint
     *  @param[in] instanceId - instance ID to match request and response
     *  @param[in] type - PLDM type
     *  @param[in] command - PLDM command
     *  @param[in] requestMsg - PLDM request message
     *  @param[in] responseHandler - Response handler for this request
     *
     *  @return return PLDM_SUCCESS on success and PLDM_ERROR otherwise
     */
    int registerRequest(mctp_eid_t eid, uint8_t instanceId, uint8_t type,
                        uint8_t command, pldm::Request&& requestMsg,
                        ResponseHandler&& responseHandler)
    {
        RequestKey key{eid, instanceId, type, command};

        if (handlers.contains(key))
        {
            error(
                "Register request for EID '{EID}' is using InstanceID '{INSTANCEID}'",
                "EID", eid, "INSTANCEID", instanceId);
            return PLDM_ERROR;
        }

        auto inputRequest = std::make_shared<RegisteredRequest>(
            key, std::move(requestMsg), std::move(responseHandler));
        if (endpointMessageQueues.contains(eid))
        {
            endpointMessageQueues[eid]->requestQueue.push_back(inputRequest);
        }
        else
        {
            std::deque<std::shared_ptr<RegisteredRequest>> reqQueue;
            reqQueue.push_back(inputRequest);
            endpointMessageQueues[eid] =
                std::make_shared<EndpointMessageQueue>(eid, reqQueue, false);
        }

        /* try to send new request if the endpoint is free */
        auto rc = pollEndpointQueue(eid);
        if (rc != PLDM_SUCCESS)
        {
            error(
                "Failed to process request queue for EID {EID}, response code {RC}.",
                "EID", eid, "RC", rc);
            return rc;
        }

        return PLDM_SUCCESS;
    }

    /** @brief Unregister a PLDM request message
     *
     *  @param[in] eid - endpoint ID of the remote MCTP endpoint
     *  @param[in] instanceId - instance ID to match request and response
     *  @param[in] type - PLDM type
     *  @param[in] command - PLDM command
     *
     *  @return return PLDM_SUCCESS on success and PLDM_ERROR otherwise
     */
    int unregisterRequest(mctp_eid_t eid, uint8_t instanceId, uint8_t type,
                          uint8_t command)
    {
        RequestKey key{eid, instanceId, type, command};

        /* handlers only contain key when the message is already sent */
        if (handlers.contains(key))
        {
            auto& [request, responseHandler, timerInstance] = handlers[key];
            request->stop();
            auto rc = timerInstance->stop();
            if (rc)
            {
                error(
                    "Failed to stop the instance ID expiry timer, response code '{RC}'",
                    "RC", static_cast<int>(rc));
            }

            instanceIdDb.free(key.eid, key.instanceId);
            handlers.erase(key);
            endpointMessageQueues[eid]->activeRequest = false;
            /* try to send new request if the endpoint is free */
            pollEndpointQueue(eid);

            return PLDM_SUCCESS;
        }
        else
        {
            if (!endpointMessageQueues.contains(eid))
            {
                error(
                    "Can't find request for EID '{EID}' using InstanceID '{INSTANCEID}' in the endpoint message queue",
                    "EID", (unsigned)eid, "INSTANCEID", (unsigned)instanceId);
                return PLDM_ERROR;
            }
            auto& requestQueue = endpointMessageQueues[eid]->requestQueue;
            if (auto it = std::ranges::find_if(
                    requestQueue,
                    [&key](const auto& msg) { return msg->key == key; });
                it != requestQueue.end())
            {
                requestQueue.erase(it);
                instanceIdDb.free(key.eid, key.instanceId);
                return PLDM_SUCCESS;
            }
        }

        return PLDM_ERROR;
    }

    /** @brief Handle PLDM response message
     *
     *  @param[in] eid - endpoint ID of the remote MCTP endpoint
     *  @param[in] instanceId - instance ID to match request and response
     *  @param[in] type - PLDM type
     *  @param[in] command - PLDM command
     *  @param[in] response - PLDM response message
     *  @param[in] respMsgLen - length of the response message
     */
    void handleResponse(mctp_eid_t eid, uint8_t instanceId, uint8_t type,
                        uint8_t command, const pldm_msg* response,
                        size_t respMsgLen)
    {
        RequestKey key{eid, instanceId, type, command};
        if (handlers.contains(key) && !removeRequestContainer.contains(key))
        {
            auto& [request, responseHandler, timerInstance] = handlers[key];
            request->stop();
            auto rc = timerInstance->stop();
            if (rc)
            {
                error(
                    "Failed to stop the instance ID expiry timer, response code '{RC}'",
                    "RC", rc);
            }
            responseHandler(eid, response, respMsgLen);
            instanceIdDb.free(key.eid, key.instanceId);
            handlers.erase(key);
            eidTimeoutLogLimiter.clear(eid);

            endpointMessageQueues[eid]->activeRequest = false;
            /* try to send new request if the endpoint is free */
            pollEndpointQueue(eid);
        }
    }

    /** @brief Wrap registerRequest with coroutine API.
     *
     *  @return Return [PLDM_ERROR, _, _] if registerRequest fails.
     *          Return [PLDM_ERROR_NOT_READY, nullptr, 0] if timed out.
     *          Return [PLDM_SUCCESS, resp, len] if succeeded
     */
    stdexec::sender_of<stdexec::set_value_t(SendRecvCoResp)> auto sendRecvMsg(
        mctp_eid_t eid, pldm::Request&& request);

  private:
    PldmTransport* pldmTransport; //!< PLDM transport object
    sdeventplus::Event& event; //!< reference to PLDM daemon's main event loop
    pldm::InstanceIdDb& instanceIdDb; //!< reference to an InstanceIdDb
    bool verbose;                     //!< verbose tracing flag
    std::chrono::seconds
        instanceIdExpiryInterval;     //!< Instance ID expiration interval
    uint8_t numRetries;               //!< number of request retries
    std::chrono::milliseconds
        responseTimeOut;              //!< time to wait between each retry

    /** @brief Container for storing the details of the PLDM request
     *         message, handler for the corresponding PLDM response and the
     *         timer object for the Instance ID expiration
     */
    using RequestValue =
        std::tuple<std::unique_ptr<RequestInterface>, ResponseHandler,
                   std::unique_ptr<sdbusplus::Timer>>;

    // Manage the requests of responders base on MCTP EID
    std::map<mctp_eid_t, std::shared_ptr<EndpointMessageQueue>>
        endpointMessageQueues;

    /** @brief Container for storing the PLDM request entries */
    std::unordered_map<RequestKey, RequestValue, RequestKeyHasher> handlers;

    /** @brief Container to store information about the request entries to be
     *         removed after the instance ID timer expires
     */
    std::unordered_map<RequestKey, std::unique_ptr<sdeventplus::source::Defer>,
                       RequestKeyHasher>
        removeRequestContainer;

    /** @brief Container to store MCTP transport errors per endpoint
     *
     *  Stores the last transport error detected for each EID. This allows
     *  distinguishing between timeout (no error stored) and transport failure
     *  (error stored) when instance ID expires.
     */
    std::unordered_map<mctp_eid_t, TransportErrorInfo> transportErrors;

    /** @brief Rate-limit timeout/expiry logs per EID to avoid journal flooding
     *  when a terminus (e.g. T2/SatMC) repeatedly fails to respond.
     */
    pldm::utils::LogRateLimiter<mctp_eid_t> eidTimeoutLogLimiter;

    /** @brief Remove request entry for which the instance ID expired
     *
     *  @param[in] key - key for the Request
     */
    void removeRequestEntry(RequestKey key)
    {
        if (removeRequestContainer.contains(key))
        {
            removeRequestContainer[key].reset();
            instanceIdDb.free(key.eid, key.instanceId);
            handlers.erase(key);
            removeRequestContainer.erase(key);
        }
    }

  public:
    /** @brief Store transport error for an EID
     *
     *  Called from pldmd event loop when POLLERR is detected with an MCTP
     *  transport error. The error is stored silently and later retrieved by
     *  requester with context about which command failed.
     *
     *  @param[in] mctpError - MCTP error structure from error queue
     */
    void storeTransportError(const pldm::transport::MctpError& mctpError)
    {
        uint8_t direction = mctpError.direction;
        uint8_t srcEid = mctpError.src_eid;
        uint8_t destEid = mctpError.dest_eid;
        uint32_t errorCode = mctpError.error_code;
        uint8_t binding = mctpError.binding;
        uint64_t timestampNs = mctpError.timestamp_ns;
        uint16_t payloadLen = mctpError.payload_len;

        mctp_eid_t affectedEid = (direction == MCTP_DIR_TX) ? destEid : srcEid;

        uint8_t pldmType = pldm::transport::extractPldmType(mctpError);

        std::vector<uint8_t> requestPayload(mctpError.payload,
                                            mctpError.payload + payloadLen);

        TransportErrorInfo errInfo;
        errInfo.errorCode = errorCode;
        errInfo.binding = binding;
        errInfo.direction = direction;
        errInfo.srcEid = srcEid;
        errInfo.destEid = destEid;
        errInfo.timestampNs = timestampNs;
        errInfo.pldmType = pldmType;
        errInfo.requestPayload = requestPayload;

        transportErrors[affectedEid] = errInfo;
    }

    /** @brief Get transport error details for an EID
     *
     *  Allows application code to query transport error details after
     *  a request fails with PLDM_REQUESTER_MCTP_TRANSPORT_ERROR.
     *
     *  Optionally filters by PLDM type to ensure T5 firmware update operations
     *  only see T5 errors (not T2 platform monitoring errors, etc.)
     *
     *  @param[in] eid - endpoint ID
     *  @param[in] expectedPldmType - Optional PLDM type filter (0xFF = any
     * type)
     *
     *  @return std::optional<TransportErrorInfo> - Error info if found and
     * matches filter, std::nullopt otherwise
     */
    std::optional<TransportErrorInfo> getTransportError(
        mctp_eid_t eid, uint8_t expectedPldmType = 0xFF) const
    {
        auto it = transportErrors.find(eid);
        if (it != transportErrors.end())
        {
            if (expectedPldmType != 0xFF && it->second.pldmType != 0xFF &&
                it->second.pldmType != expectedPldmType)
            {
                return std::nullopt;
            }

            return it->second;
        }
        return std::nullopt;
    }

    /** @brief Clear transport error for an EID
     *
     *  Clears stored transport error after it has been handled by application.
     *
     *  @param[in] eid - endpoint ID to clear error for
     */
    void clearTransportError(mctp_eid_t eid)
    {
        transportErrors.erase(eid);
    }

    /** @brief Check if transport error exists for an EID
     *
     *  @param[in] eid - endpoint ID
     *  @return true if transport error exists for this EID, false otherwise
     */
    bool hasTransportError(mctp_eid_t eid) const
    {
        return transportErrors.contains(eid);
    }
};

/** @class SendRecvMsgOperation
 *
 *  Represents the state and logic for a single send/receive message operation
 *
 * @tparam RequestInterface - Request class type
 * @tparam stdexec::receiver - Execute receiver
 */
template <class RequestInterface, stdexec::receiver R>
struct SendRecvMsgOperation
{
    SendRecvMsgOperation() = delete;

    explicit SendRecvMsgOperation(Handler<RequestInterface>& handler,
                                  mctp_eid_t eid, pldm::Request&& request,
                                  R&& r) :
        handler(handler), request(std::move(request)), receiver(std::move(r))
    {
        auto requestMsg =
            reinterpret_cast<const pldm_msg*>(this->request.data());
        requestKey = RequestKey{
            eid,
            requestMsg->hdr.instance_id,
            requestMsg->hdr.type,
            requestMsg->hdr.command,
        };
        response = nullptr;
        respMsgLen = 0;
    }

    /** @brief Checks if the operation has been requested to stop.
     *         If so, it sets the state to stopped.Registers the request with
     *         the handler. If registration fails, sets an error on the
     *         receiver. If stopping is possible, sets up a stop callback.
     *
     *  @return Execute errors
     */
    void start() noexcept
    {
        auto stopToken = stdexec::get_stop_token(stdexec::get_env(receiver));

        // operation already cancelled
        if (stopToken.stop_requested())
        {
            return stdexec::set_stopped(std::move(receiver));
        }

        using namespace std::placeholders;
        registering = true;
        auto rc = handler.registerRequest(
            requestKey.eid, requestKey.instanceId, requestKey.type,
            requestKey.command, std::move(request),
            std::bind(&SendRecvMsgOperation::onComplete, this, _1, _2, _3));
        registering = false;
        if (rc && !completed)
        {
            completed = true;
            return stdexec::set_value(std::move(receiver), rc,
                                      static_cast<const pldm_msg*>(nullptr),
                                      static_cast<size_t>(0));
        }

        if (stopToken.stop_possible())
        {
            stopCallback.emplace(
                std::move(stopToken),
                std::bind(&SendRecvMsgOperation::onStop, this));
        }
    }

    /** @brief Unregisters the request and sets the state to stopped on the
     *         receiver.
     */
    void onStop()
    {
        handler.unregisterRequest(requestKey.eid, requestKey.instanceId,
                                  requestKey.type, requestKey.command);
        completed = true;
        return stdexec::set_stopped(std::move(receiver));
    }

    /** @brief This function resets the stop callback. Validates the response
     *         and sets either an error or a value on the receiver.
     *
     *  @param[in] eid - endpoint ID of the remote MCTP endpoint
     *  @param[in] response - PLDM response message
     *  @param[in] respMsgLen - length of the response message
     *
     *  @return PLDM completion code
     */
    void onComplete(mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen)
    {
        stopCallback.reset();
        assert(eid == this->requestKey.eid);
        int rc = PLDM_SUCCESS;
        if (!response && !respMsgLen)
        {
            if (handler.hasTransportError(eid))
            {
                rc = PLDM_REQUESTER_MCTP_TRANSPORT_ERROR;
            }
            else if (registering)
            {
                rc = PLDM_ERROR;
            }
            else
            {
                rc = PLDM_ERROR_NOT_READY;
            }
        }
        completed = true;
        return stdexec::set_value(std::move(receiver), rc, response,
                                  respMsgLen);
    }

  private:
    /** @brief Reference to a Handler object that manages the request/response
     *         logic.
     */
    requester::Handler<RequestInterface>& handler;

    /** @brief Stores information about the request such as eid, instanceId,
     *         type, and command.
     */
    RequestKey requestKey;

    /** @brief The request message to be sent.
     */
    pldm::Request request;

    /** @brief The response message for the sent request message.
     */
    const pldm_msg* response;

    /** @brief The length of response message for the sent request message.
     */
    size_t respMsgLen;

    /** @brief The receiver to be notified with the result of the operation.
     */
    R receiver;

    /** @brief An optional callback that handles stopping the operation if
     *         requested.
     */
    std::optional<typename stdexec::stop_token_of_t<
        stdexec::env_of_t<R>>::template callback_type<std::function<void()>>>
        stopCallback = std::nullopt;

    /** @brief True while registerRequest() is running synchronously. */
    bool registering = false;

    /** @brief True once the receiver has been completed. */
    bool completed = false;
};

/** @class SendRecvMsgSender
 *
 *  Represents the single message sender
 *
 * @tparam RequestInterface - Request class type
 */
template <class RequestInterface>
struct SendRecvMsgSender
{
    using sender_concept = stdexec::sender_t;

    SendRecvMsgSender() = delete;

    explicit SendRecvMsgSender(requester::Handler<RequestInterface>& handler,
                               mctp_eid_t eid, pldm::Request&& request) :
        handler(handler), eid(eid), request(std::move(request))
    {}

    template <typename... Env>
    auto get_completion_signatures(Env&&...) const
        -> stdexec::completion_signatures<
            stdexec::set_value_t(int, const pldm_msg*, size_t),
            stdexec::set_stopped_t()>;

    /** @brief Execute the sending the request message */
    template <stdexec::receiver R>
    auto connect(R r) &&
    {
        return SendRecvMsgOperation<RequestInterface, R>(
            handler, eid, std::move(request), std::move(r));
    }

  private:
    /** @brief Reference to a Handler object that manages the request/response
     *         logic.
     */
    requester::Handler<RequestInterface>& handler;

    /** @brief MCTP Endpoint ID of request message */
    mctp_eid_t eid;

    /** @brief Request message */
    pldm::Request request;
};

/** @brief Wrap registerRequest with coroutine API.
 *
 *  @param[in] eid - endpoint ID of the remote MCTP endpoint
 *  @param[in] request - PLDM request message
 *
 *  @return Return [PLDM_ERROR, _, _] if registerRequest fails.
 *          Return [PLDM_ERROR_NOT_READY, nullptr, 0] if timed out.
 *          Return [PLDM_REQUESTER_MCTP_TRANSPORT_ERROR, _, _] if MCTP transport
 *                 layer reports an error (e.g., EHOSTUNREACH, ETIMEDOUT).
 *                 Use getTransportError() to retrieve detailed error
 * information. Return [PLDM_SUCCESS, resp, len] if succeeded
 */
template <class RequestInterface>
stdexec::sender_of<stdexec::set_value_t(SendRecvCoResp)> auto
    Handler<RequestInterface>::sendRecvMsg(mctp_eid_t eid,
                                           pldm::Request&& request)
{
    return SendRecvMsgSender(*this, eid, std::move(request)) |
           stdexec::then([](int rc, const pldm_msg* resp, size_t respLen) {
               return std::make_tuple(rc, resp, respLen);
           });
}

} // namespace requester

} // namespace pldm
