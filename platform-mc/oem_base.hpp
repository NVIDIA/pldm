#pragma once

namespace pldm
{
namespace platform_mc
{

/**
 * @brief OemIntf
 *
 * This class is a base class of OEM intefaces
 */
class OemIntf
{
  public:
    OemIntf()
    {}
    virtual ~OemIntf() = default;
};

} // namespace platform_mc
} // namespace pldm
