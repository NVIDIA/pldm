#include "oem_nvidia.hpp"

#include "platform-mc/terminus.hpp"

using namespace pldm::pdr;

namespace pldm
{
namespace platform_mc
{

namespace nvidia
{

static void processEffecterLifetimePdr(Terminus& terminus,
                                       nvidia_oem_effecter_lifetime_pdr* pdr)
{
    for (auto& effecter : terminus.numericEffecters)
    {
        if (effecter->effecterId != pdr->associated_effecterid)
        {
            continue;
        }

        auto persistenceIntf = std::make_unique<OemPersistenceIntf>(
            utils::DBusHandler().getBus(), effecter->path.c_str());
        bool persistence =
            (pdr->oem_effecter_lifetime ==
             static_cast<uint8_t>(
                 OemLifetimePersistence::OEM_LIFETIME_NONVOLATILE))
                ? true
                : false;
        persistenceIntf->persistent(persistence);
        effecter->oemIntfs.push_back(std::move(persistenceIntf));
    }
}

static void processEffecterStoragePdr(Terminus& terminus,
                                      nvidia_oem_effecter_storage_pdr* pdr)
{
    for (auto& effecter : terminus.stateEffecters)
    {
        if (effecter->effecterId != pdr->associated_effecterid)
        {
            continue;
        }

        auto secureStateIntf = std::make_unique<OemStorageIntf>(
            utils::DBusHandler().getBus(), effecter->path.c_str());
        bool secureState =
            (pdr->oem_effecter_storage ==
             static_cast<uint8_t>(
                 OemStorageSecureState::OEM_STORAGE_SECURE_VARIABLE))
                ? true
                : false;
        secureStateIntf->secure(secureState);
        effecter->oemIntfs.push_back(std::move(secureStateIntf));
    }
}

void nvidiaInitTerminus(Terminus& terminus)
{
    for (const auto& pdr : terminus.oemPdrs)
    {
        const auto& [iana, recordId, data] = pdr;

        if (iana != NvidiaIana)
        {
            continue;
        }

        if (data.size() < sizeof(nvidia_oem_pdr))
        {
            continue;
        }

        nvidia_oem_pdr* commonPdr = (nvidia_oem_pdr*)data.data();
        NvidiaOemPdrType type =
            static_cast<NvidiaOemPdrType>(commonPdr->oem_pdr_type);

        switch (type)
        {
            case NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_EFFECTER_LIFETIME:
                if (data.size() < sizeof(nvidia_oem_effecter_lifetime_pdr))
                {
                    continue;
                }
                processEffecterLifetimePdr(
                    terminus, (nvidia_oem_effecter_lifetime_pdr*)commonPdr);
                break;
            case NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_EFFECTER_STORAGE:
                if (data.size() < sizeof(nvidia_oem_effecter_storage_pdr))
                {
                    continue;
                }
                processEffecterStoragePdr(
                    terminus, (nvidia_oem_effecter_storage_pdr*)commonPdr);
                break;
            default:
                continue;
        }
    }
}

} // namespace nvidia
} // namespace platform_mc
} // namespace pldm
