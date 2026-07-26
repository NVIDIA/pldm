#include "manager.hpp"

namespace pldm
{
namespace platform_mc
{

exec::task<int> Manager::oemPollForPlatformEvent(pldm_tid_t /*tid*/)
{
    co_return PLDM_SUCCESS;
}

} // namespace platform_mc
} // namespace pldm
