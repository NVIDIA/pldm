#pragma once

#include "common/types.hpp"
#include "platform-mc/oem_base.hpp"

#include <sdbusplus/server/object.hpp>
#include <sdeventplus/event.hpp>
#include <xyz/openbmc_project/State/Decorator/Persistence/server.hpp>
#include <xyz/openbmc_project/State/Decorator/SecureState/server.hpp>

using namespace pldm::pdr;

namespace pldm
{
namespace platform_mc
{

class Terminus;

namespace nvidia
{

constexpr VendorIANA NvidiaIana = 0x1647;

enum class NvidiaOemPdrType : uint8_t
{
    NVIDIA_OEM_PDR_TYPE_EFFECTER_POWERCAP = 1,
    NVIDIA_OEM_PDR_TYPE_EFFECTER_STORAGE = 2,
};

struct nvidia_oem_pdr
{
    uint16_t terminus_handle;
    uint8_t oem_pdr_type;
} __attribute__((packed));

enum class OemPowerCapPersistence : uint8_t
{ 
    OEM_POWERCAP_TDP_VOLATILE,
    OEM_POWERCAP_TDP_NONVOLATILE,
    OEM_POWERCAP_EDPP_VOLATILE,
    OEM_POWERCAP_EDPP_NONVOLATILE
};

enum class OemStorageSecureState : uint8_t
{
    OEM_STORAGE_NONSECURE_VARIABLE,
    OEM_STORAGE_SECURE_VARIABLE
};

struct nvidia_oem_effecter_powercap_pdr
{
    uint16_t terminus_handle;
    uint8_t oem_pdr_type;
    uint8_t oem_effecter_powercap;
    uint16_t associated_effecterid;
} __attribute__((packed));

struct nvidia_oem_effecter_storage_pdr
{
    uint16_t terminus_handle;
    uint8_t oem_pdr_type;
    uint8_t oem_effecter_storage;
    uint16_t associated_effecterid;
} __attribute__((packed));

using namespace sdbusplus;
using PersistenceIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::State::Decorator::server::Persistence>;
using SecureStateIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::State::Decorator::server::SecureState>;

class OemPersistenceIntf : public OemIntf, public PersistenceIntf
{
  public:
    OemPersistenceIntf(bus::bus& bus, const char* path) :
        PersistenceIntf(bus, path)
    {}
};

class OemStorageIntf : public OemIntf, public SecureStateIntf
{
  public:
    OemStorageIntf(bus::bus& bus, const char* path) : SecureStateIntf(bus, path)
    {}
};

void nvidiaInitTerminus(Terminus& terminus);

} // namespace nvidia
} // namespace platform_mc
} // namespace pldm
