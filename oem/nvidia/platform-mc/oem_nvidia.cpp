#include "oem_nvidia.hpp"

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
            (pdr->oem_effecter_lifetime == OEM_LIFETIME_NONVOLATILE) ? true
                                                                     : false;
        persistenceIntf->persistent(persistence);
        effecter->oemIntfs.push_back(std::move(persistenceIntf));
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
        switch (commonPdr->oem_pdr_type)
        {
            case NVIDIA_OEM_PDR_TYPE_EFFECTER_LIFETIME:
                if (data.size() < sizeof(nvidia_oem_effecter_lifetime_pdr))
                {
                    continue;
                }
                processEffecterLifetimePdr(
                    terminus, (nvidia_oem_effecter_lifetime_pdr*)commonPdr);
                break;
            default:
                continue;
        }
    }
}

} // namespace nvidia
} // namespace platform_mc
} // namespace pldm
