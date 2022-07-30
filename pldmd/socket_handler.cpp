#include "socket_handler.hpp"

#include "common/flight_recorder.hpp"
#include "socket_manager.hpp"

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

namespace pldm::mctp_socket
{

int Handler::registerMctpEndpoint(EID eid, int type, int protocol,
                                  const std::vector<uint8_t>& pathName)
{
    auto entry = socketInfoMap.find(pathName);
    if (entry == socketInfoMap.end())
    {
        auto [fd, currentSendBufferSize] = initSocket(type, protocol, pathName);
        if (fd < 0)
        {
            return fd;
        }
        else
        {
            manager.registerEndpoint(eid, fd, currentSendBufferSize);
        }
    }
    else
    {
        manager.registerEndpoint(eid, (*(std::get<0>(entry->second)).get())(),
                                 std::get<1>(entry->second));
    }

    return 0;
}

SocketInfo Handler::initSocket(int type, int protocol,
                               const std::vector<uint8_t>& pathName)
{
    auto callback = [this](IO& io, int fd, uint32_t revents) mutable {
        using namespace pldm::flightrecorder;
        using namespace pldm::utils;
        if (!(revents & EPOLLIN))
        {
            return;
        }

        // Outgoing message.
        struct iovec iov[2]{};

        // This structure contains the parameter information for the response
        // message.
        struct msghdr msg
        {};

        int returnCode = 0;
        ssize_t peekedLength = recv(fd, nullptr, 0, MSG_PEEK | MSG_TRUNC);
        if (peekedLength == 0)
        {
            // MCTP daemon has closed the socket this daemon is connected to.
            // This may or may not be an error scenario, in either case the
            // recovery mechanism for this daemon is to restart, and hence
            // exit the event loop, that will cause this daemon to exit with a
            // failure code.
            io.get_event().exit(0);
        }
        else if (peekedLength <= -1)
        {
            returnCode = -errno;
            std::cerr << "recv system call failed, RC= " << returnCode << "\n";
        }
        else
        {
            std::vector<uint8_t> requestMsg(peekedLength);
            auto recvDataLength = recv(
                fd, static_cast<void*>(requestMsg.data()), peekedLength, 0);
            if (recvDataLength == peekedLength)
            {
                FlightRecorder::GetInstance().saveRecord(requestMsg, false);
                if (verbose)
                {
                    printBuffer(Rx, requestMsg);
                }

                if (MCTP_MSG_TYPE_PLDM != requestMsg[1])
                {
                    // Skip this message and continue.
                }
                else
                {
                    // process message and send response
                    auto response = processRxMsg(requestMsg);
                    if (response.has_value())
                    {
                        FlightRecorder::GetInstance().saveRecord(*response,
                                                                 true);
                        if (verbose)
                        {
                            printBuffer(Tx, *response);
                        }

                        iov[0].iov_base = &requestMsg[0];
                        iov[0].iov_len =
                            sizeof(requestMsg[0]) + sizeof(requestMsg[1]);
                        iov[1].iov_base = (*response).data();
                        iov[1].iov_len = (*response).size();

                        msg.msg_iov = iov;
                        msg.msg_iovlen = sizeof(iov) / sizeof(iov[0]);

                        int result = sendmsg(fd, &msg, 0);
                        if (-1 == result)
                        {
                            returnCode = -errno;
                            std::cerr << "sendto system call failed, RC= "
                                      << returnCode << "\n";
                        }
                    }
                }
            }
            else
            {
                std::cerr
                    << "Failure to read peeked length packet. peekedLength= "
                    << peekedLength << " recvDataLength=" << recvDataLength
                    << "\n";
            }
        }
    };

    /* Create socket */
    int rc = 0;
    int sendBufferSize = 0;
    int sockFd = socket(AF_UNIX, type, protocol);
    if (sockFd == -1)
    {
        rc = -errno;
        std::cerr << "Failed to create the socket, RC= " << strerror(-rc)
                  << "\n";
        return {rc, sendBufferSize};
    }

    auto fd = std::make_unique<pldm::utils::CustomFD>(sockFd);

    /* Get socket current buffer size */
    socklen_t optlen;
    optlen = sizeof(sendBufferSize);
    rc = getsockopt(sockFd, SOL_SOCKET, SO_SNDBUF, &sendBufferSize, &optlen);
    if (rc == -1)
    {
        rc = -errno;
        std::cerr << "Error getting the default socket send buffer size, RC= "
                  << strerror(-rc) << "\n";
        return {rc, sendBufferSize};
    }

    // /* Initiate a connection to the socket */
    struct sockaddr_un addr
    {};
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, pathName.data(), pathName.size());
    rc = connect(sockFd, reinterpret_cast<struct sockaddr*>(&addr),
                 pathName.size() + sizeof(addr.sun_family));
    if (rc == -1)
    {
        rc = -errno;
        std::cerr << "Failed to connect to the socket, RC= " << strerror(-rc)
                  << "\n";
        return {rc, sendBufferSize};
    }

    /* Register for PLDM message type */
    ssize_t result =
        write(sockFd, &MCTP_MSG_TYPE_PLDM, sizeof(MCTP_MSG_TYPE_PLDM));
    if (result == -1)
    {
        rc = -errno;
        std::cerr << "Failed to send message type as PLDM to demux daemon, RC= "
                  << strerror(-rc) << "\n";
        return {rc, sendBufferSize};
    }

    auto io = std::make_unique<IO>(event, sockFd, EPOLLIN, std::move(callback));
    socketInfoMap[pathName] =
        std::tuple(std::move(fd), sendBufferSize, std::move(io));

    return {sockFd, sendBufferSize};
}

std::optional<Response>
    Handler::processRxMsg(const std::vector<uint8_t>& requestMsg)
{
    using type = uint8_t;
    uint8_t eid = requestMsg[0];
    pldm_header_info hdrFields{};
    auto hdr = reinterpret_cast<const pldm_msg_hdr*>(
        requestMsg.data() + sizeof(eid) + sizeof(type));
    if (PLDM_SUCCESS != unpack_pldm_header(hdr, &hdrFields))
    {
        std::cerr << "Empty PLDM request header \n";
        return std::nullopt;
    }

    if (PLDM_RESPONSE != hdrFields.msg_type)
    {
        Response response;
        auto request = reinterpret_cast<const pldm_msg*>(hdr);
        size_t requestLen = requestMsg.size() - sizeof(struct pldm_msg_hdr) -
                            sizeof(eid) - sizeof(type);
        try
        {
            if (hdrFields.pldm_type != PLDM_FWUP)
            {
                response =
                    invoker.handle(hdrFields.pldm_type, hdrFields.command,
                                   request, requestLen);
            }
            else
            {
                response = fwManager.handleRequest(eid, hdrFields.command,
                                                   request, requestLen);
            }
        }
        catch (const std::out_of_range& e)
        {
            uint8_t completion_code = PLDM_ERROR_UNSUPPORTED_PLDM_CMD;
            response.resize(sizeof(pldm_msg_hdr));
            auto responseHdr = reinterpret_cast<pldm_msg_hdr*>(response.data());
            pldm_header_info header{};
            header.msg_type = PLDM_RESPONSE;
            header.instance = hdrFields.instance;
            header.pldm_type = hdrFields.pldm_type;
            header.command = hdrFields.command;
            if (PLDM_SUCCESS != pack_pldm_header(&header, responseHdr))
            {
                std::cerr << "Failed adding response header \n";
                return std::nullopt;
            }
            response.insert(response.end(), completion_code);
        }
        return response;
    }
    else if (PLDM_RESPONSE == hdrFields.msg_type)
    {
        auto response = reinterpret_cast<const pldm_msg*>(hdr);
        size_t responseLen = requestMsg.size() - sizeof(struct pldm_msg_hdr) -
                             sizeof(eid) - sizeof(type);
        handler.handleResponse(eid, hdrFields.instance, hdrFields.pldm_type,
                               hdrFields.command, response, responseLen);
    }
    return std::nullopt;
}

} // namespace pldm::mctp_socket