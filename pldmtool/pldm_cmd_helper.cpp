#include "pldm_cmd_helper.hpp"

#include "libpldm/requester/pldm.h"
#include "libpldm/firmware_update.h"

#include "xyz/openbmc_project/Common/error.hpp"

#include <systemd/sd-bus.h>

#include <sdbusplus/server.hpp>
#include <xyz/openbmc_project/Logging/Entry/server.hpp>

#include <exception>

using namespace pldm::utils;

namespace pldmtool
{

namespace helper
{

/** @enum Enumeration to represent the PLDM completion codes
 */
enum class CompletionCodes : uint8_t
{
    SUCCESS = PLDM_SUCCESS,
    ERROR = PLDM_ERROR,
    ERROR_INVALID_DATA = PLDM_ERROR_INVALID_DATA,
    ERROR_INVALID_LENGTH = PLDM_ERROR_INVALID_LENGTH,
    ERROR_NOT_READY = PLDM_ERROR_NOT_READY,
    ERROR_UNSUPPORTED_PLDM_CMD = PLDM_ERROR_UNSUPPORTED_PLDM_CMD,
    ERROR_INVALID_PLDM_TYPE = PLDM_ERROR_INVALID_PLDM_TYPE,
    NOT_IN_UPDATE_MODE = PLDM_FWUP_NOT_IN_UPDATE_MODE,
    ALREADY_IN_UPDATE_MODE = PLDM_FWUP_ALREADY_IN_UPDATE_MODE,
    DATA_OUT_OF_RANGE = PLDM_FWUP_DATA_OUT_OF_RANGE,
    INVALID_TRANSFER_LENGTH = PLDM_FWUP_INVALID_TRANSFER_LENGTH,
    INVALID_STATE_FOR_COMMAND = PLDM_FWUP_INVALID_STATE_FOR_COMMAND,
    INCOMPLETE_UPDATE = PLDM_FWUP_INCOMPLETE_UPDATE,
    BUSY_IN_BACKGROUND = PLDM_FWUP_BUSY_IN_BACKGROUND,
    CANCEL_PENDING = PLDM_FWUP_CANCEL_PENDING,
    COMMAND_NOT_EXPECTED = PLDM_FWUP_COMMAND_NOT_EXPECTED,
    RETRY_REQUEST_FW_DATA = PLDM_FWUP_RETRY_REQUEST_FW_DATA,
    UNABLE_TO_INITIATE_UPDATE = PLDM_FWUP_UNABLE_TO_INITIATE_UPDATE,
    ACTIVATION_NOT_REQUIRED = PLDM_FWUP_ACTIVATION_NOT_REQUIRED,
    SELF_CONTAINED_ACTIVATION_NOT_PERMITTED =
        PLDM_FWUP_SELF_CONTAINED_ACTIVATION_NOT_PERMITTED,
    NO_DEVICE_METADATA = PLDM_FWUP_NO_DEVICE_METADATA,
    RETRY_REQUEST_UPDATE = PLDM_FWUP_RETRY_REQUEST_UPDATE,
    NO_PACKAGE_DATA = PLDM_FWUP_NO_PACKAGE_DATA,
    INVALID_DATA_TRANSFER_HAND = PLDM_FWUP_INVALID_TRANSFER_HANDLE,
    INVALID_TRANSFER_OPERATION_FLAG = PLDM_FWUP_INVALID_TRANSFER_OPERATION_FLAG,
    ACTIVATE_PENDING_IMAGE_NOT_PERMITTED =
        PLDM_FWUP_ACTIVATE_PENDING_IMAGE_NOT_PERMITTED,
    PACKAGE_DATA_ERROR = PLDM_FWUP_PACKAGE_DATA_ERROR
};

std::map<CompletionCodes, std::string> completionCodesMap{
    {CompletionCodes::SUCCESS, "SUCCESS"},
    {CompletionCodes::ERROR, "ERROR"},
    {CompletionCodes::ERROR_INVALID_DATA, "ERROR_INVALID_DATA"},
    {CompletionCodes::ERROR_INVALID_LENGTH, "ERROR_INVALID_LENGTH"},
    {CompletionCodes::ERROR_NOT_READY, "ERROR_NOT_READY"},
    {CompletionCodes::ERROR_UNSUPPORTED_PLDM_CMD, "ERROR_UNSUPPORTED_PLDM_CMD"},
    {CompletionCodes::ERROR_INVALID_PLDM_TYPE, "ERROR_INVALID_PLDM_TYPE"},
    {CompletionCodes::NOT_IN_UPDATE_MODE, "NOT_IN_UPDATE_MODE"},
    {CompletionCodes::ALREADY_IN_UPDATE_MODE, "ALREADY_IN_UPDATE_MODE"},
    {CompletionCodes::DATA_OUT_OF_RANGE, "DATA_OUT_OF_RANGE"},
    {CompletionCodes::INVALID_TRANSFER_LENGTH, "INVALID_TRANSFER_LENGTH"},
    {CompletionCodes::INVALID_STATE_FOR_COMMAND, "INVALID_STATE_FOR_COMMAND"},
    {CompletionCodes::INCOMPLETE_UPDATE, "INCOMPLETE_UPDATE"},
    {CompletionCodes::BUSY_IN_BACKGROUND, "BUSY_IN_BACKGROUND"},
    {CompletionCodes::CANCEL_PENDING, "CANCEL_PENDING"},
    {CompletionCodes::COMMAND_NOT_EXPECTED, "COMMAND_NOT_EXPECTED"},
    {CompletionCodes::RETRY_REQUEST_FW_DATA, "RETRY_REQUEST_FW_DATA"},
    {CompletionCodes::UNABLE_TO_INITIATE_UPDATE, "UNABLE_TO_INITIATE_UPDATE"},
    {CompletionCodes::ACTIVATION_NOT_REQUIRED, "ACTIVATION_NOT_REQUIRED"},
    {CompletionCodes::SELF_CONTAINED_ACTIVATION_NOT_PERMITTED,
     "SELF_CONTAINED_ACTIVATION_NOT_PERMITTED"},
    {CompletionCodes::NO_DEVICE_METADATA, "NO_DEVICE_METADATA"},
    {CompletionCodes::RETRY_REQUEST_UPDATE, "RETRY_REQUEST_UPDATE"},
    {CompletionCodes::NO_PACKAGE_DATA, "NO_PACKAGE_DATA"},
    {CompletionCodes::INVALID_DATA_TRANSFER_HAND, "INVALID_DATA_TRANSFER_HAND"},
    {CompletionCodes::INVALID_TRANSFER_OPERATION_FLAG,
     "INVALID_TRANSFER_OPERATION_FLAG"}};

/*
 * Translate PLDM completion code as human-readable string.
 *
 */
void fillCompletionCode(uint8_t completionCode, ordered_json& data)
{
    auto ccValue = static_cast<CompletionCodes>(completionCode);
    if (completionCodesMap.contains(ccValue))
    {
        data["CompletionCode"] = completionCodesMap[ccValue];
    }
    else
    {
        data["CompletionCode"] = "UNKNOWN_COMPLETION_CODE";
    }
}

/*
 * Initialize the socket, send pldm command & recieve response from socket
 *
 */
int mctpSockSendRecv(std::string socketName,
                     const std::vector<uint8_t>& requestMsg,
                     std::vector<uint8_t>& responseMsg, bool pldmVerbose)
{
    auto devPath = std::string(1, '\0') + socketName;
    int returnCode = 0;
    int sockFd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (-1 == sockFd)
    {
        returnCode = -errno;
        std::cerr << "Failed to create the socket : RC = " << sockFd << "\n";
        return returnCode;
    }
    Logger(pldmVerbose, "Success in creating the socket : RC = ", sockFd);

    struct sockaddr_un addr
    {};
    addr.sun_family = AF_UNIX;

    memcpy(addr.sun_path, devPath.data(), devPath.size());

    CustomFD socketFd(sockFd);
    int result = connect(socketFd(), reinterpret_cast<struct sockaddr*>(&addr),
                         devPath.size() + sizeof(addr.sun_family));
    if (-1 == result)
    {
        returnCode = -errno;
        std::cerr << "Failed to connect to socket '" << socketName
                  << "' : RC = " << returnCode << "\n";
        return returnCode;
    }
    Logger(pldmVerbose, "Success in connecting to socket : RC = ", returnCode);

    auto pldmType = MCTP_MSG_TYPE_PLDM;
    result = write(socketFd(), &pldmType, sizeof(pldmType));
    if (-1 == result)
    {
        returnCode = -errno;
        std::cerr << "Failed to send message type as pldm to mctp : RC = "
                  << returnCode << "\n";
        return returnCode;
    }
    Logger(
        pldmVerbose,
        "Success in sending message type as pldm to mctp : RC = ", returnCode);

    result = send(socketFd(), requestMsg.data(), requestMsg.size(), 0);
    if (-1 == result)
    {
        returnCode = -errno;
        std::cerr << "Write to socket failure : RC = " << returnCode << "\n";
        return returnCode;
    }
    Logger(pldmVerbose, "Write to socket successful : RC = ", result);

    // Read the response from socket
    ssize_t peekedLength = recv(socketFd(), nullptr, 0, MSG_TRUNC | MSG_PEEK);
    if (0 == peekedLength)
    {
        std::cerr << "Socket is closed : peekedLength = " << peekedLength
                  << "\n";
        return returnCode;
    }
    else if (peekedLength <= -1)
    {
        returnCode = -errno;
        std::cerr << "recv() system call failed : RC = " << returnCode << "\n";
        return returnCode;
    }
    else
    {
        auto reqhdr = reinterpret_cast<const pldm_msg_hdr*>(&requestMsg[2]);
        do
        {
            auto peekedLength =
                recv(socketFd(), nullptr, 0, MSG_PEEK | MSG_TRUNC);
            responseMsg.resize(peekedLength);
            auto recvDataLength =
                recv(socketFd(), reinterpret_cast<void*>(responseMsg.data()),
                     peekedLength, 0);
            auto resphdr =
                reinterpret_cast<const pldm_msg_hdr*>(&responseMsg[2]);
            if (recvDataLength == peekedLength &&
                resphdr->instance_id == reqhdr->instance_id &&
                resphdr->request != PLDM_REQUEST)
            {
                Logger(pldmVerbose, "Total length:", recvDataLength);
                break;
            }
            else if (recvDataLength != peekedLength)
            {
                std::cerr << "Failure to read response length packet: length = "
                          << recvDataLength << "\n";
                return returnCode;
            }
        } while (1);
    }

    returnCode = shutdown(socketFd(), SHUT_RDWR);
    if (-1 == returnCode)
    {
        returnCode = -errno;
        std::cerr << "Failed to shutdown the socket : RC = " << returnCode
                  << "\n";
        return returnCode;
    }

    Logger(pldmVerbose, "Shutdown Socket successful :  RC = ", returnCode);
    return PLDM_SUCCESS;
}

void CommandInterface::exec()
{

    if (mctp_eid == PLDM_ENTITY_ID && !socketName.has_value())
    {
        std::cout << "--socket_name is required when "
                  << "--mctp_eid is equal to "
                  << static_cast<int>(PLDM_ENTITY_ID)
                  << " or when MCTP endpoint is not provided\n"
                  << "Run with --help for more information.\n";
        return;
    }

    static constexpr auto pldmObjPath = "/xyz/openbmc_project/pldm";
    static constexpr auto pldmRequester = "xyz.openbmc_project.PLDM.Requester";
    auto& bus = pldm::utils::DBusHandler::getBus();
    try
    {
        auto service =
            pldm::utils::DBusHandler().getService(pldmObjPath, pldmRequester);
        auto method = bus.new_method_call(service.c_str(), pldmObjPath,
                                          pldmRequester, "GetInstanceId");
        method.append(mctp_eid);
        auto reply = bus.call(method);
        reply.read(instanceId);
    }
    catch (const std::exception& e)
    {
        std::cerr << "GetInstanceId D-Bus call failed, MCTP id = "
                  << (unsigned)mctp_eid << ", error = " << e.what() << "\n";
        return;
    }
    auto [rc, requestMsg] = createRequestMsg();
    if (rc != PLDM_SUCCESS)
    {
        std::cerr << "Failed to encode request message for " << pldmType << ":"
                  << commandName << " rc = " << rc << "\n";
        return;
    }

    std::vector<uint8_t> responseMsg;
    rc = pldmSendRecv(requestMsg, responseMsg);

    if (rc != PLDM_SUCCESS)
    {
        std::cerr << "pldmSendRecv: Failed to receive RC = " << rc << "\n";
        return;
    }

    auto responsePtr = reinterpret_cast<struct pldm_msg*>(responseMsg.data());
    parseResponseMsg(responsePtr, responseMsg.size() - sizeof(pldm_msg_hdr));
}

std::tuple<int, int, std::vector<uint8_t>>
    CommandInterface::getMctpSockInfo(uint8_t remoteEID)
{
    using namespace pldm;
    std::set<dbus::Service> mctpCtrlServices;
    int type = 0;
    int protocol = 0;
    std::vector<uint8_t> address{};
    auto& bus = pldm::utils::DBusHandler::getBus();
    const auto mctpEndpointIntfName{"xyz.openbmc_project.MCTP.Endpoint"};
    const auto unixSocketIntfName{"xyz.openbmc_project.Common.UnixSocket"};

    try
    {
        const dbus::Interfaces ifaceList{"xyz.openbmc_project.MCTP.Endpoint"};
        auto getSubTreeResponse = utils::DBusHandler().getSubtree(
            "/xyz/openbmc_project/mctp", 0, ifaceList);
        for (const auto& [objPath, mapperServiceMap] : getSubTreeResponse)
        {
            for (const auto& [serviceName, interfaces] : mapperServiceMap)
            {
                dbus::ObjectValueTree objects{};

                auto method = bus.new_method_call(
                    serviceName.c_str(), "/xyz/openbmc_project/mctp",
                    "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
                auto reply = bus.call(method);
                reply.read(objects);
                for (const auto& [objectPath, interfaces] : objects)
                {
                    if (interfaces.contains(mctpEndpointIntfName))
                    {
                        const auto& mctpProperties =
                            interfaces.at(mctpEndpointIntfName);
                        auto eid = std::get<size_t>(mctpProperties.at("EID"));
                        if (remoteEID == eid)
                        {
                            if (interfaces.contains(unixSocketIntfName))
                            {
                                const auto& properties =
                                    interfaces.at(unixSocketIntfName);
                                type = std::get<size_t>(properties.at("Type"));
                                protocol =
                                    std::get<size_t>(properties.at("Protocol"));
                                address = std::get<std::vector<uint8_t>>(
                                    properties.at("Address"));
                                if (address.empty() || !type)
                                {
                                    address.clear();
                                    return {0, 0, address};
                                }
                                else
                                {
                                    return {type, protocol, address};
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << "\n";
        address.clear();
        return {0, 0, address};
    }

    return {type, protocol, address};
}

int CommandInterface::pldmSendRecv(std::vector<uint8_t>& requestMsg,
                                   std::vector<uint8_t>& responseMsg)
{

    // Insert the PLDM message type and EID at the beginning of the
    // msg.
    requestMsg.insert(requestMsg.begin(), MCTP_MSG_TYPE_PLDM);
    requestMsg.insert(requestMsg.begin(), mctp_eid);

    bool mctpVerbose = pldmVerbose;

    // By default enable request/response msgs for pldmtool raw commands.
    if (CommandInterface::pldmType == "raw")
    {
        pldmVerbose = true;
    }

    if (pldmVerbose)
    {
        std::cout << "pldmtool: ";
        printBuffer(Tx, requestMsg);
    }

    if (mctp_eid != PLDM_ENTITY_ID)
    {
        auto [type, protocol, sockAddress] = getMctpSockInfo(mctp_eid);
        if (sockAddress.empty())
        {
            std::cerr << "pldmtool: Remote MCTP endpoint not found"
                      << "\n";
            return -1;
        }

        int rc = 0;
        int sockFd = socket(AF_UNIX, type, protocol);
        if (-1 == sockFd)
        {
            rc = -errno;
            std::cerr << "Failed to create the socket : RC = " << sockFd
                      << "\n";
            return rc;
        }
        Logger(pldmVerbose, "Success in creating the socket : RC = ", sockFd);

        CustomFD socketFd(sockFd);

        struct sockaddr_un addr
        {};
        addr.sun_family = AF_UNIX;
        memcpy(addr.sun_path, sockAddress.data(), sockAddress.size());
        rc = connect(sockFd, reinterpret_cast<struct sockaddr*>(&addr),
                     sockAddress.size() + sizeof(addr.sun_family));
        if (-1 == rc)
        {
            rc = -errno;
            std::cerr << "Failed to connect to socket : RC = " << rc << "\n";
            return rc;
        }
        Logger(pldmVerbose, "Success in connecting to socket : RC = ", rc);

        auto pldmType = MCTP_MSG_TYPE_PLDM;
        rc = write(socketFd(), &pldmType, sizeof(pldmType));
        if (-1 == rc)
        {
            rc = -errno;
            std::cerr
                << "Failed to send message type as pldm to mctp demux daemon: RC = "
                << rc << "\n";
            return rc;
        }
        Logger(
            pldmVerbose,
            "Success in sending message type as pldm to mctp demux daemon : RC = ",
            rc);

        uint8_t* responseMessage = nullptr;
        size_t responseMessageSize{};
        pldm_send_recv(mctp_eid, sockFd, requestMsg.data() + 2,
                       requestMsg.size() - 2, &responseMessage,
                       &responseMessageSize);

        responseMsg.resize(responseMessageSize);
        memcpy(responseMsg.data(), responseMessage, responseMsg.size());

        free(responseMessage);
        if (pldmVerbose)
        {
            std::cout << "pldmtool: ";
            printBuffer(Rx, responseMsg);
        }
    }
    else
    {
        auto rc = mctpSockSendRecv(socketName.value(), requestMsg, responseMsg,
                                   mctpVerbose);

        if (rc != PLDM_SUCCESS)
        {
            return rc;
        }

        if (pldmVerbose)
        {
            std::cout << "pldmtool: ";
            printBuffer(Rx, responseMsg);
        }
        responseMsg.erase(responseMsg.begin(),
                          responseMsg.begin() + 2 /* skip the mctp header */);
    }
    return PLDM_SUCCESS;
}
} // namespace helper
} // namespace pldmtool
