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
#include "platform-mc/pldmServiceReadyInterface.hpp"
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

#include <phosphor-logging/lg2.hpp>
#include <sdeventplus/event.hpp>
#include <sdeventplus/source/io.hpp>
#include <sdeventplus/source/signal.hpp>
#include <stdplus/signal.hpp>
#include <tal.hpp>

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
    lg2::error("Received SIGUR1(10) Signal interrupt");

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
#ifdef PLDM_TYPE2
    std::cerr
        << "  --num-sens-wo-aux-name Optional flag to enable Numeric Sensors without Auxillary Names\n";
#endif
    std::cerr << "Defaulted settings:  --verbose=0 \n";
}

int main(int argc, char** argv)
{

    bool verbose = false;
    bool fwDebug = false;
#ifdef PLDM_TYPE2
    bool numericSensorsWithoutAuxName = false;
#endif
    int argflag;
    static struct option long_options[] = {
        {"verbose", required_argument, 0, 'v'},
        {"fw-debug", no_argument, 0, 'd'},
#ifdef PLDM_TYPE2
        {"num-sens-wo-aux-name", no_argument, 0, 'u'},
#endif
        {0, 0, 0, 0}};

#ifdef PLDM_TYPE2
    while ((argflag = getopt_long(argc, argv, "v:du", long_options, nullptr)) >=
           0)
#else
    while ((argflag = getopt_long(argc, argv, "v:d", long_options, nullptr)) >=
           0)
#endif
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
            case 'u':
#ifdef PLDM_TYPE2
                numericSensorsWithoutAuxName = true;
#endif
                break;
            default:
                exit(EXIT_FAILURE);
        }
    }

    auto event = Event::get_default();
    auto& bus = pldm::utils::DBusHandler::getBus();
    sdbusplus::server::manager::manager objManager(bus, "/");
    PldmServiceReadyIntf::initialize(bus, "/xyz/openbmc_project/pldm");
    sdbusplus::server::manager::manager sensorsObjManager(
        bus, "/xyz/openbmc_project/sensors");
    dbus_api::Requester dbusImplReq(bus, "/xyz/openbmc_project/pldm");

    event.set_watchdog(true);

    Invoker invoker{};
    mctp_socket::Manager sockManager;
    requester::Handler<requester::Request> reqHandler(event, dbusImplReq,
                                                      sockManager, verbose);
    DBusHandler dbusHandler;

    std::unique_ptr<fw_update::Manager> fwManager =
        std::make_unique<fw_update::Manager>(event, reqHandler, dbusImplReq,
                                             FW_UPDATE_CONFIG_JSON,
                                             &dbusHandler, fwDebug);

#ifdef PLDM_TYPE2
    std::unique_ptr<platform_mc::Manager> platformManager =
        std::make_unique<platform_mc::Manager>(event, reqHandler, dbusImplReq,
                                               *(fwManager.get()), verbose,
                                               numericSensorsWithoutAuxName);

    // Initializing telemetry for pldmd
    if (tal::TelemetryAggregator::namespaceInit(tal::ProcessType::Producer,
                                                "pldmd"))
    {
        lg2::info("Initialized tal from pldmd");
    }
#endif

    try
    {
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
        auto hostEID = pldm::utils::readHostEID();
        if (hostEID)
        {
            hostPDRHandler = std::make_shared<HostPDRHandler>(
                sockfd, hostEID, event, pdrRepo.get(), EVENTS_JSONS_DIR,
                entityTree.get(), bmcEntityTree.get(), dbusImplReq,
                &reqHandler);
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
        invoker.registerHandler(PLDM_OEM,
                                std::make_unique<oem_ibm::Handler>(
                                    oemPlatformHandler.get(), sockfd, hostEID,
                                    &dbusImplReq, &reqHandler));
#endif
        invoker.registerHandler(
            PLDM_BIOS, std::make_unique<bios::Handler>(
                           sockfd, hostEID, &dbusImplReq, &reqHandler));
        auto fruHandler = std::make_unique<fru::Handler>(
            FRU_JSONS_DIR, FRU_MASTER_JSON, pdrRepo.get(), entityTree.get(),
            bmcEntityTree.get());
        // FRU table is built lazily when a FRU command or Get PDR command is
        // handled. To enable building FRU table, the FRU handler is passed to
        // the Platform handler.

#ifdef PLDM_TYPE2
        pldm::responder::platform::EventMap addOnEventHandlers{
            {PLDM_OEM_EVENT_CLASS_0xFA,
             {[&platformManager](const pldm_msg* request, size_t payloadLength,
                                 uint8_t formatVersion, uint8_t tid,
                                 size_t eventDataOffset,
                                 uint8_t& platformEventStatus) {
                 return platformManager->handleCperEvent(
                     request, payloadLength, formatVersion, tid,
                     eventDataOffset, platformEventStatus);
             }}},
            {PLDM_OEM_EVENT_CLASS_0xFB,
             {[&platformManager](const pldm_msg* request, size_t payloadLength,
                                 uint8_t formatVersion, uint8_t tid,
                                 size_t eventDataOffset,
                                 uint8_t& platformEventStatus) {
                 return platformManager->handleActiveFWVersionChangeEvent(
                     request, payloadLength, formatVersion, tid,
                     eventDataOffset, platformEventStatus);
             }}},
            {PLDM_OEM_EVENT_CLASS_0xFC,
             {[&platformManager](const pldm_msg* request, size_t payloadLength,
                                 uint8_t formatVersion, uint8_t tid,
                                 size_t eventDataOffset,
                                 uint8_t& platformEventStatus) {
                 return platformManager->handleSmbiosEvent(
                     request, payloadLength, formatVersion, tid,
                     eventDataOffset, platformEventStatus);
             }}},
            {PLDM_MESSAGE_POLL_EVENT,
             {[&platformManager](const pldm_msg* request, size_t payloadLength,
                                 uint8_t formatVersion, uint8_t tid,
                                 size_t eventDataOffset,
                                 uint8_t& platformEventStatus) {
                 return platformManager->handlePldmMessagePollEvent(
                     request, payloadLength, formatVersion, tid,
                     eventDataOffset, platformEventStatus);
             }}},
            {PLDM_SENSOR_EVENT,
             {[&platformManager](const pldm_msg* request, size_t payloadLength,
                                 uint8_t formatVersion, uint8_t tid,
                                 size_t eventDataOffset,
                                 uint8_t& platformEventStatus) {
                 return platformManager->handleSensorEvent(
                     request, payloadLength, formatVersion, tid,
                     eventDataOffset, platformEventStatus);
             }}}};
#endif

        auto platformHandler = std::make_unique<platform::Handler>(
            &dbusHandler, PDR_JSONS_DIR, pdrRepo.get(), hostPDRHandler.get(),
            dbusToPLDMEventHandler.get(), fruHandler.get(),
            oemPlatformHandler.get(), event, true
#ifdef PLDM_TYPE2
            ,
            addOnEventHandlers
#endif
        );
#ifdef OEM_IBM
        pldm::responder::oem_ibm_platform::Handler* oemIbmPlatformHandler =
            dynamic_cast<pldm::responder::oem_ibm_platform::Handler*>(
                oemPlatformHandler.get());
        oemIbmPlatformHandler->setPlatformHandler(platformHandler.get());
#endif

        invoker.registerHandler(PLDM_PLATFORM, std::move(platformHandler));
        invoker.registerHandler(PLDM_BASE,
                                std::make_unique<base::Handler>(
                                    hostEID, dbusImplReq, event,
                                    oemPlatformHandler.get(), &reqHandler));
        invoker.registerHandler(PLDM_FRU, std::move(fruHandler));
        dbus_api::Pdr dbusImplPdr(bus, "/xyz/openbmc_project/pldm",
                                  pdrRepo.get());
        sdbusplus::xyz::openbmc_project::PLDM::server::Event dbusImplEvent(
            bus, "/xyz/openbmc_project/pldm");

#endif

        pldm::mctp_socket::Handler sockHandler(event, reqHandler, invoker,
                                               *(fwManager.get()), sockManager,
                                               verbose);

        std::unique_ptr<MctpDiscovery> mctpDiscoveryHandler =
            std::make_unique<MctpDiscovery>(
                bus, sockHandler,
                // For refreshing the firmware version, it's important to invoke
                // PLDM type 5 code prior to type 2. The descriptor Map with
                // firmware version info is maintained in fwManager, so
                // that whenever platform event for version change is received
                // in plaformManager, the same descriptor Map is updated.
                std::initializer_list<MctpDiscoveryHandlerIntf*>{
                    fwManager.get(),
#ifdef PLDM_TYPE2
                    platformManager.get()
#endif
                });

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
    }
    catch (const std::exception& e)
    {
        lg2::error("Exception: {HANDLER_EXCEPTION}", "HANDLER_EXCEPTION",
                   e.what());
        exit(EXIT_FAILURE);
    }

    exit(EXIT_SUCCESS);
}
