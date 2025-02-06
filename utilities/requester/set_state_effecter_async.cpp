#include "common/transport.hpp"

#include <libpldm/base.h>
#include <libpldm/platform.h>
#include <libpldm/pldm.h>

#include <CLI/CLI.hpp>
#include <sdeventplus/event.hpp>
#include <sdeventplus/source/io.hpp>

#include <array>
#include <iostream>

using namespace sdeventplus;
using namespace sdeventplus::source;

int main(int argc, char** argv)
{
    try
    {
        CLI::App app{"Send PLDM command SetStateEffecterStates"};

        uint8_t mctpEid{};
        app.add_option("-m,--mctp_eid", mctpEid, "MCTP EID")->required();
        uint16_t effecterId{};
        app.add_option("-e,--effecter", effecterId, "Effecter Id")->required();
        uint8_t state{};
        app.add_option("-s,--state", state, "New state value")->required();
        CLI11_PARSE(app, argc, argv);

        pldm_tid_t dstTid = static_cast<pldm_tid_t>(mctpEid);

        // Encode PLDM Request message
        uint8_t effecterCount = 1;
        std::array<uint8_t, sizeof(pldm_msg_hdr) + sizeof(effecterId) +
                                sizeof(effecterCount) +
                                sizeof(set_effecter_state_field)>
            requestMsg{};
        auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());
        set_effecter_state_field stateField{PLDM_REQUEST_SET, state};
        auto rc = encode_set_state_effecter_states_req(
            0, effecterId, effecterCount, &stateField, request);
        if (rc != PLDM_SUCCESS)
        {
            std::ostringstream tempStream;
            tempStream << std::setfill('0') << std::setw(2) << std::hex << rc;
            std::cerr << "Message encode failure. PLDM error code = "
                      << tempStream.str() << rc << "\n";
            return -1;
        }

        PldmTransport pldmTransport{};

        // Create event loop and add a callback to handle EPOLLIN on fd
        auto event = Event::get_default();
        auto callback = [=, &pldmTransport](IO& io, int fd,
                                            uint32_t revents) mutable {
            if (!(revents & EPOLLIN))
            {
                return;
            }

            if (pldmTransport.getEventSource() != fd)
            {
                return;
            }
            size_t responseMsgSize{};
            void* responseMsg = nullptr;
            pldm_tid_t srcTid;
            auto rc =
                pldmTransport.recvMsg(srcTid, responseMsg, responseMsgSize);
            pldm_msg* response = reinterpret_cast<pldm_msg*>(responseMsg);
            if (rc || dstTid != srcTid ||
                !pldm_msg_hdr_correlate_response(&request->hdr, &response->hdr))
            {
                return;
            }
            // We've got the response meant for the PLDM request msg
            // that was sent out
            io.set_enabled(Enabled::Off);
            std::ostringstream tempStream;
            tempStream << std::setfill('0') << std::setw(2) << std::hex << rc;
            std::cout << "Done. PLDM RC = " << tempStream.str()
                      << static_cast<uint16_t>(response->payload[0])
                      << std::endl;
            free(responseMsg);
            exit(EXIT_SUCCESS);
        };
        IO io(event, pldmTransport.getEventSource(), EPOLLIN,
              std::move(callback));

        // Send PLDM Request message - pldm_send doesn't wait for response
        rc =
            pldmTransport.sendMsg(dstTid, requestMsg.data(), requestMsg.size());
        if (0 > rc)
        {
            std::cerr << "Failed to send message/receive response. RC = " << rc
                      << ", errno = " << errno << "\n";
            return -1;
        }

        event.loop();
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return -1;
    }

    return 0;
}
