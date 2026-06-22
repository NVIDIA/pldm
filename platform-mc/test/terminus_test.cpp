/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION &
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
#include "libpldm/entity.h"

#include "common/dBusAsyncUtils.hpp"
#include "common/instance_id.hpp"
#include "oem/nvidia/platform-mc/mirrorEffecter.hpp"
#include "oem/nvidia/platform-mc/oem_nvidia.hpp"
#include "oem/nvidia/platform-mc/remoteDebug.hpp"
#include "oem/nvidia/platform-mc/state_set/memoryPerformance.hpp"
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "oem/nvidia/platform-mc/staticPowerHint.hpp"
#undef private
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "platform-mc/entity.hpp"
#include "platform-mc/errors.hpp"
#include "platform-mc/state_set/ethIBPortLinkState.hpp"
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#define protected public
#include "platform-mc/terminus.hpp"
#undef protected
#undef private
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "mock_terminus_manager.hpp"
#include "platform-mc/platform_manager.hpp"
#include "platform-mc/sensor_manager.hpp"
#include "test/test_instance_id.hpp"

#include <boost/asio/io_context.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/async.hpp>
#include <sdeventplus/event.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <future>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

using namespace pldm::platform_mc;
const uint8_t localEid = 0x08;

template <typename Sender>
auto syncWaitWithDbusIo(Sender&& sender)
{
    using Result = decltype(stdexec::sync_wait(std::forward<Sender>(sender)));
    using ResultValue = typename Result::value_type;

    auto& io = pldm::utils::DBusHandler::getAsioConnection()->get_io_context();
    io.restart();

    exec::async_scope scope;
    std::exception_ptr operationException;
    Result result;
    bool done = false;

    auto captureError = [&](auto&& error) {
        using Error = std::decay_t<decltype(error)>;
        if constexpr (std::is_same_v<Error, std::exception_ptr>)
        {
            operationException = error;
        }
        else if constexpr (std::is_base_of_v<std::exception, Error>)
        {
            operationException =
                std::make_exception_ptr(std::forward<decltype(error)>(error));
        }
        else
        {
            operationException = std::make_exception_ptr(std::runtime_error(
                "Sender completed with an unexpected "
                "error type"));
        }
        done = true;
    };

    try
    {
        scope.spawn(
            std::forward<Sender>(sender) | stdexec::then([&](auto&&... values) {
                result.emplace(
                    ResultValue{std::forward<decltype(values)>(values)...});
                done = true;
            }) | stdexec::upon_error(captureError) |
                stdexec::upon_stopped([&] { done = true; }),
            exec::default_task_context<void>(stdexec::inline_scheduler{}));

        while (true)
        {
            io.run_for(std::chrono::milliseconds(1));
            io.restart();
            if (done)
            {
                break;
            }
        }

        stdexec::sync_wait(scope.on_empty());
    }
    catch (...)
    {
        operationException = std::current_exception();
    }

    if (operationException)
    {
        std::rethrow_exception(operationException);
    }

    return result;
}

static void waitForRefreshAssociationsTask(Terminus& terminus)
{
    if (!terminus.refreshAssociationsTaskHandle.has_value())
    {
        return;
    }

    auto& rcOpt = std::get<1>(*terminus.refreshAssociationsTaskHandle);
    auto& io = pldm::utils::DBusHandler::getAsioConnection()->get_io_context();
    for (size_t attempt = 0; attempt < 1000 && !rcOpt.has_value(); ++attempt)
    {
        io.run_for(std::chrono::milliseconds(1));
        io.restart();
    }

    const bool completed = rcOpt.has_value();
    terminus.refreshAssociationsTaskHandle.reset();
    EXPECT_TRUE(completed);
}

class AsyncEntityManagerServer
{
  public:
    static constexpr auto entityManagerService =
        "xyz.openbmc_project.EntityManager";

    AsyncEntityManagerServer()
    {
        connection = std::make_shared<sdbusplus::asio::connection>(
            io, sdbusplus::bus::new_bus());
        connection->request_name(pldm::utils::mapperService);
        connection->request_name(entityManagerService);
        server = std::make_unique<sdbusplus::asio::object_server>(connection);

        mapperIface = server->add_interface(pldm::utils::mapperPath,
                                            pldm::utils::mapperInterface);
        mapperIface->register_method(
            "GetSubTree", [this](const std::string& rootPath, int /*depth*/,
                                 const std::vector<std::string>& interfaces) {
                return getSubTree(rootPath, interfaces);
            });
        mapperIface->register_method(
            "GetObject", [this](const std::string& objectPath,
                                const std::vector<std::string>& interfaces) {
                return getObject(objectPath, interfaces);
            });
        mapperIface->initialize();

        ioThread = std::thread([this] { io.run(); });
        waitUntilIoDrained();
    }

    ~AsyncEntityManagerServer()
    {
        io.stop();
        if (ioThread.joinable())
        {
            ioThread.join();
        }

        std::lock_guard<std::mutex> lock(mutex);
        registeredInterfaces.clear();
        subtreeOverrides.clear();
        mapperIface.reset();
        server.reset();
        connection.reset();
    }

    void addInterface(const std::string& path, const std::string& interfaceName,
                      const pldm::utils::PropertyMap& properties,
                      const std::string& serviceName = entityManagerService)
    {
        auto iface = server->add_interface(path, interfaceName);
        for (const auto& [propertyName, propertyValue] : properties)
        {
            if (std::holds_alternative<uint64_t>(propertyValue))
            {
                iface->register_property(propertyName,
                                         std::get<uint64_t>(propertyValue));
            }
            else if (std::holds_alternative<std::string>(propertyValue))
            {
                iface->register_property(propertyName,
                                         std::get<std::string>(propertyValue));
            }
            else if (std::holds_alternative<std::vector<std::string>>(
                         propertyValue))
            {
                iface->register_property(
                    propertyName,
                    std::get<std::vector<std::string>>(propertyValue));
            }
            else
            {
                throw std::invalid_argument("Unsupported property type");
            }
        }
        iface->initialize();
        {
            std::lock_guard<std::mutex> lock(mutex);
            registeredInterfaces[path].push_back(
                {serviceName, interfaceName, std::move(iface)});
        }
        waitUntilIoDrained();
    }

    void setSubTreeResponse(const std::string& rootPath,
                            pldm::utils::GetSubTreeResponse response)
    {
        std::lock_guard<std::mutex> lock(mutex);
        subtreeOverrides[rootPath] = std::move(response);
    }

  private:
    struct RegisteredInterface
    {
        std::string serviceName;
        std::string interfaceName;
        std::shared_ptr<sdbusplus::asio::dbus_interface> iface;
    };

    static bool isInSubTree(const std::string& path, const std::string& root)
    {
        if (path == root)
        {
            return true;
        }

        if (root == "/")
        {
            return !path.empty() && path.front() == '/';
        }

        return path.size() > root.size() &&
               path.compare(0, root.size(), root) == 0 &&
               path[root.size()] == '/';
    }

    static bool wantsInterface(const std::vector<std::string>& interfaces,
                               const std::string& interfaceName)
    {
        return interfaces.empty() ||
               std::find(interfaces.begin(), interfaces.end(), interfaceName) !=
                   interfaces.end();
    }

    pldm::utils::MapperServiceMap buildServiceMap(
        const std::vector<RegisteredInterface>& entries,
        const std::vector<std::string>& interfaces) const
    {
        std::map<std::string, std::vector<std::string>> grouped;
        for (const auto& entry : entries)
        {
            if (!wantsInterface(interfaces, entry.interfaceName))
            {
                continue;
            }
            grouped[entry.serviceName].push_back(entry.interfaceName);
        }

        pldm::utils::MapperServiceMap serviceMap;
        for (auto& [serviceName, matchedInterfaces] : grouped)
        {
            serviceMap.emplace_back(serviceName, std::move(matchedInterfaces));
        }
        return serviceMap;
    }

    pldm::utils::GetSubTreeResponse getSubTree(
        const std::string& rootPath,
        const std::vector<std::string>& interfaces) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto overrideIt = subtreeOverrides.find(rootPath);
        if (overrideIt != subtreeOverrides.end())
        {
            return overrideIt->second;
        }

        pldm::utils::GetSubTreeResponse response;
        for (const auto& [path, entries] : registeredInterfaces)
        {
            if (!isInSubTree(path, rootPath))
            {
                continue;
            }

            auto serviceMap = buildServiceMap(entries, interfaces);
            if (!serviceMap.empty())
            {
                response.emplace_back(path, std::move(serviceMap));
            }
        }
        return response;
    }

    pldm::utils::MapperServiceMap getObject(
        const std::string& objectPath,
        const std::vector<std::string>& interfaces) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = registeredInterfaces.find(objectPath);
        if (it == registeredInterfaces.end())
        {
            return {};
        }
        return buildServiceMap(it->second, interfaces);
    }

    void waitUntilIoDrained()
    {
        std::promise<void> ready;
        auto readyFuture = ready.get_future();
        boost::asio::post(io, [&ready] {
            try
            {
                ready.set_value();
            }
            catch (...)
            {}
        });

        if (readyFuture.wait_for(std::chrono::seconds(1)) !=
            std::future_status::ready)
        {
            ADD_FAILURE() << "Timed out waiting for AsyncEntityManagerServer "
                             "io_context to drain";
        }
    }

    boost::asio::io_context io;
    std::shared_ptr<sdbusplus::asio::connection> connection;
    std::unique_ptr<sdbusplus::asio::object_server> server;
    std::shared_ptr<sdbusplus::asio::dbus_interface> mapperIface;
    std::map<std::string, std::vector<RegisteredInterface>>
        registeredInterfaces;
    std::map<std::string, pldm::utils::GetSubTreeResponse> subtreeOverrides;
    std::thread ioThread;
    mutable std::mutex mutex;
};

class TerminusTest : public testing::Test
{
  protected:
    TerminusTest() :
        bus(pldm::utils::DBusHandler::getBus()),
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        terminusManager(event, reqHandler, instanceIdDb, termini, localEid,
                        nullptr),
        sensorManager(event, terminusManager, termini, nullptr),
        platformManager(terminusManager, termini)
    {}

    void runEventLoopForMilliseconds(uint64_t msec)
    {
        uint64_t t0 = 0;
        uint64_t t1 = 0;
        uint64_t usec = msec * 1000;
        uint64_t elapsed = 0;
        sd_event_now(event.get(), CLOCK_MONOTONIC, &t0);
        do
        {
            sd_event_run(event.get(), usec - elapsed);
            sd_event_now(event.get(), CLOCK_MONOTONIC, &t1);
            elapsed = t1 - t0;
        } while (elapsed < usec);
    }

    void setupResponsesForDiscoverTerminus()
    {
        auto rc = terminusManager.clearQueuedResponses();
        EXPECT_EQ(rc, PLDM_SUCCESS);

        std::vector<uint8_t> getTidResp0{0x00, PLDM_BASE, PLDM_GET_TID,
                                         PLDM_SUCCESS, 0x00};
        rc = terminusManager.enqueueResponse(getTidResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);

        std::vector<uint8_t> setTidResp0{0x00, PLDM_BASE, PLDM_SET_TID,
                                         PLDM_SUCCESS};
        rc = terminusManager.enqueueResponse(setTidResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);

        // support pldm type0 and type2
        std::vector<uint8_t> getPldmTypesResp0{
            0x00, PLDM_BASE, 0x04, 0x00, 0x05, 0x00,
            0x00, 0x00,      0x00, 0x00, 0x00, 0x00};
        rc = terminusManager.enqueueResponse(getPldmTypesResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);

        std::vector<uint8_t> getTerminusUidResp0{
            0x00, PLDM_PLATFORM, PLDM_GET_TERMINUS_UID,
            PLDM_ERROR_UNSUPPORTED_PLDM_CMD};
        rc = terminusManager.enqueueResponse(getTerminusUidResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);
    }

    void setupResponsesForInitTerminus()
    {
        auto rc = terminusManager.clearQueuedResponses();
        EXPECT_EQ(rc, PLDM_SUCCESS);

        std::vector<uint8_t> eventMessageBufferSizeResp0{
            0x00, PLDM_PLATFORM, PLDM_EVENT_MESSAGE_BUFFER_SIZE,
            PLDM_ERROR_UNSUPPORTED_PLDM_CMD};
        rc = terminusManager.enqueueResponse(eventMessageBufferSizeResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);

        std::vector<uint8_t> eventMessageSupportedResp0{
            0x00, PLDM_PLATFORM, PLDM_EVENT_MESSAGE_SUPPORTED,
            PLDM_ERROR_UNSUPPORTED_PLDM_CMD};
        rc = terminusManager.enqueueResponse(eventMessageSupportedResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);

        std::vector<uint8_t> getPDRRepositoryInfoResp0{
            0x00, PLDM_PLATFORM, PLDM_GET_PDR_REPOSITORY_INFO,
            PLDM_ERROR_UNSUPPORTED_PLDM_CMD};
        rc = terminusManager.enqueueResponse(getPDRRepositoryInfoResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);

        std::vector<uint8_t> getPdrResp0{
            0x00,
            PLDM_PLATFORM,
            PLDM_GET_PDR,
            PLDM_SUCCESS,
            0x00,
            0x00,
            0x00,
            0x00, // nextRecordHandle
            0x00,
            0x00,
            0x00,
            0x00, // nextDataTransferHandle
            0x05, // startAndEnd
            71,
            0,    // responseCount (PDR = 10 header + 61 body)
            0x00,
            0x00,
            0x00,
            0x01,                        // record handle
            0x01,                        // PDRHeaderVersion
            PLDM_NUMERIC_SENSOR_PDR,     // PDRType
            0x00,
            0x00,                        // recordChangeNumber
            61,
            0,                           // dataLength (body = 61 bytes)
            0x00,
            0x00,                        // PLDMTerminusHandle
            0x01,
            0x00,                        // sensorID=1
            PLDM_ENTITY_POWER_SUPPLY,
            0,                           // entityType=Power Supply(120)
            1,
            0,                           // entityInstanceNumber
            0x1,
            0x0,                         // containerID=1
            PLDM_NO_INIT,                // sensorInit
            false,                       // sensorAuxiliaryNamesPDR
            PLDM_SENSOR_UNIT_DEGRESS_C,  // baseUint(2)=degrees C
            0,                           // unitModifier = 0
            0,                           // rateUnit
            0,                           // baseOEMUnitHandle
            0,                           // auxUnit
            0,                           // auxUnitModifier
            0,                           // auxRateUnit
            0,                           // rel
            0,                           // auxOEMUnitHandle
            true,                        // isLinear
            PLDM_SENSOR_DATA_SIZE_UINT8, // sensorDataSize
            0,
            0,
            0xc0,
            0x3f, // resolution=1.5
            0,
            0,
            0x80,
            0x3f, // offset=1.0
            0,
            0,    // accuracy
            0,    // plusTolerance
            0,    // minusTolerance
            2,    // hysteresis
            0,    // supportedThresholds
            0,    // thresholdAndHysteresisVolatility
            0,
            0,
            0x80,
            0x3f, // stateTransistionInterval=1.0
            0,
            0,
            0x80,
            0x3f,                          // updateInverval=1.0
            255,                           // maxReadable
            0,                             // minReadable
            PLDM_RANGE_FIELD_FORMAT_UINT8, // rangeFieldFormat
            0,                             // rangeFieldsupport
            0,                             // nominalValue
            0,                             // normalMax
            0,                             // normalMin
            0,                             // warningHigh
            0,                             // warningLow
            0,                             // criticalHigh
            0,                             // criticalLow
            0,                             // fatalHigh
            0,                             // fatalLow
            0,
            0,
            0 // padding (body = 61 bytes)
        };
        rc = terminusManager.enqueueResponse(getPdrResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);
    }

    void setupResponsesForStartPolling()
    {
        auto rc = terminusManager.clearQueuedResponses();
        EXPECT_EQ(rc, PLDM_SUCCESS);

        std::vector<uint8_t> getSensorReadingResp0{
            0x00,
            PLDM_PLATFORM,
            PLDM_GET_SENSOR_READING,
            PLDM_SUCCESS,
            PLDM_SENSOR_DATA_SIZE_UINT8,
            PLDM_SENSOR_ENABLED,
            PLDM_NO_EVENT_GENERATION,
            PLDM_SENSOR_NORMAL,
            PLDM_SENSOR_NORMAL,
            PLDM_SENSOR_NORMAL,
            0x12};
        rc = terminusManager.enqueueResponse(getSensorReadingResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);
        rc = terminusManager.enqueueResponse(getSensorReadingResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);
        rc = terminusManager.enqueueResponse(getSensorReadingResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);
    }

    sdbusplus::bus_t& bus;
    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler;
    pldm::platform_mc::MockTerminusManager terminusManager;
    pldm::platform_mc::SensorManager sensorManager;
    pldm::platform_mc::PlatformManager platformManager;
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
};

static void sealAndRewind(sdbusplus::message::message& msg)
{
    EXPECT_GE(sd_bus_message_seal(msg.get(), 0, 0), 0);
    EXPECT_GE(sd_bus_message_rewind(msg.get(), true), 0);
}

static std::vector<uint8_t> makeEntityAssociationPdr()
{
    std::vector<uint8_t> pdr(
        sizeof(pldm_pdr_hdr) + sizeof(pldm_pdr_entity_association), 0);
    auto* hdr = reinterpret_cast<pldm_pdr_hdr*>(pdr.data());
    hdr->record_handle = 4;
    hdr->version = 1;
    hdr->type = PLDM_PDR_ENTITY_ASSOCIATION;
    hdr->record_change_num = 0;
    hdr->length = pdr.size() - sizeof(pldm_pdr_hdr);

    auto* assoc = reinterpret_cast<pldm_pdr_entity_association*>(
        pdr.data() + sizeof(pldm_pdr_hdr));
    assoc->container_id = 1;
    assoc->association_type = PLDM_ENTITY_ASSOCIAION_PHYSICAL;
    assoc->container.entity_type = PLDM_ENTITY_SYSTEM_CHASSIS;
    assoc->container.entity_instance_num = 1;
    assoc->container.entity_container_id = 0;
    assoc->num_children = 1;
    assoc->children[0].entity_type = PLDM_ENTITY_POWER_SUPPLY;
    assoc->children[0].entity_instance_num = 1;
    assoc->children[0].entity_container_id = 1;
    return pdr;
}

static std::vector<uint8_t> makeAuxNamePdr(uint16_t effecterId, uint8_t pdrType)
{
    std::vector<uint8_t> names{
        1,                     // nameStringCount
        'e',  'n',  0x00,      // name language tag: "en"
        0x00, 0x41, 0x00, 0x00 // UTF16-BE "A"
    };

    std::vector<uint8_t> pdr(
        sizeof(pldm_effecter_aux_name_pdr) + names.size() - 1, 0);
    auto* aux = reinterpret_cast<pldm_effecter_aux_name_pdr*>(pdr.data());
    aux->hdr.record_handle = 6;
    aux->hdr.version = 1;
    aux->hdr.type = pdrType;
    aux->hdr.record_change_num = 0;
    aux->hdr.length = pdr.size() - sizeof(pldm_pdr_hdr);
    aux->terminus_handle = 1;
    aux->effecter_id = effecterId;
    aux->effecter_count = 1;
    memcpy(aux->effecter_names, names.data(), names.size());
    return pdr;
}

/* Build a compact (wire-format) numeric effecter PDR byte vector.
 * decode_numeric_effecter_pdr_data() expects each variable-width field
 * (max_settable, min_settable, range fields) to occupy exactly the number
 * of bytes indicated by effecter_data_size / range_field_format, NOT the
 * full sizeof(union_effecter_data_size) = 8 bytes used by the C struct. */
static std::vector<uint8_t> serializeNumericEffecterPdr(
    const pldm_numeric_effecter_value_pdr& pdr)
{
    /* Build payload bytes separately so we can compute hdr.length. */
    std::vector<uint8_t> payload;
    auto pu8 = [&](uint8_t x) { payload.push_back(x); };
    auto ple16 = [&](uint16_t x) {
        payload.push_back(x & 0xFF);
        payload.push_back((x >> 8) & 0xFF);
    };
    auto ple32 = [&](uint32_t x) {
        payload.push_back(x & 0xFF);
        payload.push_back((x >> 8) & 0xFF);
        payload.push_back((x >> 16) & 0xFF);
        payload.push_back((x >> 24) & 0xFF);
    };
    auto ple64 = [&](uint64_t x) {
        ple32(static_cast<uint32_t>(x & 0xFFFFFFFF));
        ple32(static_cast<uint32_t>(x >> 32));
    };
    auto plef32 = [&](float x) {
        uint32_t bits;
        memcpy(&bits, &x, 4);
        ple32(bits);
    };

    ple16(pdr.terminus_handle);
    ple16(pdr.effecter_id);
    ple16(pdr.entity_type);
    ple16(pdr.entity_instance);
    ple16(pdr.container_id);
    ple16(pdr.effecter_semantic_id);
    pu8(pdr.effecter_init);
    pu8(pdr.effecter_auxiliary_names);
    pu8(pdr.base_unit);
    pu8(static_cast<uint8_t>(pdr.unit_modifier));
    pu8(pdr.rate_unit);
    pu8(pdr.base_oem_unit_handle);
    pu8(pdr.aux_unit);
    pu8(static_cast<uint8_t>(pdr.aux_unit_modifier));
    pu8(pdr.aux_rate_unit);
    pu8(pdr.aux_oem_unit_handle);
    pu8(pdr.is_linear);
    pu8(pdr.effecter_data_size);
    plef32(pdr.resolution);
    plef32(pdr.offset);
    ple16(pdr.accuracy);
    pu8(pdr.plus_tolerance);
    pu8(pdr.minus_tolerance);
    plef32(pdr.state_transition_interval);
    plef32(pdr.transition_interval);

    /* Variable-width effecter data (max_settable, min_settable). */
    auto appendEff = [&](const union_effecter_data_size& d) {
        switch (pdr.effecter_data_size)
        {
            case PLDM_EFFECTER_DATA_SIZE_UINT8:
                pu8(d.value_u8);
                break;
            case PLDM_EFFECTER_DATA_SIZE_SINT8:
                pu8(static_cast<uint8_t>(d.value_s8));
                break;
            case PLDM_EFFECTER_DATA_SIZE_UINT16:
                ple16(d.value_u16);
                break;
            case PLDM_EFFECTER_DATA_SIZE_SINT16:
                ple16(static_cast<uint16_t>(d.value_s16));
                break;
            case PLDM_EFFECTER_DATA_SIZE_UINT32:
                ple32(d.value_u32);
                break;
            case PLDM_EFFECTER_DATA_SIZE_SINT32:
                ple32(static_cast<uint32_t>(d.value_s32));
                break;
            case PLDM_EFFECTER_DATA_SIZE_UINT64:
                ple64(d.value_u64);
                break;
            case PLDM_EFFECTER_DATA_SIZE_SINT64:
                ple64(static_cast<uint64_t>(d.value_s64));
                break;
            default:
                break;
        }
    };
    appendEff(pdr.max_settable);
    appendEff(pdr.min_settable);

    pu8(pdr.range_field_format);
    pu8(pdr.range_field_support.byte);

    /* Variable-width range fields. */
    auto appendRng = [&](const union_range_field_format& d) {
        switch (pdr.range_field_format)
        {
            case PLDM_RANGE_FIELD_FORMAT_UINT8:
                pu8(d.value_u8);
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT8:
                pu8(static_cast<uint8_t>(d.value_s8));
                break;
            case PLDM_RANGE_FIELD_FORMAT_UINT16:
                ple16(d.value_u16);
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT16:
                ple16(static_cast<uint16_t>(d.value_s16));
                break;
            case PLDM_RANGE_FIELD_FORMAT_UINT32:
                ple32(d.value_u32);
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT32:
                ple32(static_cast<uint32_t>(d.value_s32));
                break;
            case PLDM_RANGE_FIELD_FORMAT_REAL32:
                plef32(d.value_f32);
                break;
            case PLDM_RANGE_FIELD_FORMAT_UINT64:
                ple64(d.value_u64);
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT64:
                ple64(static_cast<uint64_t>(d.value_s64));
                break;
            default:
                break;
        }
    };
    appendRng(pdr.nominal_value);
    appendRng(pdr.normal_max);
    appendRng(pdr.normal_min);
    appendRng(pdr.rated_max);
    appendRng(pdr.rated_min);

    /* Prepend the 10-byte PDR header (record_handle, version, type,
     * record_change_num, length) where length = payload byte count. */
    std::vector<uint8_t> v;
    auto hu8 = [&](uint8_t x) { v.push_back(x); };
    auto hle16 = [&](uint16_t x) {
        v.push_back(x & 0xFF);
        v.push_back((x >> 8) & 0xFF);
    };
    auto hle32 = [&](uint32_t x) {
        v.push_back(x & 0xFF);
        v.push_back((x >> 8) & 0xFF);
        v.push_back((x >> 16) & 0xFF);
        v.push_back((x >> 24) & 0xFF);
    };
    hle32(pdr.hdr.record_handle);
    hu8(pdr.hdr.version);
    hu8(pdr.hdr.type);
    hle16(pdr.hdr.record_change_num);
    hle16(static_cast<uint16_t>(payload.size()));
    v.insert(v.end(), payload.begin(), payload.end());
    return v;
}

static std::vector<uint8_t> makeNumericEffecterPdr(uint16_t effecterId,
                                                   bool withAuxName)
{
    pldm_numeric_effecter_value_pdr pdr{};
    pdr.hdr.record_handle = 7;
    pdr.hdr.version = 1;
    pdr.hdr.type = PLDM_NUMERIC_EFFECTER_PDR;
    pdr.hdr.record_change_num = 0;
    pdr.terminus_handle = 1;
    pdr.effecter_id = effecterId;
    pdr.entity_type = PLDM_ENTITY_SYS_BOARD;
    pdr.entity_instance = 1;
    pdr.container_id = 1;
    pdr.effecter_semantic_id = 1;
    pdr.effecter_init = PLDM_NO_INIT;
    pdr.effecter_auxiliary_names = withAuxName;
    pdr.base_unit = PLDM_SENSOR_UNIT_NONE;
    pdr.unit_modifier = 0;
    pdr.is_linear = true;
    pdr.effecter_data_size = PLDM_EFFECTER_DATA_SIZE_UINT8;
    pdr.max_settable.value_u8 = 100;
    pdr.min_settable.value_u8 = 0;
    pdr.range_field_format = PLDM_RANGE_FIELD_FORMAT_UINT8;
    pdr.range_field_support.byte = 0x1F;
    pdr.nominal_value.value_u8 = 50;
    pdr.normal_max.value_u8 = 60;
    pdr.normal_min.value_u8 = 40;
    pdr.rated_max.value_u8 = 70;
    pdr.rated_min.value_u8 = 30;
    return serializeNumericEffecterPdr(pdr);
}

static std::vector<uint8_t> makeNumericEffecterPdrVariant(
    uint16_t effecterId, uint8_t effecterDataSize, uint8_t rangeFieldFormat)
{
    pldm_numeric_effecter_value_pdr pdr{};
    pdr.hdr.record_handle = effecterId;
    pdr.hdr.version = 1;
    pdr.hdr.type = PLDM_NUMERIC_EFFECTER_PDR;
    pdr.hdr.record_change_num = 0;
    pdr.terminus_handle = 1;
    pdr.effecter_id = effecterId;
    pdr.entity_type = PLDM_ENTITY_SYS_BOARD;
    pdr.entity_instance = 1;
    pdr.container_id = 1;
    pdr.effecter_semantic_id = 1;
    pdr.effecter_init = PLDM_NO_INIT;
    pdr.effecter_auxiliary_names = false;
    pdr.base_unit = PLDM_SENSOR_UNIT_NONE;
    pdr.unit_modifier = 0;
    pdr.is_linear = true;
    pdr.effecter_data_size = effecterDataSize;

    switch (effecterDataSize)
    {
        case PLDM_EFFECTER_DATA_SIZE_UINT8:
            pdr.max_settable.value_u8 = 200;
            pdr.min_settable.value_u8 = 5;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT8:
            pdr.max_settable.value_s8 = 100;
            pdr.min_settable.value_s8 = -100;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT16:
            pdr.max_settable.value_u16 = 2000;
            pdr.min_settable.value_u16 = 10;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT16:
            pdr.max_settable.value_s16 = 1000;
            pdr.min_settable.value_s16 = -1000;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT32:
            pdr.max_settable.value_u32 = 200000;
            pdr.min_settable.value_u32 = 100;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT32:
            pdr.max_settable.value_s32 = 100000;
            pdr.min_settable.value_s32 = -100000;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT64:
            pdr.max_settable.value_u64 = 2000000;
            pdr.min_settable.value_u64 = 1000;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT64:
            pdr.max_settable.value_s64 = 1000000;
            pdr.min_settable.value_s64 = -1000000;
            break;
        default:
            break;
    }

    pdr.range_field_format = rangeFieldFormat;
    pdr.range_field_support.byte = 0x1F;
    switch (rangeFieldFormat)
    {
        case PLDM_RANGE_FIELD_FORMAT_UINT8:
            pdr.nominal_value.value_u8 = 50;
            pdr.normal_max.value_u8 = 60;
            pdr.normal_min.value_u8 = 40;
            pdr.rated_max.value_u8 = 70;
            pdr.rated_min.value_u8 = 30;
            break;
        case PLDM_RANGE_FIELD_FORMAT_SINT8:
            pdr.nominal_value.value_s8 = 10;
            pdr.normal_max.value_s8 = 20;
            pdr.normal_min.value_s8 = -20;
            pdr.rated_max.value_s8 = 30;
            pdr.rated_min.value_s8 = -30;
            break;
        case PLDM_RANGE_FIELD_FORMAT_UINT16:
            pdr.nominal_value.value_u16 = 500;
            pdr.normal_max.value_u16 = 600;
            pdr.normal_min.value_u16 = 400;
            pdr.rated_max.value_u16 = 700;
            pdr.rated_min.value_u16 = 300;
            break;
        case PLDM_RANGE_FIELD_FORMAT_SINT16:
            pdr.nominal_value.value_s16 = 100;
            pdr.normal_max.value_s16 = 200;
            pdr.normal_min.value_s16 = -200;
            pdr.rated_max.value_s16 = 300;
            pdr.rated_min.value_s16 = -300;
            break;
        case PLDM_RANGE_FIELD_FORMAT_UINT32:
            pdr.nominal_value.value_u32 = 50000;
            pdr.normal_max.value_u32 = 60000;
            pdr.normal_min.value_u32 = 40000;
            pdr.rated_max.value_u32 = 70000;
            pdr.rated_min.value_u32 = 30000;
            break;
        case PLDM_RANGE_FIELD_FORMAT_SINT32:
            pdr.nominal_value.value_s32 = 10000;
            pdr.normal_max.value_s32 = 20000;
            pdr.normal_min.value_s32 = -20000;
            pdr.rated_max.value_s32 = 30000;
            pdr.rated_min.value_s32 = -30000;
            break;
        case PLDM_RANGE_FIELD_FORMAT_REAL32:
            pdr.nominal_value.value_f32 = 12.5f;
            pdr.normal_max.value_f32 = 20.5f;
            pdr.normal_min.value_f32 = -20.5f;
            pdr.rated_max.value_f32 = 30.5f;
            pdr.rated_min.value_f32 = -30.5f;
            break;
        case PLDM_RANGE_FIELD_FORMAT_UINT64:
            pdr.nominal_value.value_u64 = 500000;
            pdr.normal_max.value_u64 = 600000;
            pdr.normal_min.value_u64 = 400000;
            pdr.rated_max.value_u64 = 700000;
            pdr.rated_min.value_u64 = 300000;
            break;
        case PLDM_RANGE_FIELD_FORMAT_SINT64:
            pdr.nominal_value.value_s64 = 100000;
            pdr.normal_max.value_s64 = 200000;
            pdr.normal_min.value_s64 = -200000;
            pdr.rated_max.value_s64 = 300000;
            pdr.rated_min.value_s64 = -300000;
            break;
        default:
            break;
    }

    return serializeNumericEffecterPdr(pdr);
}

static void appendCString(std::vector<uint8_t>& buffer, const char* value)
{
    while (*value != '\0')
    {
        buffer.push_back(static_cast<uint8_t>(*value++));
    }
    buffer.push_back(0);
}

static void appendUtf16BeCString(std::vector<uint8_t>& buffer,
                                 const char* value)
{
    while (*value != '\0')
    {
        buffer.push_back(0);
        buffer.push_back(static_cast<uint8_t>(*value++));
    }
    buffer.push_back(0);
    buffer.push_back(0);
}

static void updatePdrLength(std::vector<uint8_t>& pdr)
{
    auto* hdr = reinterpret_cast<pldm_pdr_hdr*>(pdr.data());
    hdr->length = pdr.size() - sizeof(pldm_pdr_hdr);
}

static std::vector<uint8_t> makeOemPdr()
{
    std::vector<uint8_t> pdr(sizeof(pldm_oem_pdr) + 3, 0);
    auto* oem = reinterpret_cast<pldm_oem_pdr*>(pdr.data());
    oem->hdr.record_handle = 8;
    oem->hdr.version = 1;
    oem->hdr.type = PLDM_OEM_PDR;
    oem->hdr.record_change_num = 0;
    oem->hdr.length = pdr.size() - sizeof(pldm_pdr_hdr);
    oem->vendor_iana = 0x1234;
    oem->ome_record_id = 1;
    oem->data_length = 3;
    oem->vendor_specific_data[0] = 0xAA;
    oem->vendor_specific_data[1] = 0xBB;
    oem->vendor_specific_data[2] = 0xCC;
    return pdr;
}

static std::vector<uint8_t> makeNvidiaEnergyCountOemPdr(uint16_t sensorId)
{
    pldm_oem_energycount_numeric_sensor_value_pdr energyPdr{};
    energyPdr.terminus_handle = 1;
    energyPdr.nvidia_oem_pdr_type = 3; // NVIDIA_OEM_PDR_TYPE_SENSOR_ENERGYCOUNT
    energyPdr.sensor_id = sensorId;
    energyPdr.entity_type = PLDM_ENTITY_POWER_SUPPLY;
    energyPdr.entity_instance_num = 1;
    energyPdr.container_id = 1;
    energyPdr.sensor_auxiliary_names_pdr = false;
    energyPdr.base_unit = PLDM_SENSOR_UNIT_WATTS;
    energyPdr.unit_modifier = 0;
    energyPdr.sensor_data_size = PLDM_SENSOR_DATA_SIZE_UINT16;
    energyPdr.update_interval = 1.0f;
    energyPdr.max_readable.value_u16 = 1000;
    energyPdr.min_readable.value_u16 = 1;

    std::vector<uint8_t> pdr(sizeof(pldm_oem_pdr) + sizeof(energyPdr) - 1, 0);
    auto* oem = reinterpret_cast<pldm_oem_pdr*>(pdr.data());
    oem->hdr.record_handle = 9;
    oem->hdr.version = 1;
    oem->hdr.type = PLDM_OEM_PDR;
    oem->hdr.record_change_num = 0;
    oem->hdr.length = pdr.size() - sizeof(pldm_pdr_hdr);
    oem->vendor_iana = 0x1647;
    oem->ome_record_id = 1;
    oem->data_length = sizeof(energyPdr) - 1;
    memcpy(oem->vendor_specific_data, &energyPdr, sizeof(energyPdr));
    return pdr;
}

static std::vector<uint8_t> makeZeroLengthOemPdr()
{
    std::vector<uint8_t> pdr(sizeof(pldm_oem_pdr), 0);
    auto* oem = reinterpret_cast<pldm_oem_pdr*>(pdr.data());
    oem->hdr.record_handle = 10;
    oem->hdr.version = 1;
    oem->hdr.type = PLDM_OEM_PDR;
    oem->hdr.record_change_num = 0;
    oem->hdr.length = pdr.size() - sizeof(pldm_pdr_hdr);
    oem->vendor_iana = 0x4321;
    oem->ome_record_id = 2;
    oem->data_length = 0;
    return pdr;
}

static std::vector<uint8_t> makeSensorAuxNamePdr(uint16_t sensorId)
{
    std::vector<uint8_t> pdr{
        0x0,
        0x0,
        0x0,
        0x1,                             // record handle
        0x1,                             // PDRHeaderVersion
        PLDM_SENSOR_AUXILIARY_NAMES_PDR, // PDRType
        0x0,
        0x0,                             // recordChangeNumber
        0x9,
        0x0,                             // dataLength
        0x0,
        0x0,                             // terminus handle
        static_cast<uint8_t>(sensorId & 0xFF),
        static_cast<uint8_t>((sensorId >> 8) & 0xFF),
        0x1, // sensor count
        0x1, // name string count
        'e',
        'n',
        0x0, // language tag
        0x0,
        'S',
        0x0,
        0x0 // UTF16-BE "S"
    };
    return pdr;
}

static std::vector<uint8_t> makeZeroCountSensorAuxNamePdr(uint16_t sensorId)
{
    std::vector<uint8_t> pdr{
        0x0,
        0x0,
        0x0,
        0x23,                            // record handle
        0x1,                             // PDRHeaderVersion
        PLDM_SENSOR_AUXILIARY_NAMES_PDR, // PDRType
        0x0,
        0x0,                             // recordChangeNumber
        0x0,
        0x0,                             // dataLength
        0x0,
        0x0,                             // terminus handle
        static_cast<uint8_t>(sensorId & 0xFF),
        static_cast<uint8_t>((sensorId >> 8) & 0xFF),
        0x0 // sensor count
    };
    updatePdrLength(pdr);
    return pdr;
}

static std::vector<uint8_t> makeEmptyCompositeSensorAuxNamePdr(
    uint16_t sensorId)
{
    std::vector<uint8_t> pdr{
        0x0,
        0x0,
        0x0,
        0x24,                            // record handle
        0x1,                             // PDRHeaderVersion
        PLDM_SENSOR_AUXILIARY_NAMES_PDR, // PDRType
        0x0,
        0x0,                             // recordChangeNumber
        0x0,
        0x0,                             // dataLength
        0x0,
        0x0,                             // terminus handle
        static_cast<uint8_t>(sensorId & 0xFF),
        static_cast<uint8_t>((sensorId >> 8) & 0xFF),
        0x2 // sensor count
    };

    pdr.push_back(0x0);
    pdr.push_back(0x1);
    appendCString(pdr, "en");
    appendUtf16BeCString(pdr, "DIMM1");

    updatePdrLength(pdr);
    return pdr;
}

static std::vector<uint8_t> makeMultiSensorAuxNamePdr(uint16_t sensorId)
{
    std::vector<uint8_t> pdr{
        0x0,
        0x0,
        0x0,
        0x21,                            // record handle
        0x1,                             // PDRHeaderVersion
        PLDM_SENSOR_AUXILIARY_NAMES_PDR, // PDRType
        0x0,
        0x0,                             // recordChangeNumber
        0x0,
        0x0,                             // dataLength
        0x0,
        0x0,                             // terminus handle
        static_cast<uint8_t>(sensorId & 0xFF),
        static_cast<uint8_t>((sensorId >> 8) & 0xFF),
        0x2 // sensor count
    };

    pdr.push_back(0x2);
    appendCString(pdr, "en");
    appendUtf16BeCString(pdr, "CPU0");
    appendCString(pdr, "fr");
    appendUtf16BeCString(pdr, "UC0");

    pdr.push_back(0x1);
    appendCString(pdr, "en");
    appendUtf16BeCString(pdr, "DIMM0");

    updatePdrLength(pdr);
    return pdr;
}

static std::vector<uint8_t> makeInvalidSensorAuxNamePdr(uint16_t sensorId)
{
    auto pdr = makeSensorAuxNamePdr(sensorId);
    pdr[pdr.size() - 4] = 0xDC;
    pdr[pdr.size() - 3] = 0x00;
    pdr[pdr.size() - 2] = 0x00;
    pdr[pdr.size() - 1] = 0x00;
    return pdr;
}

static std::vector<uint8_t> makeMultiEffecterAuxNamePdr(uint16_t effecterId)
{
    std::vector<uint8_t> pdr{
        0x0,
        0x0,
        0x0,
        0x22,                              // record handle
        0x1,                               // PDRHeaderVersion
        PLDM_EFFECTER_AUXILIARY_NAMES_PDR, // PDRType
        0x0,
        0x0,                               // recordChangeNumber
        0x0,
        0x0,                               // dataLength
        0x0,
        0x0,                               // terminus handle
        static_cast<uint8_t>(effecterId & 0xFF),
        static_cast<uint8_t>((effecterId >> 8) & 0xFF),
        0x2 // effecter count
    };

    pdr.push_back(0x2);
    appendCString(pdr, "en");
    appendUtf16BeCString(pdr, "PowerCap");
    appendCString(pdr, "es");
    appendUtf16BeCString(pdr, "Potencia");

    pdr.push_back(0x1);
    appendCString(pdr, "en");
    appendUtf16BeCString(pdr, "Throttle");

    updatePdrLength(pdr);
    return pdr;
}

static std::vector<uint8_t> makeInvalidEffecterAuxNamePdr(uint16_t effecterId)
{
    auto pdr = makeAuxNamePdr(effecterId, PLDM_EFFECTER_AUXILIARY_NAMES_PDR);
    pdr[pdr.size() - 4] = 0xDC;
    pdr[pdr.size() - 3] = 0x00;
    pdr[pdr.size() - 2] = 0x00;
    pdr[pdr.size() - 1] = 0x00;
    return pdr;
}

static std::vector<uint8_t> makeZeroCountEffecterAuxNamePdr(uint16_t effecterId)
{
    std::vector<uint8_t> pdr{
        0x0,
        0x0,
        0x0,
        0x25,                              // record handle
        0x1,                               // PDRHeaderVersion
        PLDM_EFFECTER_AUXILIARY_NAMES_PDR, // PDRType
        0x0,
        0x0,                               // recordChangeNumber
        0x0,
        0x0,                               // dataLength
        0x0,
        0x0,                               // terminus handle
        static_cast<uint8_t>(effecterId & 0xFF),
        static_cast<uint8_t>((effecterId >> 8) & 0xFF),
        0x0 // effecter count
    };
    updatePdrLength(pdr);
    return pdr;
}

static std::vector<uint8_t> makeEmptyCompositeEffecterAuxNamePdr(
    uint16_t effecterId)
{
    std::vector<uint8_t> pdr{
        0x0,
        0x0,
        0x0,
        0x26,                              // record handle
        0x1,                               // PDRHeaderVersion
        PLDM_EFFECTER_AUXILIARY_NAMES_PDR, // PDRType
        0x0,
        0x0,                               // recordChangeNumber
        0x0,
        0x0,                               // dataLength
        0x0,
        0x0,                               // terminus handle
        static_cast<uint8_t>(effecterId & 0xFF),
        static_cast<uint8_t>((effecterId >> 8) & 0xFF),
        0x2 // effecter count
    };

    pdr.push_back(0x0);
    pdr.push_back(0x1);
    appendCString(pdr, "en");
    appendUtf16BeCString(pdr, "Throttle");

    updatePdrLength(pdr);
    return pdr;
}

static std::vector<uint8_t> makeStateSensorPdr(
    uint16_t sensorId, uint16_t entityType, uint16_t stateSetId,
    bool hasAuxNames)
{
    return {
        0x0,
        0x0,
        0x0,
        0x1, // record handle
        0x1, // PDRHeaderVersion
        PLDM_STATE_SENSOR_PDR,
        0x0,
        0x0,  // recordChangeNumber
        0x0,
        0x11, // dataLength
        0,
        0,    // PLDMTerminusHandle
        static_cast<uint8_t>(sensorId & 0xFF),
        static_cast<uint8_t>((sensorId >> 8) & 0xFF),
        static_cast<uint8_t>(entityType & 0xFF),
        static_cast<uint8_t>((entityType >> 8) & 0xFF),
        1,
        0,                                 // entityInstance
        0x1,
        0x0,                               // containerID
        PLDM_NO_INIT,
        static_cast<uint8_t>(hasAuxNames), // sensorAuxiliaryNamesPDR
        1,                                 // compositeSensorCount
        static_cast<uint8_t>(stateSetId & 0xFF),
        static_cast<uint8_t>((stateSetId >> 8) & 0xFF),
        0x1, // possibleStatesSize
        0x3  // possibleStates
    };
}

static std::vector<uint8_t> makeStateEffecterPdr(
    uint16_t effecterId, uint16_t entityType, uint16_t stateSetId)
{
    return {
        0x0,
        0x0,
        0x0,
        0x1, // record handle
        0x1, // PDRHeaderVersion
        PLDM_STATE_EFFECTER_PDR,
        0x0,
        0x0,  // recordChangeNumber
        0x0,
        0x13, // dataLength
        0,
        0,    // PLDMTerminusHandle
        static_cast<uint8_t>(effecterId & 0xFF),
        static_cast<uint8_t>((effecterId >> 8) & 0xFF),
        static_cast<uint8_t>(entityType & 0xFF),
        static_cast<uint8_t>((entityType >> 8) & 0xFF),
        1,
        0,     // entityInstance
        0x1,
        0x0,   // containerID
        0x0,
        0x0,   // effecterSemanticID
        PLDM_NO_INIT,
        false, // effecterDescriptionPDR
        1,     // compositeEffecterCount
        static_cast<uint8_t>(stateSetId & 0xFF),
        static_cast<uint8_t>((stateSetId >> 8) & 0xFF),
        0x1, // possibleStatesSize
        0x7  // possibleStates
    };
}

static std::shared_ptr<pldm_numeric_sensor_value_pdr>
    makeNumericSensorValuePdrStruct(
        uint16_t sensorId, uint16_t entityType = PLDM_ENTITY_SYS_BOARD,
        uint16_t entityInstance = 1, uint16_t containerId = 1)
{
    auto pdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    pdr->sensor_id = sensorId;
    pdr->entity_type = entityType;
    pdr->entity_instance_num = entityInstance;
    pdr->container_id = containerId;
    pdr->base_unit = PLDM_SENSOR_UNIT_DEGRESS_C;
    pdr->sensor_data_size = PLDM_SENSOR_DATA_SIZE_UINT8;
    pdr->max_readable.value_u8 = 100;
    pdr->min_readable.value_u8 = 0;
    pdr->hysteresis.value_u8 = 1;
    pdr->supported_thresholds.byte = 0;
    pdr->range_field_support.byte = 0;
    pdr->range_field_format = PLDM_RANGE_FIELD_FORMAT_UINT8;
    pdr->resolution = 1.0f;
    pdr->offset = 0.0f;
    pdr->update_interval = 1.0f;
    return pdr;
}

static std::shared_ptr<pldm_numeric_effecter_value_pdr>
    makeNumericEffecterValuePdrStruct(
        uint16_t effecterId, uint16_t entityType = PLDM_ENTITY_SYS_BOARD,
        uint16_t entityInstance = 1, uint16_t containerId = 1)
{
    auto pdr = std::make_shared<pldm_numeric_effecter_value_pdr>();
    pdr->effecter_id = effecterId;
    pdr->entity_type = entityType;
    pdr->entity_instance = entityInstance;
    pdr->container_id = containerId;
    pdr->base_unit = PLDM_SENSOR_UNIT_NONE;
    pdr->effecter_data_size = PLDM_EFFECTER_DATA_SIZE_UINT8;
    pdr->max_settable.value_u8 = 100;
    pdr->min_settable.value_u8 = 0;
    pdr->range_field_format = PLDM_RANGE_FIELD_FORMAT_UINT8;
    pdr->range_field_support.byte = 0x1F;
    pdr->nominal_value.value_u8 = 50;
    pdr->normal_max.value_u8 = 60;
    pdr->normal_min.value_u8 = 40;
    pdr->rated_max.value_u8 = 70;
    pdr->rated_min.value_u8 = 30;
    return pdr;
}

static StateSetInfo makeSimpleStateSetInfo()
{
    StateSetData healthStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_HEALTHSTATE),
                        PossibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                       PLDM_STATESET_HEALTH_STATE_CRITICAL});
    return std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
                           std::vector<StateSetData>{healthStateData});
}

template <typename T>
static std::vector<uint8_t> structToBytes(const T& input)
{
    std::vector<uint8_t> bytes(sizeof(T), 0);
    memcpy(bytes.data(), &input, sizeof(T));
    return bytes;
}

static std::vector<uint8_t> makeGetNumericEffecterValueResp(
    uint8_t effecterDataSize, pldm_effecter_oper_state operState,
    uint8_t completionCode = PLDM_SUCCESS)
{
    union_effecter_data_size pending{};
    union_effecter_data_size present{};
    size_t payloadLen = PLDM_GET_NUMERIC_EFFECTER_VALUE_MIN_RESP_BYTES;

    switch (effecterDataSize)
    {
        case PLDM_EFFECTER_DATA_SIZE_UINT8:
            pending.value_u8 = 10;
            present.value_u8 = 20;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT8:
            pending.value_s8 = -10;
            present.value_s8 = 20;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT16:
            pending.value_u16 = 1000;
            present.value_u16 = 2000;
            payloadLen += 2;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT16:
            pending.value_s16 = -1000;
            present.value_s16 = 2000;
            payloadLen += 2;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT32:
            pending.value_u32 = 100000;
            present.value_u32 = 200000;
            payloadLen += 6;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT32:
            pending.value_s32 = -100000;
            present.value_s32 = 200000;
            payloadLen += 6;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT64:
            pending.value_u64 = 1000000;
            present.value_u64 = 2000000;
            payloadLen += 14;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT64:
        default:
            pending.value_s64 = -1000000;
            present.value_s64 = 2000000;
            payloadLen += 14;
            break;
    }

    std::vector<uint8_t> response(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* responseMsg = reinterpret_cast<pldm_msg*>(response.data());
    auto rc = encode_get_numeric_effecter_value_resp(
        0, completionCode, effecterDataSize, operState,
        reinterpret_cast<uint8_t*>(&pending),
        reinterpret_cast<uint8_t*>(&present), responseMsg, payloadLen);
    EXPECT_EQ(PLDM_SUCCESS, rc);
    return response;
}

static std::vector<uint8_t> makeSetNumericEffecterValueResp(
    uint8_t completionCode = PLDM_SUCCESS)
{
    std::vector<uint8_t> response(
        sizeof(pldm_msg_hdr) + PLDM_SET_NUMERIC_EFFECTER_VALUE_RESP_BYTES, 0);
    auto* responseMsg = reinterpret_cast<pldm_msg*>(response.data());
    auto rc = encode_set_numeric_effecter_value_resp(
        0, completionCode, responseMsg,
        PLDM_SET_NUMERIC_EFFECTER_VALUE_RESP_BYTES);
    EXPECT_EQ(PLDM_SUCCESS, rc);
    return response;
}

static StateSetInfo makeSingleStateSetInfo(EntityType entityType,
                                           uint16_t stateSetId)
{
    StateSetData stateData =
        std::make_tuple(stateSetId, PossibleStates{0, 1, 2, 3, 4, 5, 6, 7});
    return std::make_tuple(EntityInfo{1, entityType, 1},
                           std::vector<StateSetData>{stateData});
}

static StateSetInfo makeDebugStateSetInfo(EntityType entityType,
                                          size_t compositeCount = 6)
{
    std::vector<StateSetData> stateSets;
    stateSets.reserve(compositeCount);
    for (size_t i = 0; i < compositeCount; ++i)
    {
        stateSets.emplace_back(
            static_cast<uint16_t>(PLDM_NVIDIA_OEM_STATE_SET_DEBUG_STATE),
            PossibleStates{PLDM_STATE_SET_DEBUG_STATE_DISABLED,
                           PLDM_STATE_SET_DEBUG_STATE_ENABLED,
                           PLDM_STATE_SET_DEBUG_STATE_OFFLINE});
    }
    return std::make_tuple(EntityInfo{1, entityType, 1}, stateSets);
}

template <typename T>
exec::task<T> awaitDbusPropertyForCoverage(const std::string& objectPath,
                                           const std::string& property,
                                           const std::string& interface)
{
    co_return co_await pldm::utils::coGetDbusProperty<T>(objectPath, property,
                                                         interface);
}

TEST_F(TerminusTest, directCoGetDbusPropertySuccessCoverage)
{
    AsyncEntityManagerServer entityManager;
    const std::string objectPath =
        "/xyz/openbmc_project/inventory/system/chassis/" + std::string(56, 'p');
    const std::string interface =
        "xyz.openbmc_project.Configuration.TerminusCoverage." +
        std::string(24, 'I');
    const std::string name = "TerminusCoverageName" + std::string(72, 'N');
    const std::vector<std::string> parents{
        "/xyz/openbmc_project/inventory/system/" + std::string(40, 'a'),
        "/xyz/openbmc_project/inventory/system/" + std::string(44, 'b')};

    entityManager.addInterface(
        objectPath, interface,
        {{"Name", name}, {"Bus", uint64_t{73}}, {"Parents", parents}});

    auto nameRc = syncWaitWithDbusIo(awaitDbusPropertyForCoverage<std::string>(
        objectPath, "Name", interface));
    ASSERT_TRUE(nameRc.has_value());
    EXPECT_EQ(name, std::get<0>(*nameRc));

    auto busRc = syncWaitWithDbusIo(
        awaitDbusPropertyForCoverage<uint64_t>(objectPath, "Bus", interface));
    ASSERT_TRUE(busRc.has_value());
    EXPECT_EQ(uint64_t{73}, std::get<0>(*busRc));

    auto parentsRc = syncWaitWithDbusIo(
        awaitDbusPropertyForCoverage<std::vector<std::string>>(
            objectPath, "Parents", interface));
    ASSERT_TRUE(parentsRc.has_value());
    EXPECT_EQ(parents, std::get<0>(*parentsRc));
}

TEST_F(TerminusTest, directCoGetDbusPropertyErrorCoverage)
{
    AsyncEntityManagerServer entityManager;
    const std::string objectPath =
        "/xyz/openbmc_project/inventory/system/chassis/" + std::string(48, 'e');
    const std::string interface =
        "xyz.openbmc_project.Configuration.TerminusCoverage." +
        std::string(20, 'E');

    entityManager.addInterface(objectPath, interface,
                               {{"Name", std::string("existing_property")},
                                {"Bus", uint64_t{91}},
                                {"Parents", std::vector<std::string>{}}});

    auto missingNameRc =
        syncWaitWithDbusIo(awaitDbusPropertyForCoverage<std::string>(
            objectPath, "MissingName", interface));
    ASSERT_TRUE(missingNameRc.has_value());
    EXPECT_TRUE(std::get<0>(*missingNameRc).empty());

    auto missingBusRc = syncWaitWithDbusIo(
        awaitDbusPropertyForCoverage<uint64_t>(objectPath, "MissingBus",
                                               interface));
    ASSERT_TRUE(missingBusRc.has_value());
    EXPECT_EQ(uint64_t{0}, std::get<0>(*missingBusRc));

    auto missingParentsRc = syncWaitWithDbusIo(
        awaitDbusPropertyForCoverage<std::vector<std::string>>(
            objectPath, "MissingParents", interface));
    ASSERT_TRUE(missingParentsRc.has_value());
    EXPECT_TRUE(std::get<0>(*missingParentsRc).empty());
}

TEST_F(TerminusTest, directCoGetDbusPropertyMissingObjectCoverage)
{
    AsyncEntityManagerServer entityManager;
    const std::string objectPath =
        "/xyz/openbmc_project/inventory/system/chassis/" + std::string(40, 'm');
    const std::string interface =
        "xyz.openbmc_project.Configuration.TerminusCoverage." +
        std::string(18, 'M');

    const std::string missingObjectPath = objectPath + "/missing";

    entityManager.addInterface(
        objectPath, interface,
        {{"Name", std::string("existing_value")},
         {"Bus", uint64_t{55}},
         {"Parents",
          std::vector<std::string>{"/xyz/openbmc_project/inventory/system/" +
                                   std::string(32, 'p')}}});

    auto missingNameRc =
        syncWaitWithDbusIo(awaitDbusPropertyForCoverage<std::string>(
            missingObjectPath, "Name", interface));
    ASSERT_TRUE(missingNameRc.has_value());
    EXPECT_TRUE(std::get<0>(*missingNameRc).empty());

    auto missingBusRc = syncWaitWithDbusIo(
        awaitDbusPropertyForCoverage<uint64_t>(missingObjectPath, "Bus",
                                               interface));
    ASSERT_TRUE(missingBusRc.has_value());
    EXPECT_EQ(uint64_t{0}, std::get<0>(*missingBusRc));
}

TEST_F(TerminusTest, checkDeviceInventoryReturnsSuccessWithoutAssociations)
{
    std::string uuid("00000000-0000-0000-0000-000000000210");
    Terminus terminus(0x21, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    auto rc = syncWaitWithDbusIo(terminus.checkDeviceInventory(
        "/xyz/openbmc_project/inventory/system/chassis/cov210"));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
}

TEST_F(TerminusTest, checkDeviceInventoryReturnsFailedWithoutMappedMctpInfo)
{
    AsyncEntityManagerServer entityManager;
    std::string uuid("00000000-0000-0000-0000-000000000211");
    Terminus terminus(0x22, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov211";
    entityManager.addInterface(
        rootPath + "/usb_assoc",
        "xyz.openbmc_project.Configuration.USBDeviceAssociation",
        {{"EID", uint64_t{0x41}}});

    auto rc = syncWaitWithDbusIo(terminus.checkDeviceInventory(rootPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_FAILED, std::get<0>(*rc));
}

TEST_F(TerminusTest, checkDeviceInventoryUsbAssociationCoverage)
{
    AsyncEntityManagerServer entityManager;
    std::string uuid("00000000-0000-0000-0000-000000000212");
    Terminus terminus(0x23, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const pldm::MctpInfo mctpInfo(0x42, uuid, "smbus", 1, std::nullopt,
                                  "mctp-over-smbus", std::nullopt);
    ASSERT_EQ(terminusManager.mapTid(mctpInfo, terminus.getTid()),
              terminus.getTid());

    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov212";
    entityManager.addInterface(
        rootPath + "/usb_assoc",
        "xyz.openbmc_project.Configuration.USBDeviceAssociation",
        {{"EID", uint64_t{0x42}}});
    entityManager.addInterface(
        rootPath + "/aux0", "xyz.openbmc_project.Configuration.SensorAuxName",
        {{"SensorId", uint64_t{0x5500}},
         {"AuxNames", std::vector<std::string>{"Port 0", "Port 1"}},
         {"ParentObjPath", rootPath},
         {"EID", uint64_t{0x42}}});

    auto rc = syncWaitWithDbusIo(terminus.checkDeviceInventory(rootPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
}

TEST_F(TerminusTest, checkDeviceInventoryNsmAssociationCoverage)
{
    AsyncEntityManagerServer entityManager;
    std::string uuid("00000000-0000-0000-0000-000000000213");
    Terminus terminus(0x24, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const pldm::MctpInfo mctpInfo(0x43, uuid, "usb", 1, std::nullopt,
                                  "mctp-over-usb", std::nullopt);
    ASSERT_EQ(terminusManager.mapTid(mctpInfo, terminus.getTid()),
              terminus.getTid());

    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov213";
    entityManager.addInterface(
        rootPath + "/nsm_assoc",
        "xyz.openbmc_project.Configuration.NsmDeviceAssociation",
        {{"UUID", uuid}});

    auto rc = syncWaitWithDbusIo(terminus.checkDeviceInventory(rootPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
}

TEST_F(TerminusTest, checkDeviceInventoryI2cAssociationCoverage)
{
    AsyncEntityManagerServer entityManager;
    std::string uuid("00000000-0000-0000-0000-000000000215");
    Terminus terminus(0x26, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const pldm::MctpInfo mctpInfo(0x46, uuid, "smbus", 1, std::nullopt,
                                  "mctp-over-smbus", std::nullopt);
    ASSERT_EQ(terminusManager.mapTid(mctpInfo, terminus.getTid()),
              terminus.getTid());

    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov215";
    entityManager.addInterface(
        rootPath + "/i2c_assoc",
        "xyz.openbmc_project.Configuration.I2CDeviceAssociation",
        {{"Bus", uint64_t{91}},
         {"Address", uint64_t{0x56}},
         {"EID", uint64_t{0x46}}});

    auto rc = syncWaitWithDbusIo(terminus.checkDeviceInventory(rootPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
}

TEST_F(TerminusTest, getSensorAuxNameFromEMI2cFilterCoverage)
{
    AsyncEntityManagerServer entityManager;
    std::string uuid("00000000-0000-0000-0000-000000000215a");
    Terminus terminus(0x26, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov215a";

    entityManager.addInterface(
        rootPath + "/aux_match",
        "xyz.openbmc_project.Configuration.SensorAuxName",
        {{"SensorId", uint64_t{0x5600}},
         {"AuxNames", std::vector<std::string>{"Port 0"}},
         {"ParentObjPath", rootPath},
         {"Bus", uint64_t{91}},
         {"Address", uint64_t{0x56}},
         {"EID", uint64_t{0x46}}});
    entityManager.addInterface(
        rootPath + "/aux_bus_mismatch",
        "xyz.openbmc_project.Configuration.SensorAuxName",
        {{"SensorId", uint64_t{0x5601}},
         {"AuxNames", std::vector<std::string>{"WrongBus"}},
         {"ParentObjPath", rootPath},
         {"Bus", uint64_t{92}}});
    entityManager.addInterface(
        rootPath + "/aux_addr_mismatch",
        "xyz.openbmc_project.Configuration.SensorAuxName",
        {{"SensorId", uint64_t{0x5602}},
         {"AuxNames", std::vector<std::string>{"WrongAddress"}},
         {"ParentObjPath", rootPath},
         {"Address", uint64_t{0x57}}});
    entityManager.addInterface(
        rootPath + "/aux_eid_mismatch",
        "xyz.openbmc_project.Configuration.SensorAuxName",
        {{"SensorId", uint64_t{0x5603}},
         {"AuxNames", std::vector<std::string>{"WrongEid"}},
         {"ParentObjPath", rootPath},
         {"EID", uint64_t{0x47}}});
    entityManager.addInterface(
        rootPath + "/aux_no_filter",
        "xyz.openbmc_project.Configuration.SensorAuxName",
        {{"SensorId", uint64_t{0x5604}},
         {"AuxNames", std::vector<std::string>{"NoFilter"}},
         {"ParentObjPath", rootPath}});

    auto rc = syncWaitWithDbusIo(
        terminus.getSensorAuxNameFromEM(91, 0x56, 0x46, rootPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
    EXPECT_TRUE(terminus.sensorAuxNameOverwriteTbl.contains(0x5600));
    EXPECT_FALSE(terminus.sensorAuxNameOverwriteTbl.contains(0x5601));
    EXPECT_FALSE(terminus.sensorAuxNameOverwriteTbl.contains(0x5602));
    EXPECT_FALSE(terminus.sensorAuxNameOverwriteTbl.contains(0x5603));
    EXPECT_TRUE(terminus.sensorAuxNameOverwriteTbl.contains(0x5604));
}

TEST_F(TerminusTest,
       checkDeviceInventoryReturnsFailedWhenAssociationDoesNotMatch)
{
    AsyncEntityManagerServer entityManager;
    std::string uuid("00000000-0000-0000-0000-000000000214");
    Terminus terminus(0x25, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const pldm::MctpInfo mctpInfo(0x44, uuid, "usb", 1, std::nullopt,
                                  "mctp-over-usb", std::nullopt);
    ASSERT_EQ(terminusManager.mapTid(mctpInfo, terminus.getTid()),
              terminus.getTid());

    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov214";
    entityManager.addInterface(
        rootPath + "/usb_assoc",
        "xyz.openbmc_project.Configuration.USBDeviceAssociation",
        {{"EID", uint64_t{0x45}}});

    auto rc = syncWaitWithDbusIo(terminus.checkDeviceInventory(rootPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_FAILED, std::get<0>(*rc));
}

TEST_F(TerminusTest,
       getSensorAuxNameFromEMSkipsEntriesMissingMandatoryPropertiesCoverage)
{
    AsyncEntityManagerServer entityManager;
    std::string uuid("00000000-0000-0000-0000-000000000216");
    Terminus terminus(0x27, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov216";

    entityManager.addInterface(
        rootPath + "/missing_sensor_id",
        "xyz.openbmc_project.Configuration.SensorAuxName",
        {{"AuxNames", std::vector<std::string>{"MissingSensorId"}},
         {"ParentObjPath", rootPath}});
    entityManager.addInterface(
        rootPath + "/missing_aux_names",
        "xyz.openbmc_project.Configuration.SensorAuxName",
        {{"SensorId", uint64_t{0x5611}}, {"ParentObjPath", rootPath}});
    entityManager.addInterface(
        rootPath + "/valid", "xyz.openbmc_project.Configuration.SensorAuxName",
        {{"SensorId", uint64_t{0x5612}},
         {"AuxNames", std::vector<std::string>{"ValidAux"}},
         {"ParentObjPath", rootPath}});

    auto rc =
        syncWaitWithDbusIo(terminus.getSensorAuxNameFromEM(0, 0, 0, rootPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
    EXPECT_FALSE(terminus.sensorAuxNameOverwriteTbl.contains(0x5611));
    EXPECT_TRUE(terminus.sensorAuxNameOverwriteTbl.contains(0x5612));
}

TEST_F(TerminusTest, getPortInfoFromEMRejectsMalformedAssociationCoverage)
{
    AsyncEntityManagerServer entityManager;
    std::string uuid("00000000-0000-0000-0000-000000000218");
    Terminus terminus(0x29, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov218";

    entityManager.addInterface(
        rootPath + "/port_bad",
        "xyz.openbmc_project.Configuration.SensorPortInfo",
        {{"SensorId", uint64_t{0x5701}},
         {"MaxSpeedMBps", uint64_t{16000}},
         {"PortType", std::string("xyz.openbmc_project.Inventory.Item.Port."
                                  "PortType.BidirectionalPort")},
         {"PortProtocol",
          std::string("xyz.openbmc_project.Inventory.Item.Port.PortProtocol."
                      "NVLink")},
         {"Association", std::vector<std::string>{"only", "two"}}});

    auto rc = syncWaitWithDbusIo(terminus.getPortInfoFromEM(rootPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_FAILED, std::get<0>(*rc));
    EXPECT_TRUE(terminus.sensorPortInfoOverwriteTbl.empty());
}

TEST_F(TerminusTest, getPortInfoFromEMReturnsFailedWithoutEntriesCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000219");
    Terminus terminus(0x2A, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    auto rc = syncWaitWithDbusIo(terminus.getPortInfoFromEM(
        "/xyz/openbmc_project/inventory/system/chassis/cov219"));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_FAILED, std::get<0>(*rc));
    EXPECT_TRUE(terminus.sensorPortInfoOverwriteTbl.empty());
}

TEST_F(TerminusTest,
       getInfoForNVSwitchFromEMEarlyReturnAndMalformedAssociationCoverage)
{
    AsyncEntityManagerServer entityManager;
    std::string uuid("00000000-0000-0000-0000-000000000219");
    Terminus terminus(0x2A, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov219";

    std::string switchType =
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet";
    std::vector<std::string> switchProtocols{};
    std::vector<pldm::dbus::PathAssociation> associations{};
    terminus.switchBandwidthSensor =
        std::make_shared<oem_nvidia::SwitchBandwidthSensor>(
            static_cast<pldm::tid_t>(0x2A), "preexisting", switchType,
            switchProtocols, associations);
    auto existingSensor = terminus.switchBandwidthSensor;

    auto earlyRc = syncWaitWithDbusIo(
        terminus.getInfoForNVSwitchFromEM(rootPath + "/early"));
    ASSERT_TRUE(earlyRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*earlyRc));
    EXPECT_EQ(existingSensor, terminus.switchBandwidthSensor);

    terminus.switchBandwidthSensor.reset();
    entityManager.addInterface(
        rootPath + "/switch_bad",
        "xyz.openbmc_project.Configuration.NSM_NVSwitch.Switch",
        {{"Name", std::string("switch219")},
         {"SwitchType",
          std::string(
              "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet")},
         {"SwitchSupportedProtocols", std::vector<std::string>{"PCIe"}},
         {"Association", std::vector<std::string>{"bad", "assoc"}}});

    auto malformedRc =
        syncWaitWithDbusIo(terminus.getInfoForNVSwitchFromEM(rootPath));
    ASSERT_TRUE(malformedRc.has_value());
    EXPECT_EQ(PLDM_FAILED, std::get<0>(*malformedRc));
    EXPECT_EQ(nullptr, terminus.switchBandwidthSensor);
}

TEST_F(TerminusTest,
       getInfoForNVSwitchFromEMReturnsFailedWithoutEntriesCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000021A");
    Terminus terminus(0x2B, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    auto rc = syncWaitWithDbusIo(terminus.getInfoForNVSwitchFromEM(
        "/xyz/openbmc_project/inventory/system/chassis/cov21A"));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_FAILED, std::get<0>(*rc));
    EXPECT_EQ(nullptr, terminus.switchBandwidthSensor);
}

TEST_F(TerminusTest, getInventoryParentStoresFirstEndpointCoverage)
{
    AsyncEntityManagerServer entityManager;
    std::string uuid("00000000-0000-0000-0000-00000000021B");
    Terminus terminus(0x2C, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov21B";

    entityManager.addInterface(
        rootPath + "/parent_chassis", "xyz.openbmc_project.Association",
        {{"endpoints",
          std::vector<std::string>{
              "/xyz/openbmc_project/inventory/system/chassis/parent0",
              "/xyz/openbmc_project/inventory/system/chassis/parent1"}}},
        pldm::utils::mapperService);

    auto rc = syncWaitWithDbusIo(terminus.getInventoryParent(rootPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
    ASSERT_TRUE(terminus.inventoryParentMap.contains(rootPath));
    EXPECT_EQ("/xyz/openbmc_project/inventory/system/chassis/parent0",
              terminus.inventoryParentMap.at(rootPath));
}

TEST_F(TerminusTest, getInventoryParentIgnoresEmptyEndpointsCoverage)
{
    AsyncEntityManagerServer entityManager;
    std::string uuid("00000000-0000-0000-0000-00000000021C");
    Terminus terminus(0x2D, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov21C";

    entityManager.addInterface(
        rootPath + "/parent_chassis", "xyz.openbmc_project.Association",
        {{"endpoints", std::vector<std::string>{}}},
        pldm::utils::mapperService);

    auto rc = syncWaitWithDbusIo(terminus.getInventoryParent(rootPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
    EXPECT_FALSE(terminus.inventoryParentMap.contains(rootPath));
}

TEST_F(TerminusTest, supportedTypeTest)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    std::string uuid2("00000000-0000-0000-0000-000000000002");
    auto t1 = Terminus(1, 1 << PLDM_BASE, uuid1, terminusManager);
    auto t2 = Terminus(2, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid2,
                       terminusManager);

    EXPECT_EQ(true, t1.doesSupport(PLDM_BASE));
    EXPECT_EQ(false, t1.doesSupport(PLDM_PLATFORM));
    EXPECT_EQ(true, t2.doesSupport(PLDM_BASE));
    EXPECT_EQ(true, t2.doesSupport(PLDM_PLATFORM));
}

TEST_F(TerminusTest, getTidTest)
{
    const pldm::tid_t tid = 1;
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(tid, 1 << PLDM_BASE, uuid1, terminusManager);

    EXPECT_EQ(tid, t1.getTid());
}

TEST_F(TerminusTest, parseSensorAuxiliaryNamesPDRTest)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                             // record handle
        0x1,                             // PDRHeaderVersion
        PLDM_SENSOR_AUXILIARY_NAMES_PDR, // PDRType
        0x0,
        0x0,                             // recordChangeNumber
        21,
        0,                               // dataLength
        0,
        0x0,                             // PLDMTerminusHandle
        0x1,
        0x0,                             // sensorID
        0x1,                             // sensorCount
        0x1,                             // nameStringCount
        'e',
        'n',
        0x0, // nameLanguageTag
        0x0,
        'T',
        0x0,
        'E',
        0x0,
        'M',
        0x0,
        'P',
        0x0,
        '1',
        0x0,
        0x0 // sensorName
    };

    std::vector<uint8_t> pdr2{
        0x0, 0x0, 0x0,
        0x1,                             // record handle
        0x1,                             // PDRHeaderVersion
        PLDM_SENSOR_AUXILIARY_NAMES_PDR, // PDRType
        0x0,
        0x0,                             // recordChangeNumber
        21,
        0,                               // dataLength
        0,
        0x0,                             // PLDMTerminusHandle
        0x2,
        0x0,                             // sensorID
        0x2,                             // sensorCount
                                         // sensor0
        0x0,                             // nameStringCount
                                         // sensor1
        0x1,                             // nameStringCount
        'e', 'n',
        0x0,                             // nameLanguageTag
        0x0, 'T', 0x0, 'E', 0x0, 'M', 0x0, 'P', 0x0, '2', 0x0,
        0x0                              // sensorName
    };

    t1.pdrs.emplace_back(pdr1);
    t1.pdrs.emplace_back(pdr2);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);

    auto sensorAuxNames = t1.getSensorAuxiliaryNames(0);
    EXPECT_EQ(nullptr, sensorAuxNames);

    sensorAuxNames = t1.getSensorAuxiliaryNames(1);
    EXPECT_NE(nullptr, sensorAuxNames);

    const auto& [sensorId, sensorCnt, names] = *sensorAuxNames;
    EXPECT_EQ(1, sensorId);
    EXPECT_EQ(1, sensorCnt);
    EXPECT_EQ(1, names.size());
    EXPECT_EQ(1, names[0].size());
    EXPECT_EQ("en", names[0][0].first);
    EXPECT_EQ("TEMP1", names[0][0].second);

    sensorAuxNames = t1.getSensorAuxiliaryNames(2);
    EXPECT_NE(nullptr, sensorAuxNames);

    const auto& [sensorId2, sensorCnt2, names2] = *sensorAuxNames;
    EXPECT_EQ(2, sensorId2);
    EXPECT_EQ(2, sensorCnt2);
    EXPECT_EQ(2, names2.size());
    EXPECT_EQ(0, names2[0].size());
    EXPECT_EQ(1, names2[1].size());
    EXPECT_EQ("en", names2[1][0].first);
    EXPECT_EQ("TEMP2", names2[1][0].second);
}

TEST_F(TerminusTest, parseAuxiliaryNamesLookupCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000163");
    Terminus terminus(0x63, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    std::vector<uint8_t> sensorAuxPdr{
        0x0, 0x0,  0x0, 0x1,  0x1, PLDM_SENSOR_AUXILIARY_NAMES_PDR,
        0x0, 0x0,  0x0, 0x0,  0x0, 0x0,
        0x1, 0x0,  0x1, 0x1,  'f', 'r',
        0x0, 0x00, 'T', 0x00, 'E', 0x00,
        'M', 0x00, 'P', 0x00, 0x00};
    auto sensorAuxNames = terminus.parseSensorAuxiliaryNamesPDR(sensorAuxPdr);
    ASSERT_NE(nullptr, sensorAuxNames);
    terminus.sensorAuxiliaryNamesTbl.emplace_back(sensorAuxNames);
    EXPECT_EQ(std::nullopt, terminus.getAuxNameForNumericSensor(0x1));

    std::vector<uint8_t> effecterAuxPdr{
        0x0, 0x0,  0x0, 0x1,  0x1, PLDM_EFFECTER_AUXILIARY_NAMES_PDR,
        0x0, 0x0,  0x0, 0x0,  0x0, 0x0,
        0x2, 0x0,  0x1, 0x1,  'e', 'n',
        0x0, 0x00, 'E', 0x00, 'F', 0x00,
        'F', 0x00, 0x00};
    auto effecterAuxNames =
        terminus.parseEffecterAuxiliaryNamesPDR(effecterAuxPdr);
    ASSERT_NE(nullptr, effecterAuxNames);
    const auto& [effecterId, effecterCount, names] = *effecterAuxNames;
    EXPECT_EQ(0x2, effecterId);
    EXPECT_EQ(1, effecterCount);
    ASSERT_EQ(1u, names.size());
    ASSERT_EQ(1u, names[0].size());
    EXPECT_EQ("EFF", names[0][0].second);
}

TEST_F(TerminusTest, parseAuxiliaryNamesInvalidUtf16Coverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000173");
    Terminus terminus(0x73, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    auto invalidSensorAuxPdr = makeInvalidSensorAuxNamePdr(0x173);
    auto invalidEffecterAuxPdr = makeInvalidEffecterAuxNamePdr(0x273);

    auto invalidSensorAuxNames =
        terminus.parseSensorAuxiliaryNamesPDR(invalidSensorAuxPdr);
    auto invalidEffecterAuxNames =
        terminus.parseEffecterAuxiliaryNamesPDR(invalidEffecterAuxPdr);

    ASSERT_NE(nullptr, invalidSensorAuxNames);
    EXPECT_EQ(0x173, std::get<0>(*invalidSensorAuxNames));
    EXPECT_EQ(1, std::get<1>(*invalidSensorAuxNames));
    ASSERT_EQ(1u, std::get<2>(*invalidSensorAuxNames).size());
    EXPECT_TRUE(std::get<2>(*invalidSensorAuxNames)[0].empty());

    ASSERT_NE(nullptr, invalidEffecterAuxNames);
    EXPECT_EQ(0x273, std::get<0>(*invalidEffecterAuxNames));
    EXPECT_EQ(1, std::get<1>(*invalidEffecterAuxNames));
    ASSERT_EQ(1u, std::get<2>(*invalidEffecterAuxNames).size());
    EXPECT_TRUE(std::get<2>(*invalidEffecterAuxNames)[0].empty());

    terminus.pdrs.emplace_back(std::move(invalidSensorAuxPdr));
    terminus.pdrs.emplace_back(std::move(invalidEffecterAuxPdr));
    EXPECT_TRUE(terminus.parsePDRs());
    ASSERT_EQ(1u, terminus.sensorAuxiliaryNamesTbl.size());
    ASSERT_EQ(1u, terminus.effecterAuxiliaryNamesTbl.size());
    EXPECT_TRUE(
        std::get<2>(*terminus.sensorAuxiliaryNamesTbl.front())[0].empty());
    EXPECT_TRUE(
        std::get<2>(*terminus.effecterAuxiliaryNamesTbl.front())[0].empty());
}

TEST_F(TerminusTest, parseSensorAuxiliaryNamesZeroCountCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000194");
    Terminus terminus(0x94, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    auto sensorAuxNames = terminus.parseSensorAuxiliaryNamesPDR(
        makeZeroCountSensorAuxNamePdr(static_cast<uint16_t>(0x194)));
    ASSERT_NE(nullptr, sensorAuxNames);
    EXPECT_EQ(0u, std::get<1>(*sensorAuxNames));
    EXPECT_TRUE(std::get<2>(*sensorAuxNames).empty());
}

TEST_F(TerminusTest, parseEffecterAuxiliaryNamesZeroCountCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000195");
    Terminus terminus(0x95, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    auto effecterAuxNames = terminus.parseEffecterAuxiliaryNamesPDR(
        makeZeroCountEffecterAuxNamePdr(static_cast<uint16_t>(0x295)));
    ASSERT_NE(nullptr, effecterAuxNames);
    EXPECT_EQ(0u, std::get<1>(*effecterAuxNames));
    EXPECT_TRUE(std::get<2>(*effecterAuxNames).empty());
}

TEST_F(TerminusTest, parseSensorAuxiliaryNamesEmptyCompositeCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000196");
    Terminus terminus(0x96, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    auto sensorAuxNames = terminus.parseSensorAuxiliaryNamesPDR(
        makeEmptyCompositeSensorAuxNamePdr(static_cast<uint16_t>(0x196)));
    ASSERT_NE(nullptr, sensorAuxNames);
    ASSERT_EQ(2u, std::get<2>(*sensorAuxNames).size());
    EXPECT_TRUE(std::get<2>(*sensorAuxNames).front().empty());
    ASSERT_EQ(1u, std::get<2>(*sensorAuxNames).back().size());
    EXPECT_EQ("DIMM1", std::get<2>(*sensorAuxNames).back().front().second);
}

TEST_F(TerminusTest, parseEffecterAuxiliaryNamesEmptyCompositeCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000197");
    Terminus terminus(0x97, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    auto effecterAuxNames = terminus.parseEffecterAuxiliaryNamesPDR(
        makeEmptyCompositeEffecterAuxNamePdr(static_cast<uint16_t>(0x297)));
    ASSERT_NE(nullptr, effecterAuxNames);
    ASSERT_EQ(2u, std::get<2>(*effecterAuxNames).size());
    EXPECT_TRUE(std::get<2>(*effecterAuxNames).front().empty());
    ASSERT_EQ(1u, std::get<2>(*effecterAuxNames).back().size());
    EXPECT_EQ("Throttle", std::get<2>(*effecterAuxNames).back().front().second);
}

TEST_F(TerminusTest, addNumericSensorTest)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                             // record handle
        0x1,                             // PDRHeaderVersion
        PLDM_SENSOR_AUXILIARY_NAMES_PDR, // PDRType
        0x0,
        0x0,                             // recordChangeNumber
        21,
        0,                               // dataLength
        0,
        0x0,                             // PLDMTerminusHandle
        0x1,
        0x0,                             // sensorID
        0x1,                             // sensorCount
        0x1,                             // nameStringCount
        'e',
        'n',
        0x0, // nameLanguageTag
        0x0,
        'T',
        0x0,
        'E',
        0x0,
        'M',
        0x0,
        'P',
        0x0,
        '1',
        0x0,
        0x0 // sensorName
    };

    std::vector<uint8_t> pdr2{
        0x0,
        0x0,
        0x0,
        0x1,                         // record handle
        0x1,                         // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,     // PDRType
        0x0,
        0x0,                         // recordChangeNumber
        56,
        0,                           // dataLength
        0,
        0,                           // PLDMTerminusHandle
        0x1,
        0x0,                         // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                           // entityType=Power Supply(120)
        1,
        0,                           // entityInstanceNumber
        0x1,
        0x0,                         // containerID=1
        PLDM_NO_INIT,                // sensorInit
        true,                        // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,  // baseUint(2)=degrees C
        0,                           // unitModifier
        0,                           // rateUnit
        0,                           // baseOEMUnitHandle
        0,                           // auxUnit
        0,                           // auxUnitModifier
        0,                           // auxRateUnit
        0,                           // rel
        0,                           // auxOEMUnitHandle
        true,                        // isLinear
        PLDM_SENSOR_DATA_SIZE_UINT8, // sensorDataSize
        0,
        0,
        0,
        0, // resolution
        0,
        0,
        0,
        0, // offset
        0,
        0, // accuracy
        0, // plusTolerance
        0, // minusTolerance
        0, // hysteresis
        0, // supportedThresholds
        0, // thresholdAndHysteresisVolatility
        0,
        0,
        0x80,
        0x3f, // stateTransistionInterval=1.0
        0,
        0,
        0x80,
        0x3f,                          // updateInverval=1.0
        255,                           // maxReadable
        0,                             // minReadable
        PLDM_RANGE_FIELD_FORMAT_UINT8, // rangeFieldFormat
        0,                             // rangeFieldsupport
        0,                             // nominalValue
        0,                             // normalMax
        0,                             // normalMin
        0,                             // warningHigh
        0,                             // warningLow
        0,                             // criticalHigh
        0,                             // criticalLow
        0,                             // fatalHigh
        0                              // fatalLow
    };

    t1.pdrs.emplace_back(pdr1);
    t1.pdrs.emplace_back(pdr2);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());
    EXPECT_EQ(1, t1.numericSensors.size());
}

TEST_F(TerminusTest, parseNumericSensorPdrTest)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                         // record handle
        0x1,                         // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,     // PDRType
        0x0,
        0x0,                         // recordChangeNumber
        56,
        0,                           // dataLength
        0,
        0,                           // PLDMTerminusHandle
        0x1,
        0x0,                         // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                           // entityType=Power Supply(120)
        1,
        0,                           // entityInstanceNumber
        0x1,
        0x0,                         // containerID=1
        PLDM_NO_INIT,                // sensorInit
        false,                       // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,  // baseUint(2)=degrees C
        0,                           // unitModifier
        0,                           // rateUnit
        0,                           // baseOEMUnitHandle
        0,                           // auxUnit
        0,                           // auxUnitModifier
        0,                           // auxRateUnit
        0,                           // rel
        0,                           // auxOEMUnitHandle
        true,                        // isLinear
        PLDM_SENSOR_DATA_SIZE_UINT8, // sensorDataSize
        0,
        0,
        0,
        0, // resolution
        0,
        0,
        0,
        0, // offset
        0,
        0, // accuracy
        0, // plusTolerance
        0, // minusTolerance
        3, // hysteresis = 3
        0, // supportedThresholds
        0, // thresholdAndHysteresisVolatility
        0,
        0,
        0x80,
        0x3f, // stateTransistionInterval=1.0
        0,
        0,
        0x80,
        0x3f,                          // updateInverval=1.0
        255,                           // maxReadable
        0,                             // minReadable
        PLDM_RANGE_FIELD_FORMAT_UINT8, // rangeFieldFormat
        0,                             // rangeFieldsupport
        50,                            // nominalValue = 50
        60,                            // normalMax = 60
        40,                            // normalMin = 40
        70,                            // warningHigh = 70
        30,                            // warningLow = 30
        80,                            // criticalHigh = 80
        20,                            // criticalLow = 20
        90,                            // fatalHigh = 90
        10                             // fatalLow = 10
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());

    auto numericSensorPdrs = t1.numericSensorPdrs[0];
    EXPECT_EQ(1, numericSensorPdrs->sensor_id);
    EXPECT_EQ(PLDM_SENSOR_DATA_SIZE_UINT8, numericSensorPdrs->sensor_data_size);
    EXPECT_EQ(PLDM_ENTITY_POWER_SUPPLY, numericSensorPdrs->entity_type);
    EXPECT_EQ(2, numericSensorPdrs->base_unit);
    EXPECT_EQ(0.0, numericSensorPdrs->offset);
    EXPECT_EQ(3, numericSensorPdrs->hysteresis.value_u8);
    EXPECT_EQ(1.0, numericSensorPdrs->update_interval);
    EXPECT_EQ(255, numericSensorPdrs->max_readable.value_u8);
    EXPECT_EQ(0, numericSensorPdrs->min_readable.value_u8);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_UINT8,
              numericSensorPdrs->range_field_format);
    EXPECT_EQ(0, numericSensorPdrs->range_field_support.byte);
    EXPECT_EQ(50, numericSensorPdrs->nominal_value.value_u8);
    EXPECT_EQ(60, numericSensorPdrs->normal_max.value_u8);
    EXPECT_EQ(40, numericSensorPdrs->normal_min.value_u8);
    EXPECT_EQ(70, numericSensorPdrs->warning_high.value_u8);
    EXPECT_EQ(30, numericSensorPdrs->warning_low.value_u8);
    EXPECT_EQ(80, numericSensorPdrs->critical_high.value_u8);
    EXPECT_EQ(20, numericSensorPdrs->critical_low.value_u8);
    EXPECT_EQ(90, numericSensorPdrs->fatal_high.value_u8);
    EXPECT_EQ(10, numericSensorPdrs->fatal_low.value_u8);
}

TEST_F(TerminusTest, parseNumericSensorPdrSint8Test)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                           // record handle
        0x1,                           // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,       // PDRType
        0x0,
        0x0,                           // recordChangeNumber
        56,
        0,                             // dataLength
        0,
        0,                             // PLDMTerminusHandle
        0x1,
        0x0,                           // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                             // entityType=Power Supply(120)
        1,
        0,                             // entityInstanceNumber
        0x1,
        0x0,                           // containerID=1
        PLDM_NO_INIT,                  // sensorInit
        false,                         // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,    // baseUint(2)=degrees C
        0,                             // unitModifier
        0,                             // rateUnit
        0,                             // baseOEMUnitHandle
        0,                             // auxUnit
        0,                             // auxUnitModifier
        0,                             // auxRateUnit
        0,                             // rel
        0,                             // auxOEMUnitHandle
        true,                          // isLinear
        PLDM_RANGE_FIELD_FORMAT_SINT8, // sensorDataSize
        0,
        0,
        0,
        0, // resolution
        0,
        0,
        0,
        0, // offset
        0,
        0, // accuracy
        0, // plusTolerance
        0, // minusTolerance
        3, // hysteresis = 3
        0, // supportedThresholds
        0, // thresholdAndHysteresisVolatility
        0,
        0,
        0x80,
        0x3f, // stateTransistionInterval=1.0
        0,
        0,
        0x80,
        0x3f,                          // updateInverval=1.0
        0x64,                          // maxReadable = 100
        0x9c,                          // minReadable = -100
        PLDM_RANGE_FIELD_FORMAT_SINT8, // rangeFieldFormat
        0,                             // rangeFieldsupport
        0,                             // nominalValue = 0
        5,                             // normalMax = 5
        0xfb,                          // normalMin = -5
        10,                            // warningHigh = 10
        0xf6,                          // warningLow = -10
        20,                            // criticalHigh = 20
        0xec,                          // criticalLow = -20
        30,                            // fatalHigh = 30
        0xe2                           // fatalLow = -30
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());

    auto numericSensorPdrs = t1.numericSensorPdrs[0];
    EXPECT_EQ(1, numericSensorPdrs->sensor_id);
    EXPECT_EQ(PLDM_SENSOR_DATA_SIZE_SINT8, numericSensorPdrs->sensor_data_size);
    EXPECT_EQ(PLDM_ENTITY_POWER_SUPPLY, numericSensorPdrs->entity_type);
    EXPECT_EQ(2, numericSensorPdrs->base_unit);
    EXPECT_EQ(0.0, numericSensorPdrs->offset);
    EXPECT_EQ(3, numericSensorPdrs->hysteresis.value_s8);
    EXPECT_EQ(1.0, numericSensorPdrs->update_interval);
    EXPECT_EQ(100, numericSensorPdrs->max_readable.value_s8);
    EXPECT_EQ(-100, numericSensorPdrs->min_readable.value_s8);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_SINT8,
              numericSensorPdrs->range_field_format);
    EXPECT_EQ(0, numericSensorPdrs->range_field_support.byte);
    EXPECT_EQ(0, numericSensorPdrs->nominal_value.value_s8);
    EXPECT_EQ(5, numericSensorPdrs->normal_max.value_s8);
    EXPECT_EQ(-5, numericSensorPdrs->normal_min.value_s8);
    EXPECT_EQ(10, numericSensorPdrs->warning_high.value_s8);
    EXPECT_EQ(-10, numericSensorPdrs->warning_low.value_s8);
    EXPECT_EQ(20, numericSensorPdrs->critical_high.value_s8);
    EXPECT_EQ(-20, numericSensorPdrs->critical_low.value_s8);
    EXPECT_EQ(30, numericSensorPdrs->fatal_high.value_s8);
    EXPECT_EQ(-30, numericSensorPdrs->fatal_low.value_s8);
}

TEST_F(TerminusTest, parseNumericSensorPdrUint16Test)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                          // record handle
        0x1,                          // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,      // PDRType
        0x0,
        0x0,                          // recordChangeNumber
        56,
        0,                            // dataLength
        0,
        0,                            // PLDMTerminusHandle
        0x1,
        0x0,                          // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                            // entityType=Power Supply(120)
        1,
        0,                            // entityInstanceNumber
        0x1,
        0x0,                          // containerID=1
        PLDM_NO_INIT,                 // sensorInit
        false,                        // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,   // baseUint(2)=degrees C
        0,                            // unitModifier
        0,                            // rateUnit
        0,                            // baseOEMUnitHandle
        0,                            // auxUnit
        0,                            // auxUnitModifier
        0,                            // auxRateUnit
        0,                            // rel
        0,                            // auxOEMUnitHandle
        true,                         // isLinear
        PLDM_SENSOR_DATA_SIZE_UINT16, // sensorDataSize
        0,
        0,
        0,
        0, // resolution
        0,
        0,
        0,
        0, // offset
        0,
        0, // accuracy
        0, // plusTolerance
        0, // minusTolerance
        3,
        0, // hysteresis = 3
        0, // supportedThresholds
        0, // thresholdAndHysteresisVolatility
        0,
        0,
        0x80,
        0x3f, // stateTransistionInterval=1.0
        0,
        0,
        0x80,
        0x3f,                           // updateInverval=1.0
        0,
        0x10,                           // maxReadable = 4096
        0,
        0,                              // minReadable = 0
        PLDM_RANGE_FIELD_FORMAT_UINT16, // rangeFieldFormat
        0,                              // rangeFieldsupport
        0x88,
        0x13,                           // nominalValue = 5,000
        0x70,
        0x17,                           // normalMax = 6,000
        0xa0,
        0x0f,                           // normalMin = 4,000
        0x58,
        0x1b,                           // warningHigh = 7,000
        0xb8,
        0x0b,                           // warningLow = 3,000
        0x40,
        0x1f,                           // criticalHigh = 8,000
        0xd0,
        0x07,                           // criticalLow = 2,000
        0x28,
        0x23,                           // fatalHigh = 9,000
        0xe8,
        0x03                            // fatalLow = 1,000
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());

    auto numericSensorPdrs = t1.numericSensorPdrs[0];
    EXPECT_EQ(1, numericSensorPdrs->sensor_id);
    EXPECT_EQ(PLDM_SENSOR_DATA_SIZE_UINT16,
              numericSensorPdrs->sensor_data_size);
    EXPECT_EQ(PLDM_ENTITY_POWER_SUPPLY, numericSensorPdrs->entity_type);
    EXPECT_EQ(2, numericSensorPdrs->base_unit);
    EXPECT_EQ(0.0, numericSensorPdrs->offset);
    EXPECT_EQ(3, numericSensorPdrs->hysteresis.value_u16);
    EXPECT_EQ(1.0, numericSensorPdrs->update_interval);
    EXPECT_EQ(4096, numericSensorPdrs->max_readable.value_u16);
    EXPECT_EQ(0, numericSensorPdrs->min_readable.value_u16);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_UINT16,
              numericSensorPdrs->range_field_format);
    EXPECT_EQ(0, numericSensorPdrs->range_field_support.byte);
    EXPECT_EQ(5000, numericSensorPdrs->nominal_value.value_u16);
    EXPECT_EQ(6000, numericSensorPdrs->normal_max.value_u16);
    EXPECT_EQ(4000, numericSensorPdrs->normal_min.value_u16);
    EXPECT_EQ(7000, numericSensorPdrs->warning_high.value_u16);
    EXPECT_EQ(3000, numericSensorPdrs->warning_low.value_u16);
    EXPECT_EQ(8000, numericSensorPdrs->critical_high.value_u16);
    EXPECT_EQ(2000, numericSensorPdrs->critical_low.value_u16);
    EXPECT_EQ(9000, numericSensorPdrs->fatal_high.value_u16);
    EXPECT_EQ(1000, numericSensorPdrs->fatal_low.value_u16);
}

TEST_F(TerminusTest, parseNumericSensorPdrSint16Test)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                          // record handle
        0x1,                          // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,      // PDRType
        0x0,
        0x0,                          // recordChangeNumber
        56,
        0,                            // dataLength
        0,
        0,                            // PLDMTerminusHandle
        0x1,
        0x0,                          // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                            // entityType=Power Supply(120)
        1,
        0,                            // entityInstanceNumber
        0x1,
        0x0,                          // containerID=1
        PLDM_NO_INIT,                 // sensorInit
        false,                        // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,   // baseUint(2)=degrees C
        0,                            // unitModifier
        0,                            // rateUnit
        0,                            // baseOEMUnitHandle
        0,                            // auxUnit
        0,                            // auxUnitModifier
        0,                            // auxRateUnit
        0,                            // rel
        0,                            // auxOEMUnitHandle
        true,                         // isLinear
        PLDM_SENSOR_DATA_SIZE_SINT16, // sensorDataSize
        0,
        0,
        0,
        0, // resolution
        0,
        0,
        0,
        0, // offset
        0,
        0, // accuracy
        0, // plusTolerance
        0, // minusTolerance
        3,
        0, // hysteresis
        0, // supportedThresholds
        0, // thresholdAndHysteresisVolatility
        0,
        0,
        0x80,
        0x3f, // stateTransistionInterval=1.0
        0,
        0,
        0x80,
        0x3f,                           // updateInverval=1.0
        0xe8,
        0x03,                           // maxReadable = 1000
        0x18,
        0xfc,                           // minReadable = -1000
        PLDM_RANGE_FIELD_FORMAT_SINT16, // rangeFieldFormat
        0,                              // rangeFieldsupport
        0,
        0,                              // nominalValue = 0
        0xf4,
        0x01,                           // normalMax = 500
        0x0c,
        0xfe,                           // normalMin = -500
        0xe8,
        0x03,                           // warningHigh = 1,000
        0x18,
        0xfc,                           // warningLow = -1,000
        0xd0,
        0x07,                           // criticalHigh = 2,000
        0x30,
        0xf8,                           // criticalLow = -2,000
        0xb8,
        0x0b,                           // fatalHigh = 3,000
        0x48,
        0xf4                            // fatalLow = -3,000
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());

    auto numericSensorPdrs = t1.numericSensorPdrs[0];
    EXPECT_EQ(1, numericSensorPdrs->sensor_id);
    EXPECT_EQ(PLDM_SENSOR_DATA_SIZE_SINT16,
              numericSensorPdrs->sensor_data_size);
    EXPECT_EQ(PLDM_ENTITY_POWER_SUPPLY, numericSensorPdrs->entity_type);
    EXPECT_EQ(2, numericSensorPdrs->base_unit);
    EXPECT_EQ(0.0, numericSensorPdrs->offset);
    EXPECT_EQ(3, numericSensorPdrs->hysteresis.value_s16);
    EXPECT_EQ(1.0, numericSensorPdrs->update_interval);
    EXPECT_EQ(1000, numericSensorPdrs->max_readable.value_s16);
    EXPECT_EQ(-1000, numericSensorPdrs->min_readable.value_s16);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_SINT16,
              numericSensorPdrs->range_field_format);
    EXPECT_EQ(0, numericSensorPdrs->range_field_support.byte);
    EXPECT_EQ(0, numericSensorPdrs->nominal_value.value_s16);
    EXPECT_EQ(500, numericSensorPdrs->normal_max.value_s16);
    EXPECT_EQ(-500, numericSensorPdrs->normal_min.value_s16);
    EXPECT_EQ(1000, numericSensorPdrs->warning_high.value_s16);
    EXPECT_EQ(-1000, numericSensorPdrs->warning_low.value_s16);
    EXPECT_EQ(2000, numericSensorPdrs->critical_high.value_s16);
    EXPECT_EQ(-2000, numericSensorPdrs->critical_low.value_s16);
    EXPECT_EQ(3000, numericSensorPdrs->fatal_high.value_s16);
    EXPECT_EQ(-3000, numericSensorPdrs->fatal_low.value_s16);
}

TEST_F(TerminusTest, parseNumericSensorPdrUint32Test)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                          // record handle
        0x1,                          // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,      // PDRType
        0x0,
        0x0,                          // recordChangeNumber
        56,
        0,                            // dataLength
        0,
        0,                            // PLDMTerminusHandle
        0x1,
        0x0,                          // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                            // entityType=Power Supply(120)
        1,
        0,                            // entityInstanceNumber
        0x1,
        0x0,                          // containerID=1
        PLDM_NO_INIT,                 // sensorInit
        false,                        // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,   // baseUint(2)=degrees C
        0,                            // unitModifier
        0,                            // rateUnit
        0,                            // baseOEMUnitHandle
        0,                            // auxUnit
        0,                            // auxUnitModifier
        0,                            // auxRateUnit
        0,                            // rel
        0,                            // auxOEMUnitHandle
        true,                         // isLinear
        PLDM_SENSOR_DATA_SIZE_UINT32, // sensorDataSize
        0,
        0,
        0,
        0, // resolution
        0,
        0,
        0,
        0, // offset
        0,
        0, // accuracy
        0, // plusTolerance
        0, // minusTolerance
        3,
        0,
        0,
        0, // hysteresis
        0, // supportedThresholds
        0, // thresholdAndHysteresisVolatility
        0,
        0,
        0x80,
        0x3f, // stateTransistionInterval=1.0
        0,
        0,
        0x80,
        0x3f, // updateInverval=1.0
        0,
        0x10,
        0,
        0, // maxReadable = 4096
        0,
        0,
        0,
        0,                              // minReadable = 0
        PLDM_RANGE_FIELD_FORMAT_UINT32, // rangeFieldFormat
        0,                              // rangeFieldsupport
        0x40,
        0x4b,
        0x4c,
        0x00, // nominalValue = 5,000,000
        0x80,
        0x8d,
        0x5b,
        0x00, // normalMax = 6,000,000
        0x00,
        0x09,
        0x3d,
        0x00, // normalMin = 4,000,000
        0xc0,
        0xcf,
        0x6a,
        0x00, // warningHigh = 7,000,000
        0xc0,
        0xc6,
        0x2d,
        0x00, // warningLow = 3,000,000
        0x00,
        0x12,
        0x7a,
        0x00, // criticalHigh = 8,000,000
        0x80,
        0x84,
        0x1e,
        0x00, // criticalLow = 2,000,000
        0x40,
        0x54,
        0x89,
        0x00, // fatalHigh = 9,000,000
        0x40,
        0x42,
        0x0f,
        0x00 // fatalLow = 1,000,000
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());

    auto numericSensorPdrs = t1.numericSensorPdrs[0];
    EXPECT_EQ(1, numericSensorPdrs->sensor_id);
    EXPECT_EQ(PLDM_SENSOR_DATA_SIZE_UINT32,
              numericSensorPdrs->sensor_data_size);
    EXPECT_EQ(PLDM_ENTITY_POWER_SUPPLY, numericSensorPdrs->entity_type);
    EXPECT_EQ(2, numericSensorPdrs->base_unit);
    EXPECT_EQ(0.0, numericSensorPdrs->offset);
    EXPECT_EQ(3, numericSensorPdrs->hysteresis.value_u32);
    EXPECT_EQ(1.0, numericSensorPdrs->update_interval);
    EXPECT_EQ(4096, numericSensorPdrs->max_readable.value_u32);
    EXPECT_EQ(0, numericSensorPdrs->min_readable.value_u32);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_UINT32,
              numericSensorPdrs->range_field_format);
    EXPECT_EQ(0, numericSensorPdrs->range_field_support.byte);
    EXPECT_EQ(5000000, numericSensorPdrs->nominal_value.value_u32);
    EXPECT_EQ(6000000, numericSensorPdrs->normal_max.value_u32);
    EXPECT_EQ(4000000, numericSensorPdrs->normal_min.value_u32);
    EXPECT_EQ(7000000, numericSensorPdrs->warning_high.value_u32);
    EXPECT_EQ(3000000, numericSensorPdrs->warning_low.value_u32);
    EXPECT_EQ(8000000, numericSensorPdrs->critical_high.value_u32);
    EXPECT_EQ(2000000, numericSensorPdrs->critical_low.value_u32);
    EXPECT_EQ(9000000, numericSensorPdrs->fatal_high.value_u32);
    EXPECT_EQ(1000000, numericSensorPdrs->fatal_low.value_u32);
}

TEST_F(TerminusTest, parseNumericSensorPdrSint32Test)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                          // record handle
        0x1,                          // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,      // PDRType
        0x0,
        0x0,                          // recordChangeNumber
        56,
        0,                            // dataLength
        0,
        0,                            // PLDMTerminusHandle
        0x1,
        0x0,                          // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                            // entityType=Power Supply(120)
        1,
        0,                            // entityInstanceNumber
        0x1,
        0x0,                          // containerID=1
        PLDM_NO_INIT,                 // sensorInit
        false,                        // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,   // baseUint(2)=degrees C
        0,                            // unitModifier
        0,                            // rateUnit
        0,                            // baseOEMUnitHandle
        0,                            // auxUnit
        0,                            // auxUnitModifier
        0,                            // auxRateUnit
        0,                            // rel
        0,                            // auxOEMUnitHandle
        true,                         // isLinear
        PLDM_SENSOR_DATA_SIZE_SINT32, // sensorDataSize
        0,
        0,
        0,
        0, // resolution
        0,
        0,
        0,
        0, // offset
        0,
        0, // accuracy
        0, // plusTolerance
        0, // minusTolerance
        3,
        0,
        0,
        0, // hysteresis
        0, // supportedThresholds
        0, // thresholdAndHysteresisVolatility
        0,
        0,
        0x80,
        0x3f, // stateTransistionInterval=1.0
        0,
        0,
        0x80,
        0x3f, // updateInverval=1.0
        0xa0,
        0x86,
        0x01,
        0x00, // maxReadable = 100000
        0x60,
        0x79,
        0xfe,
        0xff,                           // minReadable = -10000
        PLDM_RANGE_FIELD_FORMAT_SINT32, // rangeFieldFormat
        0,                              // rangeFieldsupport
        0,
        0,
        0,
        0, // nominalValue = 0
        0x20,
        0xa1,
        0x07,
        0x00, // normalMax = 500,000
        0xe0,
        0x5e,
        0xf8,
        0xff, // normalMin = -500,000
        0x40,
        0x42,
        0x0f,
        0x00, // warningHigh = 1,000,000
        0xc0,
        0xbd,
        0xf0,
        0xff, // warningLow = -1,000,000
        0x80,
        0x84,
        0x1e,
        0x00, // criticalHigh = 2,000,000
        0x80,
        0x7b,
        0xe1,
        0xff, // criticalLow = -2,000,000
        0xc0,
        0xc6,
        0x2d,
        0x00, // fatalHigh = 3,000,000
        0x40,
        0x39,
        0xd2,
        0xff // fatalLow = -3,000,000
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());

    auto numericSensorPdrs = t1.numericSensorPdrs[0];
    EXPECT_EQ(1, numericSensorPdrs->sensor_id);
    EXPECT_EQ(PLDM_SENSOR_DATA_SIZE_SINT32,
              numericSensorPdrs->sensor_data_size);
    EXPECT_EQ(PLDM_ENTITY_POWER_SUPPLY, numericSensorPdrs->entity_type);
    EXPECT_EQ(2, numericSensorPdrs->base_unit);
    EXPECT_EQ(0.0, numericSensorPdrs->offset);
    EXPECT_EQ(3, numericSensorPdrs->hysteresis.value_s32);
    EXPECT_EQ(1.0, numericSensorPdrs->update_interval);
    EXPECT_EQ(100000, numericSensorPdrs->max_readable.value_s32);
    EXPECT_EQ(-100000, numericSensorPdrs->min_readable.value_s32);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_SINT32,
              numericSensorPdrs->range_field_format);
    EXPECT_EQ(0, numericSensorPdrs->range_field_support.byte);
    EXPECT_EQ(0, numericSensorPdrs->nominal_value.value_s32);
    EXPECT_EQ(500000, numericSensorPdrs->normal_max.value_s32);
    EXPECT_EQ(-500000, numericSensorPdrs->normal_min.value_s32);
    EXPECT_EQ(1000000, numericSensorPdrs->warning_high.value_s32);
    EXPECT_EQ(-1000000, numericSensorPdrs->warning_low.value_s32);
    EXPECT_EQ(2000000, numericSensorPdrs->critical_high.value_s32);
    EXPECT_EQ(-2000000, numericSensorPdrs->critical_low.value_s32);
    EXPECT_EQ(3000000, numericSensorPdrs->fatal_high.value_s32);
    EXPECT_EQ(-3000000, numericSensorPdrs->fatal_low.value_s32);
}

TEST_F(TerminusTest, parseNumericSensorPdrReal32Test)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                          // record handle
        0x1,                          // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,      // PDRType
        0x0,
        0x0,                          // recordChangeNumber
        56,
        0,                            // dataLength
        0,
        0,                            // PLDMTerminusHandle
        0x1,
        0x0,                          // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                            // entityType=Power Supply(120)
        1,
        0,                            // entityInstanceNumber
        0x1,
        0x0,                          // containerID=1
        PLDM_NO_INIT,                 // sensorInit
        false,                        // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,   // baseUint(2)=degrees C
        0,                            // unitModifier
        0,                            // rateUnit
        0,                            // baseOEMUnitHandle
        0,                            // auxUnit
        0,                            // auxUnitModifier
        0,                            // auxRateUnit
        0,                            // rel
        0,                            // auxOEMUnitHandle
        true,                         // isLinear
        PLDM_SENSOR_DATA_SIZE_SINT32, // sensorDataSize
        0,
        0,
        0,
        0, // resolution
        0,
        0,
        0,
        0, // offset
        0,
        0, // accuracy
        0, // plusTolerance
        0, // minusTolerance
        3,
        0,
        0,
        0, // hysteresis
        0, // supportedThresholds
        0, // thresholdAndHysteresisVolatility
        0,
        0,
        0x80,
        0x3f, // stateTransistionInterval=1.0
        0,
        0,
        0x80,
        0x3f, // updateInverval=1.0
        0xa0,
        0x86,
        0x01,
        0x00, // maxReadable = 100000
        0x60,
        0x79,
        0xfe,
        0xff,                           // minReadable = -10000
        PLDM_RANGE_FIELD_FORMAT_REAL32, // rangeFieldFormat
        0,                              // rangeFieldsupport
        0,
        0,
        0,
        0, // nominalValue = 0.0
        0x33,
        0x33,
        0x48,
        0x42, // normalMax = 50.05
        0x33,
        0x33,
        0x48,
        0xc2, // normalMin = -50.05
        0x83,
        0x00,
        0xc8,
        0x42, // warningHigh = 100.001
        0x83,
        0x00,
        0xc8,
        0xc2, // warningLow = -100.001
        0x83,
        0x00,
        0x48,
        0x43, // criticalHigh = 200.002
        0x83,
        0x00,
        0x48,
        0xc3, // criticalLow = -200.002
        0x62,
        0x00,
        0x96,
        0x43, // fatalHigh = 300.003
        0x62,
        0x00,
        0x96,
        0xc3 // fatalLow = -300.003
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());

    auto numericSensorPdrs = t1.numericSensorPdrs[0];
    EXPECT_EQ(1, numericSensorPdrs->sensor_id);
    EXPECT_EQ(PLDM_SENSOR_DATA_SIZE_SINT32,
              numericSensorPdrs->sensor_data_size);
    EXPECT_EQ(PLDM_ENTITY_POWER_SUPPLY, numericSensorPdrs->entity_type);
    EXPECT_EQ(2, numericSensorPdrs->base_unit);
    EXPECT_EQ(0.0, numericSensorPdrs->offset);
    EXPECT_EQ(3, numericSensorPdrs->hysteresis.value_s32);
    EXPECT_EQ(1.0, numericSensorPdrs->update_interval);
    EXPECT_EQ(100000, numericSensorPdrs->max_readable.value_s32);
    EXPECT_EQ(-100000, numericSensorPdrs->min_readable.value_s32);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_REAL32,
              numericSensorPdrs->range_field_format);
    EXPECT_FLOAT_EQ(0, numericSensorPdrs->range_field_support.byte);
    EXPECT_FLOAT_EQ(0, numericSensorPdrs->nominal_value.value_f32);
    EXPECT_FLOAT_EQ(50.05f, numericSensorPdrs->normal_max.value_f32);
    EXPECT_FLOAT_EQ(-50.05f, numericSensorPdrs->normal_min.value_f32);
    EXPECT_FLOAT_EQ(100.001f, numericSensorPdrs->warning_high.value_f32);
    EXPECT_FLOAT_EQ(-100.001f, numericSensorPdrs->warning_low.value_f32);
    EXPECT_FLOAT_EQ(200.002f, numericSensorPdrs->critical_high.value_f32);
    EXPECT_FLOAT_EQ(-200.002f, numericSensorPdrs->critical_low.value_f32);
    EXPECT_FLOAT_EQ(300.003f, numericSensorPdrs->fatal_high.value_f32);
    EXPECT_FLOAT_EQ(-300.003f, numericSensorPdrs->fatal_low.value_f32);
}

TEST_F(TerminusTest, parseNumericSensorPDRInvalidSizeTest)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    // A corrupted PDR. The data after plusTolerance missed.
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                         // record handle
        0x1,                         // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,     // PDRType
        0x0,
        0x0,                         // recordChangeNumber
        34,
        0,                           // dataLength
        0,
        0,                           // PLDMTerminusHandle
        0x1,
        0x0,                         // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                           // entityType=Power Supply(120)
        1,
        0,                           // entityInstanceNumber
        0x1,
        0x0,                         // containerID=1
        PLDM_NO_INIT,                // sensorInit
        false,                       // sensorAuxiliaryNamesPDR
        2,                           // baseUint(2)=degrees C
        0,                           // unitModifier
        0,                           // rateUnit
        0,                           // baseOEMUnitHandle
        0,                           // auxUnit
        0,                           // auxUnitModifier
        0,                           // auxRateUnit
        0,                           // rel
        0,                           // auxOEMUnitHandle
        true,                        // isLinear
        PLDM_SENSOR_DATA_SIZE_UINT8, // sensorDataSize
        0,
        0,
        0,
        0, // resolution
        0,
        0,
        0,
        0, // offset
        0,
        0, // accuracy
        0  // plusTolerance
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(0, t1.numericSensorPdrs.size());
}

TEST_F(TerminusTest, platformManagerInitTerminusCoverage)
{
    pldm::UUID uuid{"f72d6f90-5675-11ed-9b6a-0242ac120002"};
    pldm::MctpInfos mctpInfos{pldm::MctpInfo(
        12, uuid, "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1,
        std::nullopt, "xyz.openbmc_project.MCTP.Endpoint.BindingTypes.PCIe",
        std::nullopt)};

    setupResponsesForDiscoverTerminus();
    terminusManager.discoverMctpTerminus(mctpInfos);
    ASSERT_EQ(termini.size(), 1u);

    auto terminus = terminusManager.getTerminus(uuid);
    ASSERT_NE(terminus, nullptr);

    setupResponsesForInitTerminus();
    auto rc = syncWaitWithDbusIo(platformManager.initTerminus());
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(std::get<0>(*rc), PLDM_SUCCESS);
    EXPECT_EQ(terminus->numericSensorPdrs.size(), 1u);
}

TEST(Entity, getClosestInventoriesCoverage)
{
    std::vector<std::string> inventories{
        "/xyz/openbmc_project/inventory/system/chassis"};
    std::vector<std::string> containerInventories{
        "/xyz/openbmc_project/inventory/system"};
    Entity entity(inventories, containerInventories);
    EXPECT_EQ(entity.getInventories().size(), 1u);
    EXPECT_EQ(entity.getClosestInventories().size(), 1u);
    EXPECT_EQ(entity.getClosestInventories().front(),
              "/xyz/openbmc_project/inventory/system/chassis");

    std::vector<std::string> emptyInventories{};
    Entity fallbackEntity(emptyInventories, containerInventories);
    EXPECT_EQ(fallbackEntity.getInventories().size(), 0u);
    EXPECT_EQ(fallbackEntity.getClosestInventories().size(), 1u);
    EXPECT_EQ(fallbackEntity.getClosestInventories().front(),
              "/xyz/openbmc_project/inventory/system");
}

TEST(PlatformMcErrors, invalidArgumentCoverage)
{
    ::errors::InvalidArgument ex{"TelemetryEndpoint"};
    EXPECT_STREQ(ex.name(), "xyz.openbmc_project.Common.Error.InvalidArgument");
    EXPECT_STREQ(ex.description(), "Out of range");
    EXPECT_EQ(ex.propertyName, "TelemetryEndpoint");
    EXPECT_FALSE(std::string(ex.what()).empty());
    EXPECT_GT(ex.get_errno(), 0);

    ::errors::InvalidArgument exWithInfo{"TelemetryEndpoint",
                                         "must be non-empty"};
    EXPECT_EQ(exWithInfo.propertyName, "TelemetryEndpoint");
    EXPECT_NE(std::string(exWithInfo.what()).find("must be non-empty"),
              std::string::npos);
}

TEST_F(TerminusTest, parseAdditionalPdrCoverage)
{
    std::string uuid("00000000-0000-0000-0000-0000000000AA");
    Terminus terminus(0x0A, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setTerminusName("TerminusA");

    auto effecterAuxPdr =
        makeAuxNamePdr(0x88, PLDM_EFFECTER_AUXILIARY_NAMES_PDR);
    auto numericEffecterPdr = makeNumericEffecterPdr(0x88, true);
    auto entityAssociationPdr = makeEntityAssociationPdr();
    auto oemPdr = makeOemPdr();
    std::vector<uint8_t> unknownPdr(sizeof(pldm_pdr_hdr), 0);
    auto* unknownHdr = reinterpret_cast<pldm_pdr_hdr*>(unknownPdr.data());
    unknownHdr->version = 1;
    unknownHdr->type = 0xFF;

    terminus.pdrs.emplace_back(effecterAuxPdr);
    terminus.pdrs.emplace_back(numericEffecterPdr);
    terminus.pdrs.emplace_back(entityAssociationPdr);
    terminus.pdrs.emplace_back(oemPdr);
    terminus.pdrs.emplace_back(unknownPdr);

    auto rc = terminus.parsePDRs();
    EXPECT_FALSE(rc);
    ASSERT_EQ(1u, terminus.numericEffecters.size());
    EXPECT_NE(nullptr, terminus.getEffecterAuxiliaryNames(0x88));
    ASSERT_EQ(1u, terminus.oemPdrs.size());
    EXPECT_EQ(0x1234u, std::get<0>(terminus.oemPdrs[0]));
    EXPECT_EQ(1u, std::get<1>(terminus.oemPdrs[0]));
    EXPECT_EQ(4u, std::get<2>(terminus.oemPdrs[0]).size());
    EXPECT_NE(std::string::npos,
              terminus.numericEffecters[0]->path.find("TerminusA_A"));
}

TEST_F(TerminusTest, repeatedParseAndStateSetPointerCoverage)
{
    std::string uuid("00000000-0000-0000-0000-0000000000A1");
    Terminus terminus(0x1A, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    auto invalidSensorAuxPdr = makeInvalidSensorAuxNamePdr(0x91);
    auto invalidEffecterAuxPdr = makeInvalidEffecterAuxNamePdr(0x92);
    terminus.pdrs.emplace_back(invalidSensorAuxPdr);
    terminus.pdrs.emplace_back(invalidEffecterAuxPdr);
    terminus.pdrs.emplace_back(makeNumericEffecterPdrVariant(
        0x93, PLDM_EFFECTER_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16));

    EXPECT_TRUE(terminus.parsePDRs());
    ASSERT_NE(nullptr, terminus.interfaceAddedMatch);
    auto* match = terminus.interfaceAddedMatch.get();
    EXPECT_TRUE(terminus.parsePDRs());
    EXPECT_EQ(match, terminus.interfaceAddedMatch.get());

    const std::array<unsigned char, 7> stateInfoData{
        static_cast<unsigned char>(PLDM_STATESET_ID_HEALTHSTATE & 0xFF),
        static_cast<unsigned char>((PLDM_STATESET_ID_HEALTHSTATE >> 8) & 0xFF),
        0x2,
        0x01,
        0x80,
        static_cast<unsigned char>(PLDM_STATESET_ID_LINKSTATE),
        0x00};
    std::vector<StateSetData> stateSets;
    terminus.parseStateSetInfo(stateInfoData.data(), 1, stateSets);
    ASSERT_EQ(1u, stateSets.size());

    const std::array<unsigned char, 8> multiStateInfoData{
        static_cast<unsigned char>(PLDM_STATESET_ID_HEALTHSTATE & 0xFF),
        static_cast<unsigned char>((PLDM_STATESET_ID_HEALTHSTATE >> 8) & 0xFF),
        0x1,
        0x03,
        static_cast<unsigned char>(PLDM_STATESET_ID_LINKSTATE),
        static_cast<unsigned char>((PLDM_STATESET_ID_LINKSTATE >> 8) & 0xFF),
        0x1,
        0x03};
    stateSets.clear();
    terminus.parseStateSetInfo(multiStateInfoData.data(), 2, stateSets);
    ASSERT_EQ(2u, stateSets.size());
    EXPECT_EQ(2u, std::get<1>(stateSets[0]).size());
    EXPECT_EQ(2u, std::get<1>(stateSets[1]).size());
}

TEST_F(TerminusTest, parsePdrsEmptyCollectionsCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000174");
    Terminus terminus(0x74, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    EXPECT_TRUE(terminus.parsePDRs());
    EXPECT_TRUE(terminus.numericSensorPdrs.empty());
    EXPECT_TRUE(terminus.oemEnergyCountNumericSensorPdrs.empty());
    EXPECT_TRUE(terminus.numericEffecterPdrs.empty());
    EXPECT_TRUE(terminus.stateSensorPdrs.empty());
    EXPECT_TRUE(terminus.stateEffecterPdrs.empty());
    ASSERT_NE(nullptr, terminus.interfaceAddedMatch);

    auto* match = terminus.interfaceAddedMatch.get();
    EXPECT_TRUE(terminus.parsePDRs());
    EXPECT_EQ(match, terminus.interfaceAddedMatch.get());
}

TEST_F(TerminusTest, interfaceAddedAndOnlineOfflineCoverage)
{
    std::string uuid("00000000-0000-0000-0000-0000000000BB");
    Terminus terminus(0x0B, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    // Cover checkNsmDeviceInventory true/false branches.
    EXPECT_TRUE(terminus.checkNsmDeviceInventory(uuid));
    EXPECT_FALSE(terminus.checkNsmDeviceInventory(
        "00000000-0000-0000-0000-0000000000CC"));

    // Cover interfaceAdded fast path and refresh trigger path.
    pldm::dbus::PropertyMap properties;
    pldm::dbus::InterfaceMap interfaces;
    interfaces.emplace(std::string(overallSystemInterface), properties);
    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call("org.test",
                                      "/xyz/openbmc_project/inventory/test",
                                      "org.test.Interface", "Method");
    msg.append(sdbusplus::object_path("/xyz/openbmc_project/inventory/test"),
               interfaces);
    sealAndRewind(msg);
    EXPECT_NO_THROW(terminus.interfaceAdded(msg));

    terminus.initalized = true;
    EXPECT_NO_THROW(terminus.interfaceAdded(msg));

    auto numericSensorPdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    numericSensorPdr->sensor_id = 0x31;
    numericSensorPdr->entity_type = PLDM_ENTITY_SYS_BOARD;
    numericSensorPdr->entity_instance_num = 1;
    numericSensorPdr->container_id = 1;
    numericSensorPdr->base_unit = PLDM_SENSOR_UNIT_DEGRESS_C;
    numericSensorPdr->sensor_data_size = PLDM_SENSOR_DATA_SIZE_UINT8;
    numericSensorPdr->max_readable.value_u8 = 100;
    numericSensorPdr->min_readable.value_u8 = 0;
    numericSensorPdr->hysteresis.value_u8 = 1;
    numericSensorPdr->supported_thresholds.byte = 0;
    numericSensorPdr->range_field_support.byte = 0;
    numericSensorPdr->range_field_format = PLDM_RANGE_FIELD_FORMAT_UINT8;
    numericSensorPdr->resolution = 1.0f;
    numericSensorPdr->offset = 0.0f;
    numericSensorPdr->update_interval = 1.0f;

    auto numericEffecterPdr =
        std::make_shared<pldm_numeric_effecter_value_pdr>();
    numericEffecterPdr->effecter_id = 0x41;
    numericEffecterPdr->entity_type = PLDM_ENTITY_SYS_BOARD;
    numericEffecterPdr->entity_instance = 1;
    numericEffecterPdr->container_id = 1;
    numericEffecterPdr->base_unit = PLDM_SENSOR_UNIT_NONE;
    numericEffecterPdr->effecter_data_size = PLDM_EFFECTER_DATA_SIZE_UINT8;
    numericEffecterPdr->max_settable.value_u8 = 100;
    numericEffecterPdr->min_settable.value_u8 = 0;
    numericEffecterPdr->range_field_format = PLDM_RANGE_FIELD_FORMAT_UINT8;
    numericEffecterPdr->range_field_support.byte = 0x1F;
    numericEffecterPdr->nominal_value.value_u8 = 50;
    numericEffecterPdr->normal_max.value_u8 = 60;
    numericEffecterPdr->normal_min.value_u8 = 40;
    numericEffecterPdr->rated_max.value_u8 = 70;
    numericEffecterPdr->rated_min.value_u8 = 30;

    std::string sensorName{"offline_sensor"};
    std::string effecterName{"offline_effecter"};
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis0"};

    auto numericSensor = std::make_shared<NumericSensor>(
        terminus.getTid(), false, numericSensorPdr, sensorName, associationPath,
        nullptr);
    auto numericEffecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, numericEffecterPdr, effecterName,
        associationPath, terminusManager);
    numericEffecter->needUpdate = false;

    StateSetData healthStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_HEALTHSTATE),
                        PossibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                       PLDM_STATESET_HEALTH_STATE_CRITICAL});
    StateSetInfo stateSensorInfo =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
                        std::vector<StateSetData>{healthStateData});

    StateSetData bootRequestData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_BOOT_REQUEST),
                        PossibleStates{PLDM_STATESET_BOOT_REQUEST_NORMAL,
                                       PLDM_STATESET_BOOT_REQUEST_REQUESTED});
    StateSetInfo stateEffecterInfo =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
                        std::vector<StateSetData>{bootRequestData});

    auto stateSensor = std::make_shared<StateSensor>(
        terminus.getTid(), false, 0x32, stateSensorInfo, nullptr,
        associationPath, nullptr);
    auto stateEffecter = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x42, stateEffecterInfo, nullptr,
        associationPath, terminusManager);

    terminus.numericSensors.emplace_back(numericSensor);
    terminus.numericEffecters.emplace_back(numericEffecter);
    terminus.stateSensors.emplace_back(stateSensor);
    terminus.stateEffecters.emplace_back(stateEffecter);

    terminus.setOffline();
    EXPECT_FALSE(terminus.resumed);
    EXPECT_TRUE(std::isnan(numericSensor->getReading()));

    terminus.setOnline();
    EXPECT_TRUE(numericEffecter->needUpdate);

    waitForRefreshAssociationsTask(terminus);
}

TEST_F(TerminusTest, nvidiaEnergyCountOemPdrCoverage)
{
    constexpr uint16_t sensorId = 0x91;
    std::string uuid("00000000-0000-0000-0000-0000000000EE");
    Terminus terminus(0x0E, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    auto oemEnergyPdr = makeNvidiaEnergyCountOemPdr(sensorId);
    terminus.pdrs.emplace_back(oemEnergyPdr);

    ASSERT_TRUE(terminus.parsePDRs());
    ASSERT_EQ(1u, terminus.numericSensors.size());

    auto sensor = terminus.numericSensors[0];
    EXPECT_EQ(static_cast<uint8_t>(POLLING_METHOD_INDICATOR_PLDM_TYPE_OEM),
              sensor->getPollingIndicator());
    sensor->updateReading(true, true, 42);
    EXPECT_TRUE(std::isfinite(sensor->getReading()));
}

TEST_F(TerminusTest, parseNumericEffecterPdrCoverageMatrix)
{
    std::string uuid("00000000-0000-0000-0000-0000000000EF");
    Terminus terminus(0x0F, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setTerminusName("TerminusEffecter");

    struct NumericEffecterCase
    {
        uint8_t effecterDataSize;
        uint8_t rangeFieldFormat;
    };
    /* UINT64/SINT64 effecter sizes (enum values 10/11) are rejected by
     * decode_numeric_effecter_pdr_data() in libpldm 0.14.0 because it
     * guards with PLDM_SENSOR_DATA_SIZE_MAX (=7) instead of
     * PLDM_EFFECTER_DATA_SIZE_MAX (=11).  Omit them until that upstream
     * libpldm bug is fixed and the CI Docker image is rebuilt. */
    const std::array<NumericEffecterCase, 7> cases{{
        {PLDM_EFFECTER_DATA_SIZE_UINT8, PLDM_RANGE_FIELD_FORMAT_UINT8},
        {PLDM_EFFECTER_DATA_SIZE_SINT8, PLDM_RANGE_FIELD_FORMAT_SINT8},
        {PLDM_EFFECTER_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16},
        {PLDM_EFFECTER_DATA_SIZE_SINT16, PLDM_RANGE_FIELD_FORMAT_SINT16},
        {PLDM_EFFECTER_DATA_SIZE_UINT32, PLDM_RANGE_FIELD_FORMAT_UINT32},
        {PLDM_EFFECTER_DATA_SIZE_SINT32, PLDM_RANGE_FIELD_FORMAT_SINT32},
        {PLDM_EFFECTER_DATA_SIZE_UINT32, PLDM_RANGE_FIELD_FORMAT_REAL32},
    }};

    uint16_t effecterId = 0xA0;
    for (const auto& item : cases)
    {
        terminus.pdrs.emplace_back(makeNumericEffecterPdrVariant(
            effecterId++, item.effecterDataSize, item.rangeFieldFormat));
    }

    terminus.pdrs.emplace_back(
        makeNumericEffecterPdrVariant(effecterId++, 0xFF, 0xFF));

    EXPECT_TRUE(terminus.parsePDRs());
    /* The last PDR (effecterDataSize=0xFF) is invalid and rejected by
     * decode_numeric_effecter_pdr_data(), so only the 7 valid PDRs
     * are stored. */
    EXPECT_EQ(cases.size(), terminus.numericEffecterPdrs.size());
    EXPECT_EQ(cases.size(), terminus.numericEffecters.size());
    EXPECT_NE(std::string::npos,
              terminus.numericEffecters.front()->path.find("TerminusEffecter"));
}

TEST_F(TerminusTest, parseNumericEffecterPdrUint16RangeCoverage)
{
    std::string uuid("00000000-0000-0000-0000-0000000000F1");
    Terminus terminus(0x11, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    auto pdr = makeNumericEffecterPdrVariant(
        0xB1, PLDM_EFFECTER_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16);
    auto parsedPdr = terminus.parseNumericEffecterPDR(pdr);

    ASSERT_NE(nullptr, parsedPdr);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_UINT16, parsedPdr->range_field_format);
    EXPECT_EQ(500u, parsedPdr->nominal_value.value_u16);
    EXPECT_EQ(700u, parsedPdr->rated_max.value_u16);
}

TEST_F(TerminusTest, parseNumericEffecterPdrSInt16RangeCoverage)
{
    std::string uuid("00000000-0000-0000-0000-0000000000F2");
    Terminus terminus(0x12, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    auto pdr = makeNumericEffecterPdrVariant(
        0xB2, PLDM_EFFECTER_DATA_SIZE_SINT16, PLDM_RANGE_FIELD_FORMAT_SINT16);
    auto parsedPdr = terminus.parseNumericEffecterPDR(pdr);

    ASSERT_NE(nullptr, parsedPdr);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_SINT16, parsedPdr->range_field_format);
    EXPECT_EQ(100, parsedPdr->nominal_value.value_s16);
    EXPECT_EQ(-300, parsedPdr->rated_min.value_s16);
}

TEST_F(TerminusTest, parseNumericEffecterPdrUint32RangeCoverage)
{
    std::string uuid("00000000-0000-0000-0000-0000000000F3");
    Terminus terminus(0x13, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    auto pdr = makeNumericEffecterPdrVariant(
        0xB3, PLDM_EFFECTER_DATA_SIZE_UINT32, PLDM_RANGE_FIELD_FORMAT_UINT32);
    auto parsedPdr = terminus.parseNumericEffecterPDR(pdr);

    ASSERT_NE(nullptr, parsedPdr);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_UINT32, parsedPdr->range_field_format);
    EXPECT_EQ(50000u, parsedPdr->nominal_value.value_u32);
    EXPECT_EQ(30000u, parsedPdr->rated_min.value_u32);
}

TEST_F(TerminusTest, parseNumericEffecterPdrSInt32RangeCoverage)
{
    std::string uuid("00000000-0000-0000-0000-0000000000F4");
    Terminus terminus(0x14, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    auto pdr = makeNumericEffecterPdrVariant(
        0xB4, PLDM_EFFECTER_DATA_SIZE_SINT32, PLDM_RANGE_FIELD_FORMAT_SINT32);
    auto parsedPdr = terminus.parseNumericEffecterPDR(pdr);

    ASSERT_NE(nullptr, parsedPdr);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_SINT32, parsedPdr->range_field_format);
    EXPECT_EQ(10000, parsedPdr->nominal_value.value_s32);
    EXPECT_EQ(-30000, parsedPdr->rated_min.value_s32);
}

TEST_F(TerminusTest, parseNumericEffecterPdrReal32RangeCoverage)
{
    std::string uuid("00000000-0000-0000-0000-0000000000F5");
    Terminus terminus(0x15, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    auto pdr = makeNumericEffecterPdrVariant(
        0xB5, PLDM_EFFECTER_DATA_SIZE_UINT32, PLDM_RANGE_FIELD_FORMAT_REAL32);
    auto parsedPdr = terminus.parseNumericEffecterPDR(pdr);

    ASSERT_NE(nullptr, parsedPdr);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_REAL32, parsedPdr->range_field_format);
    EXPECT_FLOAT_EQ(12.5f, parsedPdr->nominal_value.value_f32);
    EXPECT_FLOAT_EQ(-30.5f, parsedPdr->rated_min.value_f32);
}

TEST_F(TerminusTest, parseSensorAuxiliaryNamesMultipleStringsCoverage)
{
    std::string uuid("00000000-0000-0000-0000-0000000000F6");
    Terminus terminus(0x16, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    auto sensorAuxNames =
        terminus.parseSensorAuxiliaryNamesPDR(makeMultiSensorAuxNamePdr(0x1B1));

    ASSERT_NE(nullptr, sensorAuxNames);
    const auto& [sensorId, sensorCount, names] = *sensorAuxNames;
    EXPECT_EQ(0x1B1, sensorId);
    EXPECT_EQ(2, sensorCount);
    ASSERT_EQ(2u, names.size());
    ASSERT_EQ(2u, names[0].size());
    EXPECT_EQ("en", names[0][0].first);
    EXPECT_EQ("CPU0", names[0][0].second);
    EXPECT_EQ("fr", names[0][1].first);
    EXPECT_EQ("UC0", names[0][1].second);
    ASSERT_EQ(1u, names[1].size());
    EXPECT_EQ("DIMM0", names[1][0].second);
}

TEST_F(TerminusTest, parseEffecterAuxiliaryNamesMultipleStringsCoverage)
{
    std::string uuid("00000000-0000-0000-0000-0000000000F7");
    Terminus terminus(0x17, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    auto effecterAuxNames = terminus.parseEffecterAuxiliaryNamesPDR(
        makeMultiEffecterAuxNamePdr(0x2B1));

    ASSERT_NE(nullptr, effecterAuxNames);
    const auto& [effecterId, effecterCount, names] = *effecterAuxNames;
    EXPECT_EQ(0x2B1, effecterId);
    EXPECT_EQ(2, effecterCount);
    ASSERT_EQ(2u, names.size());
    ASSERT_EQ(2u, names[0].size());
    EXPECT_EQ("en", names[0][0].first);
    EXPECT_EQ("PowerCap", names[0][0].second);
    EXPECT_EQ("es", names[0][1].first);
    EXPECT_EQ("Potencia", names[0][1].second);
    ASSERT_EQ(1u, names[1].size());
    EXPECT_EQ("Throttle", names[1][0].second);
}

TEST_F(TerminusTest, parseOemPdrZeroLengthCoverage)
{
    std::string uuid("00000000-0000-0000-0000-0000000000F8");
    Terminus terminus(0x18, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    auto parsedPdr = terminus.parseOemPDR(makeZeroLengthOemPdr());

    EXPECT_EQ(0x4321u, std::get<0>(parsedPdr));
    EXPECT_EQ(2u, std::get<1>(parsedPdr));
    ASSERT_EQ(1u, std::get<2>(parsedPdr).size());
    EXPECT_EQ(0u, std::get<2>(parsedPdr)[0]);
}

TEST_F(TerminusTest, parseOemPdrPayloadCopyCoverage)
{
    std::string uuid("00000000-0000-0000-0000-0000000000F9");
    Terminus terminus(0x19, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    auto parsedPdr = terminus.parseOemPDR(makeOemPdr());

    EXPECT_EQ(0x1234u, std::get<0>(parsedPdr));
    EXPECT_EQ(1u, std::get<1>(parsedPdr));
    ASSERT_EQ(4u, std::get<2>(parsedPdr).size());
    EXPECT_EQ(0xAAu, std::get<2>(parsedPdr)[0]);
    EXPECT_EQ(0xBBu, std::get<2>(parsedPdr)[1]);
    EXPECT_EQ(0xCCu, std::get<2>(parsedPdr)[2]);
    EXPECT_EQ(0u, std::get<2>(parsedPdr)[3]);
}

TEST_F(TerminusTest, parseNumericSensorPdrInvalidCoverage)
{
    std::string uuid("00000000-0000-0000-0000-0000000000FA");
    Terminus terminus(0x1A, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    auto numericPdr = makeNumericSensorValuePdrStruct(0x1C1);
    numericPdr->hdr.version = 1;
    numericPdr->hdr.type = PLDM_NUMERIC_SENSOR_PDR;

    auto invalidPdrData = structToBytes(*numericPdr);
    invalidPdrData.resize(sizeof(pldm_value_pdr_hdr) + 1);
    updatePdrLength(invalidPdrData);

    EXPECT_EQ(nullptr, terminus.parseNumericSensorPDR(invalidPdrData));
}

TEST_F(TerminusTest, parsePDRsInvalidNumericSensorCoverage)
{
    std::string uuid("00000000-0000-0000-0000-0000000000FB");
    Terminus terminus(0x1B, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    auto numericPdr = makeNumericSensorValuePdrStruct(0x1C2);
    numericPdr->hdr.version = 1;
    numericPdr->hdr.type = PLDM_NUMERIC_SENSOR_PDR;

    auto invalidPdrData = structToBytes(*numericPdr);
    invalidPdrData.resize(sizeof(pldm_value_pdr_hdr) + 1);
    updatePdrLength(invalidPdrData);

    terminus.pdrs.emplace_back(std::move(invalidPdrData));

    EXPECT_TRUE(terminus.parsePDRs());
    EXPECT_TRUE(terminus.numericSensorPdrs.empty());
}

TEST_F(TerminusTest, addStateSensorAndEffecterCoverage)
{
    constexpr uint16_t sensorId = 0x211;
    constexpr uint16_t effecterId = 0x311;
    std::string uuid("00000000-0000-0000-0000-0000000000F0");
    Terminus terminus(0x10, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setTerminusName("TerminusState");

    auto sensorAuxPdr = makeSensorAuxNamePdr(sensorId);
    auto stateSensorPdr = makeStateSensorPdr(
        sensorId, PLDM_ENTITY_SYS_BOARD, PLDM_STATESET_ID_HEALTHSTATE, true);
    auto effecterAuxPdr =
        makeAuxNamePdr(effecterId, PLDM_EFFECTER_AUXILIARY_NAMES_PDR);
    auto stateEffecterPdr = makeStateEffecterPdr(
        effecterId, PLDM_ENTITY_SYS_BOARD, PLDM_STATESET_ID_BOOT_REQUEST);

    terminus.pdrs.emplace_back(sensorAuxPdr);
    terminus.pdrs.emplace_back(stateSensorPdr);
    terminus.pdrs.emplace_back(effecterAuxPdr);
    terminus.pdrs.emplace_back(stateEffecterPdr);

    ASSERT_TRUE(terminus.parsePDRs());
    ASSERT_EQ(1u, terminus.stateSensors.size());
    ASSERT_EQ(1u, terminus.stateEffecters.size());
}

TEST_F(TerminusTest, privateFindInventoryAndPhysicalContextCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000155");
    Terminus terminus(0x55, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis55";
    terminus.setInstance(7);

    const std::string cpu0{
        "/xyz/openbmc_project/inventory/system/chassis/chassis55/cpu0"};
    const std::string cpu1{
        "/xyz/openbmc_project/inventory/system/chassis/chassis55/cpu1"};
    const std::string dimm0{
        "/xyz/openbmc_project/inventory/system/chassis/chassis55/dimm0"};
    const std::string dimm1{
        "/xyz/openbmc_project/inventory/system/chassis/chassis55/dimm1"};

    terminus.inventories.emplace_back(cpu0, PLDM_ENTITY_PROC, 7);
    terminus.inventories.emplace_back(cpu1, PLDM_ENTITY_PROC, 7);
    terminus.inventories.emplace_back(dimm0, PLDM_ENTITY_MEMORY_CONTROLLER, 2);
    terminus.inventories.emplace_back(dimm1, PLDM_ENTITY_MEMORY_CONTROLLER, 2);
    terminus.inventoryParentMap[dimm0] = cpu0;
    terminus.inventoryParentMap[dimm1] = cpu1;

    EntityInfo containerEntity{overallSystemCotainerId, PLDM_ENTITY_PROC, 1};
    terminus.entityAssociations.emplace(
        1, std::make_pair(containerEntity, std::set<EntityInfo>{}));

    auto overallPaths = terminus.findInventory(overallSystemCotainerId, false);
    ASSERT_EQ(1u, overallPaths.size());
    EXPECT_EQ(terminus.systemInventoryPath, overallPaths.front());

    auto unknownContainerPaths =
        terminus.findInventory(static_cast<ContainerID>(0x9FFF), false);
    ASSERT_EQ(1u, unknownContainerPaths.size());
    EXPECT_EQ(terminus.systemInventoryPath, unknownContainerPaths.front());

    EntityInfo cpuEntity{1, PLDM_ENTITY_PROC, 1};
    auto cpuPaths = terminus.findInventory(cpuEntity, false);
    EXPECT_EQ(2u, cpuPaths.size());

    terminus.entities.clear();
    EntityInfo dimmEntity{1, PLDM_ENTITY_MEMORY_CONTROLLER, 2};
    auto dimmPaths = terminus.findInventory(dimmEntity, false);
    EXPECT_EQ(2u, dimmPaths.size());

    terminus.entities.clear();
    terminus.inventoryParentMap.clear();
    auto dimmFallback = terminus.findInventory(dimmEntity, false);
    EXPECT_EQ(1u, dimmFallback.size());
    EXPECT_EQ(dimm0, dimmFallback.front());

    terminus.entities.clear();
    EntityInfo missingEntity{1, PLDM_ENTITY_NETWORK_CONTROLLER, 99};
    auto closest = terminus.findInventory(missingEntity, true);
    EXPECT_EQ(2u, closest.size());

    terminus.entities.clear();
    auto missingWithoutClosest = terminus.findInventory(missingEntity, false);
    EXPECT_TRUE(missingWithoutClosest.empty());

    EXPECT_EQ(PhysicalContextType::Memory,
              terminus.toPhysicalContextType(PLDM_ENTITY_MEMORY_CONTROLLER));
    EXPECT_EQ(PhysicalContextType::CPU,
              terminus.toPhysicalContextType(PLDM_ENTITY_PROC_MODULE));
    EXPECT_EQ(PhysicalContextType::CPU,
              terminus.toPhysicalContextType(PLDM_ENTITY_PROC_IO_MODULE));
    EXPECT_EQ(PhysicalContextType::VoltageRegulator,
              terminus.toPhysicalContextType(PLDM_ENTITY_DC_DC_CONVERTER));
    EXPECT_EQ(PhysicalContextType::VoltageRegulator,
              terminus.toPhysicalContextType(PLDM_ENTITY_POWER_SUPPLY));
    EXPECT_EQ(PhysicalContextType::NetworkingDevice,
              terminus.toPhysicalContextType(PLDM_ENTITY_NETWORK_CONTROLLER));
    EXPECT_EQ(PhysicalContextType::SystemBoard,
              terminus.toPhysicalContextType(PLDM_ENTITY_SYS_BOARD));
}

TEST_F(TerminusTest, privateFindInventoryOverrideCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000165");
    Terminus terminus(0x65, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis65";
    terminus.setInstance(9);

    const std::string processorModulePath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis65/"
        "ProcessorModule_9"};
    const std::string cpuPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis65/CPU_9"};
    terminus.inventories.emplace_back(processorModulePath,
                                      PLDM_ENTITY_PROC_IO_MODULE, 9);
    terminus.inventories.emplace_back(cpuPath, PLDM_ENTITY_PROC, 9);

    EntityInfo procModuleEntity{overallSystemCotainerId,
                                PLDM_ENTITY_PROC_IO_MODULE, 1};
    EntityInfo procEntity{11, PLDM_ENTITY_PROC, 1};
    terminus.entityAssociations.emplace(
        10, std::make_pair(procEntity, std::set<EntityInfo>{}));
    terminus.entityAssociations.emplace(
        11, std::make_pair(procModuleEntity, std::set<EntityInfo>{procEntity}));

    auto memoryClosest = terminus.findInventory(
        EntityInfo{10, PLDM_ENTITY_MEMORY_CONTROLLER, 1}, true);
    ASSERT_EQ(1u, memoryClosest.size());
    EXPECT_EQ(processorModulePath, memoryClosest.front());

    terminus.entities.clear();
    terminus.entityAssociations.emplace(
        30, std::make_pair(procModuleEntity, std::set<EntityInfo>{}));
    auto memoryUnderProcModule = terminus.findInventory(
        EntityInfo{30, PLDM_ENTITY_MEMORY_CONTROLLER, 1}, true);
    ASSERT_EQ(1u, memoryUnderProcModule.size());
    EXPECT_EQ(processorModulePath, memoryUnderProcModule.front());

    terminus.entities.clear();
    std::set<EntityInfo> processorModuleChildren{
        EntityInfo{20, PLDM_ENTITY_POWER_SUPPLY, 1},
        EntityInfo{20, PLDM_ENTITY_PROC, 1}};
    terminus.entityAssociations[20] =
        std::make_pair(procModuleEntity, processorModuleChildren);

    auto powerSupplyClosest = terminus.findInventory(
        EntityInfo{20, PLDM_ENTITY_POWER_SUPPLY, 1}, true);
    ASSERT_EQ(1u, powerSupplyClosest.size());
    EXPECT_EQ(cpuPath, powerSupplyClosest.front());

    terminus.entities.clear();
    terminus.entityAssociations[21] = std::make_pair(
        procModuleEntity,
        std::set<EntityInfo>{EntityInfo{21, PLDM_ENTITY_POWER_SUPPLY, 1}});
    auto powerSupplyWithoutCpu = terminus.findInventory(
        EntityInfo{21, PLDM_ENTITY_POWER_SUPPLY, 1}, true);
    ASSERT_EQ(1u, powerSupplyWithoutCpu.size());
    EXPECT_EQ(processorModulePath, powerSupplyWithoutCpu.front());
}

TEST_F(TerminusTest, privateGetterAndUpdateAssociationsCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000156");
    Terminus terminus(0x56, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setTerminusName("TerminusPrivate");
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis56";

    const std::string boardPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis56/board0"};
    terminus.inventories.emplace_back(boardPath, PLDM_ENTITY_SYS_BOARD, 1);
    terminus.inventoryParentMap[boardPath] = terminus.systemInventoryPath;
    EntityInfo containerEntity{overallSystemCotainerId, PLDM_ENTITY_SYS_BOARD,
                               1};
    terminus.entityAssociations.emplace(
        1, std::make_pair(containerEntity, std::set<EntityInfo>{}));

    pldm::platform_mc::AuxiliaryNames pdrAuxNames{{{"en", "AuxSensor"}}};
    terminus.sensorAuxiliaryNamesTbl.emplace_back(
        std::make_shared<SensorAuxiliaryNames>(0x52, 1, pdrAuxNames));

    pldm::platform_mc::AuxiliaryNames overwriteAuxNames{
        {{"en", "OverwriteSensor"}}};
    terminus.sensorAuxNameOverwriteTbl[0x53] = std::make_tuple(
        overwriteAuxNames,
        "/xyz/openbmc_project/inventory/system/chassis/chassis53");
    terminus.sensorAuxNameOverwriteTbl[0x54] = std::make_tuple(
        overwriteAuxNames, "/xyz/openbmc_project/not_inventory/chassis53");

#ifdef OEM_NVIDIA
    std::vector<pldm::dbus::PathAssociation> associations{
        {"chassis", "all_states", terminus.systemInventoryPath}};
    terminus.sensorPortInfoOverwriteTbl[0x53] = std::make_tuple(
        PortType::UpstreamPort, std::string("PCIe"), 32000, associations);
    terminus.sensorEventInfoOverwriteTbl[0x53] =
        std::make_shared<pldm::utils::SensorEventInfo>(
            "CPU56", std::unordered_map<std::string, std::string>{
                         {"PLDM_SENSOR_UPPERCRITICAL", "EID56"}});
#endif

    auto overwriteNames = terminus.getSensorAuxiliaryNames(0x53);
    ASSERT_NE(nullptr, overwriteNames);
    EXPECT_TRUE(terminus.getInventoryPath(0x53).has_value());
    EXPECT_FALSE(terminus.getInventoryPath(0x54).has_value());

#ifdef OEM_NVIDIA
    EXPECT_NE(nullptr, terminus.getSensorPortInfo(0x53));
    EXPECT_EQ(nullptr, terminus.getSensorPortInfo(0xFF));
    EXPECT_NE(nullptr, terminus.getSensorEventInfo(0x53));
    EXPECT_EQ(nullptr, terminus.getSensorEventInfo(0xFF));
#endif

    std::string associationPath = terminus.systemInventoryPath;
    auto pdrNoAux = makeNumericSensorValuePdrStruct(0x51);
    auto pdrWithAux = makeNumericSensorValuePdrStruct(0x52);
    auto pdrOverwrite = makeNumericSensorValuePdrStruct(0x53);
    std::string noAuxName{"sensor_no_aux_56"};
    std::string auxName{"sensor_aux_56"};
    std::string overwriteName{"sensor_overwrite_56"};
    auto noAuxSensor =
        std::make_shared<NumericSensor>(terminus.getTid(), false, pdrNoAux,
                                        noAuxName, associationPath, nullptr);
    auto auxSensor =
        std::make_shared<NumericSensor>(terminus.getTid(), false, pdrWithAux,
                                        auxName, associationPath, nullptr);
    auto overwriteSensor = std::make_shared<NumericSensor>(
        terminus.getTid(), false, pdrOverwrite, overwriteName, associationPath,
        nullptr);
    terminus.numericSensors.emplace_back(noAuxSensor);
    terminus.numericSensors.emplace_back(auxSensor);
    terminus.numericSensors.emplace_back(overwriteSensor);

    auto effecterPdr = makeNumericEffecterValuePdrStruct(0x61);
    std::string effecterName{"effecter_56"};
    auto effecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, effecterPdr, effecterName, associationPath,
        terminusManager);
    terminus.numericEffecters.emplace_back(effecter);

    auto stateInfo = makeSimpleStateSetInfo();
    auto stateSensor =
        std::make_shared<StateSensor>(terminus.getTid(), false, 0x62, stateInfo,
                                      nullptr, associationPath, nullptr);
    auto stateEffecter = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x63, stateInfo, nullptr, associationPath,
        terminusManager);
    terminus.stateSensors.emplace_back(stateSensor);
    terminus.stateEffecters.emplace_back(stateEffecter);

    terminusManager.numericSensorsWithoutAuxName = false;
    auto updateRc = syncWaitWithDbusIo(terminus.updateAssociations());
    ASSERT_TRUE(updateRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateRc));

    EXPECT_EQ(nullptr, noAuxSensor->valueIntf);
    EXPECT_NE(std::string::npos, auxSensor->path.find("TerminusPrivate_"));
    EXPECT_NE(std::string::npos, overwriteSensor->path.find("OverwriteSensor"));
}

TEST_F(TerminusTest, auxiliaryAndEffecterGetterMatrixCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000190");
    Terminus terminus(0x90, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    pldm::platform_mc::AuxiliaryNames sensorNames{{{"en", "SensorAux"}}};
    auto sensorAuxiliaryNames =
        std::make_shared<SensorAuxiliaryNames>(0x91, 1, sensorNames);
    terminus.sensorAuxiliaryNamesTbl.emplace_back(sensorAuxiliaryNames);

    pldm::platform_mc::AuxiliaryNames effecterNames{{{"en", "EffecterAux"}}};
    auto effecterAuxiliaryNames =
        std::make_shared<EffecterAuxiliaryNames>(0x92, 1, effecterNames);
    terminus.effecterAuxiliaryNamesTbl.emplace_back(effecterAuxiliaryNames);

    EXPECT_EQ(sensorAuxiliaryNames, terminus.getSensorAuxiliaryNames(0x91));
    EXPECT_EQ(nullptr, terminus.getSensorAuxiliaryNames(0x93));
    EXPECT_EQ(effecterAuxiliaryNames, terminus.getEffecterAuxiliaryNames(0x92));
    EXPECT_EQ(nullptr, terminus.getEffecterAuxiliaryNames(0x94));
}

TEST_F(TerminusTest, updateAssociationsNumericSensorInventoryCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000166");
    Terminus terminus(0x66, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setTerminusName("TerminusNumeric");
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis66";
    terminus.setInstance(7);

    const std::string boardPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis66/board0"};
    const std::string modulePath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis66/"
        "HGX_ProcessorModule_7"};
    const std::string cpuPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis66/CPU_7"};
    const std::string explicitPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis66/"
        "explicit_sensor0"};

    terminus.inventories.emplace_back(boardPath, PLDM_ENTITY_SYS_BOARD, 1);
    terminus.inventories.emplace_back(modulePath, PLDM_ENTITY_PROC_IO_MODULE,
                                      7);
    terminus.inventories.emplace_back(cpuPath, PLDM_ENTITY_PROC, 7);
    terminus.inventoryParentMap[boardPath] = terminus.systemInventoryPath;
    terminus.inventoryParentMap[modulePath] = terminus.systemInventoryPath;
    terminus.inventoryParentMap[cpuPath] = modulePath;

    EntityInfo boardEntity{overallSystemCotainerId, PLDM_ENTITY_SYS_BOARD, 1};
    EntityInfo moduleEntity{overallSystemCotainerId, PLDM_ENTITY_PROC_IO_MODULE,
                            1};
    EntityInfo procUnderModule{30, PLDM_ENTITY_PROC, 1};
    EntityInfo procAtRoot{overallSystemCotainerId, PLDM_ENTITY_PROC, 1};
    terminus.entityAssociations.emplace(
        1, std::make_pair(boardEntity, std::set<EntityInfo>{procUnderModule}));
    terminus.entityAssociations.emplace(
        2, std::make_pair(boardEntity, std::set<EntityInfo>{procUnderModule}));
    terminus.entityAssociations.emplace(
        30,
        std::make_pair(moduleEntity, std::set<EntityInfo>{procUnderModule}));
    terminus.entityAssociations.emplace(
        40, std::make_pair(procAtRoot, std::set<EntityInfo>{}));
    terminus.entityAssociations.emplace(
        41, std::make_pair(boardEntity, std::set<EntityInfo>{}));

    pldm::platform_mc::AuxiliaryNames directAuxNames{{{"en", "DirectNumeric"}}};
    terminus.sensorAuxNameOverwriteTbl[0x700] =
        std::make_tuple(directAuxNames, explicitPath);
    pldm::platform_mc::AuxiliaryNames moduleAuxNames{{{"en", "ModuleNumeric"}}};
    terminus.sensorAuxNameOverwriteTbl[0x702] =
        std::make_tuple(moduleAuxNames, std::string{});
    pldm::platform_mc::AuxiliaryNames countAuxNames{{{"en", "CountNumeric"}}};
    terminus.sensorAuxiliaryNamesTbl.emplace_back(
        std::make_shared<SensorAuxiliaryNames>(0x703, 1, countAuxNames));
    pldm::platform_mc::AuxiliaryNames countFallbackAuxNames{
        {{"en", "CountFallback"}}};
    terminus.sensorAuxiliaryNamesTbl.emplace_back(
        std::make_shared<SensorAuxiliaryNames>(0x704, 1,
                                               countFallbackAuxNames));

    std::string associationPath = terminus.systemInventoryPath;
    auto directPdr = makeNumericSensorValuePdrStruct(0x700);
    std::string directName{"direct_numeric_66"};
    auto directSensor =
        std::make_shared<NumericSensor>(terminus.getTid(), false, directPdr,
                                        directName, associationPath, nullptr);

    auto noAuxPdr = makeNumericSensorValuePdrStruct(0x701);
    std::string noAuxName{"no_aux_numeric_66"};
    auto noAuxSensor =
        std::make_shared<NumericSensor>(terminus.getTid(), false, noAuxPdr,
                                        noAuxName, associationPath, nullptr);

    auto modulePdr = makeNumericSensorValuePdrStruct(
        0x702, PLDM_ENTITY_PROC_IO_MODULE, 1, 30);
    std::string moduleName{"module_numeric_66"};
    auto moduleSensor =
        std::make_shared<NumericSensor>(terminus.getTid(), false, modulePdr,
                                        moduleName, associationPath, nullptr);

    auto countPdr =
        makeNumericSensorValuePdrStruct(0x703, PLDM_ENTITY_SYS_BOARD, 1, 40);
    countPdr->base_unit = PLDM_SENSOR_UNIT_COUNTS;
    std::string countName{"count_numeric_66"};
    auto countSensor =
        std::make_shared<NumericSensor>(terminus.getTid(), false, countPdr,
                                        countName, associationPath, nullptr);

    auto countFallbackPdr =
        makeNumericSensorValuePdrStruct(0x704, PLDM_ENTITY_SYS_BOARD, 1, 41);
    countFallbackPdr->base_unit = PLDM_SENSOR_UNIT_COUNTS;
    std::string countFallbackName{"count_fallback_66"};
    auto countFallbackSensor = std::make_shared<NumericSensor>(
        terminus.getTid(), false, countFallbackPdr, countFallbackName,
        associationPath, nullptr);

    terminus.numericSensors.emplace_back(directSensor);
    terminus.numericSensors.emplace_back(noAuxSensor);
    terminus.numericSensors.emplace_back(moduleSensor);
    terminus.numericSensors.emplace_back(countSensor);
    terminus.numericSensors.emplace_back(countFallbackSensor);

    terminusManager.numericSensorsWithoutAuxName = true;
    auto updateRc = syncWaitWithDbusIo(terminus.updateAssociations());
    ASSERT_TRUE(updateRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateRc));

    auto getAssociationPath = [](const std::shared_ptr<NumericSensor>& sensor) {
        const auto& assocs = sensor->associationDefinitionsIntf->associations();
        EXPECT_EQ(1u, assocs.size());
        if (assocs.empty())
        {
            return std::string{};
        }
        return std::get<2>(assocs.front());
    };

    EXPECT_EQ("DirectNumeric", directSensor->getSensorName());
    EXPECT_EQ(explicitPath, getAssociationPath(directSensor));

    EXPECT_EQ("no_aux_numeric_66", noAuxSensor->getSensorName());
    EXPECT_NE(nullptr, noAuxSensor->valueIntf);
    EXPECT_EQ(boardPath, getAssociationPath(noAuxSensor));

    EXPECT_EQ("ModuleNumeric", moduleSensor->getSensorName());
    EXPECT_EQ(cpuPath, getAssociationPath(moduleSensor));

    EXPECT_EQ("TerminusNumeric_CountNumeric", countSensor->getSensorName());
    EXPECT_EQ(cpuPath, getAssociationPath(countSensor));

    EXPECT_EQ("TerminusNumeric_CountFallback",
              countFallbackSensor->getSensorName());
    EXPECT_EQ(boardPath, getAssociationPath(countFallbackSensor));
}

TEST_F(TerminusTest, updateAssociationsNumericSensorFallbackCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000167");
    Terminus terminus(0x67, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis67";
    terminus.setInstance(8);

    const std::string boardPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis67/board0"};
    const std::string modulePath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis67/"
        "HGX_ProcessorModule_8"};

    terminus.inventories.emplace_back(boardPath, PLDM_ENTITY_SYS_BOARD, 1);
    terminus.inventories.emplace_back(modulePath, PLDM_ENTITY_PROC_IO_MODULE,
                                      8);
    terminus.inventoryParentMap[boardPath] = terminus.systemInventoryPath;
    terminus.inventoryParentMap[modulePath] = terminus.systemInventoryPath;

    EntityInfo moduleEntity{overallSystemCotainerId, PLDM_ENTITY_PROC_IO_MODULE,
                            1};
    terminus.entityAssociations.emplace(
        30, std::make_pair(moduleEntity, std::set<EntityInfo>{EntityInfo{
                                             30, PLDM_ENTITY_SYS_BOARD, 1}}));

    pldm::platform_mc::AuxiliaryNames moduleAuxNames{
        {{"en", "ModuleFallback"}}};
    terminus.sensorAuxNameOverwriteTbl[0x710] =
        std::make_tuple(moduleAuxNames, std::string{});
    pldm::platform_mc::AuxiliaryNames countAuxNames{
        {{"en", "CountMissingContainer"}}};
    terminus.sensorAuxiliaryNamesTbl.emplace_back(
        std::make_shared<SensorAuxiliaryNames>(0x711, 1, countAuxNames));

    std::string associationPath = terminus.systemInventoryPath;
    auto modulePdr = makeNumericSensorValuePdrStruct(
        0x710, PLDM_ENTITY_PROC_IO_MODULE, 1, 30);
    std::string moduleName{"module_fallback_67"};
    auto moduleSensor =
        std::make_shared<NumericSensor>(terminus.getTid(), false, modulePdr,
                                        moduleName, associationPath, nullptr);

    auto countPdr =
        makeNumericSensorValuePdrStruct(0x711, PLDM_ENTITY_SYS_BOARD, 1, 99);
    countPdr->base_unit = PLDM_SENSOR_UNIT_COUNTS;
    std::string countName{"count_missing_container_67"};
    auto countSensor =
        std::make_shared<NumericSensor>(terminus.getTid(), false, countPdr,
                                        countName, associationPath, nullptr);

    terminus.numericSensors.emplace_back(moduleSensor);
    terminus.numericSensors.emplace_back(countSensor);

    terminusManager.numericSensorsWithoutAuxName = true;
    auto updateRc = syncWaitWithDbusIo(terminus.updateAssociations());
    ASSERT_TRUE(updateRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateRc));

    const auto& moduleAssocs =
        moduleSensor->associationDefinitionsIntf->associations();
    ASSERT_EQ(1u, moduleAssocs.size());
    EXPECT_EQ(modulePath, std::get<2>(moduleAssocs.front()));

    const auto& countAssocs =
        countSensor->associationDefinitionsIntf->associations();
    ASSERT_EQ(1u, countAssocs.size());
    EXPECT_EQ(boardPath, std::get<2>(countAssocs.front()));
}

TEST_F(TerminusTest, updateAssociationsStateSensorInventoryCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000168");
    Terminus terminus(0x68, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis68";
    terminus.setInstance(5);

    const std::string boardPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis68/board0"};
    const std::string cpuPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis68/CPU_5"};
    const std::string dimm1Path{
        "/xyz/openbmc_project/inventory/system/chassis/chassis68/DIMM_1"};
    const std::string dimm2Path{
        "/xyz/openbmc_project/inventory/system/chassis/chassis68/DIMM_2"};
    const std::string dimm3Path{
        "/xyz/openbmc_project/inventory/system/chassis/chassis68/DIMM_3"};
    const std::string explicitStatePath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis68/"
        "explicit_state0"};

    terminus.inventories.emplace_back(boardPath, PLDM_ENTITY_SYS_BOARD, 1);
    terminus.inventories.emplace_back(cpuPath, PLDM_ENTITY_PROC, 5);
    terminus.inventories.emplace_back(dimm1Path, PLDM_ENTITY_MEMORY_CONTROLLER,
                                      1);
    terminus.inventories.emplace_back(dimm2Path, PLDM_ENTITY_MEMORY_CONTROLLER,
                                      2);
    terminus.inventories.emplace_back(dimm3Path, PLDM_ENTITY_MEMORY_CONTROLLER,
                                      3);
    terminus.inventoryParentMap[boardPath] = terminus.systemInventoryPath;
    terminus.inventoryParentMap[cpuPath] = terminus.systemInventoryPath;
    terminus.inventoryParentMap[dimm1Path] = cpuPath;
    terminus.inventoryParentMap[dimm2Path] = boardPath;
    terminus.inventoryParentMap[dimm3Path] = boardPath;

    EntityInfo boardEntity{overallSystemCotainerId, PLDM_ENTITY_SYS_BOARD, 1};
    EntityInfo procEntity{overallSystemCotainerId, PLDM_ENTITY_PROC, 1};
    terminus.entityAssociations.emplace(
        1, std::make_pair(boardEntity, std::set<EntityInfo>{}));
    terminus.entityAssociations.emplace(
        50, std::make_pair(procEntity, std::set<EntityInfo>{}));
    terminus.entityAssociations.emplace(
        51, std::make_pair(boardEntity, std::set<EntityInfo>{}));

    pldm::platform_mc::AuxiliaryNames explicitStateNames{
        {{"en", "ExplicitState"}}};
    terminus.sensorAuxNameOverwriteTbl[0x800] =
        std::make_tuple(explicitStateNames, explicitStatePath);

    StateSetData healthStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_HEALTHSTATE),
                        PossibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                       PLDM_STATESET_HEALTH_STATE_CRITICAL});
    StateSetData performanceStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_PERFORMANCE),
                        PossibleStates{PLDM_STATESET_PERFORMANCE_NORMAL,
                                       PLDM_STATESET_PERFORMANCE_THROTTLED});

    std::string associationPath = terminus.systemInventoryPath;
    auto explicitStateSensor = std::make_shared<StateSensor>(
        terminus.getTid(), false, 0x800,
        makeSingleStateSetInfo(PLDM_ENTITY_SYS_BOARD,
                               PLDM_STATESET_ID_HEALTHSTATE),
        nullptr, associationPath, nullptr);
    auto memGlobalSensor = std::make_shared<StateSensor>(
        terminus.getTid(), false, 0x801,
        StateSetInfo{EntityInfo{50, PLDM_ENTITY_MEMORY_CONTROLLER, 1},
                     std::vector<StateSetData>{healthStateData}},
        nullptr, associationPath, nullptr);
    auto memPerformanceSensor = std::make_shared<StateSensor>(
        terminus.getTid(), false, 0x802,
        StateSetInfo{EntityInfo{50, PLDM_ENTITY_MEMORY_CONTROLLER, 1},
                     std::vector<StateSetData>{performanceStateData}},
        nullptr, associationPath, nullptr);
    auto memBoardSensor = std::make_shared<StateSensor>(
        terminus.getTid(), false, 0x803,
        StateSetInfo{EntityInfo{51, PLDM_ENTITY_MEMORY_CONTROLLER, 2},
                     std::vector<StateSetData>{healthStateData}},
        nullptr, associationPath, nullptr);
    auto memMissingContainerSensor = std::make_shared<StateSensor>(
        terminus.getTid(), false, 0x804,
        StateSetInfo{EntityInfo{52, PLDM_ENTITY_MEMORY_CONTROLLER, 3},
                     std::vector<StateSetData>{healthStateData}},
        nullptr, associationPath, nullptr);

    terminus.stateSensors.emplace_back(explicitStateSensor);
    terminus.stateSensors.emplace_back(memGlobalSensor);
    terminus.stateSensors.emplace_back(memPerformanceSensor);
    terminus.stateSensors.emplace_back(memBoardSensor);
    terminus.stateSensors.emplace_back(memMissingContainerSensor);

    auto updateRc = syncWaitWithDbusIo(terminus.updateAssociations());
    ASSERT_TRUE(updateRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateRc));

    EXPECT_EQ("explicit_state0", explicitStateSensor->getAssociationEntityId());
    EXPECT_EQ("CPU_5", memGlobalSensor->getAssociationEntityId());
    EXPECT_EQ("DIMM_1", memPerformanceSensor->getAssociationEntityId());
    EXPECT_EQ("DIMM_2", memBoardSensor->getAssociationEntityId());
    EXPECT_EQ("DIMM_3", memMissingContainerSensor->getAssociationEntityId());
}

#ifdef OEM_NVIDIA
TEST_F(TerminusTest, nvidiaInitTerminusPowerCapAndStorageCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000157");
    Terminus terminus(0x57, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis57"};

    auto numericEffecterPdr = makeNumericEffecterValuePdrStruct(0x710);
    std::string numericEffecterName{"oem_numeric_effecter_710"};
    auto numericEffecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, numericEffecterPdr, numericEffecterName,
        associationPath, terminusManager);
    terminus.numericEffecters.emplace_back(numericEffecter);

    auto stateInfo = makeSingleStateSetInfo(PLDM_ENTITY_SYS_BOARD,
                                            PLDM_STATESET_ID_BOOT_REQUEST);
    auto stateEffecter = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x711, stateInfo, nullptr, associationPath,
        terminusManager);
    terminus.stateEffecters.emplace_back(stateEffecter);

    nvidia::nvidia_oem_effecter_powercap_pdr powerCapPdr{};
    powerCapPdr.terminus_handle = 1;
    powerCapPdr.oem_pdr_type = static_cast<uint8_t>(
        nvidia::NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_EFFECTER_POWERCAP);
    powerCapPdr.oem_effecter_powercap = static_cast<uint8_t>(
        nvidia::OemPowerCapPersistence::OEM_POWERCAP_TDP_NONVOLATILE);
    powerCapPdr.associated_effecterid = 0x710;

    nvidia::nvidia_oem_effecter_storage_pdr storagePdr{};
    storagePdr.terminus_handle = 1;
    storagePdr.oem_pdr_type = static_cast<uint8_t>(
        nvidia::NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_EFFECTER_STORAGE);
    storagePdr.oem_effecter_storage = static_cast<uint8_t>(
        nvidia::OemStorageSecureState::OEM_STORAGE_SECURE_VARIABLE);
    storagePdr.associated_effecterid = 0x711;

    auto truncatedPdrData = structToBytes(powerCapPdr);
    truncatedPdrData.resize(sizeof(nvidia::nvidia_oem_pdr));

    nvidia::nvidia_oem_pdr unknownTypePdr{};
    unknownTypePdr.terminus_handle = 1;
    unknownTypePdr.oem_pdr_type = 0xFF;

    // OemRecordId must match the effecter IDs (0x710/0x711) because
    // processEffecterPowerCapPdr/processEffecterStoragePdr now compare
    // effecter->effecterId against the PLDM OEM PDR header's oemRecordId
    // (not the vendor-specific associated_effecterid).
    terminus.oemPdrs.emplace_back(static_cast<uint32_t>(0xFFFF),
                                  static_cast<OemRecordId>(0x710),
                                  structToBytes(powerCapPdr));
    terminus.oemPdrs.emplace_back(
        nvidia::NvidiaIana, static_cast<OemRecordId>(2), truncatedPdrData);
    terminus.oemPdrs.emplace_back(nvidia::NvidiaIana,
                                  static_cast<OemRecordId>(0x710),
                                  structToBytes(powerCapPdr));
    terminus.oemPdrs.emplace_back(nvidia::NvidiaIana,
                                  static_cast<OemRecordId>(0x711),
                                  structToBytes(storagePdr));
    terminus.oemPdrs.emplace_back(nvidia::NvidiaIana,
                                  static_cast<OemRecordId>(5),
                                  structToBytes(unknownTypePdr));

    nvidia::nvidiaInitTerminus(terminus);

    ASSERT_EQ(1u, numericEffecter->oemIntfs.size());
    auto persistenceIntf =
        std::dynamic_pointer_cast<nvidia::OemPersistenceIntf>(
            numericEffecter->oemIntfs[0]);
    ASSERT_NE(nullptr, persistenceIntf);
    EXPECT_TRUE(persistenceIntf->persistent());

    ASSERT_EQ(1u, stateEffecter->oemIntfs.size());
    auto* storageIntf =
        dynamic_cast<nvidia::OemStorageIntf*>(stateEffecter->oemIntfs[0].get());
    ASSERT_NE(nullptr, storageIntf);
    EXPECT_TRUE(storageIntf->secure());
}

TEST_F(TerminusTest, nvidiaInitTerminusSkipsTooSmallCommonPdrCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000015d");
    Terminus terminus(0x5D, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis5D"};

    auto numericEffecterPdr = makeNumericEffecterValuePdrStruct(0x712);
    std::string numericEffecterName{"oem_numeric_effecter_712"};
    auto numericEffecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, numericEffecterPdr, numericEffecterName,
        associationPath, terminusManager);
    terminus.numericEffecters.emplace_back(numericEffecter);

    nvidia::nvidia_oem_effecter_powercap_pdr powerCapPdr{};
    powerCapPdr.terminus_handle = 1;
    powerCapPdr.oem_pdr_type = static_cast<uint8_t>(
        nvidia::NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_EFFECTER_POWERCAP);
    powerCapPdr.oem_effecter_powercap = static_cast<uint8_t>(
        nvidia::OemPowerCapPersistence::OEM_POWERCAP_TDP_NONVOLATILE);
    powerCapPdr.associated_effecterid = 0x712;

    std::vector<uint8_t> tooSmallPdr(sizeof(nvidia::nvidia_oem_pdr) - 1, 0xAA);
    terminus.oemPdrs.emplace_back(nvidia::NvidiaIana,
                                  static_cast<OemRecordId>(1), tooSmallPdr);
    terminus.oemPdrs.emplace_back(nvidia::NvidiaIana,
                                  static_cast<OemRecordId>(0x712),
                                  structToBytes(powerCapPdr));

    nvidia::nvidiaInitTerminus(terminus);

    ASSERT_EQ(1u, numericEffecter->oemIntfs.size());
    auto persistenceIntf =
        std::dynamic_pointer_cast<nvidia::OemPersistenceIntf>(
            numericEffecter->oemIntfs[0]);
    ASSERT_NE(nullptr, persistenceIntf);
    EXPECT_TRUE(persistenceIntf->persistent());
}

TEST_F(TerminusTest, nvidiaInitTerminusSkipsTooSmallTypedPdrCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000015e");
    Terminus terminus(0x5E, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis5E"};

    auto numericEffecterPdr = makeNumericEffecterValuePdrStruct(0x715);
    std::string numericEffecterName{"oem_numeric_effecter_715"};
    auto numericEffecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, numericEffecterPdr, numericEffecterName,
        associationPath, terminusManager);
    terminus.numericEffecters.emplace_back(numericEffecter);

    auto stateInfo = makeSingleStateSetInfo(PLDM_ENTITY_SYS_BOARD,
                                            PLDM_STATESET_ID_BOOT_REQUEST);
    auto stateEffecter = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x716, stateInfo, nullptr, associationPath,
        terminusManager);
    terminus.stateEffecters.emplace_back(stateEffecter);

    nvidia::nvidia_oem_effecter_powercap_pdr powerCapPdr{};
    powerCapPdr.terminus_handle = 1;
    powerCapPdr.oem_pdr_type = static_cast<uint8_t>(
        nvidia::NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_EFFECTER_POWERCAP);
    powerCapPdr.oem_effecter_powercap = static_cast<uint8_t>(
        nvidia::OemPowerCapPersistence::OEM_POWERCAP_TDP_NONVOLATILE);
    powerCapPdr.associated_effecterid = 0x715;

    nvidia::nvidia_oem_effecter_storage_pdr storagePdr{};
    storagePdr.terminus_handle = 1;
    storagePdr.oem_pdr_type = static_cast<uint8_t>(
        nvidia::NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_EFFECTER_STORAGE);
    storagePdr.oem_effecter_storage = static_cast<uint8_t>(
        nvidia::OemStorageSecureState::OEM_STORAGE_SECURE_VARIABLE);
    storagePdr.associated_effecterid = 0x716;

    auto truncatedPowerCap = structToBytes(powerCapPdr);
    truncatedPowerCap.resize(
        sizeof(nvidia::nvidia_oem_effecter_powercap_pdr) - 1);
    auto truncatedStorage = structToBytes(storagePdr);
    truncatedStorage.resize(
        sizeof(nvidia::nvidia_oem_effecter_storage_pdr) - 1);

    terminus.oemPdrs.emplace_back(
        nvidia::NvidiaIana, static_cast<OemRecordId>(1), truncatedPowerCap);
    terminus.oemPdrs.emplace_back(
        nvidia::NvidiaIana, static_cast<OemRecordId>(2), truncatedStorage);
    terminus.oemPdrs.emplace_back(nvidia::NvidiaIana,
                                  static_cast<OemRecordId>(0x715),
                                  structToBytes(powerCapPdr));
    terminus.oemPdrs.emplace_back(nvidia::NvidiaIana,
                                  static_cast<OemRecordId>(0x716),
                                  structToBytes(storagePdr));

    nvidia::nvidiaInitTerminus(terminus);

    ASSERT_EQ(1u, numericEffecter->oemIntfs.size());
    EXPECT_NE(nullptr, std::dynamic_pointer_cast<nvidia::OemPersistenceIntf>(
                           numericEffecter->oemIntfs[0]));

    ASSERT_EQ(1u, stateEffecter->oemIntfs.size());
    EXPECT_NE(nullptr, dynamic_cast<nvidia::OemStorageIntf*>(
                           stateEffecter->oemIntfs[0].get()));
}

TEST_F(TerminusTest, nvidiaInitTerminusVolatilePowerCapCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000157");
    Terminus terminus(0x5A, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis5A"};

    auto numericEffecterPdr = makeNumericEffecterValuePdrStruct(0x713);
    std::string numericEffecterName{"oem_numeric_effecter_713"};
    auto numericEffecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, numericEffecterPdr, numericEffecterName,
        associationPath, terminusManager);
    terminus.numericEffecters.emplace_back(numericEffecter);

    nvidia::nvidia_oem_effecter_powercap_pdr powerCapPdr{};
    powerCapPdr.terminus_handle = 1;
    powerCapPdr.oem_pdr_type = static_cast<uint8_t>(
        nvidia::NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_EFFECTER_POWERCAP);
    powerCapPdr.oem_effecter_powercap = static_cast<uint8_t>(
        nvidia::OemPowerCapPersistence::OEM_POWERCAP_TDP_VOLATILE);
    powerCapPdr.associated_effecterid = 0x713;

    terminus.oemPdrs.emplace_back(nvidia::NvidiaIana,
                                  static_cast<OemRecordId>(0x713),
                                  structToBytes(powerCapPdr));

    nvidia::nvidiaInitTerminus(terminus);

    ASSERT_EQ(1u, numericEffecter->oemIntfs.size());
    auto persistenceIntf =
        std::dynamic_pointer_cast<nvidia::OemPersistenceIntf>(
            numericEffecter->oemIntfs[0]);
    ASSERT_NE(nullptr, persistenceIntf);
    EXPECT_FALSE(persistenceIntf->persistent());
}

TEST_F(TerminusTest, nvidiaInitTerminusStorageNonSecureCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000015b");
    Terminus terminus(0x5B, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis5B"};

    auto stateInfo = makeSingleStateSetInfo(PLDM_ENTITY_SYS_BOARD,
                                            PLDM_STATESET_ID_BOOT_REQUEST);
    auto stateEffecter = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x714, stateInfo, nullptr, associationPath,
        terminusManager);
    terminus.stateEffecters.emplace_back(stateEffecter);

    nvidia::nvidia_oem_effecter_storage_pdr storagePdr{};
    storagePdr.terminus_handle = 1;
    storagePdr.oem_pdr_type = static_cast<uint8_t>(
        nvidia::NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_EFFECTER_STORAGE);
    storagePdr.oem_effecter_storage = static_cast<uint8_t>(
        nvidia::OemStorageSecureState::OEM_STORAGE_NONSECURE_VARIABLE);
    storagePdr.associated_effecterid = 0x714;

    terminus.oemPdrs.emplace_back(nvidia::NvidiaIana,
                                  static_cast<OemRecordId>(0x714),
                                  structToBytes(storagePdr));

    nvidia::nvidiaInitTerminus(terminus);

    ASSERT_EQ(1u, stateEffecter->oemIntfs.size());
    auto* storageIntf =
        dynamic_cast<nvidia::OemStorageIntf*>(stateEffecter->oemIntfs[0].get());
    ASSERT_NE(nullptr, storageIntf);
    EXPECT_FALSE(storageIntf->secure());
}

TEST_F(TerminusTest, nvidiaInitTerminusEmptyCollectionsCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000015c");
    Terminus terminus(0x5C, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    nvidia::nvidiaInitTerminus(terminus);

    EXPECT_TRUE(terminus.stateEffecters.empty());
    EXPECT_TRUE(terminus.numericEffecters.empty());
    EXPECT_TRUE(terminus.stateSensors.empty());
}

TEST_F(TerminusTest, nvidiaRemoteDebugAndStaticPowerHintCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000158");
    Terminus terminus(0x58, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis58"};

    auto debugEffecterInfo = makeDebugStateSetInfo(PLDM_ENTITY_SYS_BOARD, 6);
    auto debugSensorInfo = makeDebugStateSetInfo(PLDM_ENTITY_SYS_BOARD, 6);
    auto debugStateEffecter = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x720, debugEffecterInfo, nullptr,
        associationPath, terminusManager);
    auto debugStateSensor = std::make_shared<StateSensor>(
        terminus.getTid(), false, 0x721, debugSensorInfo, nullptr,
        associationPath, nullptr);
    terminus.stateEffecters.emplace_back(debugStateEffecter);
    terminus.stateSensors.emplace_back(debugStateSensor);

    auto createNumericEffecter =
        [&](uint16_t effecterId, uint8_t baseUnit, uint8_t minValue,
            uint8_t maxValue, const std::string& effecterName) {
            auto pdr = makeNumericEffecterValuePdrStruct(effecterId);
            pdr->base_unit = baseUnit;
            pdr->min_settable.value_u8 = minValue;
            pdr->max_settable.value_u8 = maxValue;
            std::string name = effecterName;
            auto effecter = std::make_shared<NumericEffecter>(
                terminus.getTid(), false, pdr, name, associationPath,
                terminusManager);
            terminus.numericEffecters.emplace_back(effecter);
            return effecter;
        };

    auto remoteDebugNumericEffecter = createNumericEffecter(
        0x722, PLDM_SENSOR_UNIT_MINUTES, 1, 60, "remote_debug_timeout");
    auto staticPowerTemperatureEffecter = createNumericEffecter(
        0x723, PLDM_SENSOR_UNIT_DEGRESS_C, 10, 80, "static_power_temp");
    auto staticPowerWorkloadEffecter = createNumericEffecter(
        0x724, PLDM_SENSOR_UNIT_NONE, 1, 100, "static_power_workload");
    auto staticPowerClockEffecter = createNumericEffecter(
        0x725, PLDM_SENSOR_UNIT_HERTZ, 20, 200, "static_power_clock");
    auto staticPowerPowerEffecter = createNumericEffecter(
        0x726, PLDM_SENSOR_UNIT_WATTS, 5, 250, "static_power_power");

    nvidia::nvidiaInitTerminus(terminus);

    ASSERT_EQ(1u, debugStateEffecter->oemIntfs.size());
    auto* remoteDebugIntf = dynamic_cast<oem_nvidia::OemRemoteDebugIntf*>(
        debugStateEffecter->oemIntfs[0].get());
    ASSERT_NE(nullptr, remoteDebugIntf);

    debugStateSensor->stateSets[0]->setValue(
        PLDM_STATE_SET_DEBUG_STATE_ENABLED);
    debugStateSensor->stateSets[1]->setValue(
        PLDM_STATE_SET_DEBUG_STATE_DISABLED);
    debugStateSensor->stateSets[2]->setValue(
        PLDM_STATE_SET_DEBUG_STATE_OFFLINE);
    debugStateSensor->stateSets[3]->setValue(0xFE);
    debugStateSensor->stateSets[4]->setValue(
        PLDM_STATE_SET_DEBUG_STATE_ENABLED);
    debugStateSensor->stateSets[5]->setValue(
        PLDM_STATE_SET_DEBUG_STATE_ENABLED);
    for (auto& stateSet : debugStateEffecter->stateSets)
    {
        stateSet->setOpState(EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING);
    }
    debugStateEffecter->stateSets[4]->setOpState(
        EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING);

    EXPECT_EQ(oem_nvidia::DebugState::Enabled, remoteDebugIntf->jtagDebug());
    EXPECT_EQ(oem_nvidia::DebugState::Disabled, remoteDebugIntf->deviceDebug());
    EXPECT_EQ(oem_nvidia::DebugState::Offline,
              remoteDebugIntf->securePrivilegeNonInvasiveDebug());
    EXPECT_EQ(oem_nvidia::DebugState::Unknown,
              remoteDebugIntf->securePrivilegeInvasiveDebug());
    EXPECT_EQ(oem_nvidia::DebugState::Pending,
              remoteDebugIntf->nonInvasiveDebug());
    EXPECT_EQ(oem_nvidia::DebugState::Enabled,
              remoteDebugIntf->invasiveDebug());
    EXPECT_EQ(0, remoteDebugIntf->toCompId(oem_nvidia::DebugPolicy::JtagDebug));
    EXPECT_EQ(1,
              remoteDebugIntf->toCompId(oem_nvidia::DebugPolicy::DeviceDebug));
    EXPECT_EQ(2, remoteDebugIntf->toCompId(
                     oem_nvidia::DebugPolicy::SecurePrivilegeNonInvasiveDebug));
    EXPECT_EQ(3, remoteDebugIntf->toCompId(
                     oem_nvidia::DebugPolicy::SecurePrivilegeInvasiveDebug));
    EXPECT_EQ(4, remoteDebugIntf->toCompId(
                     oem_nvidia::DebugPolicy::NonInvasiveDebug));
    EXPECT_EQ(
        5, remoteDebugIntf->toCompId(oem_nvidia::DebugPolicy::InvasiveDebug));
    EXPECT_EQ(255, remoteDebugIntf->toCompId(
                       static_cast<oem_nvidia::DebugPolicy>(0xFF)));
    EXPECT_EQ(oem_nvidia::DebugState::Unknown,
              remoteDebugIntf->toDebugState(0xFF));

    remoteDebugIntf->timeout(17, true);
    (void)remoteDebugIntf->timeout();

    EXPECT_THROW(
        remoteDebugIntf->enable({static_cast<oem_nvidia::DebugPolicy>(0xFF)}),
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument);
    EXPECT_THROW(
        remoteDebugIntf->disable({static_cast<oem_nvidia::DebugPolicy>(0xFF)}),
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument);
    EXPECT_THROW(
        remoteDebugIntf->enable(
            {oem_nvidia::DebugPolicy::SecurePrivilegeNonInvasiveDebug}),
        sdbusplus::xyz::openbmc_project::Common::Error::NotAllowed);
    EXPECT_THROW(
        remoteDebugIntf->disable(
            {oem_nvidia::DebugPolicy::SecurePrivilegeNonInvasiveDebug}),
        sdbusplus::xyz::openbmc_project::Common::Error::NotAllowed);

    debugStateSensor->stateSets[2]->setValue(
        PLDM_STATE_SET_DEBUG_STATE_ENABLED);
    EXPECT_NO_THROW(remoteDebugIntf->enable(
        {oem_nvidia::DebugPolicy::JtagDebug,
         oem_nvidia::DebugPolicy::DeviceDebug}));
    EXPECT_NO_THROW(remoteDebugIntf->disable(
        {oem_nvidia::DebugPolicy::JtagDebug,
         oem_nvidia::DebugPolicy::DeviceDebug}));
    EXPECT_NO_THROW(remoteDebugIntf->enable(
        {oem_nvidia::DebugPolicy::SecurePrivilegeNonInvasiveDebug,
         oem_nvidia::DebugPolicy::SecurePrivilegeInvasiveDebug,
         oem_nvidia::DebugPolicy::NonInvasiveDebug,
         oem_nvidia::DebugPolicy::InvasiveDebug}));
    EXPECT_NO_THROW(remoteDebugIntf->disable(
        {oem_nvidia::DebugPolicy::SecurePrivilegeNonInvasiveDebug,
         oem_nvidia::DebugPolicy::SecurePrivilegeInvasiveDebug,
         oem_nvidia::DebugPolicy::NonInvasiveDebug,
         oem_nvidia::DebugPolicy::InvasiveDebug}));

    ASSERT_EQ(1u, staticPowerPowerEffecter->oemIntfs.size());
    auto staticPowerIntf = std::dynamic_pointer_cast<OemStaticPowerHintInft>(
        staticPowerPowerEffecter->oemIntfs[0]);
    ASSERT_NE(nullptr, staticPowerIntf);

    const auto maxClock = staticPowerIntf->maxCpuClockFrequency();
    const auto minClock = staticPowerIntf->minCpuClockFrequency();
    const auto maxWorkload = staticPowerIntf->maxWorkloadFactor();
    const auto minWorkload = staticPowerIntf->minWorkloadFactor();
    const auto maxTemperature = staticPowerIntf->maxTemperature();
    const auto minTemperature = staticPowerIntf->minTemperature();
    const auto minCores = staticPowerIntf->minNumberOfCores();

    EXPECT_GT(maxClock, minClock);
    EXPECT_GT(maxWorkload, minWorkload);
    EXPECT_GT(maxTemperature, minTemperature);

    EXPECT_THROW(
        staticPowerIntf->estimatePower(maxClock + 1, minWorkload,
                                       minTemperature, minCores),
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument);
    EXPECT_THROW(
        staticPowerIntf->estimatePower(minClock, maxWorkload + 1,
                                       minTemperature, minCores),
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument);
    EXPECT_THROW(
        staticPowerIntf->estimatePower(minClock, minWorkload,
                                       maxTemperature + 1, minCores),
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument);

    const double validClock = (maxClock + minClock) / 2.0;
    const double validWorkload = (maxWorkload + minWorkload) / 2.0;
    const double validTemperature = (maxTemperature + minTemperature) / 2.0;

    EXPECT_NO_THROW(staticPowerIntf->estimatePower(validClock, validWorkload,
                                                   validTemperature, minCores));
    runEventLoopForMilliseconds(10);
    try
    {
        staticPowerIntf->estimatePower(validClock, validWorkload,
                                       validTemperature, minCores);
    }
    catch (const sdbusplus::xyz::openbmc_project::Common::Error::Unavailable&)
    {}

    staticPowerClockEffecter->unitIntf.reset();
    staticPowerTemperatureEffecter->unitIntf.reset();
    staticPowerWorkloadEffecter->unitIntf.reset();
    EXPECT_EQ(0, staticPowerIntf->maxCpuClockFrequency());
    EXPECT_EQ(0, staticPowerIntf->minCpuClockFrequency());
    EXPECT_EQ(0, staticPowerIntf->maxTemperature());
    EXPECT_EQ(0, staticPowerIntf->minTemperature());
    EXPECT_EQ(0, staticPowerIntf->maxWorkloadFactor());
    EXPECT_EQ(0, staticPowerIntf->minWorkloadFactor());

    EXPECT_NE(nullptr, remoteDebugNumericEffecter);
    EXPECT_NE(nullptr, staticPowerTemperatureEffecter);
    EXPECT_NE(nullptr, staticPowerWorkloadEffecter);
    EXPECT_NE(nullptr, staticPowerClockEffecter);
}

TEST_F(TerminusTest, nvidiaStaticPowerHintFailureCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000166");
    Terminus terminus(0x66, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis66"};

    auto createNumericEffecter =
        [&](uint16_t effecterId, uint8_t baseUnit, uint8_t minValue,
            uint8_t maxValue, const std::string& effecterName) {
            auto pdr = makeNumericEffecterValuePdrStruct(effecterId);
            pdr->base_unit = baseUnit;
            pdr->min_settable.value_u8 = minValue;
            pdr->max_settable.value_u8 = maxValue;
            std::string name = effecterName;
            auto effecter = std::make_shared<NumericEffecter>(
                terminus.getTid(), false, pdr, name, associationPath,
                terminusManager);
            terminus.numericEffecters.emplace_back(effecter);
            return effecter;
        };

    auto staticPowerTemperatureEffecter = createNumericEffecter(
        0x760, PLDM_SENSOR_UNIT_DEGRESS_C, 10, 80, "static_power_temp_fail");
    auto staticPowerWorkloadEffecter = createNumericEffecter(
        0x761, PLDM_SENSOR_UNIT_NONE, 1, 100, "static_power_workload_fail");
    auto staticPowerClockEffecter = createNumericEffecter(
        0x762, PLDM_SENSOR_UNIT_HERTZ, 20, 200, "static_power_clock_fail");
    auto staticPowerPowerEffecter = createNumericEffecter(
        0x763, PLDM_SENSOR_UNIT_WATTS, 5, 250, "static_power_power_fail");

    nvidia::nvidiaInitTerminus(terminus);

    ASSERT_EQ(1u, staticPowerPowerEffecter->oemIntfs.size());
    auto staticPowerIntf = std::dynamic_pointer_cast<OemStaticPowerHintInft>(
        staticPowerPowerEffecter->oemIntfs[0]);
    ASSERT_NE(nullptr, staticPowerIntf);

    const auto maxClock = staticPowerIntf->maxCpuClockFrequency();
    const auto minClock = staticPowerIntf->minCpuClockFrequency();
    const auto maxWorkload = staticPowerIntf->maxWorkloadFactor();
    const auto minWorkload = staticPowerIntf->minWorkloadFactor();
    const auto maxTemperature = staticPowerIntf->maxTemperature();
    const auto minTemperature = staticPowerIntf->minTemperature();
    const auto minCores = staticPowerIntf->minNumberOfCores();

    const double validClock = (maxClock + minClock) / 2.0;
    const double validWorkload = (maxWorkload + minWorkload) / 2.0;
    const double validTemperature = (maxTemperature + minTemperature) / 2.0;

    EXPECT_THROW(
        staticPowerIntf->estimatePower(minClock - 1, validWorkload,
                                       validTemperature, minCores),
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument);
    EXPECT_THROW(
        staticPowerIntf->estimatePower(validClock, minWorkload - 1,
                                       validTemperature, minCores),
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument);
    EXPECT_THROW(
        staticPowerIntf->estimatePower(validClock, validWorkload,
                                       minTemperature - 1, minCores),
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument);

    auto runFailureEstimate = [&](std::vector<std::vector<uint8_t>> responses) {
        ASSERT_EQ(PLDM_SUCCESS, terminusManager.clearQueuedResponses());
        for (auto response : responses)
        {
            ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
        }
        EXPECT_NO_THROW(staticPowerIntf->estimatePower(
            validClock, validWorkload, validTemperature, minCores));
        runEventLoopForMilliseconds(10);
    };

    runFailureEstimate({makeSetNumericEffecterValueResp(PLDM_ERROR),
                        makeGetNumericEffecterValueResp(
                            PLDM_EFFECTER_DATA_SIZE_UINT8,
                            EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING)});

    runFailureEstimate(
        {makeSetNumericEffecterValueResp(),
         makeGetNumericEffecterValueResp(
             PLDM_EFFECTER_DATA_SIZE_UINT8,
             EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING),
         makeSetNumericEffecterValueResp(PLDM_ERROR),
         makeGetNumericEffecterValueResp(
             PLDM_EFFECTER_DATA_SIZE_UINT8,
             EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING)});

    runFailureEstimate(
        {makeSetNumericEffecterValueResp(),
         makeGetNumericEffecterValueResp(
             PLDM_EFFECTER_DATA_SIZE_UINT8,
             EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING),
         makeSetNumericEffecterValueResp(),
         makeGetNumericEffecterValueResp(
             PLDM_EFFECTER_DATA_SIZE_UINT8,
             EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING),
         makeSetNumericEffecterValueResp(PLDM_ERROR),
         makeGetNumericEffecterValueResp(
             PLDM_EFFECTER_DATA_SIZE_UINT8,
             EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING)});

    runFailureEstimate(
        {makeSetNumericEffecterValueResp(),
         makeGetNumericEffecterValueResp(
             PLDM_EFFECTER_DATA_SIZE_UINT8,
             EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING),
         makeSetNumericEffecterValueResp(),
         makeGetNumericEffecterValueResp(
             PLDM_EFFECTER_DATA_SIZE_UINT8,
             EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING),
         makeSetNumericEffecterValueResp(),
         makeGetNumericEffecterValueResp(
             PLDM_EFFECTER_DATA_SIZE_UINT8,
             EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING),
         makeGetNumericEffecterValueResp(
             PLDM_EFFECTER_DATA_SIZE_UINT8,
             EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING, PLDM_ERROR)});
}

TEST_F(TerminusTest, nvidiaRemoteDebugMissingStateEffecterCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000167");
    Terminus terminus(0x67, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis67"};

    auto debugSensorInfo = makeDebugStateSetInfo(PLDM_ENTITY_SYS_BOARD, 6);
    auto debugStateSensor = std::make_shared<StateSensor>(
        terminus.getTid(), false, 0x770, debugSensorInfo, nullptr,
        associationPath, nullptr);
    terminus.stateSensors.emplace_back(debugStateSensor);

    auto remoteDebugTimeoutPdr = makeNumericEffecterValuePdrStruct(0x771);
    remoteDebugTimeoutPdr->base_unit = PLDM_SENSOR_UNIT_MINUTES;
    std::string effecterName{"remote_debug_timeout_missing_state_effecter"};
    auto remoteDebugNumericEffecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, remoteDebugTimeoutPdr, effecterName,
        associationPath, terminusManager);
    terminus.numericEffecters.emplace_back(remoteDebugNumericEffecter);

    nvidia::nvidiaInitTerminus(terminus);

    EXPECT_TRUE(remoteDebugNumericEffecter->oemIntfs.empty());
}

TEST_F(TerminusTest, nvidiaRemoteDebugMissingNumericEffecterCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000168");
    Terminus terminus(0x68, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis68"};

    auto debugEffecterInfo = makeDebugStateSetInfo(PLDM_ENTITY_SYS_BOARD, 6);
    auto debugSensorInfo = makeDebugStateSetInfo(PLDM_ENTITY_SYS_BOARD, 6);
    auto debugStateEffecter = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x772, debugEffecterInfo, nullptr,
        associationPath, terminusManager);
    auto debugStateSensor = std::make_shared<StateSensor>(
        terminus.getTid(), false, 0x773, debugSensorInfo, nullptr,
        associationPath, nullptr);
    terminus.stateEffecters.emplace_back(debugStateEffecter);
    terminus.stateSensors.emplace_back(debugStateSensor);

    nvidia::nvidiaInitTerminus(terminus);

    EXPECT_TRUE(debugStateEffecter->oemIntfs.empty());
}

TEST_F(TerminusTest, nvidiaRemoteDebugMissingStateSensorCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000169");
    Terminus terminus(0x69, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis69"};

    auto debugEffecterInfo = makeDebugStateSetInfo(PLDM_ENTITY_SYS_BOARD, 6);
    auto debugStateEffecter = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x774, debugEffecterInfo, nullptr,
        associationPath, terminusManager);
    terminus.stateEffecters.emplace_back(debugStateEffecter);

    auto remoteDebugTimeoutPdr = makeNumericEffecterValuePdrStruct(0x775);
    remoteDebugTimeoutPdr->base_unit = PLDM_SENSOR_UNIT_MINUTES;
    std::string effecterName{"remote_debug_timeout_missing_state_sensor"};
    auto remoteDebugNumericEffecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, remoteDebugTimeoutPdr, effecterName,
        associationPath, terminusManager);
    terminus.numericEffecters.emplace_back(remoteDebugNumericEffecter);

    nvidia::nvidiaInitTerminus(terminus);

    EXPECT_TRUE(debugStateEffecter->oemIntfs.empty());
}

TEST_F(TerminusTest, nvidiaRemoteDebugScanSkipsNonMatchingEntriesCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000016A");
    Terminus terminus(0x6A, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis6A"};

    auto unrelatedInfo = makeSingleStateSetInfo(PLDM_ENTITY_SYS_BOARD,
                                                PLDM_STATESET_ID_BOOT_REQUEST);
    auto unrelatedStateEffecter = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x776, unrelatedInfo, nullptr,
        associationPath, terminusManager);
    terminus.stateEffecters.emplace_back(unrelatedStateEffecter);

    auto debugEffecterInfo = makeDebugStateSetInfo(PLDM_ENTITY_SYS_BOARD, 6);
    auto debugSensorInfo = makeDebugStateSetInfo(PLDM_ENTITY_SYS_BOARD, 6);
    auto debugStateEffecter = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x777, debugEffecterInfo, nullptr,
        associationPath, terminusManager);
    auto unrelatedStateSensor = std::make_shared<StateSensor>(
        terminus.getTid(), false, 0x778, unrelatedInfo, nullptr,
        associationPath, nullptr);
    auto debugStateSensor = std::make_shared<StateSensor>(
        terminus.getTid(), false, 0x779, debugSensorInfo, nullptr,
        associationPath, nullptr);
    terminus.stateEffecters.emplace_back(debugStateEffecter);
    terminus.stateSensors.emplace_back(unrelatedStateSensor);
    terminus.stateSensors.emplace_back(debugStateSensor);

    auto createNumericEffecter =
        [&](uint16_t effecterId, uint8_t baseUnit, const std::string& name) {
            auto pdr = makeNumericEffecterValuePdrStruct(effecterId);
            pdr->base_unit = baseUnit;
            pdr->min_settable.value_u8 = 1;
            pdr->max_settable.value_u8 = 100;
            std::string effecterName = name;
            auto effecter = std::make_shared<NumericEffecter>(
                terminus.getTid(), false, pdr, effecterName, associationPath,
                terminusManager);
            terminus.numericEffecters.emplace_back(effecter);
            return effecter;
        };

    auto unrelatedNumeric = createNumericEffecter(0x77A, PLDM_SENSOR_UNIT_NONE,
                                                  "unrelated_numeric");
    auto remoteDebugTimeout = createNumericEffecter(
        0x77B, PLDM_SENSOR_UNIT_MINUTES, "remote_debug_timeout_scan");
    auto staticPowerPower = createNumericEffecter(0x77C, PLDM_SENSOR_UNIT_WATTS,
                                                  "static_power_power_scan");
    auto staticPowerWorkload = createNumericEffecter(
        0x77D, PLDM_SENSOR_UNIT_NONE, "static_power_workload_scan");
    auto staticPowerTemp = createNumericEffecter(
        0x77E, PLDM_SENSOR_UNIT_DEGRESS_C, "static_power_temp_scan");
    auto staticPowerClock = createNumericEffecter(0x77F, PLDM_SENSOR_UNIT_HERTZ,
                                                  "static_power_clock_scan");

    nvidia::nvidiaInitTerminus(terminus);

    EXPECT_TRUE(unrelatedStateEffecter->oemIntfs.empty());
    EXPECT_TRUE(unrelatedStateSensor->stateSets.empty() ||
                unrelatedStateSensor->stateSets[0] != nullptr);
    EXPECT_TRUE(unrelatedNumeric->oemIntfs.empty());
    ASSERT_EQ(1u, debugStateEffecter->oemIntfs.size());
    EXPECT_NE(nullptr, dynamic_cast<oem_nvidia::OemRemoteDebugIntf*>(
                           debugStateEffecter->oemIntfs[0].get()));
    ASSERT_EQ(1u, staticPowerPower->oemIntfs.size());
    EXPECT_NE(nullptr, std::dynamic_pointer_cast<OemStaticPowerHintInft>(
                           staticPowerPower->oemIntfs[0]));
    EXPECT_TRUE(remoteDebugTimeout->oemIntfs.empty());
    EXPECT_TRUE(staticPowerWorkload->oemIntfs.empty());
    EXPECT_TRUE(staticPowerTemp->oemIntfs.empty());
    EXPECT_TRUE(staticPowerClock->oemIntfs.empty());
}

TEST_F(TerminusTest, nvidiaRemoteDebugUsesFirstMatchingStateEntriesCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000016C");
    Terminus terminus(0x6C, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis6C"};

    auto debugEffecterInfo = makeDebugStateSetInfo(PLDM_ENTITY_SYS_BOARD, 6);
    auto debugSensorInfo = makeDebugStateSetInfo(PLDM_ENTITY_SYS_BOARD, 6);
    auto firstStateEffecter = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x784, debugEffecterInfo, nullptr,
        associationPath, terminusManager);
    auto secondStateEffecter = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x785, debugEffecterInfo, nullptr,
        associationPath, terminusManager);
    auto firstStateSensor = std::make_shared<StateSensor>(
        terminus.getTid(), false, 0x786, debugSensorInfo, nullptr,
        associationPath, nullptr);
    auto secondStateSensor = std::make_shared<StateSensor>(
        terminus.getTid(), false, 0x787, debugSensorInfo, nullptr,
        associationPath, nullptr);
    terminus.stateEffecters.emplace_back(firstStateEffecter);
    terminus.stateEffecters.emplace_back(secondStateEffecter);
    terminus.stateSensors.emplace_back(firstStateSensor);
    terminus.stateSensors.emplace_back(secondStateSensor);

    auto remoteDebugTimeoutPdr = makeNumericEffecterValuePdrStruct(0x788);
    remoteDebugTimeoutPdr->base_unit = PLDM_SENSOR_UNIT_MINUTES;
    remoteDebugTimeoutPdr->min_settable.value_u8 = 1;
    remoteDebugTimeoutPdr->max_settable.value_u8 = 60;
    std::string effecterName{"remote_debug_timeout_first_match"};
    auto remoteDebugNumericEffecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, remoteDebugTimeoutPdr, effecterName,
        associationPath, terminusManager);
    terminus.numericEffecters.emplace_back(remoteDebugNumericEffecter);

    nvidia::nvidiaInitTerminus(terminus);

    ASSERT_EQ(1u, firstStateEffecter->oemIntfs.size());
    EXPECT_TRUE(secondStateEffecter->oemIntfs.empty());
    EXPECT_NE(nullptr, dynamic_cast<oem_nvidia::OemRemoteDebugIntf*>(
                           firstStateEffecter->oemIntfs[0].get()));
}

TEST_F(TerminusTest, nvidiaInitTerminusMatchesLaterAssociatedEffectersCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000016B");
    Terminus terminus(0x6B, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis6B"};

    auto unrelatedNumericPdr = makeNumericEffecterValuePdrStruct(0x780);
    auto matchedNumericPdr = makeNumericEffecterValuePdrStruct(0x781);
    std::string unrelatedNumericName{"oem_numeric_effecter_unrelated"};
    std::string matchedNumericName{"oem_numeric_effecter_matched"};
    auto unrelatedNumeric = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, unrelatedNumericPdr, unrelatedNumericName,
        associationPath, terminusManager);
    auto matchedNumeric = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, matchedNumericPdr, matchedNumericName,
        associationPath, terminusManager);
    terminus.numericEffecters.emplace_back(unrelatedNumeric);
    terminus.numericEffecters.emplace_back(matchedNumeric);

    auto unrelatedStateInfo = makeSingleStateSetInfo(
        PLDM_ENTITY_SYS_BOARD, PLDM_STATESET_ID_BOOT_REQUEST);
    auto matchedStateInfo = makeSingleStateSetInfo(
        PLDM_ENTITY_SYS_BOARD, PLDM_STATESET_ID_BOOT_REQUEST);
    auto unrelatedState = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x782, unrelatedStateInfo, nullptr,
        associationPath, terminusManager);
    auto matchedState = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x783, matchedStateInfo, nullptr,
        associationPath, terminusManager);
    terminus.stateEffecters.emplace_back(unrelatedState);
    terminus.stateEffecters.emplace_back(matchedState);

    nvidia::nvidia_oem_effecter_powercap_pdr powerCapPdr{};
    powerCapPdr.terminus_handle = 1;
    powerCapPdr.oem_pdr_type = static_cast<uint8_t>(
        nvidia::NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_EFFECTER_POWERCAP);
    powerCapPdr.oem_effecter_powercap = static_cast<uint8_t>(
        nvidia::OemPowerCapPersistence::OEM_POWERCAP_EDPP_NONVOLATILE);
    powerCapPdr.associated_effecterid = 0x781;

    nvidia::nvidia_oem_effecter_storage_pdr storagePdr{};
    storagePdr.terminus_handle = 1;
    storagePdr.oem_pdr_type = static_cast<uint8_t>(
        nvidia::NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_EFFECTER_STORAGE);
    storagePdr.oem_effecter_storage = static_cast<uint8_t>(
        nvidia::OemStorageSecureState::OEM_STORAGE_SECURE_VARIABLE);
    storagePdr.associated_effecterid = 0x783;

    terminus.oemPdrs.emplace_back(nvidia::NvidiaIana,
                                  static_cast<OemRecordId>(0x781),
                                  structToBytes(powerCapPdr));
    terminus.oemPdrs.emplace_back(nvidia::NvidiaIana,
                                  static_cast<OemRecordId>(0x783),
                                  structToBytes(storagePdr));

    nvidia::nvidiaInitTerminus(terminus);

    EXPECT_TRUE(unrelatedNumeric->oemIntfs.empty());
    ASSERT_EQ(1u, matchedNumeric->oemIntfs.size());
    auto persistenceIntf =
        std::dynamic_pointer_cast<nvidia::OemPersistenceIntf>(
            matchedNumeric->oemIntfs[0]);
    ASSERT_NE(nullptr, persistenceIntf);
    EXPECT_TRUE(persistenceIntf->persistent());

    EXPECT_TRUE(unrelatedState->oemIntfs.empty());
    ASSERT_EQ(1u, matchedState->oemIntfs.size());
    auto* storageIntf =
        dynamic_cast<nvidia::OemStorageIntf*>(matchedState->oemIntfs[0].get());
    ASSERT_NE(nullptr, storageIntf);
    EXPECT_TRUE(storageIntf->secure());
}

TEST_F(TerminusTest, nvidiaStaticPowerHintPowerOnlyCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000170");
    Terminus terminus(0x70, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis70"};

    auto powerPdr = makeNumericEffecterValuePdrStruct(0x776);
    powerPdr->base_unit = PLDM_SENSOR_UNIT_WATTS;
    powerPdr->min_settable.value_u8 = 5;
    powerPdr->max_settable.value_u8 = 250;
    std::string powerName{"static_power_power_only"};
    auto powerEffecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, powerPdr, powerName, associationPath,
        terminusManager);
    terminus.numericEffecters.emplace_back(powerEffecter);

    nvidia::nvidiaInitTerminus(terminus);

    EXPECT_TRUE(powerEffecter->oemIntfs.empty());
}

TEST_F(TerminusTest, nvidiaStaticPowerHintMissingPowerEstimationCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000171");
    Terminus terminus(0x71, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis71"};

    auto createNumericEffecter =
        [&](uint16_t effecterId, uint8_t baseUnit, uint8_t minValue,
            uint8_t maxValue, const std::string& effecterName) {
            auto pdr = makeNumericEffecterValuePdrStruct(effecterId);
            pdr->base_unit = baseUnit;
            pdr->min_settable.value_u8 = minValue;
            pdr->max_settable.value_u8 = maxValue;
            std::string name = effecterName;
            auto effecter = std::make_shared<NumericEffecter>(
                terminus.getTid(), false, pdr, name, associationPath,
                terminusManager);
            terminus.numericEffecters.emplace_back(effecter);
            return effecter;
        };

    auto staticPowerTemperatureEffecter =
        createNumericEffecter(0x777, PLDM_SENSOR_UNIT_DEGRESS_C, 10, 80,
                              "static_power_temp_missing_power");
    auto staticPowerWorkloadEffecter =
        createNumericEffecter(0x778, PLDM_SENSOR_UNIT_NONE, 1, 100,
                              "static_power_workload_missing_power");
    auto staticPowerClockEffecter =
        createNumericEffecter(0x779, PLDM_SENSOR_UNIT_HERTZ, 20, 200,
                              "static_power_clock_missing_power");

    nvidia::nvidiaInitTerminus(terminus);

    EXPECT_TRUE(staticPowerTemperatureEffecter->oemIntfs.empty());
    EXPECT_TRUE(staticPowerWorkloadEffecter->oemIntfs.empty());
    EXPECT_TRUE(staticPowerClockEffecter->oemIntfs.empty());
}

TEST_F(TerminusTest, nvidiaStaticPowerHintMissingTemperatureCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000172");
    Terminus terminus(0x72, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis72"};

    auto createNumericEffecter =
        [&](uint16_t effecterId, uint8_t baseUnit, const std::string& name) {
            auto pdr = makeNumericEffecterValuePdrStruct(effecterId);
            pdr->base_unit = baseUnit;
            pdr->min_settable.value_u8 = 1;
            pdr->max_settable.value_u8 = 100;
            std::string effecterName = name;
            auto effecter = std::make_shared<NumericEffecter>(
                terminus.getTid(), false, pdr, effecterName, associationPath,
                terminusManager);
            terminus.numericEffecters.emplace_back(effecter);
            return effecter;
        };

    auto workloadEffecter = createNumericEffecter(
        0x780, PLDM_SENSOR_UNIT_NONE, "static_power_workload_missing_temp");
    auto clockEffecter = createNumericEffecter(
        0x781, PLDM_SENSOR_UNIT_HERTZ, "static_power_clock_missing_temp");
    auto powerEffecter = createNumericEffecter(
        0x782, PLDM_SENSOR_UNIT_WATTS, "static_power_power_missing_temp");

    nvidia::nvidiaInitTerminus(terminus);

    EXPECT_TRUE(workloadEffecter->oemIntfs.empty());
    EXPECT_TRUE(clockEffecter->oemIntfs.empty());
    EXPECT_TRUE(powerEffecter->oemIntfs.empty());
}

TEST_F(TerminusTest, nvidiaStaticPowerHintMissingWorkloadCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000173");
    Terminus terminus(0x73, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis73"};

    auto createNumericEffecter =
        [&](uint16_t effecterId, uint8_t baseUnit, const std::string& name) {
            auto pdr = makeNumericEffecterValuePdrStruct(effecterId);
            pdr->base_unit = baseUnit;
            pdr->min_settable.value_u8 = 1;
            pdr->max_settable.value_u8 = 100;
            std::string effecterName = name;
            auto effecter = std::make_shared<NumericEffecter>(
                terminus.getTid(), false, pdr, effecterName, associationPath,
                terminusManager);
            terminus.numericEffecters.emplace_back(effecter);
            return effecter;
        };

    auto temperatureEffecter =
        createNumericEffecter(0x783, PLDM_SENSOR_UNIT_DEGRESS_C,
                              "static_power_temperature_missing_workload");
    auto clockEffecter = createNumericEffecter(
        0x784, PLDM_SENSOR_UNIT_HERTZ, "static_power_clock_missing_workload");
    auto powerEffecter = createNumericEffecter(
        0x785, PLDM_SENSOR_UNIT_WATTS, "static_power_power_missing_workload");

    nvidia::nvidiaInitTerminus(terminus);

    EXPECT_TRUE(temperatureEffecter->oemIntfs.empty());
    EXPECT_TRUE(clockEffecter->oemIntfs.empty());
    EXPECT_TRUE(powerEffecter->oemIntfs.empty());
}

TEST_F(TerminusTest, nvidiaStaticPowerHintMissingClockCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000174");
    Terminus terminus(0x74, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis74"};

    auto createNumericEffecter =
        [&](uint16_t effecterId, uint8_t baseUnit, const std::string& name) {
            auto pdr = makeNumericEffecterValuePdrStruct(effecterId);
            pdr->base_unit = baseUnit;
            pdr->min_settable.value_u8 = 1;
            pdr->max_settable.value_u8 = 100;
            std::string effecterName = name;
            auto effecter = std::make_shared<NumericEffecter>(
                terminus.getTid(), false, pdr, effecterName, associationPath,
                terminusManager);
            terminus.numericEffecters.emplace_back(effecter);
            return effecter;
        };

    auto temperatureEffecter =
        createNumericEffecter(0x786, PLDM_SENSOR_UNIT_DEGRESS_C,
                              "static_power_temperature_missing_clock");
    auto workloadEffecter = createNumericEffecter(
        0x787, PLDM_SENSOR_UNIT_NONE, "static_power_workload_missing_clock");
    auto powerEffecter = createNumericEffecter(
        0x788, PLDM_SENSOR_UNIT_WATTS, "static_power_power_missing_clock");

    nvidia::nvidiaInitTerminus(terminus);

    EXPECT_TRUE(temperatureEffecter->oemIntfs.empty());
    EXPECT_TRUE(workloadEffecter->oemIntfs.empty());
    EXPECT_TRUE(powerEffecter->oemIntfs.empty());
}

TEST_F(TerminusTest, nvidiaStaticPowerHintEstimateInProgressCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000172");
    Terminus terminus(0x72, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis72"};

    auto createNumericEffecter =
        [&](uint16_t effecterId, uint8_t baseUnit, uint8_t minValue,
            uint8_t maxValue, const std::string& effecterName) {
            auto pdr = makeNumericEffecterValuePdrStruct(effecterId);
            pdr->base_unit = baseUnit;
            pdr->min_settable.value_u8 = minValue;
            pdr->max_settable.value_u8 = maxValue;
            std::string name = effecterName;
            auto effecter = std::make_shared<NumericEffecter>(
                terminus.getTid(), false, pdr, name, associationPath,
                terminusManager);
            terminus.numericEffecters.emplace_back(effecter);
            return effecter;
        };

    createNumericEffecter(0x77A, PLDM_SENSOR_UNIT_DEGRESS_C, 10, 80,
                          "static_power_temp_progress");
    createNumericEffecter(0x77B, PLDM_SENSOR_UNIT_NONE, 1, 100,
                          "static_power_workload_progress");
    createNumericEffecter(0x77C, PLDM_SENSOR_UNIT_HERTZ, 20, 200,
                          "static_power_clock_progress");
    auto staticPowerPowerEffecter = createNumericEffecter(
        0x77D, PLDM_SENSOR_UNIT_WATTS, 5, 250, "static_power_power_progress");

    nvidia::nvidiaInitTerminus(terminus);

    ASSERT_EQ(1u, staticPowerPowerEffecter->oemIntfs.size());
    auto staticPowerIntf = std::dynamic_pointer_cast<OemStaticPowerHintInft>(
        staticPowerPowerEffecter->oemIntfs[0]);
    ASSERT_NE(nullptr, staticPowerIntf);

    staticPowerIntf->estimationTaskHandle.emplace();
    EXPECT_THROW(staticPowerIntf->estimatePower(
                     staticPowerIntf->minCpuClockFrequency(),
                     staticPowerIntf->minWorkloadFactor(),
                     staticPowerIntf->minTemperature(),
                     staticPowerIntf->minNumberOfCores()),
                 sdbusplus::xyz::openbmc_project::Common::Error::Unavailable);
    EXPECT_TRUE(staticPowerIntf->estimationTaskHandle.has_value());
    staticPowerIntf->estimationTaskHandle.reset();
}

TEST_F(TerminusTest, nvidiaInitTerminusRemoteDebugMatrixCoverage)
{
    auto makeRepeatedStateSetInfo = [](uint16_t stateSetId) {
        std::vector<StateSetData> stateSets;
        stateSets.reserve(6);
        for (size_t i = 0; i < 6; ++i)
        {
            stateSets.emplace_back(stateSetId,
                                   PossibleStates{0, 1, 2, 3, 4, 5, 6, 7});
        }
        return std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
                               stateSets);
    };

    {
        std::string uuid("00000000-0000-0000-0000-000000000173");
        Terminus terminus(0x73, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                          terminusManager);
        std::string associationPath{
            "/xyz/openbmc_project/inventory/system/chassis/chassis73"};

        auto nonDebugEffecterInfo =
            makeRepeatedStateSetInfo(PLDM_STATESET_ID_BOOT_REQUEST);
        auto debugEffecterInfo =
            makeDebugStateSetInfo(PLDM_ENTITY_SYS_BOARD, 6);
        auto nonDebugSensorInfo =
            makeRepeatedStateSetInfo(PLDM_STATESET_ID_BOOT_REQUEST);
        auto debugSensorInfo = makeDebugStateSetInfo(PLDM_ENTITY_SYS_BOARD, 6);

        auto nonDebugEffecter = std::make_shared<StateEffecter>(
            terminus.getTid(), false, 0x780, nonDebugEffecterInfo, nullptr,
            associationPath, terminusManager);
        auto debugEffecter = std::make_shared<StateEffecter>(
            terminus.getTid(), false, 0x781, debugEffecterInfo, nullptr,
            associationPath, terminusManager);
        auto nonDebugSensor = std::make_shared<StateSensor>(
            terminus.getTid(), false, 0x782, nonDebugSensorInfo, nullptr,
            associationPath, nullptr);
        auto debugSensor = std::make_shared<StateSensor>(
            terminus.getTid(), false, 0x783, debugSensorInfo, nullptr,
            associationPath, nullptr);
        auto timeoutPdr = makeNumericEffecterValuePdrStruct(0x784);
        timeoutPdr->base_unit = PLDM_SENSOR_UNIT_MINUTES;
        std::string timeoutName{"remote_debug_timeout_matrix"};
        auto timeoutEffecter = std::make_shared<NumericEffecter>(
            terminus.getTid(), false, timeoutPdr, timeoutName, associationPath,
            terminusManager);

        terminus.stateEffecters.emplace_back(nonDebugEffecter);
        terminus.stateEffecters.emplace_back(debugEffecter);
        terminus.stateSensors.emplace_back(nonDebugSensor);
        terminus.stateSensors.emplace_back(debugSensor);
        terminus.numericEffecters.emplace_back(timeoutEffecter);

        nvidia::nvidiaInitTerminus(terminus);

        EXPECT_TRUE(nonDebugEffecter->oemIntfs.empty());
        ASSERT_EQ(1u, debugEffecter->oemIntfs.size());
    }

    for (uint8_t mask = 0; mask < 8; ++mask)
    {
        std::string uuid("00000000-0000-0000-0000-000000000174");
        Terminus terminus(static_cast<uint8_t>(0x80 + mask),
                          1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                          terminusManager);
        std::string associationPath =
            "/xyz/openbmc_project/inventory/system/chassis/remote_debug_" +
            std::to_string(mask);

        std::shared_ptr<StateEffecter> debugEffecter;
        if (mask & 0x1)
        {
            auto debugEffecterInfo =
                makeDebugStateSetInfo(PLDM_ENTITY_SYS_BOARD, 6);
            debugEffecter = std::make_shared<StateEffecter>(
                terminus.getTid(), false, static_cast<uint16_t>(0x790 + mask),
                debugEffecterInfo, nullptr, associationPath, terminusManager);
            terminus.stateEffecters.emplace_back(debugEffecter);
        }

        if (mask & 0x2)
        {
            auto timeoutPdr = makeNumericEffecterValuePdrStruct(
                static_cast<uint16_t>(0x7A0 + mask));
            timeoutPdr->base_unit = PLDM_SENSOR_UNIT_MINUTES;
            std::string timeoutName =
                "remote_debug_timeout_" + std::to_string(mask);
            terminus.numericEffecters.emplace_back(
                std::make_shared<NumericEffecter>(
                    terminus.getTid(), false, timeoutPdr, timeoutName,
                    associationPath, terminusManager));
        }

        if (mask & 0x4)
        {
            auto debugSensorInfo =
                makeDebugStateSetInfo(PLDM_ENTITY_SYS_BOARD, 6);
            terminus.stateSensors.emplace_back(std::make_shared<StateSensor>(
                terminus.getTid(), false, static_cast<uint16_t>(0x7B0 + mask),
                debugSensorInfo, nullptr, associationPath, nullptr));
        }

        nvidia::nvidiaInitTerminus(terminus);

        if (debugEffecter)
        {
            EXPECT_EQ(mask == 0x7 ? 1u : 0u, debugEffecter->oemIntfs.size());
        }
    }
}

TEST_F(TerminusTest, nvidiaInitTerminusNumericEntityTypeGuardCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000175");
    Terminus terminus(0x90, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis90"};

    auto createNumericEffecter = [&](uint16_t effecterId, uint8_t baseUnit,
                                     const std::string& effecterName) {
        auto pdr =
            makeNumericEffecterValuePdrStruct(effecterId, PLDM_ENTITY_PROC, 1);
        pdr->base_unit = baseUnit;
        std::string name = effecterName;
        auto effecter = std::make_shared<NumericEffecter>(
            terminus.getTid(), false, pdr, name, associationPath,
            terminusManager);
        terminus.numericEffecters.emplace_back(effecter);
        return effecter;
    };

    auto remoteDebugTimeout = createNumericEffecter(
        0x7C0, PLDM_SENSOR_UNIT_MINUTES, "remote_debug_timeout_proc");
    auto staticPowerTemperature = createNumericEffecter(
        0x7C1, PLDM_SENSOR_UNIT_DEGRESS_C, "static_power_temp_proc");
    auto staticPowerClock = createNumericEffecter(0x7C2, PLDM_SENSOR_UNIT_HERTZ,
                                                  "static_power_clock_proc");

    nvidia::nvidiaInitTerminus(terminus);

    EXPECT_TRUE(remoteDebugTimeout->oemIntfs.empty());
    EXPECT_TRUE(staticPowerTemperature->oemIntfs.empty());
    EXPECT_TRUE(staticPowerClock->oemIntfs.empty());
}

TEST_F(TerminusTest, nvidiaInitTerminusStaticPowerMatrixCoverage)
{
    for (uint8_t mask = 0; mask < 16; ++mask)
    {
        std::string uuid("00000000-0000-0000-0000-000000000176");
        Terminus terminus(static_cast<uint8_t>(0xA0 + mask),
                          1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                          terminusManager);
        std::string associationPath =
            "/xyz/openbmc_project/inventory/system/chassis/static_power_" +
            std::to_string(mask);

        auto createNumericEffecter =
            [&](uint16_t effecterId, uint8_t baseUnit, uint8_t minValue,
                uint8_t maxValue, const std::string& effecterName) {
                auto pdr = makeNumericEffecterValuePdrStruct(effecterId);
                pdr->base_unit = baseUnit;
                pdr->min_settable.value_u8 = minValue;
                pdr->max_settable.value_u8 = maxValue;
                std::string name = effecterName;
                auto effecter = std::make_shared<NumericEffecter>(
                    terminus.getTid(), false, pdr, name, associationPath,
                    terminusManager);
                terminus.numericEffecters.emplace_back(effecter);
                return effecter;
            };

        std::shared_ptr<NumericEffecter> powerEffecter;
        std::shared_ptr<NumericEffecter> temperatureEffecter;
        std::shared_ptr<NumericEffecter> workloadEffecter;
        std::shared_ptr<NumericEffecter> clockEffecter;

        if (mask & 0x1)
        {
            temperatureEffecter = createNumericEffecter(
                static_cast<uint16_t>(0x7D0 + mask), PLDM_SENSOR_UNIT_DEGRESS_C,
                10, 80, "static_power_temp_" + std::to_string(mask));
        }
        if (mask & 0x2)
        {
            workloadEffecter = createNumericEffecter(
                static_cast<uint16_t>(0x7E0 + mask), PLDM_SENSOR_UNIT_NONE, 1,
                100, "static_power_workload_" + std::to_string(mask));
        }
        if (mask & 0x4)
        {
            clockEffecter = createNumericEffecter(
                static_cast<uint16_t>(0x7F0 + mask), PLDM_SENSOR_UNIT_HERTZ, 20,
                200, "static_power_clock_" + std::to_string(mask));
        }
        if (mask & 0x8)
        {
            powerEffecter = createNumericEffecter(
                static_cast<uint16_t>(0x800 + mask), PLDM_SENSOR_UNIT_WATTS, 5,
                250, "static_power_power_" + std::to_string(mask));
        }

        nvidia::nvidiaInitTerminus(terminus);

        if (powerEffecter)
        {
            EXPECT_EQ(mask == 0xF ? 1u : 0u, powerEffecter->oemIntfs.size());
        }
        if (mask != 0xF)
        {
            if (temperatureEffecter)
            {
                EXPECT_TRUE(temperatureEffecter->oemIntfs.empty());
            }
            if (workloadEffecter)
            {
                EXPECT_TRUE(workloadEffecter->oemIntfs.empty());
            }
            if (clockEffecter)
            {
                EXPECT_TRUE(clockEffecter->oemIntfs.empty());
            }
        }
    }
}

TEST_F(TerminusTest, nvidiaInitTerminusNonMatchingCandidatesCoverage)
{
    auto makeRepeatedStateSetInfo = [](uint16_t stateSetId) {
        std::vector<StateSetData> stateSets;
        stateSets.reserve(6);
        for (size_t i = 0; i < 6; ++i)
        {
            stateSets.emplace_back(stateSetId,
                                   PossibleStates{0, 1, 2, 3, 4, 5, 6, 7});
        }
        return std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
                               stateSets);
    };

    std::string uuid("00000000-0000-0000-0000-000000000177");
    Terminus terminus(0xB0, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassisB0"};

    auto nonDebugEffecterInfo =
        makeRepeatedStateSetInfo(PLDM_STATESET_ID_BOOT_REQUEST);
    auto nonDebugSensorInfo =
        makeRepeatedStateSetInfo(PLDM_STATESET_ID_BOOT_REQUEST);
    auto nonDebugEffecter = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x810, nonDebugEffecterInfo, nullptr,
        associationPath, terminusManager);
    auto nonDebugSensor = std::make_shared<StateSensor>(
        terminus.getTid(), false, 0x811, nonDebugSensorInfo, nullptr,
        associationPath, nullptr);
    terminus.stateEffecters.emplace_back(nonDebugEffecter);
    terminus.stateSensors.emplace_back(nonDebugSensor);

    auto irrelevantPdr =
        makeNumericEffecterValuePdrStruct(0x812, PLDM_ENTITY_PROC, 1);
    irrelevantPdr->base_unit = PLDM_SENSOR_UNIT_MINUTES;
    std::string irrelevantName{"irrelevant_numeric_effecter"};
    auto irrelevantEffecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, irrelevantPdr, irrelevantName,
        associationPath, terminusManager);
    terminus.numericEffecters.emplace_back(irrelevantEffecter);

    nvidia::nvidiaInitTerminus(terminus);

    EXPECT_TRUE(nonDebugEffecter->oemIntfs.empty());
    EXPECT_TRUE(irrelevantEffecter->oemIntfs.empty());
}

TEST_F(TerminusTest, nvidiaInitTerminusPrepopulatedOemInterfaceCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000178");
    Terminus terminus(0xB1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassisB1"};

    auto powerCapEffecterPdr = makeNumericEffecterValuePdrStruct(0x820);
    std::string powerCapName{"power_cap_prepopulated"};
    auto powerCapEffecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, powerCapEffecterPdr, powerCapName,
        associationPath, terminusManager);
    terminus.numericEffecters.emplace_back(powerCapEffecter);

    auto createNumericEffecter =
        [&](uint16_t effecterId, uint8_t baseUnit, uint8_t minValue,
            uint8_t maxValue, const std::string& effecterName) {
            auto pdr = makeNumericEffecterValuePdrStruct(effecterId);
            pdr->base_unit = baseUnit;
            pdr->min_settable.value_u8 = minValue;
            pdr->max_settable.value_u8 = maxValue;
            std::string name = effecterName;
            auto effecter = std::make_shared<NumericEffecter>(
                terminus.getTid(), false, pdr, name, associationPath,
                terminusManager);
            terminus.numericEffecters.emplace_back(effecter);
            return effecter;
        };

    createNumericEffecter(0x821, PLDM_SENSOR_UNIT_DEGRESS_C, 10, 80,
                          "static_power_temp_prepopulated");
    createNumericEffecter(0x822, PLDM_SENSOR_UNIT_NONE, 1, 100,
                          "static_power_workload_prepopulated");
    createNumericEffecter(0x823, PLDM_SENSOR_UNIT_HERTZ, 20, 200,
                          "static_power_clock_prepopulated");
    auto staticPowerPowerEffecter =
        createNumericEffecter(0x824, PLDM_SENSOR_UNIT_WATTS, 5, 250,
                              "static_power_power_prepopulated");

    nvidia::nvidia_oem_effecter_powercap_pdr powerCapPdr{};
    powerCapPdr.terminus_handle = 1;
    powerCapPdr.oem_pdr_type = static_cast<uint8_t>(
        nvidia::NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_EFFECTER_POWERCAP);
    powerCapPdr.oem_effecter_powercap = static_cast<uint8_t>(
        nvidia::OemPowerCapPersistence::OEM_POWERCAP_TDP_NONVOLATILE);
    powerCapPdr.associated_effecterid = 0x820;
    terminus.oemPdrs.emplace_back(nvidia::NvidiaIana,
                                  static_cast<OemRecordId>(0x820),
                                  structToBytes(powerCapPdr));

    auto& bus = pldm::utils::DBusHandler::getBus();
    powerCapEffecter->oemIntfs.emplace_back(
        std::make_shared<nvidia::OemPersistenceIntf>(
            bus,
            "/xyz/openbmc_project/control/coverage/power_cap_preexisting"));
    staticPowerPowerEffecter->oemIntfs.emplace_back(
        std::make_shared<nvidia::OemPersistenceIntf>(
            bus,
            "/xyz/openbmc_project/control/coverage/static_power_preexisting"));

    nvidia::nvidiaInitTerminus(terminus);

    EXPECT_EQ(2u, powerCapEffecter->oemIntfs.size());
    EXPECT_EQ(2u, staticPowerPowerEffecter->oemIntfs.size());
}

TEST_F(TerminusTest, nvidiaStaticPowerHintCompletedHandleCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000179");
    Terminus terminus(0xB2, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassisB2"};

    auto createNumericEffecter =
        [&](uint16_t effecterId, uint8_t baseUnit, uint8_t minValue,
            uint8_t maxValue, const std::string& effecterName) {
            auto pdr = makeNumericEffecterValuePdrStruct(effecterId);
            pdr->base_unit = baseUnit;
            pdr->min_settable.value_u8 = minValue;
            pdr->max_settable.value_u8 = maxValue;
            std::string name = effecterName;
            auto effecter = std::make_shared<NumericEffecter>(
                terminus.getTid(), false, pdr, name, associationPath,
                terminusManager);
            terminus.numericEffecters.emplace_back(effecter);
            return effecter;
        };

    createNumericEffecter(0x830, PLDM_SENSOR_UNIT_DEGRESS_C, 10, 80,
                          "static_power_temp_completed");
    createNumericEffecter(0x831, PLDM_SENSOR_UNIT_NONE, 1, 100,
                          "static_power_workload_completed");
    createNumericEffecter(0x832, PLDM_SENSOR_UNIT_HERTZ, 20, 200,
                          "static_power_clock_completed");
    auto staticPowerPowerEffecter = createNumericEffecter(
        0x833, PLDM_SENSOR_UNIT_WATTS, 5, 250, "static_power_power_completed");
    createNumericEffecter(0x834, PLDM_SENSOR_UNIT_COUNTS, 0, 128,
                          "PowerHint_cores_completed");

    nvidia::nvidiaInitTerminus(terminus);

    ASSERT_EQ(1u, staticPowerPowerEffecter->oemIntfs.size());
    auto staticPowerIntf = std::dynamic_pointer_cast<OemStaticPowerHintInft>(
        staticPowerPowerEffecter->oemIntfs[0]);
    ASSERT_NE(nullptr, staticPowerIntf);

    staticPowerIntf->estimationTaskHandle.emplace();
    staticPowerIntf->estimationTaskHandle->second.emplace(PLDM_SUCCESS);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.clearQueuedResponses());
    auto setResp0 = makeSetNumericEffecterValueResp();
    auto getResp0 = makeGetNumericEffecterValueResp(
        PLDM_EFFECTER_DATA_SIZE_UINT8,
        EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING);
    auto setResp1 = makeSetNumericEffecterValueResp();
    auto getResp1 = makeGetNumericEffecterValueResp(
        PLDM_EFFECTER_DATA_SIZE_UINT8,
        EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING);
    auto setResp2 = makeSetNumericEffecterValueResp();
    auto getResp2 = makeGetNumericEffecterValueResp(
        PLDM_EFFECTER_DATA_SIZE_UINT8,
        EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING);
    auto setResp3 = makeSetNumericEffecterValueResp();
    auto getResp3 = makeGetNumericEffecterValueResp(
        PLDM_EFFECTER_DATA_SIZE_UINT8,
        EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING);
    auto getResp4 = makeGetNumericEffecterValueResp(
        PLDM_EFFECTER_DATA_SIZE_UINT8,
        EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(setResp0));
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(getResp0));
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(setResp1));
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(getResp1));
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(setResp2));
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(getResp2));
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(setResp3));
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(getResp3));
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(getResp4));

    EXPECT_NO_THROW(staticPowerIntf->estimatePower(
        staticPowerIntf->minCpuClockFrequency(),
        staticPowerIntf->minWorkloadFactor(), staticPowerIntf->minTemperature(),
        staticPowerIntf->minNumberOfCores()));
    runEventLoopForMilliseconds(10);
}

TEST_F(TerminusTest, nvidiaUpdateAssociationsOemCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000159");
    Terminus terminus(0x59, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis59"};

    StateSetData linkStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_LINKSTATE),
                        PossibleStates{PLDM_STATESET_LINK_STATE_DISCONNECTED,
                                       PLDM_STATESET_LINK_STATE_CONNECTED});
    StateSetData performanceStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_PERFORMANCE),
                        PossibleStates{PLDM_STATESET_PERFORMANCE_NORMAL,
                                       PLDM_STATESET_PERFORMANCE_THROTTLED});

    auto createStateSensor = [&](uint16_t sensorId, EntityType entityType,
                                 const StateSetData& stateSetData) {
        StateSetInfo info =
            std::make_tuple(EntityInfo{1, entityType, 1},
                            std::vector<StateSetData>{stateSetData});
        auto sensor = std::make_shared<StateSensor>(
            terminus.getTid(), false, sensorId, info, nullptr, associationPath,
            nullptr);
        terminus.stateSensors.emplace_back(sensor);
        return sensor;
    };

    auto ethSensor =
        createStateSensor(0x801, PLDM_ENTITY_ETHERNET, linkStateData);
    auto ethSensorFallback =
        createStateSensor(0x802, PLDM_ENTITY_ETHERNET, linkStateData);
    auto ibSensor =
        createStateSensor(0x803, PLDM_ENTITY_INFINIBAND, linkStateData);
    auto memSensorCpu0 = createStateSensor(0x804, PLDM_ENTITY_MEMORY_CONTROLLER,
                                           performanceStateData);
    auto memSensorUnknown = createStateSensor(
        0x805, PLDM_ENTITY_MEMORY_CONTROLLER, performanceStateData);

    ethSensor->setInventoryPaths({associationPath}, false);
    ethSensorFallback->setInventoryPaths({associationPath}, false);
    ibSensor->setInventoryPaths({associationPath}, false);
    memSensorCpu0->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis59/CPU_0"},
        false);
    memSensorUnknown->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis59/cpu_unknown"},
        false);

    std::vector<pldm::dbus::PathAssociation> portAssociations{
        {"chassis", "all_states", associationPath},
        {"associated_port", "associated_port",
         "/xyz/openbmc_project/inventory/system/fabrics/fabric0/port0"}};
    const std::string ethernetProtocol{
        "xyz.openbmc_project.Inventory.Decorator.PortInfo.PortProtocol.Ethernet"};
    terminus.sensorPortInfoOverwriteTbl[0x801] = std::make_tuple(
        PortType::UpstreamPort, ethernetProtocol, 25000, portAssociations);
    terminus.sensorPortInfoOverwriteTbl[0x802] = std::make_tuple(
        PortType::BidirectionalPort, std::string(""), 10000, portAssociations);
    terminus.sensorPortInfoOverwriteTbl[0x803] = std::make_tuple(
        PortType::DownstreamPort, std::string(""), 50000, portAssociations);

    auto sensorEventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    sensorEventInfo->impactedComponent = "CoverageComponent";
    sensorEventInfo->eventIdsMap["LinkDown"] = "ResourceEvent.1.0.LinkDown";
    terminus.sensorEventInfoOverwriteTbl[0x801] = sensorEventInfo;
    terminus.sensorEventInfoOverwriteTbl[0x803] = sensorEventInfo;

    auto numericSensorPdr = makeNumericSensorValuePdrStruct(0x901);
    std::string numericSensorName{"oem_numeric_sensor_901"};
    auto numericSensor = std::make_shared<NumericSensor>(
        terminus.getTid(), false, numericSensorPdr, numericSensorName,
        associationPath, nullptr);
    terminus.numericSensors.emplace_back(numericSensor);
    terminus.sensorEventInfoOverwriteTbl[0x901] = sensorEventInfo;

    std::string switchType{
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet"};
    std::vector<std::string> switchProtocols{
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet",
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.PCIe"};
    std::vector<pldm::dbus::PathAssociation> switchAssociations{
        {"chassis", "all_states", associationPath}};
    terminus.switchBandwidthSensor =
        std::make_shared<oem_nvidia::SwitchBandwidthSensor>(
            terminus.getTid(), "switch_bandwidth_cov", switchType,
            switchProtocols, switchAssociations);

    auto updateRc =
        syncWaitWithDbusIo(nvidia::nvidiaUpdateAssociations(terminus));
    ASSERT_TRUE(updateRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateRc));

    auto ethStateSet = std::dynamic_pointer_cast<StateSetEthIBPortLinkState>(
        ethSensor->stateSets[0]);
    auto ethFallbackStateSet =
        std::dynamic_pointer_cast<StateSetEthIBPortLinkState>(
            ethSensorFallback->stateSets[0]);
    auto ibStateSet = std::dynamic_pointer_cast<StateSetEthIBPortLinkState>(
        ibSensor->stateSets[0]);
    ASSERT_NE(nullptr, ethStateSet);
    ASSERT_NE(nullptr, ethFallbackStateSet);
    ASSERT_NE(nullptr, ibStateSet);
    EXPECT_TRUE(ethStateSet->isDerivedSensorAssociated());
    EXPECT_TRUE(ethFallbackStateSet->isDerivedSensorAssociated());
    EXPECT_TRUE(ibStateSet->isDerivedSensorAssociated());
    EXPECT_EQ("switch_bandwidth_cov",
              terminus.switchBandwidthSensor->getSensorName());
    EXPECT_GT(terminus.switchBandwidthSensor->switchIntf->maxBandwidth(), 0);

    EXPECT_NE(nullptr, ethSensor->getSensorEventInfo());
    EXPECT_NE(nullptr, ibSensor->getSensorEventInfo());
    EXPECT_NE(nullptr, numericSensor->getSensorEventInfo());

    auto updateSecondRc =
        syncWaitWithDbusIo(nvidia::nvidiaUpdateAssociations(terminus));
    ASSERT_TRUE(updateSecondRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateSecondRc));

    auto memPerformanceStateSet =
        std::dynamic_pointer_cast<StateSetMemoryPerformance>(
            memSensorCpu0->stateSets[0]);
    ASSERT_NE(nullptr, memPerformanceStateSet);
    memPerformanceStateSet->updateShmemReading("Value");
}

TEST_F(TerminusTest,
       nvidiaUpdateAssociationsUpdatesEventInfoWhenStateSetIsMissingCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000017a");
    Terminus terminus(0xBA, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassisBA"};

    StateSetData linkStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_LINKSTATE),
                        PossibleStates{PLDM_STATESET_LINK_STATE_DISCONNECTED,
                                       PLDM_STATESET_LINK_STATE_CONNECTED});
    StateSetInfo info =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_ETHERNET, 1},
                        std::vector<StateSetData>{linkStateData});
    auto sensor =
        std::make_shared<StateSensor>(terminus.getTid(), false, 0x840, info,
                                      nullptr, associationPath, nullptr);
    terminus.stateSensors.emplace_back(sensor);
    sensor->setInventoryPaths({associationPath}, false);
    sensor->stateSets[0].reset();

    std::vector<pldm::dbus::PathAssociation> portAssociations{
        {"chassis", "all_states", associationPath},
        {"associated_port", "associated_port",
         "/xyz/openbmc_project/inventory/system/fabrics/fabricBA/port0"}};
    terminus.sensorPortInfoOverwriteTbl[0x840] = std::make_tuple(
        PortType::UpstreamPort, std::string(""), 25000, portAssociations);
    terminus.sensorEventInfoOverwriteTbl[0x840] =
        std::make_shared<pldm::utils::SensorEventInfo>(
            "CoveragePort", std::unordered_map<std::string, std::string>{
                                {"LinkDown", "ResourceEvent.1.0.LinkDown"}});

    auto updateRc =
        syncWaitWithDbusIo(nvidia::nvidiaUpdateAssociations(terminus));
    ASSERT_TRUE(updateRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateRc));
    ASSERT_NE(nullptr, sensor->getSensorEventInfo());
    EXPECT_EQ("CoveragePort", sensor->getSensorEventInfo()->impactedComponent);
}

TEST_F(TerminusTest, nvidiaUpdateAssociationsEmptyPortProtocolFallbackCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000017b");
    Terminus terminus(0xBB, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassisBB"};

    StateSetData linkStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_LINKSTATE),
                        PossibleStates{PLDM_STATESET_LINK_STATE_DISCONNECTED,
                                       PLDM_STATESET_LINK_STATE_CONNECTED});
    StateSetInfo info =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_ETHERNET, 1},
                        std::vector<StateSetData>{linkStateData});
    auto sensor =
        std::make_shared<StateSensor>(terminus.getTid(), false, 0x841, info,
                                      nullptr, associationPath, nullptr);
    terminus.stateSensors.emplace_back(sensor);
    sensor->setInventoryPaths({associationPath}, false);

    std::vector<pldm::dbus::PathAssociation> portAssociations{
        {"chassis", "all_states", associationPath},
        {"associated_port", "associated_port",
         "/xyz/openbmc_project/inventory/system/fabrics/fabricBB/port0"}};
    terminus.sensorPortInfoOverwriteTbl[0x841] = std::make_tuple(
        PortType::DownstreamPort, std::string(""), 40000, portAssociations);

    auto updateRc =
        syncWaitWithDbusIo(nvidia::nvidiaUpdateAssociations(terminus));
    ASSERT_TRUE(updateRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateRc));

    auto stateSet = std::dynamic_pointer_cast<StateSetEthIBPortLinkState>(
        sensor->stateSets[0]);
    ASSERT_NE(nullptr, stateSet);
    EXPECT_EQ("chassisBB", sensor->getAssociationEntityId());
}

TEST_F(TerminusTest, nvidiaUpdateAssociationsConfiguredPortProtocolCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000017d");
    Terminus terminus(0xBD, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassisBD"};

    StateSetData linkStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_LINKSTATE),
                        PossibleStates{PLDM_STATESET_LINK_STATE_DISCONNECTED,
                                       PLDM_STATESET_LINK_STATE_CONNECTED});
    StateSetInfo info =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_ETHERNET, 1},
                        std::vector<StateSetData>{linkStateData});
    auto sensor =
        std::make_shared<StateSensor>(terminus.getTid(), false, 0x84D, info,
                                      nullptr, associationPath, nullptr);
    terminus.stateSensors.emplace_back(sensor);
    sensor->setInventoryPaths({associationPath}, false);

    std::vector<pldm::dbus::PathAssociation> portAssociations{
        {"chassis", "all_states", associationPath}};
    terminus.sensorPortInfoOverwriteTbl[0x84D] = std::make_tuple(
        PortType::UpstreamPort,
        std::string(
            "xyz.openbmc_project.Inventory.Decorator.PortInfo.PortProtocol.InfiniBand"),
        100000, portAssociations);

    auto updateRc =
        syncWaitWithDbusIo(nvidia::nvidiaUpdateAssociations(terminus));
    ASSERT_TRUE(updateRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateRc));
    EXPECT_EQ("chassisBD", sensor->getAssociationEntityId());
}

TEST_F(TerminusTest, nvidiaUpdateAssociationsConfiguredEthernetProtocolCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000017f");
    Terminus terminus(0xBF, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassisBF"};

    StateSetData linkStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_LINKSTATE),
                        PossibleStates{PLDM_STATESET_LINK_STATE_DISCONNECTED,
                                       PLDM_STATESET_LINK_STATE_CONNECTED});
    StateSetInfo info =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_ETHERNET, 1},
                        std::vector<StateSetData>{linkStateData});
    auto sensor =
        std::make_shared<StateSensor>(terminus.getTid(), false, 0x84F, info,
                                      nullptr, associationPath, nullptr);
    terminus.stateSensors.emplace_back(sensor);
    sensor->setInventoryPaths({associationPath}, false);

    std::vector<pldm::dbus::PathAssociation> portAssociations{
        {"chassis", "all_states", associationPath},
        {"associated_port", "associated_port",
         "/xyz/openbmc_project/inventory/system/fabrics/fabricBF/port0"}};
    terminus.sensorPortInfoOverwriteTbl[0x84F] = std::make_tuple(
        PortType::UpstreamPort,
        std::string(
            "xyz.openbmc_project.Inventory.Decorator.PortInfo.PortProtocol.Ethernet"),
        50000, portAssociations);

    auto updateRc =
        syncWaitWithDbusIo(nvidia::nvidiaUpdateAssociations(terminus));
    ASSERT_TRUE(updateRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateRc));
    EXPECT_EQ("chassisBF", sensor->getAssociationEntityId());
}

TEST_F(TerminusTest, nvidiaUpdateAssociationsInfiniBandFallbackCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000017e");
    Terminus terminus(0xBE, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassisBE"};

    StateSetData linkStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_LINKSTATE),
                        PossibleStates{PLDM_STATESET_LINK_STATE_DISCONNECTED,
                                       PLDM_STATESET_LINK_STATE_CONNECTED});
    StateSetInfo info =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_INFINIBAND, 1},
                        std::vector<StateSetData>{linkStateData});
    auto sensor =
        std::make_shared<StateSensor>(terminus.getTid(), false, 0x84E, info,
                                      nullptr, associationPath, nullptr);
    terminus.stateSensors.emplace_back(sensor);
    sensor->setInventoryPaths({associationPath}, false);

    std::vector<pldm::dbus::PathAssociation> portAssociations{
        {"chassis", "all_states", associationPath}};
    terminus.sensorPortInfoOverwriteTbl[0x84E] = std::make_tuple(
        PortType::DownstreamPort, std::string(""), 100000, portAssociations);

    auto updateRc =
        syncWaitWithDbusIo(nvidia::nvidiaUpdateAssociations(terminus));
    ASSERT_TRUE(updateRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateRc));
    EXPECT_EQ("chassisBE", sensor->getAssociationEntityId());
}

TEST_F(TerminusTest, nvidiaUpdateAssociationsWithoutSwitchSensorCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000017c");
    Terminus terminus(0xBC, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassisBC"};

    StateSetData linkStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_LINKSTATE),
                        PossibleStates{PLDM_STATESET_LINK_STATE_DISCONNECTED,
                                       PLDM_STATESET_LINK_STATE_CONNECTED});
    StateSetInfo info =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_ETHERNET, 1},
                        std::vector<StateSetData>{linkStateData});
    auto sensor =
        std::make_shared<StateSensor>(terminus.getTid(), false, 0x842, info,
                                      nullptr, associationPath, nullptr);
    terminus.stateSensors.emplace_back(sensor);
    sensor->setInventoryPaths({associationPath}, false);

    std::vector<pldm::dbus::PathAssociation> portAssociations{
        {"chassis", "all_states", associationPath},
        {"associated_port", "associated_port",
         "/xyz/openbmc_project/inventory/system/fabrics/fabricBC/port0"}};
    terminus.sensorPortInfoOverwriteTbl[0x842] = std::make_tuple(
        PortType::UpstreamPort, std::string(""), 25000, portAssociations);
    terminus.sensorEventInfoOverwriteTbl[0x842] =
        std::make_shared<pldm::utils::SensorEventInfo>(
            "CoverageNoSwitch",
            std::unordered_map<std::string, std::string>{
                {"LinkDown", "ResourceEvent.1.0.LinkDown"}});

    auto updateRc =
        syncWaitWithDbusIo(nvidia::nvidiaUpdateAssociations(terminus));
    ASSERT_TRUE(updateRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateRc));

    auto ethStateSet = std::dynamic_pointer_cast<StateSetEthIBPortLinkState>(
        sensor->stateSets[0]);
    ASSERT_NE(nullptr, ethStateSet);
    EXPECT_FALSE(ethStateSet->isDerivedSensorAssociated());
    ASSERT_NE(nullptr, sensor->getSensorEventInfo());
    EXPECT_EQ("CoverageNoSwitch",
              sensor->getSensorEventInfo()->impactedComponent);
}

TEST_F(TerminusTest,
       nvidiaUpdateAssociationsMissingPortInfoStillUpdatesEventCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000017d");
    Terminus terminus(0xBD, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassisBD"};

    StateSetData linkStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_LINKSTATE),
                        PossibleStates{PLDM_STATESET_LINK_STATE_DISCONNECTED,
                                       PLDM_STATESET_LINK_STATE_CONNECTED});
    StateSetInfo info =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_ETHERNET, 1},
                        std::vector<StateSetData>{linkStateData});
    auto sensor =
        std::make_shared<StateSensor>(terminus.getTid(), false, 0x843, info,
                                      nullptr, associationPath, nullptr);
    terminus.stateSensors.emplace_back(sensor);
    sensor->setInventoryPaths({associationPath}, false);

    terminus.sensorEventInfoOverwriteTbl[0x843] =
        std::make_shared<pldm::utils::SensorEventInfo>(
            "CoverageMissingPortInfo",
            std::unordered_map<std::string, std::string>{
                {"LinkDown", "ResourceEvent.1.0.LinkDown"}});

    auto updateRc =
        syncWaitWithDbusIo(nvidia::nvidiaUpdateAssociations(terminus));
    ASSERT_TRUE(updateRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateRc));

    auto ethStateSet = std::dynamic_pointer_cast<StateSetEthIBPortLinkState>(
        sensor->stateSets[0]);
    ASSERT_NE(nullptr, ethStateSet);
    EXPECT_FALSE(ethStateSet->isDerivedSensorAssociated());
    ASSERT_NE(nullptr, sensor->getSensorEventInfo());
    EXPECT_EQ("CoverageMissingPortInfo",
              sensor->getSensorEventInfo()->impactedComponent);
}

TEST_F(TerminusTest,
       nvidiaUpdateAssociationsMemoryPerformanceMissingProcModuleCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000017b");
    Terminus terminus(0xBB, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassisBB"};

    StateSetData performanceStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_PERFORMANCE),
                        PossibleStates{PLDM_STATESET_PERFORMANCE_NORMAL,
                                       PLDM_STATESET_PERFORMANCE_THROTTLED});
    StateSetInfo info =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_MEMORY_CONTROLLER, 1},
                        std::vector<StateSetData>{performanceStateData});
    auto sensor =
        std::make_shared<StateSensor>(terminus.getTid(), false, 0x841, info,
                                      nullptr, associationPath, nullptr);
    terminus.stateSensors.emplace_back(sensor);
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassisBB/CPU_0"},
        false);

    terminus.entityAssociations.emplace(
        1,
        std::make_pair(EntityInfo{overallSystemCotainerId, PLDM_ENTITY_PROC, 1},
                       std::set<EntityInfo>{}));
    terminus.sensorEventInfoOverwriteTbl[0x841] =
        std::make_shared<pldm::utils::SensorEventInfo>(
            "CoverageMemory",
            std::unordered_map<std::string, std::string>{
                {"MemoryThrottle",
                 "ResourceEvent.1.0.ResourceErrorsDetected"}});

    auto updateRc =
        syncWaitWithDbusIo(nvidia::nvidiaUpdateAssociations(terminus));
    ASSERT_TRUE(updateRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateRc));
    ASSERT_NE(nullptr, sensor->getSensorEventInfo());
    EXPECT_EQ("CoverageMemory",
              sensor->getSensorEventInfo()->impactedComponent);
}

TEST_F(
    TerminusTest,
    nvidiaUpdateAssociationsMemoryPerformanceUsesLaterProcModuleAncestorCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000017f");
    Terminus terminus(0xBF, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassisBF"};

    StateSetData performanceStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_PERFORMANCE),
                        PossibleStates{PLDM_STATESET_PERFORMANCE_NORMAL,
                                       PLDM_STATESET_PERFORMANCE_THROTTLED});
    StateSetInfo info =
        std::make_tuple(EntityInfo{41, PLDM_ENTITY_MEMORY_CONTROLLER, 1},
                        std::vector<StateSetData>{performanceStateData});
    auto sensor =
        std::make_shared<StateSensor>(terminus.getTid(), false, 0x84F, info,
                                      nullptr, associationPath, nullptr);
    terminus.stateSensors.emplace_back(sensor);
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassisBF/CPU_0"},
        false);

    terminus.entityAssociations.emplace(
        41, std::make_pair(EntityInfo{30, PLDM_ENTITY_PROC, 1},
                           std::set<EntityInfo>{}));
    terminus.entityAssociations.emplace(
        30, std::make_pair(
                EntityInfo{20, PLDM_ENTITY_SYS_BOARD, 1},
                std::set<EntityInfo>{EntityInfo{30, PLDM_ENTITY_PROC, 1}}));
    terminus.entityAssociations.emplace(
        20,
        std::make_pair(
            EntityInfo{overallSystemCotainerId, PLDM_ENTITY_PROC_IO_MODULE, 4},
            std::set<EntityInfo>{EntityInfo{20, PLDM_ENTITY_SYS_BOARD, 1}}));

    auto updateRc =
        syncWaitWithDbusIo(nvidia::nvidiaUpdateAssociations(terminus));
    ASSERT_TRUE(updateRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateRc));
}

TEST_F(
    TerminusTest,
    nvidiaUpdateAssociationsMemoryPerformanceUsesTerminusInstanceOverrideCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000180");
    Terminus terminus(0xC0, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setInstance(12);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassisC0"};

    StateSetData performanceStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_PERFORMANCE),
                        PossibleStates{PLDM_STATESET_PERFORMANCE_NORMAL,
                                       PLDM_STATESET_PERFORMANCE_THROTTLED});
    StateSetInfo info =
        std::make_tuple(EntityInfo{51, PLDM_ENTITY_MEMORY_CONTROLLER, 1},
                        std::vector<StateSetData>{performanceStateData});
    auto sensor =
        std::make_shared<StateSensor>(terminus.getTid(), false, 0x850, info,
                                      nullptr, associationPath, nullptr);
    terminus.stateSensors.emplace_back(sensor);
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassisC0/CPU_0"},
        false);

    terminus.entityAssociations.emplace(
        51, std::make_pair(EntityInfo{40, PLDM_ENTITY_PROC, 1},
                           std::set<EntityInfo>{}));
    terminus.entityAssociations.emplace(
        40,
        std::make_pair(
            EntityInfo{overallSystemCotainerId, PLDM_ENTITY_PROC_IO_MODULE, 7},
            std::set<EntityInfo>{EntityInfo{40, PLDM_ENTITY_PROC, 1}}));

    auto updateRc =
        syncWaitWithDbusIo(nvidia::nvidiaUpdateAssociations(terminus));
    ASSERT_TRUE(updateRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateRc));
}

TEST_F(TerminusTest, nvidiaEnergyCountPdrParserCoverageMatrix)
{
    auto createVendorData = [](uint8_t sensorDataSize) {
        pldm_oem_energycount_numeric_sensor_value_pdr pdr{};
        pdr.terminus_handle = 1;
        pdr.nvidia_oem_pdr_type = static_cast<uint8_t>(
            nvidia::NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_SENSOR_ENERGYCOUNT);
        pdr.sensor_id = 0x77;
        pdr.entity_type = PLDM_ENTITY_SYS_BOARD;
        pdr.entity_instance_num = 1;
        pdr.container_id = 1;
        pdr.base_unit = PLDM_SENSOR_UNIT_WATTS;
        pdr.sensor_data_size = sensorDataSize;
        pdr.update_interval = 1.0f;
        switch (sensorDataSize)
        {
            case PLDM_SENSOR_DATA_SIZE_UINT8:
            case PLDM_SENSOR_DATA_SIZE_SINT8:
                pdr.max_readable.value_u8 = 0x7F;
                pdr.min_readable.value_u8 = 0x01;
                break;
            case PLDM_SENSOR_DATA_SIZE_UINT16:
            case PLDM_SENSOR_DATA_SIZE_SINT16:
                pdr.max_readable.value_u16 = 0x1234;
                pdr.min_readable.value_u16 = 0x10;
                break;
            case PLDM_SENSOR_DATA_SIZE_UINT32:
            case PLDM_SENSOR_DATA_SIZE_SINT32:
                pdr.max_readable.value_u32 = 0x12345678;
                pdr.min_readable.value_u32 = 0x1000;
                break;
            case PLDM_SENSOR_DATA_SIZE_UINT64:
            case PLDM_SENSOR_DATA_SIZE_SINT64:
                pdr.max_readable.value_u64 = 0x123456789ABCDEF0ull;
                pdr.min_readable.value_u64 = 0x10000ull;
                break;
            default:
                break;
        }
        return structToBytes(pdr);
    };

    const std::array<uint8_t, 9> sensorDataSizes{
        PLDM_SENSOR_DATA_SIZE_UINT8,
        PLDM_SENSOR_DATA_SIZE_SINT8,
        PLDM_SENSOR_DATA_SIZE_UINT16,
        PLDM_SENSOR_DATA_SIZE_SINT16,
        PLDM_SENSOR_DATA_SIZE_UINT32,
        PLDM_SENSOR_DATA_SIZE_SINT32,
        PLDM_SENSOR_DATA_SIZE_UINT64,
        PLDM_SENSOR_DATA_SIZE_SINT64,
        0xFF};

    for (auto sensorDataSize : sensorDataSizes)
    {
        auto vendorData = createVendorData(sensorDataSize);
        auto parsed = nvidia::parseOEMEnergyCountNumericSensorPDR(vendorData);
        ASSERT_NE(nullptr, parsed);
    }

    std::vector<uint8_t> tooSmall(
        PLDM_PDR_OEM_ENERGYCOUNT_NUMERIC_SENSOR_PDR_MIN_LENGTH - 1, 0);
    EXPECT_EQ(nullptr, nvidia::parseOEMEnergyCountNumericSensorPDR(tooSmall));

    auto truncatedUint64VendorData =
        createVendorData(PLDM_SENSOR_DATA_SIZE_UINT64);
    truncatedUint64VendorData.resize(
        PLDM_PDR_OEM_ENERGYCOUNT_NUMERIC_SENSOR_PDR_MIN_LENGTH);
    EXPECT_EQ(nullptr, nvidia::parseOEMEnergyCountNumericSensorPDR(
                           truncatedUint64VendorData));

    auto truncatedUint32VendorData =
        createVendorData(PLDM_SENSOR_DATA_SIZE_UINT32);
    truncatedUint32VendorData.resize(
        PLDM_PDR_OEM_ENERGYCOUNT_NUMERIC_SENSOR_PDR_MIN_LENGTH +
        sizeof(uint32_t));
    EXPECT_EQ(nullptr, nvidia::parseOEMEnergyCountNumericSensorPDR(
                           truncatedUint32VendorData));

    auto truncatedSint16VendorData =
        createVendorData(PLDM_SENSOR_DATA_SIZE_SINT16);
    truncatedSint16VendorData.resize(
        PLDM_PDR_OEM_ENERGYCOUNT_NUMERIC_SENSOR_PDR_MIN_LENGTH +
        sizeof(uint16_t) - 1);
    EXPECT_EQ(nullptr, nvidia::parseOEMEnergyCountNumericSensorPDR(
                           truncatedSint16VendorData));
}

TEST_F(TerminusTest, switchBandwidthSensorCoverage)
{
    std::string switchType{
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet"};
    std::vector<std::string> switchProtocols{
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet",
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.PCIe"};
    std::vector<pldm::dbus::PathAssociation> associations{
        {"chassis", "all_states",
         "/xyz/openbmc_project/inventory/system/chassis/chassis60"}};

    oem_nvidia::SwitchBandwidthSensor sensor(
        0x60, "switch_bandwidth_sensor_cov", switchType, switchProtocols,
        associations);
    EXPECT_EQ("switch_bandwidth_sensor_cov", sensor.getSensorName());

    sensor.updateCurrentBandwidth(std::numeric_limits<double>::quiet_NaN(),
                                  12.5);
    sensor.updateCurrentBandwidth(2.5,
                                  std::numeric_limits<double>::quiet_NaN());
    sensor.updateMaxBandwidth(100.0);
    sensor.addAssociatedSensorID(0x1234);
    sensor.updateOnSharedMemory();

    EXPECT_GT(sensor.switchIntf->maxBandwidth(), 0);
}

TEST_F(TerminusTest, nvidiaInitTerminusMirrorWattsCreatesValueIntf)
{
    std::string uuid("00000000-0000-0000-0000-000000000160");
    Terminus terminus(0x60, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/mirror0"};

    auto pdr =
        makeNumericEffecterValuePdrStruct(0x0D01, PLDM_OEM_ENTITY_TYPE_MIRROR);
    pdr->base_unit = PLDM_SENSOR_UNIT_WATTS;
    pdr->effecter_data_size = PLDM_EFFECTER_DATA_SIZE_UINT32;
    pdr->max_settable.value_u32 = 100000;
    pdr->min_settable.value_u32 = 100;
    std::string effecterName{"mirror_watts_effecter"};
    auto effecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, pdr, effecterName, associationPath,
        terminusManager);
    terminus.numericEffecters.emplace_back(effecter);

    nvidia::nvidiaInitTerminus(terminus);

    auto* valueIntf =
        dynamic_cast<NumericEffecterValueInft*>(effecter->unitIntf.get());
    ASSERT_NE(valueIntf, nullptr);

    valueIntf->handleGetNumericEffecterValue(
        EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING, 50, 40);
    EXPECT_DOUBLE_EQ(50.0, valueIntf->value());

    valueIntf->handleGetNumericEffecterValue(
        EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING, 50, 75);
    EXPECT_DOUBLE_EQ(75.0, valueIntf->value());

    valueIntf->handleGetNumericEffecterValue(EFFECTER_OPER_STATE_DISABLED, 50,
                                             75);
    EXPECT_TRUE(std::isnan(valueIntf->value()));

    valueIntf->handleErrGetNumericEffecterValue();
    EXPECT_TRUE(std::isnan(valueIntf->value()));
}

TEST_F(TerminusTest, nvidiaInitTerminusMirrorDegreesCCreatesValueIntf)
{
    std::string uuid("00000000-0000-0000-0000-000000000161");
    Terminus terminus(0x61, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/mirror1"};

    auto pdr =
        makeNumericEffecterValuePdrStruct(0x0D02, PLDM_OEM_ENTITY_TYPE_MIRROR);
    pdr->base_unit = PLDM_SENSOR_UNIT_DEGRESS_C;
    pdr->effecter_data_size = PLDM_EFFECTER_DATA_SIZE_SINT32;
    pdr->max_settable.value_s32 = 100000;
    pdr->min_settable.value_s32 = -100000;
    std::string effecterName{"mirror_temp_effecter"};
    auto effecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, pdr, effecterName, associationPath,
        terminusManager);
    terminus.numericEffecters.emplace_back(effecter);

    nvidia::nvidiaInitTerminus(terminus);

    auto* valueIntf =
        dynamic_cast<NumericEffecterValueInft*>(effecter->unitIntf.get());
    ASSERT_NE(valueIntf, nullptr);
}

TEST_F(TerminusTest, nvidiaInitTerminusMirrorUnsupportedUnitFallsBack)
{
    std::string uuid("00000000-0000-0000-0000-000000000162");
    Terminus terminus(0x62, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/mirror2"};

    auto pdr =
        makeNumericEffecterValuePdrStruct(0x0D03, PLDM_OEM_ENTITY_TYPE_MIRROR);
    pdr->base_unit = PLDM_SENSOR_UNIT_HERTZ;
    std::string effecterName{"mirror_hertz_effecter"};
    auto effecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, pdr, effecterName, associationPath,
        terminusManager);
    terminus.numericEffecters.emplace_back(effecter);

    nvidia::nvidiaInitTerminus(terminus);

    auto* valueIntf =
        dynamic_cast<NumericEffecterValueInft*>(effecter->unitIntf.get());
    EXPECT_EQ(valueIntf, nullptr);
    EXPECT_NE(effecter->unitIntf.get(), nullptr);
}

TEST_F(TerminusTest, nvidiaInitTerminusMirrorValueSetterPath)
{
    std::string uuid("00000000-0000-0000-0000-000000000163");
    Terminus terminus(0x63, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/mirror3"};

    auto pdr =
        makeNumericEffecterValuePdrStruct(0x0D04, PLDM_OEM_ENTITY_TYPE_MIRROR);
    pdr->base_unit = PLDM_SENSOR_UNIT_WATTS;
    pdr->effecter_data_size = PLDM_EFFECTER_DATA_SIZE_UINT32;
    pdr->max_settable.value_u32 = 100000;
    pdr->min_settable.value_u32 = 100;
    std::string effecterName{"mirror_value_setter"};
    auto effecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, pdr, effecterName, associationPath,
        terminusManager);
    terminus.numericEffecters.emplace_back(effecter);

    nvidia::nvidiaInitTerminus(terminus);

    auto* valueIntf =
        dynamic_cast<NumericEffecterValueInft*>(effecter->unitIntf.get());
    ASSERT_NE(valueIntf, nullptr);

    valueIntf->handleGetNumericEffecterValue(
        EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING, 25, 25);
    EXPECT_DOUBLE_EQ(25.0, valueIntf->value());

    // Enqueue the SET response for the async SetNumericEffecterValue command
    std::vector<uint8_t> response;
    response = makeSetNumericEffecterValueResp(PLDM_SUCCESS);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));

    // Setting a valid value must not throw; the D-Bus value updates on the
    // next polling cycle, not synchronously
    EXPECT_NO_THROW((void)valueIntf->value(500));

    // Simulate the polling cycle returning the acknowledged value
    valueIntf->handleGetNumericEffecterValue(
        EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING, 500, 500);
    EXPECT_DOUBLE_EQ(500.0, valueIntf->value());

    // Out-of-range values must throw before any command is sent
    EXPECT_THROW((void)valueIntf->value(200000), errors::InvalidArgument);
    EXPECT_THROW((void)valueIntf->value(50), errors::InvalidArgument);
}

TEST_F(TerminusTest, switchBandwidthSensorEmptyCollectionsCoverage)
{
    std::string switchType{
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet"};
    std::vector<std::string> switchProtocols{};
    std::vector<pldm::dbus::PathAssociation> associations{};

    oem_nvidia::SwitchBandwidthSensor sensor(
        0x61, "switch_bandwidth_empty_cov", switchType, switchProtocols,
        associations);
    EXPECT_EQ("switch_bandwidth_empty_cov", sensor.getSensorName());
    EXPECT_TRUE(sensor.associationDefinitionsIntf->associations().empty());
    EXPECT_TRUE(sensor.switchIntf->supportedProtocols().empty());

    sensor.updateCurrentBandwidth(std::numeric_limits<double>::quiet_NaN(),
                                  std::numeric_limits<double>::quiet_NaN());
    EXPECT_DOUBLE_EQ(0.0, sensor.switchIntf->currentBandwidth());

    sensor.updateMaxBandwidth(0.0);
    EXPECT_DOUBLE_EQ(0.0, sensor.switchIntf->maxBandwidth());
}

TEST_F(TerminusTest, interfaceAddedEntityAndAssociationConflictCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000161");
    Terminus terminus(0x61, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.initalized = true;

    auto rawBus = sdbusplus::bus::new_default();
    pldm::dbus::PropertyMap properties;
    pldm::dbus::InterfaceMap interfaces;
    interfaces.emplace(std::string(entityInterfaces.begin()->second),
                       properties);
    auto msg = rawBus.new_method_call("org.test",
                                      "/xyz/openbmc_project/inventory/test",
                                      "org.test.Interface", "Method");
    msg.append(sdbusplus::object_path("/xyz/openbmc_project/inventory/test"),
               interfaces);
    sealAndRewind(msg);
    terminus.refreshAssociationsTaskHandle.reset();
    EXPECT_NO_THROW(terminus.interfaceAdded(msg));
    EXPECT_TRUE(terminus.refreshAssociationsTaskHandle.has_value());

    interfaces.clear();
    interfaces.emplace("xyz.openbmc_project.Configuration.NsmDeviceAssociation",
                       properties);
    auto nsmMsg =
        rawBus.new_method_call("org.test", "/xyz/openbmc_project/inventory/nsm",
                               "org.test.Interface", "Method");
    nsmMsg.append(sdbusplus::object_path("/xyz/openbmc_project/inventory/nsm"),
                  interfaces);
    sealAndRewind(nsmMsg);
    EXPECT_NO_THROW(terminus.interfaceAdded(nsmMsg));

    auto entityAssociationPdr = makeEntityAssociationPdr();
    terminus.parseEntityAssociationPDR(entityAssociationPdr);
    auto conflictingPdr = makeEntityAssociationPdr();
    auto* conflict = reinterpret_cast<pldm_pdr_entity_association*>(
        conflictingPdr.data() + sizeof(pldm_pdr_hdr));
    conflict->container.entity_type =
        static_cast<uint16_t>(conflict->container.entity_type + 1);
    terminus.parseEntityAssociationPDR(conflictingPdr);
    EXPECT_EQ(1u, terminus.entityAssociations.size());

    waitForRefreshAssociationsTask(terminus);
}

TEST_F(TerminusTest, auxiliaryNameFallbackCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000163");
    Terminus terminus(0x63, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setTerminusName("TerminusAux");

    pldm::platform_mc::AuxiliaryNames nonEnglishSensorNames{
        {{"fr", "Capteur"}}};
    terminus.sensorAuxiliaryNamesTbl.emplace_back(
        std::make_shared<SensorAuxiliaryNames>(0x903, 1,
                                               nonEnglishSensorNames));
    EXPECT_FALSE(terminus.getAuxNameForNumericSensor(0x903).has_value());

    auto stateInfo = makeSimpleStateSetInfo();
    terminus.addStateSensor(0x903, stateInfo);
    ASSERT_EQ(1u, terminus.stateSensors.size());
    ASSERT_EQ(1u, terminus.stateSensors[0]->stateSets.size());

    pldm::platform_mc::AuxiliaryNames multiCompositeSensorNames{
        {{"en", "FirstComposite"}}, {{"en", "SecondComposite"}}};
    terminus.sensorAuxiliaryNamesTbl.emplace_back(
        std::make_shared<SensorAuxiliaryNames>(0x904, 2,
                                               multiCompositeSensorNames));
    EXPECT_FALSE(terminus.getAuxNameForNumericSensor(0x904).has_value());

    auto numericPdr = makeNumericSensorValuePdrStruct(0x904);
    terminus.addNumericSensor(numericPdr);
    ASSERT_EQ(1u, terminus.numericSensors.size());
    EXPECT_NE(std::string::npos,
              terminus.numericSensors.back()->getSensorName().find(
                  "TerminusAux_PLDM_Sensor_2308"));

    std::vector<std::vector<std::pair<NameLanguageTag, EffecterName>>>
        nonEnglishEffecterNames{{{{"fr", "Commande"}}}};
    terminus.effecterAuxiliaryNamesTbl.emplace_back(
        std::make_shared<EffecterAuxiliaryNames>(0x905, 1,
                                                 nonEnglishEffecterNames));
    auto numericEffecterPdr = makeNumericEffecterValuePdrStruct(0x905);
    numericEffecterPdr->effecter_auxiliary_names = true;
    terminus.addNumericEffecter(numericEffecterPdr);
    ASSERT_EQ(1u, terminus.numericEffecters.size());
    EXPECT_NE(std::string::npos, terminus.numericEffecters.back()->path.find(
                                     "TerminusAux_PLDM_Effecter_2309"));

    terminus.effecterAuxiliaryNamesTbl.emplace_back(
        std::make_shared<EffecterAuxiliaryNames>(0x906, 1,
                                                 nonEnglishEffecterNames));
    terminus.addStateEffecter(
        0x906, makeSingleStateSetInfo(PLDM_ENTITY_SYS_BOARD,
                                      PLDM_STATESET_ID_BOOT_REQUEST));
    ASSERT_EQ(1u, terminus.stateEffecters.size());
    ASSERT_EQ(1u, terminus.stateEffecters.back()->stateSets.size());
}

TEST_F(TerminusTest, addNumericSensorOverwriteAndEventInfoCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000169");
    Terminus terminus(0x69, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setTerminusName("TerminusAdd");
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis69";

    pldm::platform_mc::AuxiliaryNames overwriteNames{
        {{"en", "ExplicitNumeric"}}};
    terminus.sensorAuxNameOverwriteTbl[0xA10] =
        std::make_tuple(overwriteNames, terminus.systemInventoryPath);
#ifdef OEM_NVIDIA
    terminus.sensorEventInfoOverwriteTbl[0xA10] =
        std::make_shared<pldm::utils::SensorEventInfo>(
            "CPU69", std::unordered_map<std::string, std::string>{
                         {"PLDM_SENSOR_UPPERCRITICAL", "EID69"}});
#endif

    auto overwritePdr = makeNumericSensorValuePdrStruct(0xA10);
    terminus.addNumericSensor(overwritePdr);
    ASSERT_EQ(1u, terminus.numericSensors.size());
    EXPECT_NE(std::string::npos,
              terminus.numericSensors.back()->getSensorName().find(
                  "ExplicitNumeric"));
    EXPECT_EQ(std::string::npos,
              terminus.numericSensors.back()->getSensorName().find(
                  "TerminusAdd_ExplicitNumeric"));
#ifdef OEM_NVIDIA
    ASSERT_NE(nullptr, terminus.numericSensors.back()->getSensorEventInfo());
    EXPECT_EQ("CPU69", terminus.numericSensors.back()
                           ->getSensorEventInfo()
                           ->impactedComponent);
#endif

    pldm::platform_mc::AuxiliaryNames pdrNames{{{"en", "PdrNumeric"}}};
    terminus.sensorAuxiliaryNamesTbl.emplace_back(
        std::make_shared<SensorAuxiliaryNames>(0xA11, 1, pdrNames));
    auto pdrSensor = makeNumericSensorValuePdrStruct(0xA11);
    terminus.addNumericSensor(pdrSensor);
    ASSERT_EQ(2u, terminus.numericSensors.size());
    EXPECT_NE(std::string::npos,
              terminus.numericSensors.back()->getSensorName().find(
                  "TerminusAdd_PdrNumeric"));
}

TEST_F(TerminusTest, energyCountAndNumericEffecterAuxBranchCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000016A");
    Terminus terminus(0x6A, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setTerminusName("TerminusEnergy");
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis6A";

    pldm::platform_mc::AuxiliaryNames energyAuxNames{{{"en", "EnergyAux"}}};
    terminus.sensorAuxiliaryNamesTbl.emplace_back(
        std::make_shared<SensorAuxiliaryNames>(0xA20, 1, energyAuxNames));

    auto oemEnergyPdr = makeNvidiaEnergyCountOemPdr(0xA20);
    auto* oem = reinterpret_cast<pldm_oem_pdr*>(oemEnergyPdr.data());
    auto* energy =
        reinterpret_cast<pldm_oem_energycount_numeric_sensor_value_pdr*>(
            oem->vendor_specific_data);
    energy->sensor_auxiliary_names_pdr = true;
    terminus.pdrs.emplace_back(std::move(oemEnergyPdr));

    ASSERT_TRUE(terminus.parsePDRs());
    ASSERT_EQ(1u, terminus.numericSensors.size());
    EXPECT_NE(
        std::string::npos,
        terminus.numericSensors.back()->getSensorName().find("EnergyAux"));

    auto missingAuxPdr = makeNumericEffecterValuePdrStruct(0xA21);
    missingAuxPdr->effecter_auxiliary_names = true;
    terminus.addNumericEffecter(missingAuxPdr);
    ASSERT_EQ(1u, terminus.numericEffecters.size());
    EXPECT_NE(std::string::npos, terminus.numericEffecters.back()->path.find(
                                     "TerminusEnergy_PLDM_Effecter_"));

    std::vector<std::vector<std::pair<NameLanguageTag, EffecterName>>>
        mismatchedEffecterNames{{{{"en", "FirstEffecter"}}},
                                {{{"en", "SecondEffecter"}}}};
    terminus.effecterAuxiliaryNamesTbl.emplace_back(
        std::make_shared<EffecterAuxiliaryNames>(0xA22, 2,
                                                 mismatchedEffecterNames));
    auto mismatchedAuxPdr = makeNumericEffecterValuePdrStruct(0xA22);
    mismatchedAuxPdr->effecter_auxiliary_names = true;
    terminus.addNumericEffecter(mismatchedAuxPdr);
    ASSERT_EQ(2u, terminus.numericEffecters.size());
    EXPECT_NE(std::string::npos, terminus.numericEffecters.back()->path.find(
                                     "TerminusEnergy_PLDM_Effecter_"));
}

TEST_F(TerminusTest, energyCountWithoutAuxiliaryNameCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000198");
    Terminus terminus(0x98, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setTerminusName("TerminusEnergyDefault");
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis98";

    terminus.pdrs.emplace_back(
        makeNvidiaEnergyCountOemPdr(static_cast<uint16_t>(0xA23)));

    ASSERT_TRUE(terminus.parsePDRs());
    ASSERT_EQ(1u, terminus.numericSensors.size());
    EXPECT_EQ("PLDM_Sensor_2595_152",
              terminus.numericSensors.back()->getSensorName());
}

TEST_F(TerminusTest, parsePdrsZeroCountAuxiliaryNameIntegrationCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000199");
    Terminus terminus(0x99, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setTerminusName("TerminusZeroCount");
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis99";

    constexpr uint16_t sensorId = 0xA24;
    constexpr uint16_t effecterId = 0xA25;
    terminus.pdrs.emplace_back(makeZeroCountSensorAuxNamePdr(sensorId));
    terminus.pdrs.emplace_back(makeZeroCountEffecterAuxNamePdr(effecterId));
    terminus.pdrs.emplace_back(makeStateSensorPdr(
        sensorId, PLDM_ENTITY_SYS_BOARD, PLDM_STATESET_ID_HEALTHSTATE, true));
    terminus.pdrs.emplace_back(makeNumericEffecterPdr(effecterId, true));

    ASSERT_TRUE(terminus.parsePDRs());
    ASSERT_EQ(1u, terminus.stateSensors.size());
    ASSERT_EQ(1u, terminus.numericEffecters.size());
    ASSERT_NE(nullptr, terminus.getSensorAuxiliaryNames(sensorId));
    ASSERT_NE(nullptr, terminus.getEffecterAuxiliaryNames(effecterId));
    EXPECT_EQ(0u, std::get<1>(*terminus.getSensorAuxiliaryNames(sensorId)));
    EXPECT_EQ(0u, std::get<1>(*terminus.getEffecterAuxiliaryNames(effecterId)));
    EXPECT_FALSE(terminus.getAuxNameForNumericSensor(sensorId).has_value());
    EXPECT_NE(std::string::npos, terminus.numericEffecters.back()->path.find(
                                     "TerminusZeroCount_PLDM_Effecter_"));
}

TEST_F(TerminusTest, addStateSensorAndEffecterOverwriteCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000016B");
    Terminus terminus(0x6B, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setTerminusName("TerminusStateAdd");
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis6B";

    auto healthInfo = makeSingleStateSetInfo(PLDM_ENTITY_SYS_BOARD,
                                             PLDM_STATESET_ID_HEALTHSTATE);
    auto bootInfo = makeSingleStateSetInfo(PLDM_ENTITY_SYS_BOARD,
                                           PLDM_STATESET_ID_BOOT_REQUEST);

    pldm::platform_mc::AuxiliaryNames overwriteStateNames{
        {{"en", "ExplicitState"}}};
    terminus.sensorAuxNameOverwriteTbl[0xA30] =
        std::make_tuple(overwriteStateNames, terminus.systemInventoryPath);
#ifdef OEM_NVIDIA
    terminus.sensorEventInfoOverwriteTbl[0xA30] =
        std::make_shared<pldm::utils::SensorEventInfo>(
            "CPU6B", std::unordered_map<std::string, std::string>{
                         {"LinkDown", "EID6B"}});
#endif
    terminus.addStateSensor(0xA30, healthInfo);
    ASSERT_EQ(1u, terminus.stateSensors.size());
#ifdef OEM_NVIDIA
    ASSERT_NE(nullptr, terminus.stateSensors.back()->getSensorEventInfo());
    EXPECT_EQ(
        "CPU6B",
        terminus.stateSensors.back()->getSensorEventInfo()->impactedComponent);
#endif

    pldm::platform_mc::AuxiliaryNames pdrStateNames{{{"en", "PdrState"}}};
    terminus.sensorAuxiliaryNamesTbl.emplace_back(
        std::make_shared<SensorAuxiliaryNames>(0xA31, 1, pdrStateNames));
    terminus.addStateSensor(0xA31, healthInfo);
    ASSERT_EQ(2u, terminus.stateSensors.size());

    std::vector<std::vector<std::pair<NameLanguageTag, EffecterName>>>
        effecterNames{{{{"en", "ResetReq"}}}};
    terminus.effecterAuxiliaryNamesTbl.emplace_back(
        std::make_shared<EffecterAuxiliaryNames>(0xA32, 1, effecterNames));
    terminus.addStateEffecter(0xA32, bootInfo);
    ASSERT_EQ(1u, terminus.stateEffecters.size());

    std::string emptyNameUuid("00000000-0000-0000-0000-00000000016C");
    Terminus emptyNameTerminus(0x6C, 1 << PLDM_BASE | 1 << PLDM_PLATFORM,
                               emptyNameUuid, terminusManager);
    emptyNameTerminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis6C";
    emptyNameTerminus.sensorAuxiliaryNamesTbl.emplace_back(
        std::make_shared<SensorAuxiliaryNames>(0xA33, 1, pdrStateNames));
    emptyNameTerminus.addStateSensor(0xA33, healthInfo);
    ASSERT_EQ(1u, emptyNameTerminus.stateSensors.size());

    emptyNameTerminus.effecterAuxiliaryNamesTbl.emplace_back(
        std::make_shared<EffecterAuxiliaryNames>(0xA34, 1, effecterNames));
    emptyNameTerminus.addStateEffecter(0xA34, bootInfo);
    ASSERT_EQ(1u, emptyNameTerminus.stateEffecters.size());
}

TEST_F(TerminusTest, addNumericSensorDuplicatePathExceptionCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000016E");
    Terminus terminus(0x6E, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis6E";

    auto pdr = makeNumericSensorValuePdrStruct(0xA35);
    terminus.addNumericSensor(pdr);
    ASSERT_EQ(1u, terminus.numericSensors.size());

    terminus.addNumericSensor(pdr);
    EXPECT_EQ(1u, terminus.numericSensors.size());
}

TEST_F(TerminusTest, addNumericSensorAuxNameCollisionCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000016F");
    Terminus terminus(0x6F, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis6F";

    pldm::platform_mc::AuxiliaryNames collidingAuxNames{
        {{"en", "CollidingNumeric"}}};
    terminus.sensorAuxNameOverwriteTbl[0xA36] =
        std::make_tuple(collidingAuxNames, terminus.systemInventoryPath);
    terminus.sensorAuxNameOverwriteTbl[0xA37] =
        std::make_tuple(collidingAuxNames, terminus.systemInventoryPath);

    terminus.addNumericSensor(makeNumericSensorValuePdrStruct(0xA36));
    ASSERT_EQ(1u, terminus.numericSensors.size());

    terminus.addNumericSensor(makeNumericSensorValuePdrStruct(0xA37));
    EXPECT_EQ(1u, terminus.numericSensors.size());
}

#ifdef OEM_NVIDIA
TEST_F(TerminusTest, addOemEnergyCountNumericSensorDuplicatePathCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000170");
    Terminus terminus(0x70, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis70";

    auto rawPdr = makeNvidiaEnergyCountOemPdr(0xA38);
    auto* oemPdr = reinterpret_cast<pldm_oem_pdr*>(rawPdr.data());
    auto* parsed =
        reinterpret_cast<pldm_oem_energycount_numeric_sensor_value_pdr*>(
            oemPdr->vendor_specific_data);
    auto pdr = std::make_shared<pldm_oem_energycount_numeric_sensor_value_pdr>(
        *parsed);

    terminus.addOEMEnergyCountNumericSensor(pdr);
    ASSERT_EQ(1u, terminus.numericSensors.size());

    terminus.addOEMEnergyCountNumericSensor(pdr);
    EXPECT_EQ(1u, terminus.numericSensors.size());
}
#endif

TEST_F(TerminusTest, addStateSensorDuplicatePathExceptionCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000171");
    Terminus terminus(0x71, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis71";

    auto healthInfo = makeSingleStateSetInfo(PLDM_ENTITY_SYS_BOARD,
                                             PLDM_STATESET_ID_HEALTHSTATE);
    terminus.addStateSensor(0xA39, healthInfo);
    ASSERT_EQ(1u, terminus.stateSensors.size());

    terminus.addStateSensor(0xA39, healthInfo);
    EXPECT_EQ(1u, terminus.stateSensors.size());
}

TEST_F(TerminusTest, addStateEffecterDuplicatePathExceptionCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000172");
    Terminus terminus(0x72, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis72";

    auto bootInfo = makeSingleStateSetInfo(PLDM_ENTITY_SYS_BOARD,
                                           PLDM_STATESET_ID_BOOT_REQUEST);
    terminus.addStateEffecter(0xA3A, bootInfo);
    ASSERT_EQ(1u, terminus.stateEffecters.size());

    terminus.addStateEffecter(0xA3A, bootInfo);
    EXPECT_EQ(1u, terminus.stateEffecters.size());
}

TEST_F(TerminusTest, auxiliaryNameEmptyVectorCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000016D");
    Terminus terminus(0x6D, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    pldm::platform_mc::AuxiliaryNames emptySensorNames{};
    terminus.sensorAuxiliaryNamesTbl.emplace_back(
        std::make_shared<SensorAuxiliaryNames>(0xA40, 1, emptySensorNames));
    EXPECT_FALSE(terminus.getAuxNameForNumericSensor(0xA40).has_value());
}

TEST_F(TerminusTest, auxiliaryNameEmptyCompositeCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000175");
    Terminus terminus(0x75, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setTerminusName("TerminusEmptyAux");
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis75";

    std::vector<std::vector<std::pair<NameLanguageTag, EffecterName>>>
        emptyEffecterNames;
    terminus.effecterAuxiliaryNamesTbl.emplace_back(
        std::make_shared<EffecterAuxiliaryNames>(0xA50, 1, emptyEffecterNames));

    auto numericEffecterPdr = makeNumericEffecterValuePdrStruct(0xA50);
    numericEffecterPdr->effecter_auxiliary_names = true;
    terminus.addNumericEffecter(numericEffecterPdr);
    ASSERT_EQ(1u, terminus.numericEffecters.size());
    EXPECT_NE(std::string::npos,
              terminus.numericEffecters.back()->path.find(
                  "TerminusEmptyAux_PLDM_Effecter_2640_117"));

    auto bootInfo = makeSingleStateSetInfo(PLDM_ENTITY_SYS_BOARD,
                                           PLDM_STATESET_ID_BOOT_REQUEST);
    terminus.effecterAuxiliaryNamesTbl.emplace_back(
        std::make_shared<EffecterAuxiliaryNames>(0xA51, 1, emptyEffecterNames));
    terminus.addStateEffecter(0xA51, bootInfo);
    ASSERT_EQ(1u, terminus.stateEffecters.size());
}

TEST_F(TerminusTest, addStateSensorInnerEmptyCompositeCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000176");
    Terminus terminus(0x76, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setTerminusName("TerminusInnerEmptySensor");
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis76";

    pldm::platform_mc::AuxiliaryNames sensorNames{{}};
    terminus.sensorAuxiliaryNamesTbl.emplace_back(
        std::make_shared<SensorAuxiliaryNames>(0xA60, 1, sensorNames));

    auto healthInfo = makeSingleStateSetInfo(PLDM_ENTITY_SYS_BOARD,
                                             PLDM_STATESET_ID_HEALTHSTATE);
    terminus.addStateSensor(0xA60, healthInfo);
    ASSERT_EQ(1u, terminus.stateSensors.size());
    ASSERT_EQ(1u, terminus.stateSensors.back()->stateSets.size());
    EXPECT_NE(nullptr, terminus.stateSensors.back()->stateSets.front().get());
}

TEST_F(TerminusTest, addStateEffecterInnerEmptyCompositeCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000177");
    Terminus terminus(0x77, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setTerminusName("TerminusInnerEmptyEffecter");
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis77";

    std::vector<std::vector<std::pair<NameLanguageTag, EffecterName>>>
        effecterNames{{}};
    terminus.effecterAuxiliaryNamesTbl.emplace_back(
        std::make_shared<EffecterAuxiliaryNames>(0xA61, 1, effecterNames));

    auto bootInfo = makeSingleStateSetInfo(PLDM_ENTITY_SYS_BOARD,
                                           PLDM_STATESET_ID_BOOT_REQUEST);
    terminus.addStateEffecter(0xA61, bootInfo);
    ASSERT_EQ(1u, terminus.stateEffecters.size());
    ASSERT_EQ(1u, terminus.stateEffecters.back()->stateSets.size());
    EXPECT_NE(nullptr, terminus.stateEffecters.back()->stateSets.front().get());
}

TEST_F(TerminusTest, addStateSensorTerminusNameEmptyCompositePrependedCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000179");
    Terminus terminus(0x79, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setTerminusName("TerminusPrependedSensor");
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis79";

    pldm::platform_mc::AuxiliaryNames sensorNames{{}};
    terminus.sensorAuxiliaryNamesTbl.emplace_back(
        std::make_shared<SensorAuxiliaryNames>(0xA64, 1, sensorNames));

    auto healthInfo = makeSingleStateSetInfo(PLDM_ENTITY_SYS_BOARD,
                                             PLDM_STATESET_ID_HEALTHSTATE);
    terminus.addStateSensor(0xA64, healthInfo);

    ASSERT_EQ(1u, terminus.stateSensors.size());
    ASSERT_EQ(1u, terminus.stateSensors.back()->stateSets.size());
    EXPECT_NE(nullptr, terminus.stateSensors.back()->stateSets.front().get());
}

TEST_F(TerminusTest,
       addStateEffecterTerminusNameEmptyCompositePrependedCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000017A");
    Terminus terminus(0x7A, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setTerminusName("TerminusPrependedEffecter");
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis7A";

    std::vector<std::vector<std::pair<NameLanguageTag, EffecterName>>>
        effecterNames{{}};
    terminus.effecterAuxiliaryNamesTbl.emplace_back(
        std::make_shared<EffecterAuxiliaryNames>(0xA65, 1, effecterNames));

    auto bootInfo = makeSingleStateSetInfo(PLDM_ENTITY_SYS_BOARD,
                                           PLDM_STATESET_ID_BOOT_REQUEST);
    terminus.addStateEffecter(0xA65, bootInfo);

    ASSERT_EQ(1u, terminus.stateEffecters.size());
    ASSERT_EQ(1u, terminus.stateEffecters.back()->stateSets.size());
    EXPECT_NE(nullptr, terminus.stateEffecters.back()->stateSets.front().get());
}

TEST_F(TerminusTest, addStateSensorAndEffecterMultiCompositeCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000178");
    Terminus terminus(0x78, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setTerminusName("TerminusComposite");
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis78";

    pldm::platform_mc::AuxiliaryNames sensorNames{{{"en", "Port0"}},
                                                  {{"en", "Port1"}}};
    terminus.sensorAuxiliaryNamesTbl.emplace_back(
        std::make_shared<SensorAuxiliaryNames>(0xA62, 2, sensorNames));

    std::vector<StateSetData> linkStates{
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_LINKSTATE),
                        PossibleStates{PLDM_STATESET_LINK_STATE_CONNECTED,
                                       PLDM_STATESET_LINK_STATE_DISCONNECTED}),
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_LINKSTATE),
                        PossibleStates{PLDM_STATESET_LINK_STATE_CONNECTED,
                                       PLDM_STATESET_LINK_STATE_DISCONNECTED})};
    StateSetInfo linkInfo =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_ETHERNET, 1}, linkStates);
    terminus.addStateSensor(0xA62, linkInfo);
    ASSERT_EQ(1u, terminus.stateSensors.size());
    EXPECT_EQ(2u, terminus.stateSensors.back()->stateSets.size());

    std::vector<std::vector<std::pair<NameLanguageTag, EffecterName>>>
        effecterNames{{{{"en", "Reset0"}}}, {{{"en", "Reset1"}}}};
    terminus.effecterAuxiliaryNamesTbl.emplace_back(
        std::make_shared<EffecterAuxiliaryNames>(0xA63, 2, effecterNames));

    std::vector<StateSetData> bootStates{
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_BOOT_REQUEST),
                        PossibleStates{0, 1, 2}),
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_BOOT_REQUEST),
                        PossibleStates{0, 1, 2})};
    StateSetInfo bootInfo =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1}, bootStates);
    terminus.addStateEffecter(0xA63, bootInfo);
    ASSERT_EQ(1u, terminus.stateEffecters.size());
    EXPECT_EQ(2u, terminus.stateEffecters.back()->stateSets.size());
}

TEST_F(TerminusTest, cachedFindInventoryAndProcessorInstanceGuardCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000016E");
    Terminus terminus(0x6E, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis6E";

    EntityInfo cachedEntity{1, PLDM_ENTITY_SYS_BOARD, 1};
    std::vector<std::string> cachedInventories{
        terminus.systemInventoryPath + "/board0"};
    std::vector<std::string> cachedClosest{terminus.systemInventoryPath};
    terminus.entities.emplace(cachedEntity,
                              Entity{cachedInventories, cachedClosest});

    auto cachedPaths = terminus.findInventory(cachedEntity, false);
    ASSERT_EQ(1u, cachedPaths.size());
    EXPECT_EQ(cachedInventories.front(), cachedPaths.front());

    terminus.entities.clear();
    terminus.inventories.emplace_back(terminus.systemInventoryPath + "/cpu3",
                                      PLDM_ENTITY_PROC, 3);
    auto missingStaticInstance = terminus.findInventory(
        EntityInfo{overallSystemCotainerId, PLDM_ENTITY_PROC, 1}, false);
    EXPECT_TRUE(missingStaticInstance.empty());
}

TEST_F(TerminusTest, entityAssociationLookupMissCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000016F");
    Terminus terminus(0x6F, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    auto associationPdr = makeEntityAssociationPdr();
    terminus.parseEntityAssociationPDR(associationPdr);

    auto duplicateAssociationPdr = makeEntityAssociationPdr();
    auto* duplicateAssociation = reinterpret_cast<pldm_pdr_entity_association*>(
        duplicateAssociationPdr.data() + sizeof(pldm_pdr_hdr));
    duplicateAssociation->children[0].entity_instance_num = 2;
    terminus.parseEntityAssociationPDR(duplicateAssociationPdr);

    ASSERT_EQ(1u, terminus.entityAssociations.size());
    EXPECT_EQ(2u, terminus.entityAssociations.begin()->second.second.size());

    pldm::platform_mc::AuxiliaryNames sensorNames{{{"en", "Sensor6F"}}};
    terminus.sensorAuxiliaryNamesTbl.emplace_back(
        std::make_shared<SensorAuxiliaryNames>(0xB10, 1, sensorNames));
    EXPECT_EQ(nullptr, terminus.getSensorAuxiliaryNames(0xB11));

    std::vector<std::vector<std::pair<NameLanguageTag, EffecterName>>>
        effecterNames{{{{"en", "Effecter6F"}}}};
    terminus.effecterAuxiliaryNamesTbl.emplace_back(
        std::make_shared<EffecterAuxiliaryNames>(0xB20, 1, effecterNames));
    EXPECT_EQ(nullptr, terminus.getEffecterAuxiliaryNames(0xB21));
}

TEST_F(TerminusTest, inventoryOverwritePathGuardCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000170");
    Terminus terminus(0x70, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    pldm::platform_mc::AuxiliaryNames overwriteNames{{{"en", "ExplicitPath"}}};
    terminus.sensorAuxNameOverwriteTbl[0xB30] =
        std::make_tuple(overwriteNames, "/not/an/inventory/path");
    EXPECT_EQ(std::nullopt, terminus.getInventoryPath(0xB30));

    terminus.sensorAuxNameOverwriteTbl[0xB31] = std::make_tuple(
        overwriteNames, "/xyz/openbmc_project/inventory/system/chassis/"
                        "chassis70/cpu0");
    ASSERT_TRUE(terminus.getInventoryPath(0xB31).has_value());
    EXPECT_EQ("/xyz/openbmc_project/inventory/system/chassis/chassis70/cpu0",
              terminus.getInventoryPath(0xB31).value());
}

TEST_F(TerminusTest, interfaceAddedBreakAndEmptyOfflineCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000171");
    Terminus terminus(0x71, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.initalized = true;

    auto rawBus = sdbusplus::bus::new_default();
    pldm::dbus::InterfaceMap interfaces;
    interfaces.emplace(std::string(overallSystemInterface),
                       pldm::dbus::PropertyMap{});
    auto msg = rawBus.new_method_call("org.test",
                                      "/xyz/openbmc_project/inventory/system",
                                      "org.test.Interface", "Method");
    msg.append(sdbusplus::object_path("/xyz/openbmc_project/inventory/system"),
               interfaces);
    sealAndRewind(msg);

    terminus.interfaceAdded(msg);
    EXPECT_TRUE(terminus.refreshAssociationsTaskHandle.has_value());

    std::string emptyUuid("00000000-0000-0000-0000-000000000172");
    Terminus emptyTerminus(0x72, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, emptyUuid,
                           terminusManager);
    emptyTerminus.setOffline();
    EXPECT_FALSE(emptyTerminus.resumed);

    waitForRefreshAssociationsTask(terminus);
}

TEST_F(TerminusTest, interfaceAddedEntityInterfaceCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000179");
    Terminus terminus(0x79, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.initalized = true;

    auto rawBus = sdbusplus::bus::new_default();
    pldm::dbus::InterfaceMap interfaces;
    interfaces.emplace(std::string(entityInterfaces.begin()->second),
                       pldm::dbus::PropertyMap{});
    auto msg = rawBus.new_method_call(
        "org.test", "/xyz/openbmc_project/inventory/system/chassis/chassis79",
        "org.test.Interface", "Method");
    msg.append(sdbusplus::object_path(
                   "/xyz/openbmc_project/inventory/system/chassis/chassis79"),
               interfaces);
    sealAndRewind(msg);

    EXPECT_NO_THROW(terminus.interfaceAdded(msg));
    EXPECT_TRUE(terminus.refreshAssociationsTaskHandle.has_value());

    waitForRefreshAssociationsTask(terminus);
}

TEST_F(TerminusTest, interfaceAddedEntityInterfaceCompletedHandleCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000017A");
    Terminus terminus(0x7A, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.initalized = true;
    terminus.refreshAssociationsTaskHandle.emplace();
    terminus.refreshAssociationsTaskHandle->second.emplace(PLDM_SUCCESS);

    auto rawBus = sdbusplus::bus::new_default();
    pldm::dbus::InterfaceMap interfaces;
    interfaces.emplace(std::string(entityInterfaces.begin()->second),
                       pldm::dbus::PropertyMap{});
    auto msg = rawBus.new_method_call(
        "org.test", "/xyz/openbmc_project/inventory/system/chassis/chassis7A",
        "org.test.Interface", "Method");
    msg.append(sdbusplus::object_path(
                   "/xyz/openbmc_project/inventory/system/chassis/chassis7A"),
               interfaces);
    sealAndRewind(msg);

    EXPECT_NO_THROW(terminus.interfaceAdded(msg));
    ASSERT_TRUE(terminus.refreshAssociationsTaskHandle.has_value());

    waitForRefreshAssociationsTask(terminus);
}

TEST_F(TerminusTest, interfaceAddedEntityInterfaceInProgressHandleCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000017B");
    Terminus terminus(0x7B, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.initalized = true;
    terminus.refreshAssociationsTaskHandle.emplace();

    auto rawBus = sdbusplus::bus::new_default();
    pldm::dbus::InterfaceMap interfaces;
    interfaces.emplace(std::string(entityInterfaces.begin()->second),
                       pldm::dbus::PropertyMap{});
    auto msg = rawBus.new_method_call(
        "org.test", "/xyz/openbmc_project/inventory/system/chassis/chassis7B",
        "org.test.Interface", "Method");
    msg.append(sdbusplus::object_path(
                   "/xyz/openbmc_project/inventory/system/chassis/chassis7B"),
               interfaces);
    sealAndRewind(msg);

    EXPECT_NO_THROW(terminus.interfaceAdded(msg));
    ASSERT_TRUE(terminus.refreshAssociationsTaskHandle.has_value());
    EXPECT_FALSE(terminus.refreshAssociationsTaskHandle->second.has_value());
}

TEST_F(TerminusTest, interfaceAddedNsmAssociationCoverage)
{
#ifdef OEM_NVIDIA
    std::string uuid("00000000-0000-0000-0000-000000000173");
    Terminus terminus(0x73, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.initalized = true;

    auto rawBus = sdbusplus::bus::new_default();
    pldm::dbus::InterfaceMap interfaces;
    interfaces.emplace("xyz.openbmc_project.Configuration.NsmDeviceAssociation",
                       pldm::dbus::PropertyMap{});
    auto msg = rawBus.new_method_call(
        "org.test", "/xyz/openbmc_project/inventory/system/chassis/chassis73",
        "org.test.Interface", "Method");
    msg.append(sdbusplus::object_path(
                   "/xyz/openbmc_project/inventory/system/chassis/chassis73"),
               interfaces);
    sealAndRewind(msg);

    EXPECT_NO_THROW(terminus.interfaceAdded(msg));
    EXPECT_FALSE(terminus.needRefresh);
    EXPECT_TRUE(terminus.refreshAssociationsTaskHandle.has_value());

    waitForRefreshAssociationsTask(terminus);
#endif
}

TEST_F(TerminusTest, setOfflineCollectionMatrixCoverage)
{
    std::string baseInventory =
        "/xyz/openbmc_project/inventory/system/chassis/chassis174";

    {
        std::string uuid("00000000-0000-0000-0000-000000000174");
        Terminus terminus(0x74, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                          terminusManager);
        auto numericPdr = makeNumericSensorValuePdrStruct(0x174);
        std::string sensorName{"offline_numeric_sensor"};
        auto numericSensor = std::make_shared<NumericSensor>(
            terminus.getTid(), false, numericPdr, sensorName, baseInventory,
            nullptr);
        terminus.numericSensors.emplace_back(numericSensor);

        terminus.setOffline();
        EXPECT_FALSE(terminus.resumed);
        EXPECT_TRUE(std::isnan(numericSensor->getReading()));
    }

    {
        std::string uuid("00000000-0000-0000-0000-000000000175");
        Terminus terminus(0x75, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                          terminusManager);
        auto effecterPdr = makeNumericEffecterValuePdrStruct(0x175);
        std::string effecterName{"offline_numeric_effecter"};
        auto numericEffecter = std::make_shared<NumericEffecter>(
            terminus.getTid(), false, effecterPdr, effecterName, baseInventory,
            terminusManager);
        terminus.numericEffecters.emplace_back(numericEffecter);

        EXPECT_NO_THROW(terminus.setOffline());
        EXPECT_FALSE(terminus.resumed);
    }

    {
        std::string uuid("00000000-0000-0000-0000-000000000176");
        Terminus terminus(0x76, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                          terminusManager);
        auto stateInfo = makeSimpleStateSetInfo();
        auto stateSensor = std::make_shared<StateSensor>(
            terminus.getTid(), false, 0x176, stateInfo, nullptr, baseInventory,
            nullptr);
        terminus.stateSensors.emplace_back(stateSensor);

        EXPECT_NO_THROW(terminus.setOffline());
        EXPECT_FALSE(terminus.resumed);
    }

    {
        std::string uuid("00000000-0000-0000-0000-000000000177");
        Terminus terminus(0x77, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                          terminusManager);
        auto stateInfo = makeSimpleStateSetInfo();
        auto stateEffecter = std::make_shared<StateEffecter>(
            terminus.getTid(), false, 0x177, stateInfo, nullptr, baseInventory,
            terminusManager);
        terminus.stateEffecters.emplace_back(stateEffecter);

        EXPECT_NO_THROW(terminus.setOffline());
        EXPECT_FALSE(terminus.resumed);
    }
}

TEST_F(TerminusTest, refreshAssociationsGuardCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000164");
    Terminus terminus(0x64, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.initalized = true;

    auto rawBus = sdbusplus::bus::new_default();
    pldm::dbus::InterfaceMap emptyInterfaces;
    auto msg = rawBus.new_method_call("org.test",
                                      "/xyz/openbmc_project/inventory/empty",
                                      "org.test.Interface", "Method");
    msg.append(sdbusplus::object_path("/xyz/openbmc_project/inventory/empty"),
               emptyInterfaces);
    sealAndRewind(msg);

    terminus.interfaceAdded(msg);
    EXPECT_FALSE(terminus.needRefresh);
    EXPECT_FALSE(terminus.refreshAssociationsTaskHandle.has_value());

    terminus.refreshAssociationsTaskHandle.emplace();
    EXPECT_NO_THROW(terminus.refreshAssociations());
    EXPECT_TRUE(terminus.refreshAssociationsTaskHandle.has_value());
    EXPECT_FALSE(terminus.refreshAssociationsTaskHandle->second.has_value());

    terminus.refreshAssociationsTaskHandle->second.emplace(PLDM_SUCCESS);
    terminus.needRefresh = false;
    EXPECT_NO_THROW(terminus.refreshAssociations());
    EXPECT_TRUE(terminus.refreshAssociationsTaskHandle.has_value());

    waitForRefreshAssociationsTask(terminus);
}

TEST_F(TerminusTest, refreshAssociationsTaskNeedRefreshCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000165");
    Terminus terminus(0x65, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.needRefresh = true;

    auto refreshRc = syncWaitWithDbusIo(terminus.refreshAssociationsTask());
    ASSERT_TRUE(refreshRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*refreshRc));
    EXPECT_FALSE(terminus.needRefresh);
}

TEST_F(TerminusTest, nvidiaEnergyCountMatrixAndInitErrorCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000162");
    Terminus terminus(0x62, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis62"};

    const std::array<std::pair<uint16_t, uint8_t>, 4> energyPdrCases{{
        {0x920, PLDM_SENSOR_UNIT_WATTS},
        {0x921, PLDM_SENSOR_UNIT_COUNTS},
        {0x922, PLDM_SENSOR_UNIT_SECONDS},
        {0x923, 0xFF},
    }};
    for (const auto& [sensorId, baseUnit] : energyPdrCases)
    {
        auto pdr = makeNvidiaEnergyCountOemPdr(sensorId);
        auto* oem = reinterpret_cast<pldm_oem_pdr*>(pdr.data());
        auto* energy =
            reinterpret_cast<pldm_oem_energycount_numeric_sensor_value_pdr*>(
                oem->vendor_specific_data);
        energy->base_unit = baseUnit;
        terminus.pdrs.emplace_back(std::move(pdr));
    }

    auto mismatchedNumericEffecterPdr =
        makeNumericEffecterValuePdrStruct(0x730);
    auto powerCapNumericEffecterPdr = makeNumericEffecterValuePdrStruct(0x731);
    auto remoteDebugTimeoutPdr = makeNumericEffecterValuePdrStruct(0x732);
    auto staticPowerTemperaturePdr = makeNumericEffecterValuePdrStruct(0x733);
    auto staticPowerClockPdr = makeNumericEffecterValuePdrStruct(0x734);
    auto staticPowerPowerPdr = makeNumericEffecterValuePdrStruct(0x735);

    mismatchedNumericEffecterPdr->base_unit = PLDM_SENSOR_UNIT_NONE;
    mismatchedNumericEffecterPdr->entity_type = PLDM_ENTITY_PROC;
    powerCapNumericEffecterPdr->base_unit = PLDM_SENSOR_UNIT_WATTS;
    remoteDebugTimeoutPdr->base_unit = PLDM_SENSOR_UNIT_MINUTES;
    staticPowerTemperaturePdr->base_unit = PLDM_SENSOR_UNIT_DEGRESS_C;
    staticPowerClockPdr->base_unit = PLDM_SENSOR_UNIT_HERTZ;
    staticPowerPowerPdr->base_unit = PLDM_SENSOR_UNIT_WATTS;

    std::string mismatchedNumericEffecterName{"mismatch_effecter_62"};
    std::string powerCapNumericEffecterName{"power_cap_effecter_62"};
    std::string remoteDebugTimeoutName{"remote_debug_timeout_62"};
    std::string staticPowerTemperatureName{"static_power_temp_62"};
    std::string staticPowerClockName{"static_power_clock_62"};
    std::string staticPowerPowerName{"static_power_power_62"};

    auto mismatchedNumericEffecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, mismatchedNumericEffecterPdr,
        mismatchedNumericEffecterName, associationPath, terminusManager);
    auto powerCapNumericEffecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, powerCapNumericEffecterPdr,
        powerCapNumericEffecterName, associationPath, terminusManager);
    auto remoteDebugTimeoutEffecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, remoteDebugTimeoutPdr, remoteDebugTimeoutName,
        associationPath, terminusManager);
    auto staticPowerTemperatureEffecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, staticPowerTemperaturePdr,
        staticPowerTemperatureName, associationPath, terminusManager);
    auto staticPowerClockEffecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, staticPowerClockPdr, staticPowerClockName,
        associationPath, terminusManager);
    auto staticPowerPowerEffecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, staticPowerPowerPdr, staticPowerPowerName,
        associationPath, terminusManager);
    terminus.numericEffecters.emplace_back(mismatchedNumericEffecter);
    terminus.numericEffecters.emplace_back(powerCapNumericEffecter);
    terminus.numericEffecters.emplace_back(remoteDebugTimeoutEffecter);
    terminus.numericEffecters.emplace_back(staticPowerTemperatureEffecter);
    terminus.numericEffecters.emplace_back(staticPowerClockEffecter);
    terminus.numericEffecters.emplace_back(staticPowerPowerEffecter);

    auto nonDebugStateInfo = makeSingleStateSetInfo(
        PLDM_ENTITY_SYS_BOARD, PLDM_STATESET_ID_BOOT_REQUEST);
    auto mismatchedStateEffecter = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x740, nonDebugStateInfo, nullptr,
        associationPath, terminusManager);
    auto storageStateEffecter = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x741, nonDebugStateInfo, nullptr,
        associationPath, terminusManager);
    terminus.stateEffecters.emplace_back(mismatchedStateEffecter);
    terminus.stateEffecters.emplace_back(storageStateEffecter);

    auto nonDebugStateSensor = std::make_shared<StateSensor>(
        terminus.getTid(), false, 0x742, nonDebugStateInfo, nullptr,
        associationPath, nullptr);
    terminus.stateSensors.emplace_back(nonDebugStateSensor);

    nvidia::nvidia_oem_effecter_powercap_pdr powerCapPdr{};
    powerCapPdr.terminus_handle = 1;
    powerCapPdr.oem_pdr_type = static_cast<uint8_t>(
        nvidia::NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_EFFECTER_POWERCAP);
    powerCapPdr.oem_effecter_powercap = static_cast<uint8_t>(
        nvidia::OemPowerCapPersistence::OEM_POWERCAP_TDP_NONVOLATILE);
    powerCapPdr.associated_effecterid = 0x7FF;

    nvidia::nvidia_oem_effecter_storage_pdr storagePdr{};
    storagePdr.terminus_handle = 1;
    storagePdr.oem_pdr_type = static_cast<uint8_t>(
        nvidia::NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_EFFECTER_STORAGE);
    storagePdr.oem_effecter_storage = static_cast<uint8_t>(
        nvidia::OemStorageSecureState::OEM_STORAGE_SECURE_VARIABLE);
    storagePdr.associated_effecterid = 0x7FE;

    std::vector<uint8_t> tooSmallCommon(sizeof(nvidia::nvidia_oem_pdr) - 1, 0);
    auto truncatedStoragePdr = structToBytes(storagePdr);
    truncatedStoragePdr.resize(sizeof(nvidia::nvidia_oem_pdr));

    terminus.oemPdrs.emplace_back(nvidia::NvidiaIana,
                                  static_cast<OemRecordId>(1), tooSmallCommon);
    terminus.oemPdrs.emplace_back(nvidia::NvidiaIana,
                                  static_cast<OemRecordId>(2),
                                  structToBytes(powerCapPdr));
    terminus.oemPdrs.emplace_back(
        nvidia::NvidiaIana, static_cast<OemRecordId>(3), truncatedStoragePdr);
    terminus.oemPdrs.emplace_back(nvidia::NvidiaIana,
                                  static_cast<OemRecordId>(4),
                                  structToBytes(storagePdr));

    ASSERT_TRUE(terminus.parsePDRs());
    EXPECT_EQ(energyPdrCases.size(), terminus.numericSensors.size());

    nvidia::nvidiaInitTerminus(terminus);

    EXPECT_TRUE(powerCapNumericEffecter->oemIntfs.empty());
    EXPECT_TRUE(storageStateEffecter->oemIntfs.empty());
    EXPECT_TRUE(remoteDebugTimeoutEffecter->oemIntfs.empty());
    EXPECT_TRUE(staticPowerPowerEffecter->oemIntfs.empty());

    auto invalidEffecterPdr = makeNumericEffecterValuePdrStruct(0x7FF);
    invalidEffecterPdr->base_unit = 0xFF;
    terminus.addNumericEffecter(invalidEffecterPdr);
    EXPECT_EQ(6u, terminus.numericEffecters.size());
}

TEST_F(TerminusTest, sensorHeaderInlineCoverageFromTerminusTu)
{
    std::string uuid("00000000-0000-0000-0000-0000000000A8");
    Terminus terminus(0xA8, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    auto numericPdr = makeNumericSensorValuePdrStruct(0xA80);
    std::string numericName{"terminus_inline_numeric"};
    std::string numericInventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/terminus_inline"};
    auto numericEventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    numericEventInfo->impactedComponent = "SWITCH0";
    auto numericSensor = std::make_shared<NumericSensor>(
        terminus.getTid(), false, numericPdr, numericName, numericInventoryPath,
        numericEventInfo);

    EXPECT_FALSE(numericSensor->getSensorName().empty());
    EXPECT_FALSE(numericSensor->getSensorNameSpace().empty());
    EXPECT_EQ(numericSensor->getSensorEventInfo(), numericEventInfo);
    auto replacementNumericEventInfo =
        std::make_shared<pldm::utils::SensorEventInfo>();
    replacementNumericEventInfo->impactedComponent = "SWITCH1";
    numericSensor->updateSensorEventInfo(replacementNumericEventInfo);
    EXPECT_EQ(numericSensor->getSensorEventInfo(), replacementNumericEventInfo);
    numericSensor->setRefreshed(false);
    EXPECT_FALSE(numericSensor->isRefreshed());
    numericSensor->setRefreshed(true);
    EXPECT_TRUE(numericSensor->isRefreshed());
    numericSensor->setLastUpdatedTimeStamp(75);
    EXPECT_FALSE(numericSensor->needsUpdate(75));
    EXPECT_TRUE(
        numericSensor->needsUpdate(75 + numericSensor->refreshLimitInUsec + 1));

    std::string stateInventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/terminus_state"};
    auto stateEventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    stateEventInfo->impactedComponent = "DIMM0";
    auto stateSensor = std::make_shared<StateSensor>(
        terminus.getTid(), false, 0xA81, makeSimpleStateSetInfo(), nullptr,
        stateInventoryPath, stateEventInfo);

    EXPECT_TRUE(stateSensor->isDefaultInventoryAssociated());
    stateSensor->setInventoryPaths(
        {stateInventoryPath + "/module0", stateInventoryPath + "/module1"},
        false);
    EXPECT_FALSE(stateSensor->isDefaultInventoryAssociated());
    EXPECT_EQ(stateSensor->getAssociationEntityId(), "module1");
    EXPECT_EQ(stateSensor->getSensorEventInfo(), stateEventInfo);
    auto replacementStateEventInfo =
        std::make_shared<pldm::utils::SensorEventInfo>();
    replacementStateEventInfo->impactedComponent = "DIMM1";
    stateSensor->updateSensorEventInfo(replacementStateEventInfo);
    EXPECT_EQ(stateSensor->getSensorEventInfo(), replacementStateEventInfo);
    stateSensor->setRefreshed(false);
    EXPECT_FALSE(stateSensor->isRefreshed());
    stateSensor->setRefreshed(true);
    EXPECT_TRUE(stateSensor->isRefreshed());
    stateSensor->setLastUpdatedTimeStamp(25);
    EXPECT_FALSE(stateSensor->needsUpdate(25));
    EXPECT_TRUE(
        stateSensor->needsUpdate(25 + stateSensor->refreshLimitInUsec + 1));
}
#endif

// Currently due to async nature of polling this can't be tested.
// TODO: Test this in a different way.

// TEST_F(TerminusTest, TerminusOnOffLineTest)
// {
//     pldm::UUID uuidBad{"f72d6f90-5675-11ed-9b6a-0242ac120003"};
//     pldm::UUID uuid{"f72d6f90-5675-11ed-9b6a-0242ac120002"};
//     pldm::MctpInfos mctpInfos{pldm::MctpInfo(
//         12, uuid, "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1,
//         std::nullopt,
//         "xyz.openbmc_project.MCTP.Endpoint.BindingTypes.PCIe")};

//     /* 1. test discoverMctpTerminus(): check if terminus is discovered
//      * successfully by mock responses */
//     setupResponsesForDiscoverTerminus();
//     terminusManager.discoverMctpTerminus(mctpInfos);
//     EXPECT_EQ(1, termini.size());

//     /* 2. test getTerminus(): check if terminus can be found by uuid */
//     auto terminus = terminusManager.getTerminus(uuidBad);
//     EXPECT_EQ(nullptr, terminus);

//     terminus = terminusManager.getTerminus(uuid);
//     EXPECT_NE(nullptr, terminus);
//     EXPECT_EQ(uuid, terminus->getUuid());

//     /* 3. test initTerminus(): check if sensor is created successfully by
//     mock
//      * response */
//     setupResponsesForInitTerminus();
//     stdexec::sync_wait(platformManager.initTerminus());
//     EXPECT_EQ(1, terminus->numericSensorPdrs.size());

//     /* 4. test updateReading(): check if sensor PDIs are good */
//     auto numericSensor = terminus->numericSensors[0];
//     numericSensor->updateReading(true, true, 10);
//     EXPECT_EQ(true, numericSensor->availabilityIntf->available());
//     EXPECT_EQ(true, numericSensor->operationalStatusIntf->functional());
//     // raw = 10, converted value= 10*1.5 + 1 = 16
//     EXPECT_EQ(16, numericSensor->valueIntf->value());

//     /* 5. test setOffline(): check if sensor PDIs are in offline state*/
//     sensorManager.setOffline(terminus->getTid());
//     EXPECT_EQ(false, numericSensor->operationalStatusIntf->functional());
//     EXPECT_THAT(numericSensor->valueIntf->value(), testing::IsNan());

//     /* 6. test setOnline(): check if sensor PDIs are in online state */
//     setupResponsesForStartPolling();
//     sensorManager.setOnline(terminus->getTid());
//     runEventLoopForMilliseconds(2000);
//     EXPECT_EQ(true, numericSensor->operationalStatusIntf->functional());
//     // raw = 18, converted value= 18*1.5 + 1 = 28
//     EXPECT_EQ(28, numericSensor->valueIntf->value());
// }
