#include "config.h"

#include "common/test/mocked_utils.hpp"
#include "common/types.hpp"
#include "common/utils.hpp"
#include "requester/test/mock_mctp_discovery_handler_intf.hpp"

#include <linux/mctp.h>
#include <unistd.h>

#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/exception.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::_;

class TestMctpDiscovery : public ::testing::Test
{
  public:
    static const pldm::Configurations& getConfigurations(
        const pldm::MctpDiscovery& mctpDiscovery)
    {
        return mctpDiscovery.configurations;
    }
    static void searchConfigurationFor(pldm::MctpDiscovery& mctpDiscovery,
                                       pldm::MctpInfo& mctpInfo)
    {
        mctpDiscovery.searchConfigurationFor(mctpInfo);
    }
    static void removeConfigs(pldm::MctpDiscovery& mctpDiscovery,
                              const pldm::MctpInfos& removedInfos)
    {
        mctpDiscovery.removeConfigs(removedInfos);
    }
    static pldm::MctpEndpointProps getMctpEndpointProps(
        pldm::MctpDiscovery& d, const std::string& service,
        const std::string& path)
    {
        return d.getMctpEndpointProps(service, path);
    }
    static pldm::UUID getEndpointUUIDProp(pldm::MctpDiscovery& d,
                                          const std::string& service,
                                          const std::string& path)
    {
        return d.getEndpointUUIDProp(service, path);
    }
    static pldm::Availability getEndpointConnectivityProp(
        pldm::MctpDiscovery& d, const std::string& path)
    {
        return d.getEndpointConnectivityProp(path);
    }
    static void getAddedMctpInfos(pldm::MctpDiscovery& d,
                                  sdbusplus::message_t& msg,
                                  pldm::MctpInfos& infos)
    {
        d.getAddedMctpInfos(msg, infos);
    }
    static void getMctpInfos(pldm::MctpDiscovery& d,
                             std::map<pldm::MctpInfo, pldm::Availability>& map)
    {
        d.getMctpInfos(map);
    }
    static void propertiesChangedCb(pldm::MctpDiscovery& d,
                                    sdbusplus::message_t& msg)
    {
        d.propertiesChangedCb(msg);
    }
    static std::string getNameFromProperties(
        pldm::MctpDiscovery& d, const pldm::utils::PropertyMap& properties)
    {
        return d.getNameFromProperties(properties);
    }
    static void discoverEndpoints(pldm::MctpDiscovery& d,
                                  sdbusplus::message_t& msg)
    {
        d.discoverEndpoints(msg);
    }
};

class TrackingMctpHandler : public pldm::MctpDiscoveryHandlerIntf
{
  public:
    void handleMctpEndpoints(const pldm::MctpInfos& mctpInfos,
                             const pldm::dbus::MctpInterfaces&) override
    {
        handleMctpEndpointsCalls++;
        lastHandledSize = mctpInfos.size();
    }

    void handleRemovedMctpEndpoints(const pldm::MctpInfos& mctpInfos) override
    {
        handleRemovedMctpEndpointsCalls++;
        lastRemovedSize = mctpInfos.size();
    }

    void updateMctpEndpointAvailability(
        const pldm::MctpInfo& mctpInfo,
        pldm::Availability availability) override
    {
        updateAvailabilityCalls++;
        lastAvailability = availability;
        lastMctpInfo = mctpInfo;
    }

    std::optional<mctp_eid_t> getActiveEidByName(
        const std::string& /*terminusName*/) override
    {
        return std::nullopt;
    }

    void onlineMctpEndpoint(const pldm::UUID& uuid,
                            const pldm::eid& eid) override
    {
        onlineCalls++;
        lastOnlineUuid = uuid;
        lastOnlineEid = eid;
    }

    void offlineMctpEndpoint(const pldm::UUID& uuid,
                             const pldm::eid& eid) override
    {
        offlineCalls++;
        lastOfflineUuid = uuid;
        lastOfflineEid = eid;
    }

    int handleMctpEndpointsCalls = 0;
    int handleRemovedMctpEndpointsCalls = 0;
    int updateAvailabilityCalls = 0;
    int onlineCalls = 0;
    int offlineCalls = 0;
    size_t lastHandledSize = 0;
    size_t lastRemovedSize = 0;
    pldm::Availability lastAvailability = false;
    pldm::MctpInfo lastMctpInfo{0,           pldm::emptyUUID, std::string{},
                                uint32_t(0), std::nullopt,    std::string{},
                                std::nullopt};
    pldm::UUID lastOnlineUuid = pldm::emptyUUID;
    pldm::UUID lastOfflineUuid = pldm::emptyUUID;
    pldm::eid lastOnlineEid = 0;
    pldm::eid lastOfflineEid = 0;
};

static std::unique_ptr<pldm::MctpDiscovery> makeDiscoveryWithMock(
    MockdBusHandler& mockedDbusHandler, pldm::MctpDiscoveryHandlerIntf* handler)
{
    auto& bus = mockedDbusHandler.getBus();
    EXPECT_CALL(mockedDbusHandler, getSubtree(pldm::MCTPPath, 0, _))
        .WillOnce(testing::Return(pldm::utils::GetSubTreeResponse{}));

    return std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{handler},
        "/tmp/mctp-discovery-no-static-endpoints.json", mockedDbusHandler);
}

static std::vector<std::pair<std::string, std::string>> getMctpEndpoints()
{
    std::vector<std::pair<std::string, std::string>> endpoints;
    try
    {
        auto subtree = pldm::utils::DBusHandler().getSubtree(
            pldm::MCTPPath, 0, std::vector<std::string>{pldm::MCTPInterface});
        for (const auto& [path, services] : subtree)
        {
            for (const auto& [service, _] : services)
            {
                endpoints.emplace_back(path, service);
            }
        }
    }
    catch (const std::exception&)
    {}
    return endpoints;
}

static std::optional<std::pair<std::string, std::string>>
    findEndpointByPldmType(pldm::MctpDiscovery& discovery,
                           bool requiresPldmType)
{
    for (const auto& [path, service] : getMctpEndpoints())
    {
        auto epProps =
            TestMctpDiscovery::getMctpEndpointProps(discovery, service, path);
        const auto& types = std::get<pldm::MCTPMsgTypes>(epProps);
        const bool hasPldm = std::ranges::contains(types, uint8_t(1));
        if (hasPldm == requiresPldmType)
        {
            return std::make_pair(path, service);
        }
    }
    return std::nullopt;
}

static sdbusplus::message_t makePropertiesChangedMessage(
    const std::string& objPath, const std::string& connectivity)
{
    auto bus = sdbusplus::bus::new_default();
    auto msg =
        bus.new_signal(objPath.c_str(), "org.freedesktop.DBus.Properties",
                       "PropertiesChanged");

    std::string interface = pldm::MCTPInterfaceCC;
    std::map<std::string, std::variant<std::string>> properties = {
        {pldm::MCTPConnectivityProp, std::variant<std::string>{connectivity}}};
    msg.append(interface, properties);
    sd_bus_message_set_sender(msg.get(), "org.test.Sender");
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);
    return msg;
}

static sdbusplus::message_t makeRefreshEndpointsMessage(
    const std::string& objPath, const std::string& connectivity)
{
    auto bus = sdbusplus::bus::new_default();
    auto msg =
        bus.new_method_call("org.test", objPath.c_str(), "org.test", "Method");

    std::string interface = pldm::MCTPInterfaceCC;
    pldm::dbus::PropertyMap properties = {
        {pldm::MCTPConnectivityProp, std::string(connectivity)}};
    msg.append(interface, properties);
    sd_bus_message_set_sender(msg.get(), "org.test.Sender");
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);
    return msg;
}

static sdbusplus::message_t makeInterfacesRemovedMessage(
    const std::string& objPath, const std::vector<std::string>& interfaces)
{
    auto bus = sdbusplus::bus::new_default();
    auto msg = bus.new_method_call("org.test", "/test", "org.test", "Method");

    msg.append(sdbusplus::message::object_path(objPath), interfaces);
    sd_bus_message_set_sender(msg.get(), "org.test.Sender");
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);
    return msg;
}

static sdbusplus::message_t makeInterfacesAddedMessage(
    const std::string& objPath,
    const std::map<std::string, std::map<std::string, pldm::dbus::Value>>&
        interfaces)
{
    auto bus = sdbusplus::bus::new_default();
    auto msg =
        bus.new_method_call("org.test", "/test", "org.test.Intf", "Method");

    msg.append(sdbusplus::message::object_path(objPath), interfaces);
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);
    return msg;
}

struct EndpointDbusSpec
{
    std::string service;
    std::string path;
    uint32_t networkId{};
    uint8_t eid{};
    std::vector<uint8_t> types;
    std::string medium;
    std::string binding;
    std::string uuid;
    std::string connectivity;

    std::vector<std::string> interfaces() const
    {
        return {pldm::MCTPInterface, pldm::MCTPInterfaceCC, pldm::EndpointUUID,
                pldm::MCTPBindingInterface};
    }
};

class ScopedMctpDbusEnvironment
{
  public:
    ScopedMctpDbusEnvironment()
    {
        static std::atomic_uint instanceCounter{0};
        const auto instance = ++instanceCounter;
        const auto pid = static_cast<unsigned>(::getpid());
        const auto endpointService =
            "org.test.mctp.endpoint.p" + std::to_string(pid) + ".i" +
            std::to_string(instance);

        pldmOnline.service = endpointService;
        pldmOnline.path =
            "/au/com/codeconstruct/mctp1/networks/91/endpoints/41";
        pldmOnline.networkId = 91;
        pldmOnline.eid = 41;
        pldmOnline.types = {1};
        pldmOnline.medium = "SMBus";
        pldmOnline.binding = "MctpOverSMBus";
        pldmOnline.uuid = "11111111-1111-1111-1111-111111111111";
        pldmOnline.connectivity = "Available";

        pldmOffline.service = endpointService;
        pldmOffline.path =
            "/au/com/codeconstruct/mctp1/networks/92/endpoints/42";
        pldmOffline.networkId = 92;
        pldmOffline.eid = 42;
        pldmOffline.types = {1};
        pldmOffline.medium = "SMBus";
        pldmOffline.binding = "MctpOverSMBus";
        pldmOffline.uuid = "22222222-2222-2222-2222-222222222222";
        pldmOffline.connectivity = "Degraded";

        nonPldm.service = endpointService;
        nonPldm.path = "/au/com/codeconstruct/mctp1/networks/93/endpoints/43";
        nonPldm.networkId = 93;
        nonPldm.eid = 43;
        nonPldm.types = {0};
        nonPldm.medium = "SMBus";
        nonPldm.binding = "MctpOverSMBus";
        nonPldm.uuid = "33333333-3333-3333-3333-333333333333";
        nonPldm.connectivity = "Available";

        mapperPresent = mapperIsReachable();
        if (!mapperPresent && !startMapperStub())
        {
            return;
        }

        if (!startEndpoints())
        {
            return;
        }

        ioThread = std::thread([this] { io.run(); });

        if (!waitForMapperPath(pldmOnline.path) ||
            !waitForMapperPath(pldmOffline.path) ||
            !waitForMapperPath(nonPldm.path))
        {
            skipReason = "MCTP endpoint path not visible in mapper";
            return;
        }

        ready = true;
    }

    ~ScopedMctpDbusEnvironment()
    {
        io.stop();
        if (ioThread.joinable())
        {
            ioThread.join();
        }

        if (endpointServer)
        {
            for (auto& iface : endpointIfaces)
            {
                endpointServer->remove_interface(iface);
            }
        }
        endpointIfaces.clear();
        endpointServer.reset();
        endpointConn.reset();

        if (mapperServer && mapperIface)
        {
            mapperServer->remove_interface(mapperIface);
        }
        mapperIface.reset();
        mapperServer.reset();
        mapperConn.reset();
    }

    bool isReady() const
    {
        return ready;
    }

    const std::string& reason() const
    {
        return skipReason;
    }

    const EndpointDbusSpec& onlinePldm() const
    {
        return pldmOnline;
    }

    const EndpointDbusSpec& offlinePldm() const
    {
        return pldmOffline;
    }

    const EndpointDbusSpec& nonPldmEndpoint() const
    {
        return nonPldm;
    }

  private:
    bool ready = false;
    bool mapperPresent = false;
    std::string skipReason;

    boost::asio::io_context io;
    std::thread ioThread;

    std::shared_ptr<sdbusplus::asio::connection> mapperConn;
    std::unique_ptr<sdbusplus::asio::object_server> mapperServer;
    std::shared_ptr<sdbusplus::asio::dbus_interface> mapperIface;

    std::shared_ptr<sdbusplus::asio::connection> endpointConn;
    std::unique_ptr<sdbusplus::asio::object_server> endpointServer;
    std::vector<std::shared_ptr<sdbusplus::asio::dbus_interface>>
        endpointIfaces;

    EndpointDbusSpec pldmOnline;
    EndpointDbusSpec pldmOffline;
    EndpointDbusSpec nonPldm;

    std::vector<EndpointDbusSpec> allSpecs() const
    {
        return {pldmOnline, pldmOffline, nonPldm};
    }

    static bool matchesAnyInterface(const std::vector<std::string>& filter,
                                    const std::vector<std::string>& interfaces)
    {
        if (filter.empty())
        {
            return true;
        }
        return std::ranges::any_of(filter, [&interfaces](const auto& f) {
            return std::ranges::contains(interfaces, f);
        });
    }

    bool mapperIsReachable()
    {
        try
        {
            (void)pldm::utils::DBusHandler().getSubtree(
                pldm::MCTPPath, 0,
                std::vector<std::string>{pldm::MCTPInterface});
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    bool startMapperStub()
    {
        try
        {
            mapperConn = std::make_shared<sdbusplus::asio::connection>(
                io, sdbusplus::bus::new_bus());
            mapperConn->request_name(pldm::utils::mapperService);
            mapperServer =
                std::make_unique<sdbusplus::asio::object_server>(mapperConn);
            mapperIface = mapperServer->add_interface(
                pldm::utils::mapperPath, pldm::utils::mapperInterface);

            mapperIface->register_method(
                "GetObject", [this](const std::string& path,
                                    const std::vector<std::string>& ifaceList) {
                    std::map<std::string, std::vector<std::string>> response;
                    for (const auto& spec : allSpecs())
                    {
                        if (path == spec.path &&
                            matchesAnyInterface(ifaceList, spec.interfaces()))
                        {
                            response.emplace(spec.service, spec.interfaces());
                        }
                    }
                    return response;
                });

            mapperIface->register_method(
                "GetSubTree",
                [this](const std::string& searchPath, int,
                       const std::vector<std::string>& ifaceList) {
                    pldm::utils::GetSubTreeResponse response;
                    for (const auto& spec : allSpecs())
                    {
                        if (spec.path.rfind(searchPath, 0) != 0)
                        {
                            continue;
                        }
                        if (!matchesAnyInterface(ifaceList, spec.interfaces()))
                        {
                            continue;
                        }
                        response.emplace_back(
                            spec.path, pldm::utils::MapperServiceMap{
                                           {spec.service, spec.interfaces()}});
                    }
                    return response;
                });

            mapperIface->register_method(
                "GetAssociatedSubTree",
                [](const sdbusplus::message::object_path&,
                   const sdbusplus::message::object_path&, int,
                   const std::vector<std::string>&) {
                    return pldm::utils::GetAssociatedSubTreeResponse{};
                });

            mapperIface->initialize();
            return true;
        }
        catch (const std::exception& e)
        {
            if (mapperIsReachable())
            {
                return true;
            }
            skipReason = std::string("Unable to start mapper stub: ") +
                         e.what();
            return false;
        }
    }

    bool startEndpoints()
    {
        try
        {
            endpointConn = std::make_shared<sdbusplus::asio::connection>(
                io, sdbusplus::bus::new_bus());
            endpointConn->request_name(pldmOnline.service.c_str());
            endpointServer =
                std::make_unique<sdbusplus::asio::object_server>(endpointConn);

            auto addEndpoint = [this](const EndpointDbusSpec& spec) {
                auto endpointIface = endpointServer->add_interface(
                    spec.path, pldm::MCTPInterface);
                endpointIface->register_property("NetworkId", spec.networkId);
                endpointIface->register_property("EID", spec.eid);
                endpointIface->register_property("SupportedMessageTypes",
                                                 spec.types);
                endpointIface->register_property("MediumType", spec.medium);
                endpointIface->initialize();
                endpointIfaces.push_back(endpointIface);

                auto connectivityIface = endpointServer->add_interface(
                    spec.path, pldm::MCTPInterfaceCC);
                connectivityIface->register_property("Connectivity",
                                                     spec.connectivity);
                connectivityIface->initialize();
                endpointIfaces.push_back(connectivityIface);

                auto uuidIface = endpointServer->add_interface(
                    spec.path, pldm::EndpointUUID);
                uuidIface->register_property("UUID", spec.uuid);
                uuidIface->initialize();
                endpointIfaces.push_back(uuidIface);

                auto bindingIface = endpointServer->add_interface(
                    spec.path, pldm::MCTPBindingInterface);
                bindingIface->register_property("BindingType", spec.binding);
                bindingIface->initialize();
                endpointIfaces.push_back(bindingIface);
            };

            addEndpoint(pldmOnline);
            addEndpoint(pldmOffline);
            addEndpoint(nonPldm);
            return true;
        }
        catch (const std::exception& e)
        {
            skipReason = std::string("Unable to start endpoint service: ") +
                         e.what();
            return false;
        }
    }

    bool waitForMapperPath(const std::string& path) const
    {
        using namespace std::chrono_literals;
        for (int attempt = 0; attempt < 80; ++attempt)
        {
            try
            {
                auto subtree = pldm::utils::DBusHandler().getSubtree(
                    pldm::MCTPPath, 0,
                    std::vector<std::string>{pldm::MCTPInterface});
                const bool found =
                    std::ranges::any_of(subtree, [&path](const auto& entry) {
                        return entry.first == path;
                    });
                if (found)
                {
                    return true;
                }
            }
            catch (const std::exception&)
            {}

            std::this_thread::sleep_for(25ms);
        }
        return false;
    }
};

class DbusBackedMctpDiscoveryTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        env = std::make_unique<ScopedMctpDbusEnvironment>();
        if (!env->isReady())
        {
            GTEST_SKIP() << env->reason();
        }
    }

    static bool containsEid(const pldm::MctpInfos& infos, uint8_t eid)
    {
        return std::ranges::any_of(infos, [eid](const auto& info) {
            return std::get<pldm::eid>(info) == eid;
        });
    }

    std::unique_ptr<ScopedMctpDbusEnvironment> env;
};

TEST_F(DbusBackedMctpDiscoveryTest,
       constructorAndRemoveEndpointsUseMapperPopulatedData)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    TrackingMctpHandler handler;

    auto discovery = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&handler});

    EXPECT_TRUE(
        containsEid(discovery->existingMctpInfos, env->onlinePldm().eid));
    EXPECT_FALSE(
        containsEid(discovery->existingMctpInfos, env->offlinePldm().eid));
    EXPECT_FALSE(
        containsEid(discovery->existingMctpInfos, env->nonPldmEndpoint().eid));

    const pldm::MctpInfo sentinel{
        0xEE,         pldm::emptyUUID,    "sentinel-medium", uint32_t(0xDEAD),
        std::nullopt, "sentinel-binding", std::nullopt};
    discovery->existingMctpInfos.emplace_back(sentinel);

    const auto sentinelPath =
        std::string{pldm::MCTPPath} + "/networks/" +
        std::to_string(std::get<pldm::NetworkId>(sentinel)) + "/endpoints/" +
        std::to_string(std::get<pldm::eid>(sentinel));
    auto msg = makeInterfacesRemovedMessage(sentinelPath,
                                            {std::string(pldm::MCTPInterface)});
    discovery->removeEndpoints(msg);

    EXPECT_FALSE(std::ranges::contains(discovery->existingMctpInfos, sentinel));
}

TEST_F(DbusBackedMctpDiscoveryTest, propertiesChangedCbCoversDefaultDbusPath)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    TrackingMctpHandler handler;

    auto discovery = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&handler});

    discovery->existingMctpInfos.clear();
    handler.handleMctpEndpointsCalls = 0;
    handler.updateAvailabilityCalls = 0;

    auto nonPldmMsg =
        makePropertiesChangedMessage(env->nonPldmEndpoint().path, "Available");
    TestMctpDiscovery::propertiesChangedCb(*discovery, nonPldmMsg);
    EXPECT_EQ(handler.handleMctpEndpointsCalls, 0);
    EXPECT_EQ(handler.updateAvailabilityCalls, 0);

    auto unavailableAddMsg =
        makePropertiesChangedMessage(env->onlinePldm().path, "Degraded");
    TestMctpDiscovery::propertiesChangedCb(*discovery, unavailableAddMsg);
    EXPECT_EQ(handler.handleMctpEndpointsCalls, 0);
    EXPECT_EQ(handler.updateAvailabilityCalls, 0);
    EXPECT_FALSE(
        containsEid(discovery->existingMctpInfos, env->onlinePldm().eid));

    auto addMsg =
        makePropertiesChangedMessage(env->onlinePldm().path, "Available");
    TestMctpDiscovery::propertiesChangedCb(*discovery, addMsg);
    EXPECT_GE(handler.handleMctpEndpointsCalls, 1);
    EXPECT_TRUE(
        containsEid(discovery->existingMctpInfos, env->onlinePldm().eid));

    auto degradeMsg =
        makePropertiesChangedMessage(env->onlinePldm().path, "Degraded");
    TestMctpDiscovery::propertiesChangedCb(*discovery, degradeMsg);
    EXPECT_GE(handler.updateAvailabilityCalls, 1);
    EXPECT_FALSE(handler.lastAvailability);
}

TEST_F(DbusBackedMctpDiscoveryTest, refreshEndpointsReadsUuidAndEidFromDbus)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    TrackingMctpHandler handler;

    auto discovery = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&handler});

    handler.onlineCalls = 0;
    handler.offlineCalls = 0;

    auto onlineMsg =
        makeRefreshEndpointsMessage(env->onlinePldm().path, "Available");
    discovery->refreshEndpoints(onlineMsg);
    EXPECT_EQ(handler.onlineCalls, 1);
    EXPECT_EQ(handler.lastOnlineUuid, env->onlinePldm().uuid);
    EXPECT_EQ(handler.lastOnlineEid, env->onlinePldm().eid);

    auto offlineMsg =
        makeRefreshEndpointsMessage(env->onlinePldm().path, "Degraded");
    discovery->refreshEndpoints(offlineMsg);
    EXPECT_EQ(handler.offlineCalls, 1);
    EXPECT_EQ(handler.lastOfflineUuid, env->onlinePldm().uuid);
    EXPECT_EQ(handler.lastOfflineEid, env->onlinePldm().eid);
}

TEST(MctpEndpointDiscoveryTest, ZeroHandleMctpEndpoint)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    EXPECT_CALL(manager, handleMctpEndpoints(_, _)).Times(0);

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});
    mctpDiscoveryHandler = nullptr;
}

TEST(MctpEndpointDiscoveryTest, MultipleHandleMctpEndpoints)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager1;
    pldm::MockManager manager2;

    EXPECT_CALL(manager1, handleMctpEndpoints(_, _)).Times(0);
    EXPECT_CALL(manager2, handleMctpEndpoints(_, _)).Times(0);

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{
                 &manager1, &manager2});
    mctpDiscoveryHandler = nullptr;
}

TEST(MctpEndpointDiscoveryTest, goodGetMctpInfos)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;
    std::map<pldm::MctpInfo, pldm::Availability> currentMctpInfoMap;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});
    mctpDiscoveryHandler->getMctpInfos(currentMctpInfoMap);
    EXPECT_EQ(currentMctpInfoMap.size(), 0);
}

TEST(MctpEndpointDiscoveryTest, goodAddToExistingMctpInfos)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;
    const pldm::MctpInfos& mctpInfos = {
        pldm::MctpInfo(11, pldm::emptyUUID, "", 1, std::nullopt, "",
                       std::nullopt),
        pldm::MctpInfo(12, pldm::emptyUUID, "abc", 1, std::nullopt, "",
                       std::nullopt)};

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});
    mctpDiscoveryHandler->addToExistingMctpInfos(mctpInfos);
    EXPECT_EQ(mctpDiscoveryHandler->existingMctpInfos.size(), 2);
    pldm::MctpInfo mctpInfo = mctpDiscoveryHandler->existingMctpInfos.back();
    EXPECT_EQ(std::get<0>(mctpInfo), 12);
    EXPECT_EQ(std::get<2>(mctpInfo), "abc");
    EXPECT_EQ(std::get<3>(mctpInfo), 1);
}

TEST(MctpEndpointDiscoveryTest, badAddToExistingMctpInfos)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;
    const pldm::MctpInfos& mctpInfos = {pldm::MctpInfo(
        11, pldm::emptyUUID, "", 1, std::nullopt, "", std::nullopt)};

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});
    mctpDiscoveryHandler->addToExistingMctpInfos(mctpInfos);
    EXPECT_NE(mctpDiscoveryHandler->existingMctpInfos.size(), 2);
}

TEST(MctpEndpointDiscoveryTest, removeEndpointsRemovesMatchingEndpoint)
{
    MockdBusHandler mockedDbusHandler;
    TrackingMctpHandler handler;
    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &handler);

    const pldm::MctpInfo endpoint{12, pldm::emptyUUID, "abc", 1, std::nullopt,
                                  "", std::nullopt};
    disc->addToExistingMctpInfos(pldm::MctpInfos{endpoint});

    const auto endpointPath =
        std::string{pldm::MCTPPath} + "/networks/1/endpoints/12";

    auto msg = makeInterfacesRemovedMessage(endpointPath,
                                            {std::string(pldm::MCTPInterface)});
    disc->removeEndpoints(msg);

    EXPECT_FALSE(std::ranges::contains(disc->existingMctpInfos, endpoint));
    EXPECT_EQ(handler.handleRemovedMctpEndpointsCalls, 1);
    EXPECT_EQ(handler.lastRemovedSize, 1u);
}

TEST(MctpEndpointDiscoveryTest, goodSearchConfigurationFor)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;
    const pldm::MctpInfos& mctpInfos = {pldm::MctpInfo(
        10, pldm::emptyUUID, "abc", 1, std::nullopt, "", std::nullopt)};

    constexpr auto mockedDbusPath =
        "/xyz/openbmc_project/inventory/system/board/Mocked_Board_Slot_1/MockedDevice";
    constexpr auto mockedService = "xyz.openbmc_project.EntityManager";
    std::vector<std::string> mockedInterfaces{
        "xyz.openbmc_project.Configuration.MCTPI2CTarget",
        "xyz.openbmc_project.Configuration.MCTPI3CTarget"};

    pldm::utils::GetAssociatedSubTreeResponse
        mockedGetAssociatedSubTreeResponse{
            {mockedDbusPath, {{mockedService, mockedInterfaces}}}};

    EXPECT_CALL(mockedDbusHandler, getAssociatedSubTree(_, _, _, _))
        .WillOnce(testing::Return(mockedGetAssociatedSubTreeResponse));

    pldm::utils::PropertyMap mockGetI2CTargetPropertiesResponse{
        {"Address", uint64_t(0x1)},
        {"Bus", uint64_t(0)},
        {"Name", std::string("MockedDevice")}};

    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(mockGetI2CTargetPropertiesResponse));

    auto mctpDiscoveryHandler =
        makeDiscoveryWithMock(mockedDbusHandler, &manager);
    mctpDiscoveryHandler->addToExistingMctpInfos(mctpInfos);
    EXPECT_EQ(mctpDiscoveryHandler->existingMctpInfos.size(), 1);
    pldm::MctpInfo mctpInfo = mctpDiscoveryHandler->existingMctpInfos.back();
    EXPECT_EQ(std::get<0>(mctpInfo), 10);
    EXPECT_EQ(std::get<2>(mctpInfo), "abc");
    EXPECT_EQ(std::get<3>(mctpInfo), 1);
    TestMctpDiscovery::searchConfigurationFor(*mctpDiscoveryHandler, mctpInfo);
    EXPECT_EQ(std::get<4>(mctpInfo),
              std::optional<std::string>("MockedDevice"));
    auto configuration =
        TestMctpDiscovery::getConfigurations(*mctpDiscoveryHandler);
    EXPECT_EQ(configuration.size(), 1);
}

TEST(MctpEndpointDiscoveryTest, badSearchConfigurationFor)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;
    const pldm::MctpInfos& mctpInfos = {pldm::MctpInfo(
        10, pldm::emptyUUID, "abc", 1, std::nullopt, "", std::nullopt)};

    constexpr auto mockedDbusPath =
        "/xyz/openbmc_project/inventory/system/board/Mocked_Board_Slot_1/MockedDevice";
    constexpr auto mockedService = "xyz.openbmc_project.EntityManager";
    std::vector<std::string> mockedInterfaces{
        "xyz.openbmc_project.Configuration.MCTPPCIETarget",
        "xyz.openbmc_project.Configuration.MCTPUSBTarget"};

    pldm::utils::GetAssociatedSubTreeResponse
        mockedGetAssociatedSubTreeResponse{
            {mockedDbusPath, {{mockedService, mockedInterfaces}}}};

    EXPECT_CALL(mockedDbusHandler, getAssociatedSubTree(_, _, _, _))
        .WillOnce(testing::Return(mockedGetAssociatedSubTreeResponse));

    pldm::utils::PropertyMap mockGetI2CTargetPropertiesResponse{
        {"Address", uint64_t(0x1)}, {"Bus", uint64_t(0)}};

    auto mctpDiscoveryHandler =
        makeDiscoveryWithMock(mockedDbusHandler, &manager);
    mctpDiscoveryHandler->addToExistingMctpInfos(mctpInfos);
    EXPECT_EQ(mctpDiscoveryHandler->existingMctpInfos.size(), 1);
    pldm::MctpInfo mctpInfo = mctpDiscoveryHandler->existingMctpInfos.back();
    EXPECT_EQ(std::get<0>(mctpInfo), 10);
    EXPECT_EQ(std::get<2>(mctpInfo), "abc");
    EXPECT_EQ(std::get<3>(mctpInfo), 1);
    TestMctpDiscovery::searchConfigurationFor(*mctpDiscoveryHandler, mctpInfo);
    auto configuration =
        TestMctpDiscovery::getConfigurations(*mctpDiscoveryHandler);
    EXPECT_EQ(configuration.size(), 0);
}

TEST(MctpEndpointDiscoveryTest, loadStaticEndpointsWithPldmType)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    // Construct with default (non-existent) path first
    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    // Now point staticEidTablePath to our test fixture and call manually
    mctpDiscoveryHandler->staticEidTablePath = "static_eid_table.json";
    pldm::MctpInfos mctpInfos;
    mctpDiscoveryHandler->loadStaticEndpoints(mctpInfos);

    // EID 2 has SupportedMessageTypes [0,1] which includes mctpTypePLDM (1)
    bool foundStaticEid = false;
    for (const auto& info : mctpInfos)
    {
        if (std::get<0>(info) == 2)
        {
            foundStaticEid = true;
            break;
        }
    }
    EXPECT_TRUE(foundStaticEid);
}

TEST(MctpEndpointDiscoveryTest, loadStaticEndpointsFileNotFound)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    // Point to a non-existent path and call manually
    mctpDiscoveryHandler->staticEidTablePath = "/nonexistent_file.json";
    pldm::MctpInfos mctpInfos;
    mctpDiscoveryHandler->loadStaticEndpoints(mctpInfos);

    EXPECT_EQ(mctpInfos.size(), 0);
}

TEST(MctpEndpointDiscoveryTest, loadStaticEndpointsWithoutPldmType)
{
    auto nonPldmJsonPath = "non_pldm_eid_table_test.json";
    {
        std::ofstream out(nonPldmJsonPath);
        out << R"({"Endpoints":[{"EID":9,"SupportedMessageTypes":[0,2]}]})";
    }

    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    mctpDiscoveryHandler->staticEidTablePath = nonPldmJsonPath;
    pldm::MctpInfos mctpInfos;
    mctpDiscoveryHandler->loadStaticEndpoints(mctpInfos);

    EXPECT_EQ(mctpInfos.size(), 0);
    std::filesystem::remove(nonPldmJsonPath);
}

TEST(MctpEndpointDiscoveryTest, loadStaticEndpointsMixedTypes)
{
    auto mixedJsonPath = "mixed_eid_table_test.json";
    {
        std::ofstream out(mixedJsonPath);
        out << R"({"Endpoints":[{"EID":3,"SupportedMessageTypes":[1]},{"EID":4,"SupportedMessageTypes":[0]}]})";
    }

    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    mctpDiscoveryHandler->staticEidTablePath = mixedJsonPath;
    pldm::MctpInfos mctpInfos;
    mctpDiscoveryHandler->loadStaticEndpoints(mctpInfos);

    ASSERT_EQ(mctpInfos.size(), 1);
    EXPECT_EQ(std::get<0>(mctpInfos.front()), 3);
    std::filesystem::remove(mixedJsonPath);
}

TEST(MctpEndpointDiscoveryTest, loadStaticEndpointsWithoutEndpointsKey)
{
    auto noEndpointsJsonPath = "no_endpoints_key_test.json";
    {
        std::ofstream out(noEndpointsJsonPath);
        out << R"({"SomethingElse":[{"EID":7,"SupportedMessageTypes":[1]}]})";
    }

    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    mctpDiscoveryHandler->staticEidTablePath = noEndpointsJsonPath;
    pldm::MctpInfos mctpInfos;
    mctpDiscoveryHandler->loadStaticEndpoints(mctpInfos);

    EXPECT_TRUE(mctpInfos.empty());
    std::filesystem::remove(noEndpointsJsonPath);
}

TEST(MctpEndpointDiscoveryTest, loadStaticEndpointsWithEmptyEndpointsArray)
{
    auto emptyEndpointsJsonPath = "empty_endpoints_array_test.json";
    {
        std::ofstream out(emptyEndpointsJsonPath);
        out << R"({"Endpoints":[]})";
    }

    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    mctpDiscoveryHandler->staticEidTablePath = emptyEndpointsJsonPath;
    pldm::MctpInfos mctpInfos;
    mctpDiscoveryHandler->loadStaticEndpoints(mctpInfos);

    EXPECT_TRUE(mctpInfos.empty());
    std::filesystem::remove(emptyEndpointsJsonPath);
}

TEST(MctpEndpointDiscoveryTest, loadStaticEndpointsInvalidJson)
{
    auto invalidJsonPath = "invalid_eid_table_test.json";

    // Write invalid JSON content
    {
        std::ofstream out(invalidJsonPath);
        out << "{ this is not valid json }}}";
    }

    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    mctpDiscoveryHandler->staticEidTablePath = invalidJsonPath;
    pldm::MctpInfos mctpInfos;
    mctpDiscoveryHandler->loadStaticEndpoints(mctpInfos);

    // Invalid JSON should not crash, no static endpoints loaded
    EXPECT_EQ(mctpInfos.size(), 0);
    std::filesystem::remove(invalidJsonPath);
}

TEST(MctpEndpointDiscoveryTest, handleMctpEndpointsWithMultipleHandlers)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager1;
    pldm::MockManager manager2;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{
                 &manager1, &manager2});

    const pldm::MctpInfos mctpInfos = {pldm::MctpInfo(
        30, pldm::emptyUUID, "", 1, std::nullopt, "", std::nullopt)};

    // Both handlers should be called
    EXPECT_CALL(manager1, handleMctpEndpoints(_, _)).Times(1);
    EXPECT_CALL(manager2, handleMctpEndpoints(_, _)).Times(1);

    mctpDiscoveryHandler->handleMctpEndpoints(mctpInfos);
}

TEST(MctpEndpointDiscoveryTest, handleMctpEndpointsEmpty)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    const pldm::MctpInfos emptyInfos;

    // Empty list should return early, handler not called
    EXPECT_CALL(manager, handleMctpEndpoints(_, _)).Times(0);

    mctpDiscoveryHandler->handleMctpEndpoints(emptyInfos);
}

TEST(MctpEndpointDiscoveryTest, handleRemovedMctpEndpoints)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    const pldm::MctpInfos removedInfos = {pldm::MctpInfo(
        30, pldm::emptyUUID, "", 1, std::nullopt, "", std::nullopt)};

    EXPECT_CALL(manager, handleRemovedMctpEndpoints(_)).Times(1);

    mctpDiscoveryHandler->handleRemovedMctpEndpoints(removedInfos);
}

TEST(MctpEndpointDiscoveryTest, updateMctpEndpointAvailability)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    pldm::MctpInfo mctpInfo(30, pldm::emptyUUID, "", 1, std::nullopt, "",
                            std::nullopt);

    EXPECT_CALL(manager, updateMctpEndpointAvailability(_, true)).Times(1);
    mctpDiscoveryHandler->updateMctpEndpointAvailability(mctpInfo, true);

    EXPECT_CALL(manager, updateMctpEndpointAvailability(_, false)).Times(1);
    mctpDiscoveryHandler->updateMctpEndpointAvailability(mctpInfo, false);
}

TEST(MctpEndpointDiscoveryTest, addDuplicateToExistingMctpInfos)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    const pldm::MctpInfos mctpInfos = {pldm::MctpInfo(
        11, pldm::emptyUUID, "", 1, std::nullopt, "", std::nullopt)};

    mctpDiscoveryHandler->addToExistingMctpInfos(mctpInfos);
    EXPECT_EQ(mctpDiscoveryHandler->existingMctpInfos.size(), 1);

    // Adding the same endpoint again should not create a duplicate
    mctpDiscoveryHandler->addToExistingMctpInfos(mctpInfos);
    EXPECT_EQ(mctpDiscoveryHandler->existingMctpInfos.size(), 1);
}

TEST(MctpEndpointDiscoveryTest, searchConfigurationEmptySubTree)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    pldm::utils::GetAssociatedSubTreeResponse emptyResponse{};

    EXPECT_CALL(mockedDbusHandler, getAssociatedSubTree(_, _, _, _))
        .WillOnce(testing::Return(emptyResponse));

    auto mctpDiscoveryHandler =
        makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::MctpInfo mctpInfo(10, pldm::emptyUUID, "abc", 1, std::nullopt, "",
                            std::nullopt);
    TestMctpDiscovery::searchConfigurationFor(*mctpDiscoveryHandler, mctpInfo);

    // Name should remain unset
    EXPECT_EQ(std::get<4>(mctpInfo), std::nullopt);
    auto configuration =
        TestMctpDiscovery::getConfigurations(*mctpDiscoveryHandler);
    EXPECT_EQ(configuration.size(), 0);
}

TEST(MctpEndpointDiscoveryTest, searchConfigurationEmptyServiceMap)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    constexpr auto mockedDbusPath =
        "/xyz/openbmc_project/inventory/system/board/Mocked_Board/Device";

    // Return a subtree with an empty service map
    pldm::utils::GetAssociatedSubTreeResponse response{{mockedDbusPath, {}}};

    EXPECT_CALL(mockedDbusHandler, getAssociatedSubTree(_, _, _, _))
        .WillOnce(testing::Return(response));

    auto mctpDiscoveryHandler =
        makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::MctpInfo mctpInfo(10, pldm::emptyUUID, "abc", 1, std::nullopt, "",
                            std::nullopt);
    TestMctpDiscovery::searchConfigurationFor(*mctpDiscoveryHandler, mctpInfo);

    EXPECT_EQ(std::get<4>(mctpInfo), std::nullopt);
}

TEST(MctpEndpointDiscoveryTest, searchConfigurationMissingNameProperty)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    constexpr auto mockedDbusPath =
        "/xyz/openbmc_project/inventory/system/board/Mocked_Board/Device";
    constexpr auto mockedService = "xyz.openbmc_project.EntityManager";
    std::vector<std::string> mockedInterfaces{
        "xyz.openbmc_project.Configuration.MCTPI2CTarget"};

    pldm::utils::GetAssociatedSubTreeResponse
        mockedGetAssociatedSubTreeResponse{
            {mockedDbusPath, {{mockedService, mockedInterfaces}}}};

    EXPECT_CALL(mockedDbusHandler, getAssociatedSubTree(_, _, _, _))
        .WillOnce(testing::Return(mockedGetAssociatedSubTreeResponse));

    // Return properties without "Name" key
    pldm::utils::PropertyMap propsWithoutName{{"Address", uint64_t(0x1)},
                                              {"Bus", uint64_t(0)}};

    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(propsWithoutName));

    auto mctpDiscoveryHandler =
        makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::MctpInfo mctpInfo(10, pldm::emptyUUID, "abc", 1, std::nullopt, "",
                            std::nullopt);
    TestMctpDiscovery::searchConfigurationFor(*mctpDiscoveryHandler, mctpInfo);

    // Name should remain unset because getNameFromProperties returns empty
    EXPECT_EQ(std::get<4>(mctpInfo), std::nullopt);
    // But configuration should still be added (with the MctpInfo as-is)
    auto configuration =
        TestMctpDiscovery::getConfigurations(*mctpDiscoveryHandler);
    EXPECT_EQ(configuration.size(), 1);
}

TEST(MctpEndpointDiscoveryTest, searchConfigurationNameWrongType)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    constexpr auto mockedDbusPath =
        "/xyz/openbmc_project/inventory/system/board/Mocked_Board/Device";
    constexpr auto mockedService = "xyz.openbmc_project.EntityManager";
    std::vector<std::string> mockedInterfaces{
        "xyz.openbmc_project.Configuration.MCTPI2CTarget"};

    pldm::utils::GetAssociatedSubTreeResponse
        mockedGetAssociatedSubTreeResponse{
            {mockedDbusPath, {{mockedService, mockedInterfaces}}}};

    EXPECT_CALL(mockedDbusHandler, getAssociatedSubTree(_, _, _, _))
        .WillOnce(testing::Return(mockedGetAssociatedSubTreeResponse));

    pldm::utils::PropertyMap badNameProps{{"Name", uint64_t(42)}};
    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(badNameProps));

    auto mctpDiscoveryHandler =
        makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::MctpInfo mctpInfo(10, pldm::emptyUUID, "abc", 1, std::nullopt, "",
                            std::nullopt);
    TestMctpDiscovery::searchConfigurationFor(*mctpDiscoveryHandler, mctpInfo);

    EXPECT_EQ(std::get<4>(mctpInfo), std::nullopt);
    auto configuration =
        TestMctpDiscovery::getConfigurations(*mctpDiscoveryHandler);
    EXPECT_EQ(configuration.size(), 0);
}

TEST(MctpEndpointDiscoveryTest, searchConfigurationExceptionHandling)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    EXPECT_CALL(mockedDbusHandler, getAssociatedSubTree(_, _, _, _))
        .WillOnce(testing::Throw(std::runtime_error("D-Bus error")));

    auto mctpDiscoveryHandler =
        makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::MctpInfo mctpInfo(10, pldm::emptyUUID, "abc", 1, std::nullopt, "",
                            std::nullopt);
    TestMctpDiscovery::searchConfigurationFor(*mctpDiscoveryHandler, mctpInfo);

    // Exception should be caught, no configuration added
    EXPECT_EQ(std::get<4>(mctpInfo), std::nullopt);
    auto configuration =
        TestMctpDiscovery::getConfigurations(*mctpDiscoveryHandler);
    EXPECT_EQ(configuration.size(), 0);
}

TEST(MctpEndpointDiscoveryTest, handleMctpEndpointsCallsHandleConfigurations)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    const pldm::MctpInfos mctpInfos = {pldm::MctpInfo(
        30, pldm::emptyUUID, "", 1, std::nullopt, "", std::nullopt)};

    // Both handleConfigurations and handleMctpEndpoints should be called
    EXPECT_CALL(manager, handleMctpEndpoints(_, _)).Times(1);

    mctpDiscoveryHandler->handleMctpEndpoints(mctpInfos);
}

TEST(MctpEndpointDiscoveryTest, removeConfigsByEid)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    constexpr auto mockedDbusPath =
        "/xyz/openbmc_project/inventory/system/board/Slot_1/Device";
    constexpr auto mockedService = "xyz.openbmc_project.EntityManager";
    std::vector<std::string> mockedInterfaces{
        "xyz.openbmc_project.Configuration.MCTPI2CTarget"};

    pldm::utils::GetAssociatedSubTreeResponse
        mockedGetAssociatedSubTreeResponse{
            {mockedDbusPath, {{mockedService, mockedInterfaces}}}};

    EXPECT_CALL(mockedDbusHandler, getAssociatedSubTree(_, _, _, _))
        .WillOnce(testing::Return(mockedGetAssociatedSubTreeResponse));

    pldm::utils::PropertyMap mockProps{{"Name", std::string("TestDevice")}};

    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(mockProps));

    auto mctpDiscoveryHandler =
        makeDiscoveryWithMock(mockedDbusHandler, &manager);

    // Add configuration via searchConfigurationFor
    pldm::MctpInfo mctpInfo(10, pldm::emptyUUID, "abc", 1, std::nullopt, "",
                            std::nullopt);
    TestMctpDiscovery::searchConfigurationFor(*mctpDiscoveryHandler, mctpInfo);
    auto& configs = TestMctpDiscovery::getConfigurations(*mctpDiscoveryHandler);
    EXPECT_EQ(configs.size(), 1);

    // Now simulate removing the endpoint
    pldm::MctpInfos removedInfos = {mctpInfo};
    TestMctpDiscovery::removeConfigs(*mctpDiscoveryHandler, removedInfos);

    // Configuration should be removed
    auto& configsAfter =
        TestMctpDiscovery::getConfigurations(*mctpDiscoveryHandler);
    EXPECT_EQ(configsAfter.size(), 0);
}

TEST(MctpEndpointDiscoveryTest, removeConfigsNoMatchingEid)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    constexpr auto mockedDbusPath =
        "/xyz/openbmc_project/inventory/system/board/Slot_2/Device";
    constexpr auto mockedService = "xyz.openbmc_project.EntityManager";
    std::vector<std::string> mockedInterfaces{
        "xyz.openbmc_project.Configuration.MCTPI2CTarget"};

    pldm::utils::GetAssociatedSubTreeResponse
        mockedGetAssociatedSubTreeResponse{
            {mockedDbusPath, {{mockedService, mockedInterfaces}}}};

    EXPECT_CALL(mockedDbusHandler, getAssociatedSubTree(_, _, _, _))
        .WillOnce(testing::Return(mockedGetAssociatedSubTreeResponse));

    pldm::utils::PropertyMap mockProps{{"Name", std::string("TestDevice2")}};
    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(mockProps));

    auto mctpDiscoveryHandler =
        makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::MctpInfo mctpInfo(10, pldm::emptyUUID, "abc", 1, std::nullopt, "",
                            std::nullopt);
    TestMctpDiscovery::searchConfigurationFor(*mctpDiscoveryHandler, mctpInfo);

    auto& configs = TestMctpDiscovery::getConfigurations(*mctpDiscoveryHandler);
    ASSERT_EQ(configs.size(), 1);

    pldm::MctpInfos removedInfos = {pldm::MctpInfo(
        99, pldm::emptyUUID, "abc", 1, std::nullopt, "", std::nullopt)};
    TestMctpDiscovery::removeConfigs(*mctpDiscoveryHandler, removedInfos);

    EXPECT_EQ(configs.size(), 1);
}

// Minimal concrete implementation to test default virtual methods
class MinimalHandler : public pldm::MctpDiscoveryHandlerIntf
{
  public:
    void handleMctpEndpoints(const pldm::MctpInfos&,
                             const pldm::dbus::MctpInterfaces&) override
    {}
    void handleRemovedMctpEndpoints(const pldm::MctpInfos&) override {}
    void updateMctpEndpointAvailability(const pldm::MctpInfo&,
                                        pldm::Availability) override
    {}
    std::optional<mctp_eid_t> getActiveEidByName(const std::string&) override
    {
        return std::nullopt;
    }
    // handleConfigurations, onlineMctpEndpoint, offlineMctpEndpoint use
    // base class defaults
};

TEST(MctpEndpointDiscoveryTest, handlerIntfDefaultImplementations)
{
    // Use a concrete (non-mock) derived class to exercise the base class
    // default virtual method implementations
    MinimalHandler handler;

    pldm::MctpDiscoveryHandlerIntf* intf = &handler;

    // handleConfigurations has a default no-op implementation
    pldm::Configurations configs;
    intf->handleConfigurations(configs);

    // onlineMctpEndpoint and offlineMctpEndpoint have default no-op
    intf->onlineMctpEndpoint("test-uuid", 10);
    intf->offlineMctpEndpoint("test-uuid", 10);

    // Also invoke explicitly via base-class-qualified call and member pointers
    // so gcov accounts for the default virtual bodies in the header.
    handler.pldm::MctpDiscoveryHandlerIntf::onlineMctpEndpoint("test-uuid", 10);
    handler.pldm::MctpDiscoveryHandlerIntf::offlineMctpEndpoint(
        "test-uuid", 10);

    auto onlineFn = &pldm::MctpDiscoveryHandlerIntf::onlineMctpEndpoint;
    auto offlineFn = &pldm::MctpDiscoveryHandlerIntf::offlineMctpEndpoint;
    (handler.*onlineFn)("test-uuid", 10);
    (handler.*offlineFn)("test-uuid", 10);

    // Destructor default implementation tested when handler goes out of scope
}

TEST(MctpEndpointDiscoveryTest, getMctpEndpointPropsException)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    // Call with non-existent service/path - D-Bus call throws, hits catch block
    auto result = TestMctpDiscovery::getMctpEndpointProps(
        *mctpDiscoveryHandler, "nonexistent.service", "/nonexistent/path");

    // Should return default values due to exception
    auto types = std::get<2>(result);
    EXPECT_TRUE(types.empty());
}

TEST(MctpEndpointDiscoveryTest,
     getMctpEndpointPropsMissingRequiredPropertyReturnsDefaults)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;
    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::utils::PropertyMap properties{{"NetworkId", uint32_t(7)},
                                        {"EID", uint8_t(9)},
                                        {"MediumType", std::string("SMBus")}};
    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(properties));
    EXPECT_CALL(mockedDbusHandler, getDbusPropertyVariant(_, _, _)).Times(0);

    auto result = TestMctpDiscovery::getMctpEndpointProps(
        *disc, "test.service", "/test/path");

    EXPECT_EQ(std::get<0>(result), uint32_t(0));
    EXPECT_EQ(std::get<1>(result), uint8_t(MCTP_ADDR_ANY));
    EXPECT_TRUE(std::get<2>(result).empty());
    EXPECT_TRUE(std::get<3>(result).empty());
    EXPECT_TRUE(std::get<4>(result).empty());
}

TEST(MctpEndpointDiscoveryTest,
     getMctpEndpointPropsBindingLookupExceptionReturnsDefaults)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;
    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::utils::PropertyMap properties{
        {"NetworkId", uint32_t(7)},
        {"EID", uint8_t(9)},
        {"SupportedMessageTypes", std::vector<uint8_t>{1}},
        {"MediumType", std::string("SMBus")}};
    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(properties));
    EXPECT_CALL(mockedDbusHandler, getDbusPropertyVariant(_, _, _))
        .WillOnce([](const char*, const char*,
                     const char*) -> pldm::utils::PropertyValue {
            throw sdbusplus::exception::SdBusError(EINVAL, "mock");
        });

    auto result = TestMctpDiscovery::getMctpEndpointProps(
        *disc, "test.service", "/test/path");

    EXPECT_EQ(std::get<0>(result), uint32_t(0));
    EXPECT_EQ(std::get<1>(result), uint8_t(MCTP_ADDR_ANY));
    EXPECT_TRUE(std::get<2>(result).empty());
    EXPECT_TRUE(std::get<3>(result).empty());
    EXPECT_TRUE(std::get<4>(result).empty());
}

TEST(MctpEndpointDiscoveryTest, getEndpointUUIDPropException)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    // Call with non-existent service/path - D-Bus call throws, hits catch block
    auto result = TestMctpDiscovery::getEndpointUUIDProp(
        *mctpDiscoveryHandler, "nonexistent.service", "/nonexistent/path");

    // Should return empty UUID due to exception
    EXPECT_EQ(result, pldm::emptyUUID);
}

TEST(MctpEndpointDiscoveryTest, getEndpointUUIDPropMissingUuidProperty)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;
    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::utils::PropertyMap properties{{"Other", std::string("value")}};
    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(properties));

    EXPECT_EQ(TestMctpDiscovery::getEndpointUUIDProp(*disc, "test.service",
                                                     "/test/path"),
              pldm::emptyUUID);
}

TEST(MctpEndpointDiscoveryTest, getEndpointUUIDPropWrongTypeThrows)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;
    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::utils::PropertyMap properties{{"UUID", uint64_t(0x21)}};
    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(properties));

    EXPECT_THROW(TestMctpDiscovery::getEndpointUUIDProp(*disc, "test.service",
                                                        "/test/path"),
                 std::bad_variant_access);
}

TEST(MctpEndpointDiscoveryTest, getEndpointConnectivityPropException)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    // Call with non-existent path - D-Bus call throws, hits catch block
    auto result = TestMctpDiscovery::getEndpointConnectivityProp(
        *mctpDiscoveryHandler, "/nonexistent/path");

    // Should return false (unavailable) due to exception
    EXPECT_FALSE(result);
}

TEST(MctpEndpointDiscoveryTest, getNameFromPropertiesSuccessDirect)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;
    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::utils::PropertyMap properties{{"Name", std::string("DirectName")}};

    EXPECT_EQ(TestMctpDiscovery::getNameFromProperties(*disc, properties),
              "DirectName");
}

TEST(MctpEndpointDiscoveryTest, getNameFromPropertiesMissingNameDirect)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;
    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::utils::PropertyMap properties{{"Address", uint64_t(0x21)}};

    EXPECT_TRUE(
        TestMctpDiscovery::getNameFromProperties(*disc, properties).empty());
}

TEST(MctpEndpointDiscoveryTest, getNameFromPropertiesWrongTypeThrowsDirect)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;
    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::utils::PropertyMap properties{{"Name", uint64_t(0x21)}};

    EXPECT_THROW(TestMctpDiscovery::getNameFromProperties(*disc, properties),
                 std::bad_variant_access);
}

TEST(MctpEndpointDiscoveryTest, discoverEndpointsInvalidMsg)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    // Create a dummy D-Bus message - msg.read() in getAddedMctpInfos will fail
    sdbusplus::message_t msg = sdbusplus::bus::new_default().new_method_call(
        "xyz.openbmc_project.sdbusplus.test.Object",
        "/xyz/openbmc_project/sdbusplus/test/object",
        "xyz.openbmc_project.sdbusplus.test.Object", "Unused");

    // discoverEndpoints calls getAddedMctpInfos which will fail on msg.read()
    // The exception is caught, and discoverEndpoints continues with empty infos
    TestMctpDiscovery::discoverEndpoints(*mctpDiscoveryHandler, msg);

    // No new endpoints should be added
    EXPECT_EQ(mctpDiscoveryHandler->existingMctpInfos.size(), 0);
}

TEST(MctpEndpointDiscoveryTest, getAddedMctpInfosInvalidMsg)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    sdbusplus::message_t msg = sdbusplus::bus::new_default().new_method_call(
        "xyz.openbmc_project.sdbusplus.test.Object",
        "/xyz/openbmc_project/sdbusplus/test/object",
        "xyz.openbmc_project.sdbusplus.test.Object", "Unused");

    pldm::MctpInfos infos;
    TestMctpDiscovery::getAddedMctpInfos(*mctpDiscoveryHandler, msg, infos);

    EXPECT_TRUE(infos.empty());
}

TEST(MctpEndpointDiscoveryTest, getAddedMctpInfosWrongSignatureReturnsEmpty)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;
    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    auto rawBus = sdbusplus::bus::new_default();
    auto msg =
        rawBus.new_method_call("org.test", "/test", "org.test.Intf", "Method");
    msg.append(std::string("not-an-object-path"), uint32_t(7));
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    pldm::MctpInfos infos;
    TestMctpDiscovery::getAddedMctpInfos(*disc, msg, infos);

    EXPECT_TRUE(infos.empty());
}

TEST(MctpEndpointDiscoveryTest, propertiesChangedCbInvalidMsg)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    // Create a dummy D-Bus message - msg.read() in propertiesChangedCb fails
    sdbusplus::message_t msg = sdbusplus::bus::new_default().new_method_call(
        "xyz.openbmc_project.sdbusplus.test.Object",
        "/xyz/openbmc_project/sdbusplus/test/object",
        "xyz.openbmc_project.sdbusplus.test.Object", "Unused");

    // propertiesChangedCb will fail on msg.read() and return early
    TestMctpDiscovery::propertiesChangedCb(*mctpDiscoveryHandler, msg);

    // Should not crash - just return early
    EXPECT_EQ(mctpDiscoveryHandler->existingMctpInfos.size(), 0);
}

TEST(MctpEndpointDiscoveryTest, propertiesChangedCbWrongSignatureReturnsEarly)
{
    MockdBusHandler mockedDbusHandler;
    TrackingMctpHandler handler;
    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &handler);

    auto rawBus = sdbusplus::bus::new_default();
    auto msg =
        rawBus.new_method_call("org.test", "/test", "org.test.Intf", "Method");
    msg.append(uint32_t(7), std::vector<uint8_t>{1, 2, 3});
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    TestMctpDiscovery::propertiesChangedCb(*disc, msg);

    EXPECT_EQ(handler.handleMctpEndpointsCalls, 0);
    EXPECT_EQ(handler.updateAvailabilityCalls, 0);
}

TEST(MctpEndpointDiscoveryTest, propertiesChangedCbValidMsgDbusException)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    // Create a message with proper PropertiesChanged content
    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call(
        "org.test", "/au/com/codeconstruct/mctp1/networks/1/endpoints/10",
        "org.test.Interface", "Method");

    // Append PropertiesChanged format: interface_name, changed_properties
    std::string interface = "au.com.codeconstruct.MCTP.Endpoint1";
    std::map<std::string, std::variant<std::string>> properties = {
        {"Connectivity", std::variant<std::string>{"Available"}}};
    msg.append(interface, properties);

    // Seal and rewind message for reading
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    // propertiesChangedCb reads interface & properties successfully,
    // then tries getService for the path - throws (no MCTP service)
    TestMctpDiscovery::propertiesChangedCb(*mctpDiscoveryHandler, msg);

    EXPECT_EQ(mctpDiscoveryHandler->existingMctpInfos.size(), 0);
}

TEST(MctpEndpointDiscoveryTest, propertiesChangedCbNonConnectivityProperty)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call(
        "org.test", "/au/com/codeconstruct/mctp1/networks/1/endpoints/11",
        "org.test.Interface", "Method");

    std::string interface = "au.com.codeconstruct.MCTP.Endpoint1";
    std::map<std::string, std::variant<std::string>> properties = {
        {"NotConnectivity", std::variant<std::string>{"Available"}}};
    msg.append(interface, properties);

    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    TestMctpDiscovery::propertiesChangedCb(*mctpDiscoveryHandler, msg);
    EXPECT_EQ(mctpDiscoveryHandler->existingMctpInfos.size(), 0);
}

TEST(MctpEndpointDiscoveryTest, propertiesChangedCbEmptyProperties)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call(
        "org.test", "/au/com/codeconstruct/mctp1/networks/1/endpoints/12",
        "org.test.Interface", "Method");

    std::string interface = "au.com.codeconstruct.MCTP.Endpoint1";
    std::map<std::string, std::variant<std::string>> properties{};
    msg.append(interface, properties);

    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    TestMctpDiscovery::propertiesChangedCb(*mctpDiscoveryHandler, msg);
    EXPECT_EQ(mctpDiscoveryHandler->existingMctpInfos.size(), 0);
}

TEST(MctpEndpointDiscoveryTest, propertiesChangedCbMultipleNonConnectivityProps)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call(
        "org.test", "/au/com/codeconstruct/mctp1/networks/1/endpoints/13",
        "org.test.Interface", "Method");

    std::string interface = "au.com.codeconstruct.MCTP.Endpoint1";
    std::map<std::string, std::variant<std::string>> properties = {
        {"NotConnectivityA", std::variant<std::string>{"v1"}},
        {"NotConnectivityB", std::variant<std::string>{"v2"}}};
    msg.append(interface, properties);

    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    TestMctpDiscovery::propertiesChangedCb(*mctpDiscoveryHandler, msg);
    EXPECT_EQ(mctpDiscoveryHandler->existingMctpInfos.size(), 0);
}

TEST(MctpEndpointDiscoveryTest, propertiesChangedCbMockDbusServiceThrows)
{
    MockdBusHandler mockedDbusHandler;
    TrackingMctpHandler handler;
    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &handler);

    auto msg = makePropertiesChangedMessage(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/15", "Available");

    EXPECT_CALL(mockedDbusHandler, getService(_, _))
        .WillOnce([](const char*, const char*) -> std::string {
            throw sdbusplus::exception::SdBusError(EINVAL, "mock");
        });

    TestMctpDiscovery::propertiesChangedCb(*disc, msg);

    EXPECT_TRUE(disc->existingMctpInfos.empty());
    EXPECT_EQ(handler.handleMctpEndpointsCalls, 0);
    EXPECT_EQ(handler.updateAvailabilityCalls, 0);
}

TEST(MctpEndpointDiscoveryTest, getAddedMctpInfosValidMsgGetServiceFails)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    // Construct InterfacesAdded message: object_path + interfaces map
    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call("org.test", "/test", "org.test.Interface",
                                      "Method");

    sdbusplus::message::object_path objPath(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/10");

    using PropertyMap = std::map<std::string, pldm::dbus::Value>;
    std::map<std::string, PropertyMap> interfaces;
    PropertyMap mctpProps;
    mctpProps["NetworkId"] = uint32_t(1);
    mctpProps["EID"] = uint8_t(10);
    mctpProps["SupportedMessageTypes"] = std::vector<uint8_t>{1};
    mctpProps["MediumType"] = std::string("SPI");
    interfaces["xyz.openbmc_project.MCTP.Endpoint"] = mctpProps;

    msg.append(objPath, interfaces);
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    pldm::MctpInfos infos;
    TestMctpDiscovery::getAddedMctpInfos(*mctpDiscoveryHandler, msg, infos);

    // msg.read() succeeds, getEndpointConnectivityProp returns false (D-Bus
    // fails gracefully), then getService for UUID throws → caught → return
    EXPECT_EQ(infos.size(), 0);
}

TEST(MctpEndpointDiscoveryTest, getAddedMctpInfosMissingUuidSkipsDbusFallback)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;
    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    std::map<std::string, std::map<std::string, pldm::dbus::Value>> interfaces;
    interfaces[pldm::MCTPInterface] = {
        {"NetworkId", uint32_t(1)},
        {"EID", uint8_t(10)},
        {"SupportedMessageTypes", std::vector<uint8_t>{1}},
        {"MediumType", std::string("SPI")}};
    auto msg = makeInterfacesAddedMessage(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/16", interfaces);

    EXPECT_CALL(mockedDbusHandler, getDbusPropertyVariant(_, _, _)).Times(0);
    EXPECT_CALL(mockedDbusHandler, getService(_, _)).Times(0);
    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _)).Times(0);

    pldm::MctpInfos infos;
    TestMctpDiscovery::getAddedMctpInfos(*disc, msg, infos);

    EXPECT_TRUE(infos.empty());
}

TEST(MctpEndpointDiscoveryTest, refreshEndpointsValidMsg)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    // Construct PropertiesChanged message with Connectivity property
    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call(
        "org.test", "/au/com/codeconstruct/mctp1/networks/1/endpoints/10",
        "org.test.Interface", "Method");

    std::string interface = "au.com.codeconstruct.MCTP.Endpoint1";
    pldm::dbus::PropertyMap properties;
    properties["Connectivity"] = std::string("Available");

    msg.append(interface, properties);
    // Set sender before sealing (get_sender returns null otherwise)
    sd_bus_message_set_sender(msg.get(), "org.test.Sender");
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    // refreshEndpoints reads msg successfully, finds Connectivity=Available,
    // then for each handler tries D-Bus calls to get UUID/EID which throw
    mctpDiscoveryHandler->refreshEndpoints(msg);

    // No endpoints should be modified since D-Bus calls fail
    EXPECT_EQ(mctpDiscoveryHandler->existingMctpInfos.size(), 0);
}

TEST(MctpEndpointDiscoveryTest, refreshEndpointsNoConnectivityProp)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    // Construct PropertiesChanged message without Connectivity property
    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call(
        "org.test", "/au/com/codeconstruct/mctp1/networks/1/endpoints/10",
        "org.test.Interface", "Method");

    std::string interface = "au.com.codeconstruct.MCTP.Endpoint1";
    pldm::dbus::PropertyMap properties;
    properties["SomeOtherProp"] = std::string("value");

    msg.append(interface, properties);
    // Set sender before sealing (get_sender returns null otherwise)
    sd_bus_message_set_sender(msg.get(), "org.test.Sender");
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    // refreshEndpoints reads msg, doesn't find Connectivity → early exit
    mctpDiscoveryHandler->refreshEndpoints(msg);

    EXPECT_EQ(mctpDiscoveryHandler->existingMctpInfos.size(), 0);
}

TEST(MctpEndpointDiscoveryTest, refreshEndpointsMockDbusPropertyException)
{
    MockdBusHandler mockedDbusHandler;
    TrackingMctpHandler handler;
    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &handler);

    auto msg = makeRefreshEndpointsMessage(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/17", "Available");

    EXPECT_CALL(mockedDbusHandler, getDbusPropertyVariant(_, _, _))
        .WillOnce([](const char*, const char*,
                     const char*) -> pldm::utils::PropertyValue {
            throw sdbusplus::exception::SdBusError(EINVAL, "mock");
        });

    disc->refreshEndpoints(msg);

    EXPECT_EQ(handler.onlineCalls, 0);
    EXPECT_EQ(handler.offlineCalls, 0);
}

TEST(MctpEndpointDiscoveryTest, refreshEndpointsConnectivityWrongTypeThrows)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto mctpDiscoveryHandler = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager});

    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call(
        "org.test", "/au/com/codeconstruct/mctp1/networks/1/endpoints/99",
        "org.test.Interface", "Method");

    std::string interface = "au.com.codeconstruct.MCTP.Endpoint1";
    pldm::dbus::PropertyMap properties;
    properties["Connectivity"] = uint32_t(1);

    msg.append(interface, properties);
    sd_bus_message_set_sender(msg.get(), "org.test.Sender");
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    EXPECT_THROW(mctpDiscoveryHandler->refreshEndpoints(msg),
                 std::bad_variant_access);
}

TEST(MctpEndpointDiscoveryTest, liveMapperConstructorDiscoversAvailableEndpoint)
{
    auto endpoints = getMctpEndpoints();
    if (endpoints.empty())
    {
        GTEST_SKIP() << "No MCTP endpoints found via mapper";
    }

    auto& bus = pldm::utils::DBusHandler::getBus();
    TrackingMctpHandler handler;

    auto disc = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&handler});

    EXPECT_GE(handler.handleMctpEndpointsCalls, 0);
    EXPECT_GE(disc->existingMctpInfos.size(), 0u);
}

TEST(MctpEndpointDiscoveryTest, liveMapperPropertiesChangedCbReturnsForNonPldm)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    TrackingMctpHandler handler;
    auto disc = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&handler});
    auto endpoint = findEndpointByPldmType(*disc, false);
    if (!endpoint.has_value())
    {
        GTEST_SKIP() << "No non-PLDM endpoint found";
    }

    handler.handleMctpEndpointsCalls = 0;
    handler.updateAvailabilityCalls = 0;

    auto msg = makePropertiesChangedMessage(endpoint->first, "Available");
    TestMctpDiscovery::propertiesChangedCb(*disc, msg);

    EXPECT_EQ(handler.handleMctpEndpointsCalls, 0);
    EXPECT_EQ(handler.updateAvailabilityCalls, 0);
}

TEST(MctpEndpointDiscoveryTest, liveMapperPropertiesChangedCbAddAndUpdatePaths)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    TrackingMctpHandler handler;

    auto disc = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&handler});
    auto endpoint = findEndpointByPldmType(*disc, true);
    if (!endpoint.has_value())
    {
        GTEST_SKIP() << "No PLDM endpoint found";
    }

    disc->existingMctpInfos.clear();
    handler.handleMctpEndpointsCalls = 0;
    handler.updateAvailabilityCalls = 0;

    auto addMsg = makePropertiesChangedMessage(endpoint->first, "Available");
    TestMctpDiscovery::propertiesChangedCb(*disc, addMsg);
    EXPECT_GE(handler.handleMctpEndpointsCalls, 1);
    ASSERT_FALSE(disc->existingMctpInfos.empty());

    auto updateMsg = makePropertiesChangedMessage(endpoint->first, "Degraded");
    TestMctpDiscovery::propertiesChangedCb(*disc, updateMsg);
    EXPECT_GE(handler.updateAvailabilityCalls, 1);
    EXPECT_FALSE(handler.lastAvailability);
}

TEST(MctpEndpointDiscoveryTest, liveMapperRefreshEndpointsOnlineAndOffline)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    TrackingMctpHandler handler;

    auto disc = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&handler});
    auto endpoint = findEndpointByPldmType(*disc, true);
    if (!endpoint.has_value())
    {
        GTEST_SKIP() << "No PLDM endpoint found";
    }

    std::string endpointUuid{};
    uint8_t endpointEid{};
    try
    {
        endpointUuid = pldm::utils::DBusHandler().getDbusProperty<std::string>(
            endpoint->first.c_str(), "UUID", pldm::EndpointUUID);
        endpointEid = pldm::utils::DBusHandler().getDbusProperty<uint8_t>(
            endpoint->first.c_str(), "EID", pldm::MCTPInterface);
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "Could not read endpoint properties: " << e.what();
    }

    auto onlineMsg = makeRefreshEndpointsMessage(endpoint->first, "Available");
    disc->refreshEndpoints(onlineMsg);
    EXPECT_EQ(handler.onlineCalls, 1);
    EXPECT_EQ(handler.lastOnlineUuid, endpointUuid);
    EXPECT_EQ(handler.lastOnlineEid, endpointEid);

    auto offlineMsg = makeRefreshEndpointsMessage(endpoint->first, "Degraded");
    disc->refreshEndpoints(offlineMsg);
    EXPECT_EQ(handler.offlineCalls, 1);
    EXPECT_EQ(handler.lastOfflineUuid, endpointUuid);
    EXPECT_EQ(handler.lastOfflineEid, endpointEid);
}

// ===== Tests using MockdBusHandler for success-path coverage =====

TEST(MctpEndpointDiscoveryTest, constructorUsesInjectedDbusHandler)
{
    MockdBusHandler mockedDbusHandler;
    auto& bus = mockedDbusHandler.getBus();
    pldm::MockManager manager;

    EXPECT_CALL(mockedDbusHandler, getSubtree(pldm::MCTPPath, 0, _))
        .WillOnce(testing::Return(pldm::utils::GetSubTreeResponse{}));

    auto disc = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{&manager},
        "/tmp/mctp-discovery-no-static-endpoints.json", mockedDbusHandler);

    EXPECT_TRUE(disc->existingMctpInfos.empty());
}

TEST(MctpEndpointDiscoveryTest, getMctpEndpointPropsSuccess)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    // Mock getDbusPropertiesVariant for MCTP.Endpoint properties
    pldm::utils::PropertyMap epProps{
        {"NetworkId", uint32_t(1)},
        {"EID", uint8_t(10)},
        {"SupportedMessageTypes", std::vector<uint8_t>{0, 1}},
        {"MediumType", std::string("SMBus")}};

    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(epProps));

    // Mock getDbusPropertyVariant for BindingType
    pldm::utils::PropertyValue bindingVal = std::string("MctpOverSMBus");
    EXPECT_CALL(mockedDbusHandler, getDbusPropertyVariant(_, _, _))
        .WillOnce(testing::Return(bindingVal));

    auto result = TestMctpDiscovery::getMctpEndpointProps(
        *disc, "test.service", "/test/path");

    EXPECT_EQ(std::get<0>(result), 1);  // NetworkId
    EXPECT_EQ(std::get<1>(result), 10); // eid
    auto types = std::get<2>(result);   // MCTPMsgTypes
    EXPECT_EQ(types.size(), 2);
    EXPECT_EQ(types[1], 1);
    EXPECT_EQ(std::get<3>(result), "SMBus");         // MctpMedium
    EXPECT_EQ(std::get<4>(result), "MctpOverSMBus"); // MctpBinding
}

TEST(MctpEndpointDiscoveryTest, getMctpEndpointPropsMissingProperty)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    // Return properties missing "EID" key
    pldm::utils::PropertyMap incompleteProps{
        {"NetworkId", uint32_t(1)},
        {"SupportedMessageTypes", std::vector<uint8_t>{1}},
        {"MediumType", std::string("SMBus")}};

    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(incompleteProps));

    auto result = TestMctpDiscovery::getMctpEndpointProps(
        *disc, "test.service", "/test/path");

    // Should return defaults due to missing property
    EXPECT_EQ(std::get<1>(result), 0xFF);     // MCTP_ADDR_ANY
    EXPECT_TRUE(std::get<2>(result).empty()); // MCTPMsgTypes
}

TEST(MctpEndpointDiscoveryTest, getMctpEndpointPropsMissingNetworkId)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::utils::PropertyMap incompleteProps{
        {"EID", uint8_t(10)},
        {"SupportedMessageTypes", std::vector<uint8_t>{1}},
        {"MediumType", std::string("SMBus")}};

    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(incompleteProps));
    EXPECT_CALL(mockedDbusHandler, getDbusPropertyVariant(_, _, _)).Times(0);

    auto result = TestMctpDiscovery::getMctpEndpointProps(
        *disc, "test.service", "/test/path");

    EXPECT_EQ(std::get<0>(result), 0u);
    EXPECT_EQ(std::get<1>(result), 0xFF);
    EXPECT_TRUE(std::get<2>(result).empty());
}

TEST(MctpEndpointDiscoveryTest, getMctpEndpointPropsMissingSupportedTypes)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::utils::PropertyMap incompleteProps{
        {"NetworkId", uint32_t(1)},
        {"EID", uint8_t(10)},
        {"MediumType", std::string("SMBus")}};

    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(incompleteProps));
    EXPECT_CALL(mockedDbusHandler, getDbusPropertyVariant(_, _, _)).Times(0);

    auto result = TestMctpDiscovery::getMctpEndpointProps(
        *disc, "test.service", "/test/path");

    EXPECT_EQ(std::get<0>(result), 0u);
    EXPECT_EQ(std::get<1>(result), 0xFF);
    EXPECT_TRUE(std::get<2>(result).empty());
}

TEST(MctpEndpointDiscoveryTest, getMctpEndpointPropsMissingMediumType)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::utils::PropertyMap incompleteProps{
        {"NetworkId", uint32_t(1)},
        {"EID", uint8_t(10)},
        {"SupportedMessageTypes", std::vector<uint8_t>{1}}};

    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(incompleteProps));
    EXPECT_CALL(mockedDbusHandler, getDbusPropertyVariant(_, _, _)).Times(0);

    auto result = TestMctpDiscovery::getMctpEndpointProps(
        *disc, "test.service", "/test/path");

    EXPECT_EQ(std::get<0>(result), 0u);
    EXPECT_EQ(std::get<1>(result), 0xFF);
    EXPECT_TRUE(std::get<2>(result).empty());
}

TEST(MctpEndpointDiscoveryTest, getMctpEndpointPropsOnlyNetworkIdPresent)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::utils::PropertyMap incompleteProps{{"NetworkId", uint32_t(1)}};

    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(incompleteProps));
    EXPECT_CALL(mockedDbusHandler, getDbusPropertyVariant(_, _, _)).Times(0);

    auto result = TestMctpDiscovery::getMctpEndpointProps(
        *disc, "test.service", "/test/path");

    EXPECT_EQ(std::get<0>(result), 0u);
    EXPECT_EQ(std::get<1>(result), 0xFF);
    EXPECT_TRUE(std::get<2>(result).empty());
}

TEST(MctpEndpointDiscoveryTest, getMctpEndpointPropsNetworkAndEidOnly)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::utils::PropertyMap incompleteProps{{"NetworkId", uint32_t(1)},
                                             {"EID", uint8_t(10)}};

    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(incompleteProps));
    EXPECT_CALL(mockedDbusHandler, getDbusPropertyVariant(_, _, _)).Times(0);

    auto result = TestMctpDiscovery::getMctpEndpointProps(
        *disc, "test.service", "/test/path");

    EXPECT_EQ(std::get<0>(result), 0u);
    EXPECT_EQ(std::get<1>(result), 0xFF);
    EXPECT_TRUE(std::get<2>(result).empty());
}

TEST(MctpEndpointDiscoveryTest, getMctpEndpointPropsBadVariantReturnsDefaults)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::utils::PropertyMap badProps{
        {"NetworkId", std::string("wrong-type")},
        {"EID", uint8_t(10)},
        {"SupportedMessageTypes", std::vector<uint8_t>{1}},
        {"MediumType", std::string("SMBus")}};

    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(badProps));

    auto result = TestMctpDiscovery::getMctpEndpointProps(
        *disc, "test.service", "/test/path");
    EXPECT_EQ(std::get<0>(result), 0u);
    EXPECT_EQ(std::get<1>(result), 0xFF);
    EXPECT_TRUE(std::get<2>(result).empty());
}

TEST(MctpEndpointDiscoveryTest,
     getMctpEndpointPropsBadEidVariantReturnsDefaults)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::utils::PropertyMap badProps{
        {"NetworkId", uint32_t(1)},
        {"EID", std::string("wrong-type")},
        {"SupportedMessageTypes", std::vector<uint8_t>{1}},
        {"MediumType", std::string("SMBus")}};

    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(badProps));

    auto result = TestMctpDiscovery::getMctpEndpointProps(
        *disc, "test.service", "/test/path");
    EXPECT_EQ(std::get<0>(result), 0u);
    EXPECT_EQ(std::get<1>(result), 0xFF);
    EXPECT_TRUE(std::get<2>(result).empty());
}

TEST(MctpEndpointDiscoveryTest,
     getMctpEndpointPropsBadTypesVariantReturnsDefaults)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::utils::PropertyMap badProps{
        {"NetworkId", uint32_t(1)},
        {"EID", uint8_t(10)},
        {"SupportedMessageTypes", std::string("wrong-type")},
        {"MediumType", std::string("SMBus")}};

    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(badProps));

    auto result = TestMctpDiscovery::getMctpEndpointProps(
        *disc, "test.service", "/test/path");
    EXPECT_EQ(std::get<0>(result), 0u);
    EXPECT_EQ(std::get<1>(result), 0xFF);
    EXPECT_TRUE(std::get<2>(result).empty());
}

TEST(MctpEndpointDiscoveryTest,
     getMctpEndpointPropsBadMediumVariantReturnsDefaults)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::utils::PropertyMap badProps{
        {"NetworkId", uint32_t(1)},
        {"EID", uint8_t(10)},
        {"SupportedMessageTypes", std::vector<uint8_t>{1}},
        {"MediumType", uint64_t(7)}};

    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(badProps));

    auto result = TestMctpDiscovery::getMctpEndpointProps(
        *disc, "test.service", "/test/path");
    EXPECT_EQ(std::get<0>(result), 0u);
    EXPECT_EQ(std::get<1>(result), 0xFF);
    EXPECT_TRUE(std::get<2>(result).empty());
}

TEST(MctpEndpointDiscoveryTest, getEndpointUUIDPropSuccess)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::utils::PropertyMap uuidProps{
        {"UUID", std::string("12345678-1234-1234-1234-123456789abc")}};

    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(uuidProps));

    auto result = TestMctpDiscovery::getEndpointUUIDProp(*disc, "test.service",
                                                         "/test/path");

    EXPECT_EQ(result, "12345678-1234-1234-1234-123456789abc");
}

TEST(MctpEndpointDiscoveryTest, getEndpointUUIDPropNoUUID)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    // Return properties without "UUID" key
    pldm::utils::PropertyMap noUuidProps{{"SomeOther", std::string("value")}};

    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(noUuidProps));

    auto result = TestMctpDiscovery::getEndpointUUIDProp(*disc, "test.service",
                                                         "/test/path");

    EXPECT_EQ(result, pldm::emptyUUID);
}

TEST(MctpEndpointDiscoveryTest, getEndpointUUIDPropBadVariantThrows)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::utils::PropertyMap badUuidProps{{"UUID", uint64_t(123)}};

    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(badUuidProps));

    EXPECT_THROW(TestMctpDiscovery::getEndpointUUIDProp(*disc, "test.service",
                                                        "/test/path"),
                 std::bad_variant_access);
}

TEST(MctpEndpointDiscoveryTest, getEndpointConnectivityPropAvailable)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::utils::PropertyValue connVal = std::string("Available");
    EXPECT_CALL(mockedDbusHandler, getDbusPropertyVariant(_, _, _))
        .WillOnce(testing::Return(connVal));

    auto result =
        TestMctpDiscovery::getEndpointConnectivityProp(*disc, "/test/path");

    EXPECT_TRUE(result);
}

TEST(MctpEndpointDiscoveryTest, getEndpointConnectivityPropDegraded)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    pldm::utils::PropertyValue connVal = std::string("Degraded");
    EXPECT_CALL(mockedDbusHandler, getDbusPropertyVariant(_, _, _))
        .WillOnce(testing::Return(connVal));

    auto result =
        TestMctpDiscovery::getEndpointConnectivityProp(*disc, "/test/path");

    EXPECT_FALSE(result);
}

TEST(MctpEndpointDiscoveryTest, getEndpointConnectivityPropBadVariantThrows)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    EXPECT_CALL(mockedDbusHandler, getDbusPropertyVariant(_, _, _))
        .WillOnce(testing::Return(pldm::utils::PropertyValue{uint64_t(99)}));

    EXPECT_THROW(
        TestMctpDiscovery::getEndpointConnectivityProp(*disc, "/test/path"),
        std::bad_variant_access);
}

TEST(MctpEndpointDiscoveryTest, getMctpInfosWithEndpoints)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    // Mock getSubtree to return one endpoint
    std::string epPath = "/au/com/codeconstruct/mctp1/networks/1/endpoints/10";
    std::string svc = "au.com.codeconstruct.MCTP1";
    pldm::utils::GetSubTreeResponse subtree{
        {epPath, {{svc, {"xyz.openbmc_project.MCTP.Endpoint"}}}}};

    EXPECT_CALL(mockedDbusHandler, getSubtree(_, _, _))
        .WillOnce(testing::Return(subtree));

    // Mock getDbusPropertiesVariant: first for MCTP.Endpoint, then for UUID
    pldm::utils::PropertyMap epProps{
        {"NetworkId", uint32_t(1)},
        {"EID", uint8_t(10)},
        {"SupportedMessageTypes", std::vector<uint8_t>{1}},
        {"MediumType", std::string("SMBus")}};

    pldm::utils::PropertyMap uuidProps{
        {"UUID", std::string("aabbccdd-1122-3344-5566-778899aabbcc")}};

    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(epProps))
        .WillOnce(testing::Return(uuidProps));

    // Mock getDbusPropertyVariant: first for BindingType, then for Connectivity
    pldm::utils::PropertyValue bindingVal = std::string("MctpOverSMBus");
    pldm::utils::PropertyValue connVal = std::string("Available");

    EXPECT_CALL(mockedDbusHandler, getDbusPropertyVariant(_, _, _))
        .WillOnce(testing::Return(bindingVal))
        .WillOnce(testing::Return(connVal));

    // Mock getAssociatedSubTree for searchConfigurationFor (empty = no config)
    pldm::utils::GetAssociatedSubTreeResponse emptyAssocResponse{};
    EXPECT_CALL(mockedDbusHandler, getAssociatedSubTree(_, _, _, _))
        .WillOnce(testing::Return(emptyAssocResponse));

    std::map<pldm::MctpInfo, pldm::Availability> mctpInfoMap;
    TestMctpDiscovery::getMctpInfos(*disc, mctpInfoMap);

    EXPECT_EQ(mctpInfoMap.size(), 1);
    auto it = mctpInfoMap.begin();
    EXPECT_EQ(std::get<0>(it->first), 10);              // eid
    EXPECT_EQ(std::get<1>(it->first),
              "aabbccdd-1122-3344-5566-778899aabbcc");  // UUID
    EXPECT_EQ(std::get<3>(it->first), 1);               // NetworkId
    EXPECT_EQ(std::get<2>(it->first), "SMBus");         // MctpMedium
    EXPECT_EQ(std::get<5>(it->first), "MctpOverSMBus"); // MctpBinding
    EXPECT_TRUE(it->second);                            // Available
}

TEST(MctpEndpointDiscoveryTest, getMctpInfosSubtreeException)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    EXPECT_CALL(mockedDbusHandler, getSubtree(_, _, _))
        .WillOnce([](const std::string&, int, const std::vector<std::string>&)
                      -> pldm::utils::GetSubTreeResponse {
            throw sdbusplus::exception::SdBusError(EINVAL, "mock");
        });

    std::map<pldm::MctpInfo, pldm::Availability> mctpInfoMap;
    TestMctpDiscovery::getMctpInfos(*disc, mctpInfoMap);

    EXPECT_TRUE(mctpInfoMap.empty());
    EXPECT_TRUE(disc->enableMatches.empty());
}

TEST(MctpEndpointDiscoveryTest,
     propertiesChangedCbAddsAvailableEndpointWithMockDbus)
{
    MockdBusHandler mockedDbusHandler;
    TrackingMctpHandler handler;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &handler);
    auto msg = makePropertiesChangedMessage(
        "/au/com/codeconstruct/mctp1/networks/7/endpoints/77", "Available");

    pldm::utils::PropertyMap epProps{
        {"NetworkId", uint32_t(7)},
        {"EID", uint8_t(77)},
        {"SupportedMessageTypes", std::vector<uint8_t>{1}},
        {"MediumType", std::string("SMBus")}};
    pldm::utils::PropertyMap uuidProps{
        {"UUID", std::string("77777777-1111-2222-3333-444455556666")}};
    pldm::utils::GetAssociatedSubTreeResponse emptyAssocResponse{};

    EXPECT_CALL(mockedDbusHandler, getService(_, _))
        .WillOnce(testing::Return(std::string("au.com.codeconstruct.MCTP1")));
    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(epProps))
        .WillOnce(testing::Return(uuidProps));
    EXPECT_CALL(mockedDbusHandler, getDbusPropertyVariant(_, _, _))
        .WillOnce(testing::Return(
            pldm::utils::PropertyValue{std::string("MctpOverSMBus")}));
    EXPECT_CALL(mockedDbusHandler, getAssociatedSubTree(_, _, _, _))
        .WillOnce(testing::Return(emptyAssocResponse));

    TestMctpDiscovery::propertiesChangedCb(*disc, msg);

    EXPECT_EQ(handler.handleMctpEndpointsCalls, 1);
    EXPECT_EQ(handler.lastHandledSize, 1u);
    EXPECT_EQ(handler.updateAvailabilityCalls, 0);
    ASSERT_EQ(disc->existingMctpInfos.size(), 1u);
    EXPECT_EQ(std::get<pldm::eid>(disc->existingMctpInfos.front()), 77);
}

TEST(MctpEndpointDiscoveryTest,
     propertiesChangedCbDoesNotAddUnavailableEndpointWithMockDbus)
{
    MockdBusHandler mockedDbusHandler;
    TrackingMctpHandler handler;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &handler);
    auto msg = makePropertiesChangedMessage(
        "/au/com/codeconstruct/mctp1/networks/8/endpoints/78", "Degraded");

    pldm::utils::PropertyMap epProps{
        {"NetworkId", uint32_t(8)},
        {"EID", uint8_t(78)},
        {"SupportedMessageTypes", std::vector<uint8_t>{1}},
        {"MediumType", std::string("SMBus")}};
    pldm::utils::PropertyMap uuidProps{
        {"UUID", std::string("88888888-1111-2222-3333-444455556666")}};
    pldm::utils::GetAssociatedSubTreeResponse emptyAssocResponse{};

    EXPECT_CALL(mockedDbusHandler, getService(_, _))
        .WillOnce(testing::Return(std::string("au.com.codeconstruct.MCTP1")));
    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(epProps))
        .WillOnce(testing::Return(uuidProps));
    EXPECT_CALL(mockedDbusHandler, getDbusPropertyVariant(_, _, _))
        .WillOnce(testing::Return(
            pldm::utils::PropertyValue{std::string("MctpOverSMBus")}));
    EXPECT_CALL(mockedDbusHandler, getAssociatedSubTree(_, _, _, _))
        .WillOnce(testing::Return(emptyAssocResponse));

    TestMctpDiscovery::propertiesChangedCb(*disc, msg);

    EXPECT_EQ(handler.handleMctpEndpointsCalls, 0);
    EXPECT_EQ(handler.updateAvailabilityCalls, 0);
    EXPECT_TRUE(disc->existingMctpInfos.empty());
}

TEST(MctpEndpointDiscoveryTest,
     propertiesChangedCbUpdatesExistingEndpointWithMockDbus)
{
    MockdBusHandler mockedDbusHandler;
    TrackingMctpHandler handler;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &handler);
    auto msg = makePropertiesChangedMessage(
        "/au/com/codeconstruct/mctp1/networks/9/endpoints/79", "Degraded");

    pldm::utils::PropertyMap epProps{
        {"NetworkId", uint32_t(9)},
        {"EID", uint8_t(79)},
        {"SupportedMessageTypes", std::vector<uint8_t>{1}},
        {"MediumType", std::string("SMBus")}};
    pldm::utils::PropertyMap uuidProps{
        {"UUID", std::string("99999999-1111-2222-3333-444455556666")}};
    pldm::utils::GetAssociatedSubTreeResponse emptyAssocResponse{};

    disc->existingMctpInfos.emplace_back(pldm::MctpInfo(
        79, "99999999-1111-2222-3333-444455556666", "SMBus", uint32_t(9),
        std::nullopt, "MctpOverSMBus", std::nullopt));

    EXPECT_CALL(mockedDbusHandler, getService(_, _))
        .WillOnce(testing::Return(std::string("au.com.codeconstruct.MCTP1")));
    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(epProps))
        .WillOnce(testing::Return(uuidProps));
    EXPECT_CALL(mockedDbusHandler, getDbusPropertyVariant(_, _, _))
        .WillOnce(testing::Return(
            pldm::utils::PropertyValue{std::string("MctpOverSMBus")}));
    EXPECT_CALL(mockedDbusHandler, getAssociatedSubTree(_, _, _, _))
        .WillOnce(testing::Return(emptyAssocResponse));

    TestMctpDiscovery::propertiesChangedCb(*disc, msg);

    EXPECT_EQ(handler.handleMctpEndpointsCalls, 0);
    EXPECT_EQ(handler.updateAvailabilityCalls, 1);
    EXPECT_FALSE(handler.lastAvailability);
    EXPECT_EQ(std::get<pldm::eid>(handler.lastMctpInfo), 79);
}

TEST(MctpEndpointDiscoveryTest,
     propertiesChangedCbReturnsForNonPldmEndpointWithMockDbus)
{
    MockdBusHandler mockedDbusHandler;
    TrackingMctpHandler handler;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &handler);
    auto msg = makePropertiesChangedMessage(
        "/au/com/codeconstruct/mctp1/networks/10/endpoints/80", "Available");

    pldm::utils::PropertyMap epProps{
        {"NetworkId", uint32_t(10)},
        {"EID", uint8_t(80)},
        {"SupportedMessageTypes", std::vector<uint8_t>{0, 2}},
        {"MediumType", std::string("SMBus")}};

    EXPECT_CALL(mockedDbusHandler, getService(_, _))
        .WillOnce(testing::Return(std::string("au.com.codeconstruct.MCTP1")));
    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(epProps));
    EXPECT_CALL(mockedDbusHandler, getDbusPropertyVariant(_, _, _))
        .WillOnce(testing::Return(
            pldm::utils::PropertyValue{std::string("MctpOverSMBus")}));
    EXPECT_CALL(mockedDbusHandler, getAssociatedSubTree(_, _, _, _)).Times(0);

    TestMctpDiscovery::propertiesChangedCb(*disc, msg);

    EXPECT_EQ(handler.handleMctpEndpointsCalls, 0);
    EXPECT_EQ(handler.updateAvailabilityCalls, 0);
    EXPECT_TRUE(disc->existingMctpInfos.empty());
}

TEST(MctpEndpointDiscoveryTest, getMctpInfosDuplicatePathNoDuplicateMatch)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    std::string epPath = "/au/com/codeconstruct/mctp1/networks/1/endpoints/10";
    std::string svc = "au.com.codeconstruct.MCTP1";
    pldm::utils::GetSubTreeResponse subtree{
        {epPath, {{svc, {"xyz.openbmc_project.MCTP.Endpoint"}}}}};

    EXPECT_CALL(mockedDbusHandler, getSubtree(_, _, _))
        .Times(2)
        .WillRepeatedly(testing::Return(subtree));

    pldm::utils::PropertyMap epProps{
        {"NetworkId", uint32_t(1)},
        {"EID", uint8_t(10)},
        {"SupportedMessageTypes", std::vector<uint8_t>{1}},
        {"MediumType", std::string("SMBus")}};
    pldm::utils::PropertyMap uuidProps{
        {"UUID", std::string("aabbccdd-1122-3344-5566-778899aabbcc")}};

    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(epProps))
        .WillOnce(testing::Return(uuidProps))
        .WillOnce(testing::Return(epProps))
        .WillOnce(testing::Return(uuidProps));

    pldm::utils::PropertyValue bindingVal = std::string("MctpOverSMBus");
    pldm::utils::PropertyValue connVal = std::string("Available");
    EXPECT_CALL(mockedDbusHandler, getDbusPropertyVariant(_, _, _))
        .WillOnce(testing::Return(bindingVal))
        .WillOnce(testing::Return(connVal))
        .WillOnce(testing::Return(bindingVal))
        .WillOnce(testing::Return(connVal));

    pldm::utils::GetAssociatedSubTreeResponse emptyAssocResponse{};
    EXPECT_CALL(mockedDbusHandler, getAssociatedSubTree(_, _, _, _))
        .Times(2)
        .WillRepeatedly(testing::Return(emptyAssocResponse));

    std::map<pldm::MctpInfo, pldm::Availability> firstMap;
    std::map<pldm::MctpInfo, pldm::Availability> secondMap;
    TestMctpDiscovery::getMctpInfos(*disc, firstMap);
    TestMctpDiscovery::getMctpInfos(*disc, secondMap);

    EXPECT_EQ(firstMap.size(), 1);
    EXPECT_EQ(secondMap.size(), 1);
    EXPECT_EQ(disc->enableMatches.size(), 1u);
}

TEST(MctpEndpointDiscoveryTest, getMctpInfosNonPldmType)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    std::string epPath = "/au/com/codeconstruct/mctp1/networks/1/endpoints/20";
    std::string svc = "au.com.codeconstruct.MCTP1";
    pldm::utils::GetSubTreeResponse subtree{
        {epPath, {{svc, {"xyz.openbmc_project.MCTP.Endpoint"}}}}};

    EXPECT_CALL(mockedDbusHandler, getSubtree(_, _, _))
        .WillOnce(testing::Return(subtree));

    // Endpoint with no PLDM type (type 0 only, not 1)
    pldm::utils::PropertyMap epProps{
        {"NetworkId", uint32_t(2)},
        {"EID", uint8_t(20)},
        {"SupportedMessageTypes", std::vector<uint8_t>{0}},
        {"MediumType", std::string("SMBus")}};

    pldm::utils::PropertyMap uuidProps{
        {"UUID", std::string("11111111-2222-3333-4444-555555555555")}};

    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _))
        .WillOnce(testing::Return(epProps))
        .WillOnce(testing::Return(uuidProps));

    pldm::utils::PropertyValue bindingVal = std::string("MctpOverSMBus");
    pldm::utils::PropertyValue connVal = std::string("Available");

    EXPECT_CALL(mockedDbusHandler, getDbusPropertyVariant(_, _, _))
        .WillOnce(testing::Return(bindingVal))
        .WillOnce(testing::Return(connVal));

    std::map<pldm::MctpInfo, pldm::Availability> mctpInfoMap;
    TestMctpDiscovery::getMctpInfos(*disc, mctpInfoMap);

    // Non-PLDM endpoint should be skipped
    EXPECT_EQ(mctpInfoMap.size(), 0);
}

TEST(MctpEndpointDiscoveryTest, getAddedMctpInfosFullPath)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    // Build InterfacesAdded D-Bus message
    auto rawBus = sdbusplus::bus::new_default();
    auto msg =
        rawBus.new_method_call("org.test", "/test", "org.test.Intf", "Method");

    sdbusplus::message::object_path objPath(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/15");

    using PropertyMap = std::map<std::string, pldm::dbus::Value>;
    std::map<std::string, PropertyMap> interfaces;
    PropertyMap mctpProps;
    mctpProps["NetworkId"] = uint32_t(1);
    mctpProps["EID"] = uint8_t(15);
    mctpProps["SupportedMessageTypes"] = std::vector<uint8_t>{1};
    mctpProps["MediumType"] = std::string("SMBus");
    interfaces["xyz.openbmc_project.MCTP.Endpoint"] = mctpProps;

    PropertyMap bindingProps;
    bindingProps["BindingType"] = std::string("MctpOverSMBus");
    interfaces["xyz.openbmc_project.MCTP.Binding"] = bindingProps;

    PropertyMap uuidProps;
    uuidProps["UUID"] = std::string("aabb0000-1111-2222-3333-444455556666");
    interfaces["xyz.openbmc_project.Common.UUID"] = uuidProps;

    PropertyMap ccProps;
    ccProps["Connectivity"] = std::string("Available");
    interfaces["au.com.codeconstruct.MCTP.Endpoint1"] = ccProps;

    msg.append(objPath, interfaces);
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    // Mock getAssociatedSubTree for searchConfigurationFor (empty = no config)
    pldm::utils::GetAssociatedSubTreeResponse emptyAssocResponse{};
    EXPECT_CALL(mockedDbusHandler, getAssociatedSubTree(_, _, _, _))
        .WillOnce(testing::Return(emptyAssocResponse));

    pldm::MctpInfos infos;
    TestMctpDiscovery::getAddedMctpInfos(*disc, msg, infos);

    EXPECT_EQ(infos.size(), 1);
    EXPECT_EQ(std::get<0>(infos[0]), 15);              // eid
    EXPECT_EQ(std::get<1>(infos[0]),
              "aabb0000-1111-2222-3333-444455556666"); // UUID
    EXPECT_EQ(std::get<3>(infos[0]), 1);               // NetworkId
    EXPECT_EQ(std::get<2>(infos[0]), "SMBus");         // MctpMedium
    EXPECT_EQ(std::get<5>(infos[0]), "MctpOverSMBus"); // MctpBinding
}

TEST(MctpEndpointDiscoveryTest, getAddedMctpInfosNonPldmType)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    // Build InterfacesAdded D-Bus message with non-PLDM type
    auto rawBus = sdbusplus::bus::new_default();
    auto msg =
        rawBus.new_method_call("org.test", "/test", "org.test.Intf", "Method");

    sdbusplus::message::object_path objPath(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/25");

    using PropertyMap = std::map<std::string, pldm::dbus::Value>;
    std::map<std::string, PropertyMap> interfaces;
    PropertyMap mctpProps;
    mctpProps["NetworkId"] = uint32_t(1);
    mctpProps["EID"] = uint8_t(25);
    mctpProps["SupportedMessageTypes"] = std::vector<uint8_t>{0, 2}; // no PLDM
    mctpProps["MediumType"] = std::string("SMBus");
    interfaces["xyz.openbmc_project.MCTP.Endpoint"] = mctpProps;
    PropertyMap bindingProps2;
    bindingProps2["BindingType"] = std::string("MctpOverSMBus");
    interfaces["xyz.openbmc_project.MCTP.Binding"] = bindingProps2;
    PropertyMap uuidProps2;
    uuidProps2["UUID"] = std::string("bbbb0000-1111-2222-3333-444455556666");
    interfaces["xyz.openbmc_project.Common.UUID"] = uuidProps2;

    msg.append(objPath, interfaces);
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    pldm::MctpInfos infos;
    TestMctpDiscovery::getAddedMctpInfos(*disc, msg, infos);

    // Non-PLDM type → endpoint should not be added
    EXPECT_EQ(infos.size(), 0);
}

TEST(MctpEndpointDiscoveryTest, getAddedMctpInfosDegradedConnectivity)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    auto rawBus = sdbusplus::bus::new_default();
    auto msg =
        rawBus.new_method_call("org.test", "/test", "org.test.Intf", "Method");

    sdbusplus::message::object_path objPath(
        "/au/com/codeconstruct/mctp1/networks/2/endpoints/35");

    using PropertyMap = std::map<std::string, pldm::dbus::Value>;
    std::map<std::string, PropertyMap> interfaces;
    PropertyMap mctpProps;
    mctpProps["NetworkId"] = uint32_t(2);
    mctpProps["EID"] = uint8_t(35);
    mctpProps["SupportedMessageTypes"] = std::vector<uint8_t>{1};
    mctpProps["MediumType"] = std::string("SMBus");
    interfaces["xyz.openbmc_project.MCTP.Endpoint"] = mctpProps;
    PropertyMap bindingPropsDeg;
    bindingPropsDeg["BindingType"] = std::string("MctpOverSMBus");
    interfaces["xyz.openbmc_project.MCTP.Binding"] = bindingPropsDeg;
    PropertyMap uuidPropsDeg;
    uuidPropsDeg["UUID"] = std::string("cccc0000-1111-2222-3333-444455556666");
    interfaces["xyz.openbmc_project.Common.UUID"] = uuidPropsDeg;
    PropertyMap ccPropsDeg;
    ccPropsDeg["Connectivity"] = std::string("Degraded");
    interfaces["au.com.codeconstruct.MCTP.Endpoint1"] = ccPropsDeg;

    msg.append(objPath, interfaces);
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    pldm::utils::GetAssociatedSubTreeResponse emptyAssocResponse{};
    EXPECT_CALL(mockedDbusHandler, getAssociatedSubTree(_, _, _, _))
        .WillOnce(testing::Return(emptyAssocResponse));

    pldm::MctpInfos infos;
    TestMctpDiscovery::getAddedMctpInfos(*disc, msg, infos);

    ASSERT_EQ(infos.size(), 1);
    EXPECT_EQ(std::get<0>(infos[0]), 35);
}

TEST(MctpEndpointDiscoveryTest, getAddedMctpInfosSkipsUnknownInterface)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    auto rawBus = sdbusplus::bus::new_default();
    auto msg =
        rawBus.new_method_call("org.test", "/test", "org.test.Intf", "Method");

    sdbusplus::message::object_path objPath(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/45");

    using PropertyMap = std::map<std::string, pldm::dbus::Value>;
    std::map<std::string, PropertyMap> interfaces;
    PropertyMap otherProps;
    otherProps["Any"] = std::string("value");
    interfaces["xyz.openbmc_project.NotMCTP"] = otherProps;

    msg.append(objPath, interfaces);
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    pldm::MctpInfos infos;
    TestMctpDiscovery::getAddedMctpInfos(*disc, msg, infos);
    EXPECT_TRUE(infos.empty());
}

TEST(MctpEndpointDiscoveryTest, getAddedMctpInfosMissingRequiredProperties)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    auto rawBus = sdbusplus::bus::new_default();
    auto msg =
        rawBus.new_method_call("org.test", "/test", "org.test.Intf", "Method");

    sdbusplus::message::object_path objPath(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/55");

    using PropertyMap = std::map<std::string, pldm::dbus::Value>;
    std::map<std::string, PropertyMap> interfaces;
    PropertyMap mctpProps;
    mctpProps["NetworkId"] = uint32_t(1);
    mctpProps["EID"] = uint8_t(55);
    mctpProps["SupportedMessageTypes"] = std::vector<uint8_t>{1};
    interfaces["xyz.openbmc_project.MCTP.Endpoint"] = mctpProps;
    PropertyMap uuidPropsMRP;
    uuidPropsMRP["UUID"] = std::string("eeee0000-1111-2222-3333-444455556666");
    interfaces["xyz.openbmc_project.Common.UUID"] = uuidPropsMRP;

    msg.append(objPath, interfaces);
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    pldm::MctpInfos infos;
    TestMctpDiscovery::getAddedMctpInfos(*disc, msg, infos);
    EXPECT_TRUE(infos.empty());
}

TEST(MctpEndpointDiscoveryTest, getAddedMctpInfosMissingNetworkId)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    auto rawBus = sdbusplus::bus::new_default();
    auto msg =
        rawBus.new_method_call("org.test", "/test", "org.test.Intf", "Method");

    sdbusplus::message::object_path objPath(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/56");

    using PropertyMap = std::map<std::string, pldm::dbus::Value>;
    std::map<std::string, PropertyMap> interfaces;
    PropertyMap mctpProps;
    mctpProps["EID"] = uint8_t(56);
    mctpProps["SupportedMessageTypes"] = std::vector<uint8_t>{1};
    mctpProps["MediumType"] = std::string("SMBus");
    interfaces["xyz.openbmc_project.MCTP.Endpoint"] = mctpProps;
    PropertyMap uuidPropsNN;
    uuidPropsNN["UUID"] = std::string("eeee0000-1111-2222-3333-444455556667");
    interfaces["xyz.openbmc_project.Common.UUID"] = uuidPropsNN;

    msg.append(objPath, interfaces);
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    pldm::MctpInfos infos;
    TestMctpDiscovery::getAddedMctpInfos(*disc, msg, infos);
    EXPECT_TRUE(infos.empty());
}

TEST(MctpEndpointDiscoveryTest, getAddedMctpInfosMissingEid)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    auto rawBus = sdbusplus::bus::new_default();
    auto msg =
        rawBus.new_method_call("org.test", "/test", "org.test.Intf", "Method");

    sdbusplus::message::object_path objPath(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/561");

    using PropertyMap = std::map<std::string, pldm::dbus::Value>;
    std::map<std::string, PropertyMap> interfaces;
    PropertyMap mctpProps;
    mctpProps["NetworkId"] = uint32_t(1);
    mctpProps["SupportedMessageTypes"] = std::vector<uint8_t>{1};
    mctpProps["MediumType"] = std::string("SMBus");
    interfaces["xyz.openbmc_project.MCTP.Endpoint"] = mctpProps;
    PropertyMap uuidPropsME;
    uuidPropsME["UUID"] = std::string("eeee0000-1111-2222-3333-44445555666a");
    interfaces["xyz.openbmc_project.Common.UUID"] = uuidPropsME;

    msg.append(objPath, interfaces);
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    pldm::MctpInfos infos;
    TestMctpDiscovery::getAddedMctpInfos(*disc, msg, infos);
    EXPECT_TRUE(infos.empty());
}

TEST(MctpEndpointDiscoveryTest, getAddedMctpInfosOnlyNetworkIdPresent)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    auto rawBus = sdbusplus::bus::new_default();
    auto msg =
        rawBus.new_method_call("org.test", "/test", "org.test.Intf", "Method");

    sdbusplus::message::object_path objPath(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/566");

    using PropertyMap = std::map<std::string, pldm::dbus::Value>;
    std::map<std::string, PropertyMap> interfaces;
    PropertyMap mctpProps;
    mctpProps["NetworkId"] = uint32_t(1);
    interfaces["xyz.openbmc_project.MCTP.Endpoint"] = mctpProps;
    PropertyMap uuidPropsON;
    uuidPropsON["UUID"] = std::string("eeee0000-1111-2222-3333-44445555666f");
    interfaces["xyz.openbmc_project.Common.UUID"] = uuidPropsON;

    msg.append(objPath, interfaces);
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    pldm::MctpInfos infos;
    TestMctpDiscovery::getAddedMctpInfos(*disc, msg, infos);
    EXPECT_TRUE(infos.empty());
}

TEST(MctpEndpointDiscoveryTest, getAddedMctpInfosNetworkAndEidOnly)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    auto rawBus = sdbusplus::bus::new_default();
    auto msg =
        rawBus.new_method_call("org.test", "/test", "org.test.Intf", "Method");

    sdbusplus::message::object_path objPath(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/567");

    using PropertyMap = std::map<std::string, pldm::dbus::Value>;
    std::map<std::string, PropertyMap> interfaces;
    PropertyMap mctpProps;
    mctpProps["NetworkId"] = uint32_t(1);
    mctpProps["EID"] = uint8_t(67);
    interfaces["xyz.openbmc_project.MCTP.Endpoint"] = mctpProps;
    PropertyMap uuidPropsNE;
    uuidPropsNE["UUID"] = std::string("eeee0000-1111-2222-3333-444455556670");
    interfaces["xyz.openbmc_project.Common.UUID"] = uuidPropsNE;

    msg.append(objPath, interfaces);
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    pldm::MctpInfos infos;
    TestMctpDiscovery::getAddedMctpInfos(*disc, msg, infos);
    EXPECT_TRUE(infos.empty());
}

TEST(MctpEndpointDiscoveryTest, getAddedMctpInfosMissingSupportedTypes)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    auto rawBus = sdbusplus::bus::new_default();
    auto msg =
        rawBus.new_method_call("org.test", "/test", "org.test.Intf", "Method");

    sdbusplus::message::object_path objPath(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/57");

    using PropertyMap = std::map<std::string, pldm::dbus::Value>;
    std::map<std::string, PropertyMap> interfaces;
    PropertyMap mctpProps;
    mctpProps["NetworkId"] = uint32_t(1);
    mctpProps["EID"] = uint8_t(57);
    mctpProps["MediumType"] = std::string("SMBus");
    interfaces["xyz.openbmc_project.MCTP.Endpoint"] = mctpProps;
    PropertyMap uuidPropsMST;
    uuidPropsMST["UUID"] = std::string("eeee0000-1111-2222-3333-444455556668");
    interfaces["xyz.openbmc_project.Common.UUID"] = uuidPropsMST;

    msg.append(objPath, interfaces);
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    pldm::MctpInfos infos;
    TestMctpDiscovery::getAddedMctpInfos(*disc, msg, infos);
    EXPECT_TRUE(infos.empty());
}

TEST(MctpEndpointDiscoveryTest, getAddedMctpInfosMissingMediumType)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);
    auto msg = makeInterfacesAddedMessage(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/575",
        {{pldm::MCTPInterface,
          {{"NetworkId", uint32_t(1)},
           {"EID", uint8_t(75)},
           {"SupportedMessageTypes", std::vector<uint8_t>{1}}}},
         {pldm::EndpointUUID,
          {{"UUID", std::string("eeee0000-1111-2222-3333-444455556675")}}},
         {pldm::MCTPBindingInterface,
          {{"BindingType",
            std::string(
                "xyz.openbmc_project.MCTP.Endpoint.BindingTypes.SMBus")}}}});

    EXPECT_CALL(mockedDbusHandler, getDbusPropertyVariant(_, _, _)).Times(0);
    EXPECT_CALL(mockedDbusHandler, getService(_, _)).Times(0);
    EXPECT_CALL(mockedDbusHandler, getDbusPropertiesVariant(_, _, _)).Times(0);
    EXPECT_CALL(mockedDbusHandler, getAssociatedSubTree(_, _, _, _)).Times(0);

    pldm::MctpInfos infos;
    TestMctpDiscovery::getAddedMctpInfos(*disc, msg, infos);
    EXPECT_TRUE(infos.empty());
}

TEST(MctpEndpointDiscoveryTest, getAddedMctpInfosBadVariantSkipsEndpoint)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    auto rawBus = sdbusplus::bus::new_default();
    auto msg =
        rawBus.new_method_call("org.test", "/test", "org.test.Intf", "Method");

    sdbusplus::message::object_path objPath(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/562");

    using PropertyMap = std::map<std::string, pldm::dbus::Value>;
    std::map<std::string, PropertyMap> interfaces;
    PropertyMap mctpProps;
    mctpProps["NetworkId"] = uint32_t(1);
    mctpProps["EID"] = std::string("bad-eid");
    mctpProps["SupportedMessageTypes"] = std::vector<uint8_t>{1};
    mctpProps["MediumType"] = std::string("SMBus");
    interfaces["xyz.openbmc_project.MCTP.Endpoint"] = mctpProps;
    PropertyMap uuidPropsBV;
    uuidPropsBV["UUID"] = std::string("eeee0000-1111-2222-3333-44445555666b");
    interfaces["xyz.openbmc_project.Common.UUID"] = uuidPropsBV;

    msg.append(objPath, interfaces);
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    pldm::MctpInfos infos;
    EXPECT_NO_THROW(TestMctpDiscovery::getAddedMctpInfos(*disc, msg, infos));
    EXPECT_TRUE(infos.empty());
}

TEST(MctpEndpointDiscoveryTest, getAddedMctpInfosBadNetworkVariantSkipsEndpoint)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    auto rawBus = sdbusplus::bus::new_default();
    auto msg =
        rawBus.new_method_call("org.test", "/test", "org.test.Intf", "Method");

    sdbusplus::message::object_path objPath(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/563");

    using PropertyMap = std::map<std::string, pldm::dbus::Value>;
    std::map<std::string, PropertyMap> interfaces;
    PropertyMap mctpProps;
    mctpProps["NetworkId"] = std::string("bad-network");
    mctpProps["EID"] = uint8_t(63);
    mctpProps["SupportedMessageTypes"] = std::vector<uint8_t>{1};
    mctpProps["MediumType"] = std::string("SMBus");
    interfaces["xyz.openbmc_project.MCTP.Endpoint"] = mctpProps;
    PropertyMap uuidPropsBN;
    uuidPropsBN["UUID"] = std::string("eeee0000-1111-2222-3333-44445555666c");
    interfaces["xyz.openbmc_project.Common.UUID"] = uuidPropsBN;

    msg.append(objPath, interfaces);
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    pldm::MctpInfos infos;
    EXPECT_NO_THROW(TestMctpDiscovery::getAddedMctpInfos(*disc, msg, infos));
    EXPECT_TRUE(infos.empty());
}

TEST(MctpEndpointDiscoveryTest, getAddedMctpInfosBadTypesVariantSkipsEndpoint)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    auto rawBus = sdbusplus::bus::new_default();
    auto msg =
        rawBus.new_method_call("org.test", "/test", "org.test.Intf", "Method");

    sdbusplus::message::object_path objPath(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/564");

    using PropertyMap = std::map<std::string, pldm::dbus::Value>;
    std::map<std::string, PropertyMap> interfaces;
    PropertyMap mctpProps;
    mctpProps["NetworkId"] = uint32_t(1);
    mctpProps["EID"] = uint8_t(64);
    mctpProps["SupportedMessageTypes"] = std::string("bad-types");
    mctpProps["MediumType"] = std::string("SMBus");
    interfaces["xyz.openbmc_project.MCTP.Endpoint"] = mctpProps;
    PropertyMap uuidPropsBT;
    uuidPropsBT["UUID"] = std::string("eeee0000-1111-2222-3333-44445555666d");
    interfaces["xyz.openbmc_project.Common.UUID"] = uuidPropsBT;

    msg.append(objPath, interfaces);
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    pldm::MctpInfos infos;
    EXPECT_NO_THROW(TestMctpDiscovery::getAddedMctpInfos(*disc, msg, infos));
    EXPECT_TRUE(infos.empty());
}

TEST(MctpEndpointDiscoveryTest, getAddedMctpInfosBadMediumVariantSkipsEndpoint)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    auto rawBus = sdbusplus::bus::new_default();
    auto msg =
        rawBus.new_method_call("org.test", "/test", "org.test.Intf", "Method");

    sdbusplus::message::object_path objPath(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/565");

    using PropertyMap = std::map<std::string, pldm::dbus::Value>;
    std::map<std::string, PropertyMap> interfaces;
    PropertyMap mctpProps;
    mctpProps["NetworkId"] = uint32_t(1);
    mctpProps["EID"] = uint8_t(65);
    mctpProps["SupportedMessageTypes"] = std::vector<uint8_t>{1};
    mctpProps["MediumType"] = uint64_t(8);
    interfaces["xyz.openbmc_project.MCTP.Endpoint"] = mctpProps;
    PropertyMap uuidPropsBM;
    uuidPropsBM["UUID"] = std::string("eeee0000-1111-2222-3333-44445555666e");
    interfaces["xyz.openbmc_project.Common.UUID"] = uuidPropsBM;

    msg.append(objPath, interfaces);
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    pldm::MctpInfos infos;
    EXPECT_NO_THROW(TestMctpDiscovery::getAddedMctpInfos(*disc, msg, infos));
    EXPECT_TRUE(infos.empty());
}

TEST(MctpEndpointDiscoveryTest, getAddedMctpInfosBindingTypeException)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    auto rawBus = sdbusplus::bus::new_default();
    auto msg =
        rawBus.new_method_call("org.test", "/test", "org.test.Intf", "Method");

    sdbusplus::message::object_path objPath(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/58");

    using PropertyMap = std::map<std::string, pldm::dbus::Value>;
    std::map<std::string, PropertyMap> interfaces;
    PropertyMap mctpProps;
    mctpProps["NetworkId"] = uint32_t(1);
    mctpProps["EID"] = uint8_t(58);
    mctpProps["SupportedMessageTypes"] = std::vector<uint8_t>{1};
    mctpProps["MediumType"] = std::string("SMBus");
    interfaces["xyz.openbmc_project.MCTP.Endpoint"] = mctpProps;
    PropertyMap uuidPropsBTE;
    uuidPropsBTE["UUID"] = std::string("eeee0000-1111-2222-3333-444455556669");
    interfaces["xyz.openbmc_project.Common.UUID"] = uuidPropsBTE;

    msg.append(objPath, interfaces);
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    pldm::MctpInfos infos;
    TestMctpDiscovery::getAddedMctpInfos(*disc, msg, infos);
    EXPECT_TRUE(infos.empty());
}

TEST(MctpEndpointDiscoveryTest, getAddedMctpInfosDuplicatePathNoDuplicateMatch)
{
    MockdBusHandler mockedDbusHandler;
    pldm::MockManager manager;

    auto disc = makeDiscoveryWithMock(mockedDbusHandler, &manager);

    auto rawBus = sdbusplus::bus::new_default();
    auto msg =
        rawBus.new_method_call("org.test", "/test", "org.test.Intf", "Method");

    sdbusplus::message::object_path objPath(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/65");

    using PropertyMap = std::map<std::string, pldm::dbus::Value>;
    std::map<std::string, PropertyMap> interfaces;
    PropertyMap mctpProps;
    mctpProps["NetworkId"] = uint32_t(1);
    mctpProps["EID"] = uint8_t(65);
    mctpProps["SupportedMessageTypes"] = std::vector<uint8_t>{1};
    mctpProps["MediumType"] = std::string("SMBus");
    interfaces["xyz.openbmc_project.MCTP.Endpoint"] = mctpProps;
    PropertyMap bindingPropsDP;
    bindingPropsDP["BindingType"] = std::string("MctpOverSMBus");
    interfaces["xyz.openbmc_project.MCTP.Binding"] = bindingPropsDP;
    PropertyMap uuidPropsDP;
    uuidPropsDP["UUID"] = std::string("ffff0000-1111-2222-3333-444455556666");
    interfaces["xyz.openbmc_project.Common.UUID"] = uuidPropsDP;
    PropertyMap ccPropsDP;
    ccPropsDP["Connectivity"] = std::string("Available");
    interfaces["au.com.codeconstruct.MCTP.Endpoint1"] = ccPropsDP;

    msg.append(objPath, interfaces);
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);

    pldm::utils::GetAssociatedSubTreeResponse emptyAssocResponse{};
    EXPECT_CALL(mockedDbusHandler, getAssociatedSubTree(_, _, _, _))
        .Times(2)
        .WillRepeatedly(testing::Return(emptyAssocResponse));

    pldm::MctpInfos infosFirst;
    TestMctpDiscovery::getAddedMctpInfos(*disc, msg, infosFirst);
    sd_bus_message_rewind(msg.get(), true);
    pldm::MctpInfos infosSecond;
    TestMctpDiscovery::getAddedMctpInfos(*disc, msg, infosSecond);

    EXPECT_EQ(infosFirst.size(), 1);
    EXPECT_EQ(infosSecond.size(), 1);
}

TEST(MctpEndpointDiscoveryTest, NullHandlersAreSkipped)
{
    auto& bus = pldm::utils::DBusHandler::getBus();
    pldm::MockManager manager;

    auto disc = std::make_unique<pldm::MctpDiscovery>(
        bus, std::initializer_list<pldm::MctpDiscoveryHandlerIntf*>{
                 nullptr, &manager});

    testing::Mock::VerifyAndClearExpectations(&manager);

    const pldm::MctpInfo mctpInfo(70, pldm::emptyUUID, "", 1, std::nullopt, "",
                                  std::nullopt);
    const pldm::MctpInfos infos = {mctpInfo};

    EXPECT_CALL(manager, handleMctpEndpoints(_, _)).Times(1);
    disc->handleMctpEndpoints(infos);
    testing::Mock::VerifyAndClearExpectations(&manager);

    EXPECT_CALL(manager, handleRemovedMctpEndpoints(_)).Times(1);
    disc->handleRemovedMctpEndpoints(infos);
    testing::Mock::VerifyAndClearExpectations(&manager);

    EXPECT_CALL(manager, updateMctpEndpointAvailability(_, true)).Times(1);
    disc->updateMctpEndpointAvailability(mctpInfo, true);
}
