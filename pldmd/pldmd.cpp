#include "libpldm/base.h"
#include "libpldm/bios.h"
#include "libpldm/pdr.h"
#include "libpldm/platform.h"

#include "common/flight_recorder.hpp"
#include "common/utils.hpp"
#include "dbus_impl_requester.hpp"
#include "fw-update/manager.hpp"
#include "invoker.hpp"
#include "platform-mc/manager.hpp"
#include "requester/handler.hpp"
#include "requester/mctp_endpoint_discovery.hpp"
#include "requester/request.hpp"
#include "socket_handler.hpp"
#include "socket_manager.hpp"

#include <err.h>
#include <getopt.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <sdeventplus/event.hpp>
#include <sdeventplus/source/io.hpp>
#include <sdeventplus/source/signal.hpp>
#include <stdplus/signal.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef LIBPLDMRESPONDER
#include "dbus_impl_pdr.hpp"
#include "host-bmc/dbus_to_event_handler.hpp"
#include "host-bmc/dbus_to_host_effecters.hpp"
#include "host-bmc/host_condition.hpp"
#include "host-bmc/host_pdr_handler.hpp"
#include "libpldmresponder/base.hpp"
#include "libpldmresponder/bios.hpp"
#include "libpldmresponder/fru.hpp"
#include "libpldmresponder/oem_handler.hpp"
#include "libpldmresponder/platform.hpp"
#include "xyz/openbmc_project/PLDM/Event/server.hpp"
#endif

#ifdef OEM_IBM
#include "libpldmresponder/file_io.hpp"
#include "libpldmresponder/oem_ibm_handler.hpp"
#endif

using namespace pldm;
using namespace sdeventplus;
using namespace sdeventplus::source;
using namespace pldm::responder;
using namespace pldm::utils;
using sdeventplus::source::Signal;
using namespace pldm::flightrecorder;

void interruptFlightRecorderCallBack(Signal& /*signal*/,
                                     const struct signalfd_siginfo*)
{
    std::cerr << "\nReceived SIGUR1(10) Signal interrupt\n";

    // obtain the flight recorder instance and dump the recorder
    FlightRecorder::GetInstance().playRecorder();
}

void optionUsage(void)
{
    std::cerr << "Usage: pldmd [options]\n";
    std::cerr << "Options:\n";
    std::cerr
        << "  --verbose=<0/1>  0 - Disable verbosity, 1 - Enable verbosity\n";
    std::cerr << "  --fw-debug Optional flag to enable firmware update logs\n";
    std::cerr << "Defaulted settings:  --verbose=0 \n";
}

int main(int argc, char** argv)
{

    bool verbose = false;
    bool fwDebug = false;
    int argflag;
    static struct option long_options[] = {
        {"verbose", required_argument, 0, 'v'},
        {"fw-debug", no_argument, 0, 'd'},
        {0, 0, 0, 0}};

    while ((argflag = getopt_long(argc, argv, "v:d", long_options, nullptr)) >=
           0)
    {
        switch (argflag)
        {
            case 'v':
                switch (std::stoi(optarg))
                {
                    case 0:
                        verbose = false;
                        break;
                    case 1:
                        verbose = true;
                        break;
                    default:
                        optionUsage();
                        exit(EXIT_FAILURE);
                }
                break;
            case 'd':
                fwDebug = true;
                break;
            default:
                exit(EXIT_FAILURE);
        }
    }

    auto event = Event::get_default();
    auto& bus = pldm::utils::DBusHandler::getBus();
    sdbusplus::server::manager::manager swObjManager(
        bus, "/xyz/openbmc_project/software");
    dbus_api::Requester dbusImplReq(bus, "/xyz/openbmc_project/pldm");

    event.set_watchdog(true);

    Invoker invoker{};
    mctp_socket::Manager sockManager;
    requester::Handler<requester::Request> reqHandler(event, dbusImplReq,
                                                      sockManager, verbose);

#ifdef LIBPLDMRESPONDER
    using namespace pldm::state_sensor;
    int sockfd = 0;
    dbus_api::Host dbusImplHost(bus, "/xyz/openbmc_project/pldm");
    std::unique_ptr<pldm_pdr, decltype(&pldm_pdr_destroy)> pdrRepo(
        pldm_pdr_init(), pldm_pdr_destroy);
    std::unique_ptr<pldm_entity_association_tree,
                    decltype(&pldm_entity_association_tree_destroy)>
        entityTree(pldm_entity_association_tree_init(),
                   pldm_entity_association_tree_destroy);
    std::unique_ptr<pldm_entity_association_tree,
                    decltype(&pldm_entity_association_tree_destroy)>
        bmcEntityTree(pldm_entity_association_tree_init(),
                      pldm_entity_association_tree_destroy);
    std::shared_ptr<HostPDRHandler> hostPDRHandler;
    std::unique_ptr<pldm::host_effecters::HostEffecterParser>
        hostEffecterParser;
    std::unique_ptr<DbusToPLDMEvent> dbusToPLDMEventHandler;
    DBusHandler dbusHandler;
    auto hostEID = pldm::utils::readHostEID();
    if (hostEID)
    {
        hostPDRHandler = std::make_shared<HostPDRHandler>(
            sockfd, hostEID, event, pdrRepo.get(), EVENTS_JSONS_DIR,
            entityTree.get(), bmcEntityTree.get(), dbusImplReq, &reqHandler);
        // HostFirmware interface needs access to hostPDR to know if host
        // is running
        dbusImplHost.setHostPdrObj(hostPDRHandler);

        hostEffecterParser =
            std::make_unique<pldm::host_effecters::HostEffecterParser>(
                &dbusImplReq, sockfd, pdrRepo.get(), &dbusHandler,
                HOST_JSONS_DIR, &reqHandler);
        dbusToPLDMEventHandler = std::make_unique<DbusToPLDMEvent>(
            sockfd, hostEID, dbusImplReq, &reqHandler);
    }
    std::unique_ptr<oem_platform::Handler> oemPlatformHandler{};

#ifdef OEM_IBM
    std::unique_ptr<pldm::responder::CodeUpdate> codeUpdate =
        std::make_unique<pldm::responder::CodeUpdate>(&dbusHandler);
    codeUpdate->clearDirPath(LID_STAGING_DIR);
    oemPlatformHandler = std::make_unique<oem_ibm_platform::Handler>(
        &dbusHandler, codeUpdate.get(), sockfd, hostEID, dbusImplReq, event,
        &reqHandler);
    codeUpdate->setOemPlatformHandler(oemPlatformHandler.get());
    invoker.registerHandler(PLDM_OEM, std::make_unique<oem_ibm::Handler>(
                                          oemPlatformHandler.get(), sockfd,
                                          hostEID, &dbusImplReq, &reqHandler));
#endif
    invoker.registerHandler(
        PLDM_BIOS, std::make_unique<bios::Handler>(sockfd, hostEID,
                                                   &dbusImplReq, &reqHandler));
    auto fruHandler = std::make_unique<fru::Handler>(
        FRU_JSONS_DIR, FRU_MASTER_JSON, pdrRepo.get(), entityTree.get(),
        bmcEntityTree.get());
    // FRU table is built lazily when a FRU command or Get PDR command is
    // handled. To enable building FRU table, the FRU handler is passed to the
    // Platform handler.
    auto platformHandler = std::make_unique<platform::Handler>(
        &dbusHandler, PDR_JSONS_DIR, pdrRepo.get(), hostPDRHandler.get(),
        dbusToPLDMEventHandler.get(), fruHandler.get(),
        oemPlatformHandler.get(), event, true);
#ifdef OEM_IBM
    pldm::responder::oem_ibm_platform::Handler* oemIbmPlatformHandler =
        dynamic_cast<pldm::responder::oem_ibm_platform::Handler*>(
            oemPlatformHandler.get());
    oemIbmPlatformHandler->setPlatformHandler(platformHandler.get());
#endif

    invoker.registerHandler(PLDM_PLATFORM, std::move(platformHandler));
    invoker.registerHandler(
        PLDM_BASE,
        std::make_unique<base::Handler>(hostEID, dbusImplReq, event,
                                        oemPlatformHandler.get(), &reqHandler));
    invoker.registerHandler(PLDM_FRU, std::move(fruHandler));
    dbus_api::Pdr dbusImplPdr(bus, "/xyz/openbmc_project/pldm", pdrRepo.get());
    sdbusplus::xyz::openbmc_project::PLDM::server::Event dbusImplEvent(
        bus, "/xyz/openbmc_project/pldm");

#endif

    std::unique_ptr<fw_update::Manager> fwManager =
        std::make_unique<fw_update::Manager>(event, reqHandler, dbusImplReq,
                                             FW_UPDATE_CONFIG_JSON, fwDebug);
    std::unique_ptr<platform_mc::Manager> platformManager =
        std::make_unique<platform_mc::Manager>(event, reqHandler, dbusImplReq);
    pldm::mctp_socket::Handler sockHandler(
        event, reqHandler, invoker, *(fwManager.get()), sockManager, verbose);

    std::unique_ptr<MctpDiscovery> mctpDiscoveryHandler =
        std::make_unique<MctpDiscovery>(
            bus, sockHandler,
            std::initializer_list<MctpDiscoveryHandlerIntf*>{fwManager.get()});

    bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);
    bus.request_name("xyz.openbmc_project.PLDM");

#ifdef LIBPLDMRESPONDER
    if (hostPDRHandler)
    {
        hostPDRHandler->setHostFirmwareCondition();
    }
#endif
    stdplus::signal::block(SIGUSR1);
    sdeventplus::source::Signal sigUsr1(
        event, SIGUSR1, std::bind_front(&interruptFlightRecorderCallBack));
    auto returnCode = event.loop();

    if (returnCode)
    {
        exit(EXIT_FAILURE);
    }

    exit(EXIT_SUCCESS);
}
