#pragma once

#include "platform-mc/oem_base.hpp"
#include "platform-mc/terminus.hpp"

#include <sdbusplus/server/object.hpp>
#include <sdeventplus/event.hpp>
#include <xyz/openbmc_project/State/Decorator/Persistence/server.hpp>

using namespace pldm::pdr;

namespace pldm
{
namespace platform_mc
{

class Terminus;

namespace nvidia
{

constexpr VendorIANA NvidiaIana = 0x1647;

enum
{
    NVIDIA_OEM_PDR_TYPE_EFFECTER_LIFETIME = 1,
};
struct nvidia_oem_pdr
{
    uint16_t terminus_handle;
    uint8_t oem_pdr_type;
} __attribute__((packed));

enum
{
    OEM_LIFETIME_VOLATILE,
    OEM_LIFETIME_NONVOLATILE
};
struct nvidia_oem_effecter_lifetime_pdr
{
    uint16_t terminus_handle;
    uint8_t oem_pdr_type;
    uint8_t oem_effecter_lifetime;
    uint16_t associated_effecterid;
} __attribute__((packed));

using namespace sdbusplus;
using PersistenceIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::State::Decorator::server::Persistence>;
class OemPersistenceIntf : public OemIntf, public PersistenceIntf
{
  public:
    OemPersistenceIntf(bus::bus& bus, const char* path) :
        PersistenceIntf(bus, path)
    {}
};

void nvidiaInitTerminus(Terminus& terminus);

} // namespace nvidia
} // namespace platform_mc
} // namespace pldm
