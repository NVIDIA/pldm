#pragma once

#include "common/utils.hpp"
#include "libpldmresponder/oem_handler.hpp"

#include <libpldm/pdr.h>

#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>

#include <filesystem>

PHOSPHOR_LOG2_USING;

namespace pldm
{
namespace hostbmc
{
namespace utils
{

void updateEntityAssociation(
    const pldm::utils::EntityAssociations& entityAssoc,
    pldm_entity_association_tree* entityTree,
    pldm::utils::ObjectPathMaps& objPathMap, pldm::utils::EntityMaps entityMaps,
    pldm::responder::oem_platform::Handler* oemPlatformHandler);

pldm::utils::EntityMaps parseEntityMap(const fs::path& filePath);

} // namespace utils
} // namespace hostbmc
} // namespace pldm
