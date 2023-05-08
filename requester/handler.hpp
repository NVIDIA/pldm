#pragma once

#include "config.h"

#include "libpldm/base.h"
#include "libpldm/requester/pldm.h"

#include "common/types.hpp"
#include "pldmd/dbus_impl_requester.hpp"
#include "pldmd/socket_manager.hpp"
#include "request.hpp"

#include <function2/function2.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/timer.hpp>
#include <sdeventplus/event.hpp>
#include <sdeventplus/source/event.hpp>

#include <cassert>
#include <chrono>
#include <coroutine>
#include <memory>
#include <queue>
#include <tuple>
#include <unordered_map>

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

using ResponseHandler = fu2::unique_function<void(
    mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen)>;

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
     *  @param[in] event - reference to PLDM daemon's main event loop
     *  @param[in] requester - reference to Requester object
     *  @param[in] sockManager - MCTP socket manager
     *  @param[in] verbose - verbose tracing flag
     *  @param[in] instanceIdExpiryInterval - instance ID expiration interval
     *  @param[in] numRetries - number of request retries
     *  @param[in] responseTimeOut - time to wait between each retry
     */
    explicit Handler(
        sdeventplus::Event& event, pldm::dbus_api::Requester& requester,
        pldm::mctp_socket::Manager& sockManager, bool verbose,
        std::chrono::seconds instanceIdExpiryInterval =
            std::chrono::seconds(INSTANCE_ID_EXPIRATION_INTERVAL),
        uint8_t numRetries = static_cast<uint8_t>(NUMBER_OF_REQUEST_RETRIES),
        std::chrono::milliseconds responseTimeOut =
            std::chrono::milliseconds(RESPONSE_TIME_OUT)) :
        event(event),
        requester(requester), sockManager(sockManager), verbose(verbose),
        instanceIdExpiryInterval(instanceIdExpiryInterval),
        numRetries(numRetries), responseTimeOut(responseTimeOut)
    {}

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

        auto instanceIdExpiryCallBack = [key, this](void) {
            if (this->handlers.contains(key.eid) &&
                !this->handlers[key.eid].empty())
            {
                auto& [request, responseHandler, timerInstance, requestKey] =
                    handlers[key.eid].front();
                if (key == requestKey)
                {
                    lg2::error(
                        "Response not received for the request, instance ID "
                        "expired. EID={EID}, INSTANCE_ID={INSTANCE_ID} ,"
                        "TYPE={TYPE}, COMMAND={COMMAND}",
                        "EID", key.eid, "INSTANCE_ID", key.instanceId, "TYPE",
                        key.type, "COMMAND", key.command);
                    request->stop();
                    auto rc = timerInstance->stop();
                    if (rc)
                    {
                        lg2::error(
                            "Failed to stop the instance ID expiry timer. RC={RC}",
                            "RC", rc);
                    }
                    // Call response handler with an empty response to indicate
                    // no response
                    responseHandler(key.eid, nullptr, 0);
                    this->removeRequestContainer.emplace(
                        key, std::make_unique<sdeventplus::source::Defer>(
                                 event, std::bind(&Handler::removeRequestEntry,
                                                  this, key)));
                }
                else
                {
                    // This condition is not possible, if a response is received
                    // before the instance ID expiry, then the response handler
                    // is executed and the entry will be removed.
                    assert(false);
                }
            }
        };

        if (requestMsg.size() >
            static_cast<size_t>(sockManager.getSendBufferSize(eid)))
        {
            sockManager.setSendBufferSize(sockManager.getSocket(eid),
                                          requestMsg.size());
        }

        auto request = std::make_unique<RequestInterface>(
            sockManager.getSocket(eid), eid, event, std::move(requestMsg),
            numRetries, responseTimeOut, verbose);
        auto timer = std::make_unique<phosphor::Timer>(
            event.get(), instanceIdExpiryCallBack);

        handlers[eid].emplace(
            std::make_tuple(std::move(request), std::move(responseHandler),
                            std::move(timer), std::move(key)));
        return runRegisteredRequest(eid);
    }

    int runRegisteredRequest(mctp_eid_t eid)
    {
        if (handlers[eid].empty())
        {
            return PLDM_SUCCESS;
        }

        auto& [request, responseHandler, timerInstance, key] =
            handlers[eid].front();

        if (timerInstance->isRunning())
        {
            // A PLDM request for the EID is running
            return PLDM_SUCCESS;
        }

        auto rc = request->start();
        if (rc)
        {
            requester.markFree(eid, key.instanceId);
            lg2::error("Failure to send the PLDM request message");
            return rc;
        }

        try
        {
            timerInstance->start(duration_cast<std::chrono::microseconds>(
                instanceIdExpiryInterval));
        }
        catch (const std::runtime_error& e)
        {
            requester.markFree(eid, key.instanceId);
            lg2::error("Failed to start the instance ID expiry timer.", "ERROR",
                       e);
            return PLDM_ERROR;
        }

        return PLDM_SUCCESS;
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
        bool responseHandled = false;

        if (handlers.contains(eid) && !handlers[eid].empty())
        {
            auto& [request, responseHandler, timerInstance, requestKey] =
                handlers[eid].front();
            if (key == requestKey)
            {
                request->stop();
                auto rc = timerInstance->stop();
                if (rc)
                {
                    lg2::error(
                        "Failed to stop the instance ID expiry timer. RC={RC}",
                        "RC", rc);
                }
                // Call responseHandler after erase it from the handlers to
                // avoid starting it again in runRegisteredRequest()
                auto unique_handler = std::move(responseHandler);
                requester.markFree(eid, instanceId);
                handlers[eid].pop();
                unique_handler(eid, response, respMsgLen);
                responseHandled = true;
            }
        }

        if (!responseHandled)
        {
            // Got a response for a PLDM request message not registered with
            // the request handler, so freeing up the instance ID, this can
            // be other OpenBMC applications relying on PLDM D-Bus apis like
            // openpower-occ-control and softoff or through pldmtool.
            requester.markFree(eid, instanceId);
        }
        runRegisteredRequest(eid);
    }

  private:
    int fd; //!< file descriptor of MCTP communications socket
    sdeventplus::Event& event; //!< reference to PLDM daemon's main event loop
    pldm::dbus_api::Requester& requester; //!< reference to Requester object
    pldm::mctp_socket::Manager& sockManager;

    bool verbose;                 //!< verbose tracing flag
    std::chrono::seconds
        instanceIdExpiryInterval; //!< Instance ID expiration interval
    uint8_t numRetries;           //!< number of request retries
    std::chrono::milliseconds
        responseTimeOut;          //!< time to wait between each retry

    /** @brief Container for storing the details of the PLDM request
     *         message, handler for the corresponding PLDM response, the
     *         timer object for the Instance ID expiration and RequestKey
     */
    using RequestValue =
        std::tuple<std::unique_ptr<RequestInterface>, ResponseHandler,
                   std::unique_ptr<phosphor::Timer>, RequestKey>;
    using RequestQueue = std::queue<RequestValue>;

    /** @brief Container for storing the PLDM request entries */
    std::unordered_map<mctp_eid_t, RequestQueue> handlers;

    /** @brief Container to store information about the request entries to be
     *         removed after the instance ID timer expires
     */
    std::unordered_map<RequestKey, std::unique_ptr<sdeventplus::source::Defer>,
                       RequestKeyHasher>
        removeRequestContainer;

    /** @brief Remove request entry for which the instance ID expired
     *
     *  @param[in] key - key for the Request
     */
    void removeRequestEntry(RequestKey key)
    {
        if (removeRequestContainer.contains(key))
        {
            removeRequestContainer[key].reset();
            requester.markFree(key.eid, key.instanceId);
            if (!handlers[key.eid].empty())
            {
                auto& [request, responseHandler, timerInstance, requestKey] =
                    handlers[key.eid].front();
                if (key == requestKey)
                {
                    handlers[key.eid].pop();
                }
            }
            removeRequestContainer.erase(key);
        }
        runRegisteredRequest(key.eid);
    }
};

/** @struct SendRecvPldmMsg
 *
 * An awaitable object needed by co_await operator to send/recv PLDM
 * message.
 * e.g.
 * rc = co_await SendRecvPldmMsg<h>(h, eid, req, respMsg, respLen);
 *
 * @tparam RequesterHandler - Requester::handler class type
 */
template <class RequesterHandler>
struct SendRecvPldmMsg
{
    /** @brief For recording the suspended coroutine where the co_await
     * operator is. When PLDM response message is received, the resumeHandle()
     * will be called to continue the next line of co_await operator
     */
    std::coroutine_handle<> resumeHandle;

    /** @brief The RequesterHandler to send/recv PLDM message.
     */
    RequesterHandler& handler;

    /** @brief The EID where PLDM message will be sent to.
     */
    uint8_t eid;

    /** @brief The PLDM request message.
     */
    pldm::Request& request;

    /** @brief The pointer of PLDM response message.
     */
    const pldm_msg** responseMsg;

    /** @brief The length of PLDM response message.
     */
    size_t* responseLen;

    /** @brief For keeping the return value of RequesterHandler.
     */
    uint8_t rc;

    /** @brief Returning false to make await_suspend() to be called.
     */
    bool await_ready() noexcept
    {
        return false;
    }

    /** @brief Called by co_await operator before suspending coroutine. The
     * method will send out PLDM request message, register handleResponse() as
     * call back function for the event when PLDM response message received.
     */
    bool await_suspend(std::coroutine_handle<> handle) noexcept
    {
        if (responseMsg == nullptr || responseLen == nullptr)
        {
            rc = PLDM_ERROR_INVALID_DATA;
            return false;
        }

        auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
        rc = handler.registerRequest(
            eid, requestMsg->hdr.instance_id, requestMsg->hdr.type,
            requestMsg->hdr.command, std::move(request),
            std::move(std::bind_front(&SendRecvPldmMsg::HandleResponse, this)));
        if (rc)
        {
            lg2::error("registerRequest failed, rc={RC}", "RC",
                       static_cast<unsigned>(rc));
            return false;
        }

        resumeHandle = handle;
        return true;
    }

    /** @brief Called by co_await operator to get return value when awaitable
     * object completed.
     */
    uint8_t await_resume() const noexcept
    {
        return rc;
    }

    /** @brief Constructor of awaitable object to initialize necessary member
     * variables.
     */
    SendRecvPldmMsg(RequesterHandler& handler, uint8_t eid,
                    pldm::Request& request, const pldm_msg** responseMsg,
                    size_t* responseLen) :
        handler(handler),
        eid(eid), request(request), responseMsg(responseMsg),
        responseLen(responseLen), rc(PLDM_ERROR)
    {}

    /** @brief The function will be registered by ReqisterHandler for handling
     * PLDM response message. The copied responseMsg is for preventing that the
     * response pointer in parameter becomes invalid when coroutine is
     * resumed.
     */
    void HandleResponse(mctp_eid_t eid, const pldm_msg* response, size_t length)
    {
        if (response == nullptr || !length)
        {
            lg2::error("No response received, EID={EID}", "EID", eid);
            rc = PLDM_ERROR;
        }
        else
        {
            *responseMsg = response;
            *responseLen = length;
            rc = PLDM_SUCCESS;
        }
        resumeHandle();
    }
};

/** @struct Coroutine
 *
 * A coroutine return_object supports nesting coroutine
 */
struct Coroutine
{
    /** @brief The nested struct named 'promise_type' which is needed for
     * Coroutine struct to be a coroutine return_object.
     */
    struct promise_type
    {
        /** @brief For keeping the parent coroutine handle if any. For the case
         * of nesting co_await coroutine, this handle will be used to resume to
         * continue parent coroutine.
         */
        std::coroutine_handle<> parent_handle;

        /** @brief For holding return value of coroutine
         */
        uint8_t data;

        bool detached = false;

        /** @brief Get the return object object
         */
        Coroutine get_return_object()
        {
            return {std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        /** @brief The method is called before starting a coroutine. Returning
         * std::suspend_never awaitable to execute coroutine body immediately.
         */
        std::suspend_never initial_suspend()
        {
            return {};
        }

        /** @brief The method is called after coroutine completed to return a
         * customized awaitable object to resume to parent coroutine if any.
         */
        auto final_suspend() noexcept
        {
            struct awaiter
            {
                /** @brief Returning false to make await_suspend to be called.
                 */
                bool await_ready() const noexcept
                {
                    return false;
                }

                /** @brief Do nothing here for customized awaitable object.
                 */
                void await_resume() const noexcept
                {}

                /** @brief Returning parent coroutine handle here to continue
                 * parent corotuine.
                 */
                std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<promise_type> h) noexcept
                {
                    auto parent_handle = h.promise().parent_handle;
                    if (h.promise().detached)
                    {
                        h.destroy();
                    }
                    if (parent_handle)
                    {
                        return parent_handle;
                    }
                    return std::noop_coroutine();
                }
            };
            return awaiter{};
        }

        /** @brief The handler for an exception was thrown in
         * coroutine body.
         */
        void unhandled_exception()
        {}

        /** @brief Keeping the value returned by co_return operator
         */
        void return_value(uint8_t value) noexcept
        {
            data = std::move(value);
        }
    };

    /** @brief Called by co_await to check if it needs to be
     * suspened.
     */
    bool await_ready() const noexcept
    {
        return handle.done();
    }

    /** @brief Called by co_await operator to get return value when coroutine
     * finished.
     */
    uint8_t await_resume() const noexcept
    {
        return std::move(handle.promise().data);
    }

    /** @brief Called when the coroutine itself is being suspended. The
     * recording the parent coroutine handle is for await_suspend() in
     * promise_type::final_suspend to refer.
     */
    bool await_suspend(std::coroutine_handle<> coroutine)
    {
        handle.promise().parent_handle = coroutine;
        return true;
    }

    ~Coroutine()
    {
        if (handle && handle.done())
        {
            handle.destroy();
        }
    }

    void detach()
    {
        if (!handle)
        {
            return;
        }

        if (handle.done())
        {
            handle.destroy();
        }
        else
        {
            handle.promise().detached = true;
        }
        handle = nullptr;
    }

    /** @brief Assigned by promise_type::get_return_object to keep coroutine
     * handle itself.
     */
    mutable std::coroutine_handle<promise_type> handle;
};

} // namespace requester

} // namespace pldm
