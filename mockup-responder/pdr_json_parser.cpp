/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION &
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
#include "pdr_json_parser.hpp"

#include "libpldm/pdr.h"
#include "libpldm/platform.h"

#include "pldmd/handler.hpp"
#include "sensor_to_dbus.hpp"

#include <phosphor-logging/lg2.hpp>

std::vector<std::shared_ptr<Sensor>> sensors;
std::vector<std::shared_ptr<Effecter>> effecters;

int PdrJsonParser::currentEffecterId = 0;
const std::vector<Json> PdrJsonParser::emptyList{};
const std::map<std::string, Json> PdrJsonParser::emptyMap{};
const Json PdrJsonParser::empty{};

::pldm_pdr* PdrJsonParser::parse(Json& json, ::pldm_pdr* pdrRepo)
{
    if (pdrRepo == nullptr)
        pdrRepo = (::pldm_pdr*)pldm_pdr_init();

    lg2::info("numericEffecterPDRs");
    auto nEffecterPDRs = json.value("numericEffecterPDRs", emptyList);

    for (const auto& e : nEffecterPDRs)
    {

        auto entries = e.value("entries", emptyList);

        for (Json& f : entries)
        {
            parseNumericEffecter(f, pdrRepo);
        }
    }

    lg2::info("stateEffecterPDRs");
    auto sEffecterPDRs = json.value("stateEffecterPDRs", emptyList);

    for (const auto& e : sEffecterPDRs)
    {

        auto entries = e.value("entries", emptyList);

        for (auto& f : entries)
        {
            parseStateEffecter(f, pdrRepo);
        }
    }
    lg2::info("stateSensorPDRs");
    auto sSensorPDRs = json.value("stateSensorPDRs", emptyList);

    for (const auto& e : sSensorPDRs)
    {
        auto entries = e.value("entries", emptyList);

        for (auto& f : entries)
        {
            parseStateSensor(f, pdrRepo);
        }
    }
    lg2::info("numericSensorPDRs");
    auto nSensorPDRs = json.value("numericSensorPDRs", emptyList);

    for (const auto& e : nSensorPDRs)
    {
        auto entries = e.value("entries", emptyList);

        for (auto& f : entries)
        {
            parseNumericSensor(f, pdrRepo);
        }
    }

    lg2::info("entityAssociationPDRs");
    auto entityAssociationPDRs = json.value("entityAssociationPDRs", emptyList);

    for (const auto& e : entityAssociationPDRs)
    {
        auto entries = e.value("entries", emptyList);

        for (auto& f : entries)
        {
            parseEntityAssociation(f, pdrRepo);
        }
    }

    return pdrRepo;
}

pldm_effecter_init PdrJsonParser::parseEffecterInit(std::string string)
{
    if (string == "noInit")
    {
        return PLDM_NO_INIT;
    }
    else if (string == "useInitPDR")
    {
        return PLDM_USE_INIT_PDR;
    }
    else if (string == "enableEffecter")
    {
        return PLDM_ENABLE_EFFECTER;
    }
    else if (string == "disableEffecter")
    {
        return PLDM_DISABLE_EFFECTER;
    }

    return PLDM_NO_INIT;
}

void PdrJsonParser::parseNumericEffecter(Json& json, ::pldm_pdr* pdrRepo)
{
    json = json.value("set", emptyMap);

    std::vector<uint8_t> pdr(sizeof(pldm_numeric_effecter_value_pdr), 0);

    size_t pdrSize = pdr.size();
    auto rec = reinterpret_cast<pldm_numeric_effecter_value_pdr*>(pdr.data());

    rec->hdr.type = PLDM_NUMERIC_EFFECTER_PDR;
    rec->hdr.record_handle = 0;
    rec->hdr.version = 1;
    rec->hdr.record_change_num = 0;
    rec->hdr.length = pdrSize - sizeof(pldm_pdr_hdr);

    rec->entity_type = json.value("entityType", 0);
    rec->entity_instance = json.value("entityInstanceNumber", 0);
    rec->container_id = json.value("containerID", 0);

    rec->terminus_handle = json.value("terminusHandle", 0);

    uint16_t effecterId = json.value("id", 1);
    rec->effecter_id = effecterId;
    rec->effecter_init =
        parseEffecterInit(json.value("effecterInit", "noInit"));

    rec->effecter_auxiliary_names =
        json.value("effecterAuxiliaryNamesPDR", false);
    rec->base_unit = json.value("baseUnit", 0);
    rec->unit_modifier = json.value("unitModifier", 1);
    std::string rateUnitStr = json.value("rateUnit", "None");
    if (rateUnitStr == "None")
    {
        rec->rate_unit = 0;
    }
    else
    {
        rec->rate_unit = std::stoi(rateUnitStr);
    }
    rec->base_oem_unit_handle = json.value("base_oem_unit_handle", 0);
    rec->aux_unit = json.value("aux_unit", 0);
    rec->aux_unit_modifier = json.value("aux_unit_modifier", 0);
    std::string auxRateUnitStr = json.value("aux_rate_unit", "None");
    if (auxRateUnitStr == "None")
    {
        rec->aux_rate_unit = 0;
    }
    else
    {
        rec->aux_rate_unit = std::stoi(auxRateUnitStr);
    }
    rec->aux_oem_unit_handle = json.value("aux_oem_unit_handle", 0);
    rec->is_linear = json.value("is_linear", false);
    rec->effecter_data_size =
        json.value("effecter_data_size", PLDM_EFFECTER_DATA_SIZE_UINT32);

    rec->resolution = json.value("resolution", 1);
    rec->offset = json.value("offset", 0);
    rec->accuracy = json.value("accuracy", 0);
    rec->plus_tolerance = json.value("plus_tolerance", 0);
    rec->minus_tolerance = json.value("minus_tolerance", 0);
    rec->state_transition_interval = json.value("state_transition_interval", 0);
    rec->transition_interval = json.value("transition_interval", 0);
    rec->range_field_format = PLDM_RANGE_FIELD_FORMAT_UINT32;

    auto e = std::make_shared<Effecter>(effecterId, server);
    effecters.emplace_back(e);

    pldm_pdr_add((::pldm_pdr*)pdrRepo, pdr.data(), pdr.size(), 0, false);
}

void PdrJsonParser::parseStateEffecter(Json& json, ::pldm_pdr* pdrRepo)
{
    json = json["set"];

    uint8_t composite_effecter_count =
        json.value("composite_effecter_count", 0);

    size_t totalPossibleStatesSize = 0;
    for (const auto& e : json.value("possible_states", emptyList))
    {
        totalPossibleStatesSize += e.value("possible_states_size", 0);
    }

    // Updated buffer allocation
    std::vector<uint8_t> pdr(
        sizeof(struct pldm_state_effecter_pdr) - sizeof(uint8_t) +
            composite_effecter_count *
                (sizeof(state_effecter_possible_states) - sizeof(bitfield8_t)) +
            totalPossibleStatesSize,
        0);

    size_t pdrSize = pdr.size();
    auto rec = reinterpret_cast<pldm_state_effecter_pdr*>(pdr.data());

    rec->hdr.type = PLDM_STATE_EFFECTER_PDR;
    rec->hdr.record_handle = 0;
    rec->hdr.version = 1;
    rec->hdr.record_change_num = 0;
    rec->hdr.length = pdrSize - sizeof(pldm_pdr_hdr);

    rec->entity_type = json.value("entityType", 0);
    rec->entity_instance = json.value("entityInstanceNumber", 0);
    rec->container_id = json.value("containerID", 0);

    rec->composite_effecter_count = composite_effecter_count;
    rec->effecter_semantic_id = 0x0;

    uint16_t effecterId = json.value("id", 1);
    rec->effecter_id = effecterId;
    rec->effecter_init =
        parseEffecterInit(json.value("effecter_init", "noInit"));
    rec->has_description_pdr = json.value("has_description_pdr", false);

    auto jsonPossibleStates = json.value("possible_states", emptyList);
    uint8_t* possible_states = rec->possible_states;

    for (size_t i = 0; i < composite_effecter_count; ++i)
    {
        const auto& e = jsonPossibleStates[i];
        auto effecterPossibleStates =
            reinterpret_cast<state_effecter_possible_states*>(possible_states);

        effecterPossibleStates->state_set_id = e.value("state_set_id", 0);
        effecterPossibleStates->possible_states_size =
            e.value("possible_states_size", 0);

        memset(effecterPossibleStates->states, 0,
               effecterPossibleStates->possible_states_size);

        for (const auto& stateVal : e.value("state_values", std::vector<int>{}))
        {
            uint8_t bytePosition = stateVal / CHAR_BIT;
            uint8_t bitPosition = stateVal % CHAR_BIT;
            if (bytePosition < effecterPossibleStates->possible_states_size)
            {
                effecterPossibleStates->states[bytePosition].byte |=
                    (1 << bitPosition);
            }
            else
            {
                lg2::error(
                    "State value {STATEVAL} exceeds possible_states_size limit.",
                    "STATEVAL", stateVal);
                throw std::runtime_error(
                    "Invalid state value exceeding allocated size");
            }
        }

        possible_states += sizeof(state_effecter_possible_states) +
                           effecterPossibleStates->possible_states_size - 1;
    }

    auto e = std::make_shared<Effecter>(effecterId, server);
    e->composite_count = composite_effecter_count;
    effecters.emplace_back(e);

    pldm_pdr_add((::pldm_pdr*)pdrRepo, pdr.data(), pdr.size(), 0, false);
}

void PdrJsonParser::parseStateSensor(Json& json, ::pldm_pdr* pdrRepo)
{
    json = json["set"];

    uint8_t composite_sensor_count = json.value("composite_sensor_count", 0);

    // Calculate total size for pdr dynamically
    size_t totalPossibleStatesSize = 0;
    for (const auto& e : json.value("possible_states", emptyList))
    {
        totalPossibleStatesSize += e.value("possible_states_size", 0);
    }

    std::vector<uint8_t> pdr(
        sizeof(pldm_state_sensor_pdr) - sizeof(uint8_t) +
            composite_sensor_count *
                (sizeof(state_sensor_possible_states) - sizeof(bitfield8_t)) +
            totalPossibleStatesSize,
        0);

    size_t pdrSize = pdr.size();
    auto rec = reinterpret_cast<pldm_state_sensor_pdr*>(pdr.data());

    rec->hdr.type = PLDM_STATE_SENSOR_PDR;
    rec->hdr.record_handle = 0;
    rec->hdr.version = 1;
    rec->hdr.record_change_num = 0;
    rec->hdr.length = pdrSize - sizeof(pldm_pdr_hdr);

    rec->entity_type = json.value("entityType", 0);
    rec->entity_instance = json.value("entityInstanceNumber", 0);
    rec->container_id = json.value("containerID", 0);

    rec->composite_sensor_count = composite_sensor_count;
    uint16_t sensorId = json.value("id", 1);
    rec->sensor_id = sensorId;

    auto jsonPossibleStates = json.value("possible_states", emptyList);
    uint8_t* possible_states = rec->possible_states;

    for (size_t i = 0; i < composite_sensor_count; ++i)
    {
        const auto& e = jsonPossibleStates[i];
        auto sensorPossibleStates =
            reinterpret_cast<state_sensor_possible_states*>(possible_states);

        sensorPossibleStates->state_set_id = e.value("state_set_id", 0);
        sensorPossibleStates->possible_states_size =
            e.value("possible_states_size", 0);

        // Validate that possible_states_size is within bounds
        if (sensorPossibleStates->possible_states_size >
            totalPossibleStatesSize)
        {
            throw std::runtime_error(
                "Invalid possible_states_size exceeds allocated bounds");
        }

        memset(sensorPossibleStates->states, 0,
               sensorPossibleStates->possible_states_size);

        for (const auto& stateVal : e.value("state_values", std::vector<int>{}))
        {
            uint8_t bytePosition = stateVal / CHAR_BIT;
            uint8_t bitPosition = stateVal % CHAR_BIT;

            if (bytePosition < sensorPossibleStates->possible_states_size)
            {
                sensorPossibleStates->states[bytePosition].byte |=
                    (1 << bitPosition);
            }
            else
            {
                lg2::error(
                    "State value {STATEVAL} exceeds possible_states_size limit.",
                    "STATEVAL", stateVal);
            }
        }

        // Move pointer to next possible_states block
        possible_states += sizeof(state_sensor_possible_states) +
                           sensorPossibleStates->possible_states_size - 1;
    }

    auto s = std::make_shared<Sensor>(sensorId, server);
    s->composite_count = composite_sensor_count;
    sensors.emplace_back(s);

    pldm_pdr_add((::pldm_pdr*)pdrRepo, pdr.data(), pdr.size(), 0, false);
}

void PdrJsonParser::parseNumericSensor(Json& json, ::pldm_pdr* pdrRepo)
{
    json = json["set"];

    std::vector<uint8_t> pdr(sizeof(pldm_numeric_sensor_value_pdr), 0);

    size_t pdrSize = pdr.size();
    auto rec = reinterpret_cast<pldm_numeric_sensor_value_pdr*>(pdr.data());

    rec->hdr.type = PLDM_NUMERIC_SENSOR_PDR;
    rec->hdr.record_handle = 0;
    rec->hdr.version = 1;
    rec->hdr.record_change_num = 0;
    rec->hdr.length = pdrSize - sizeof(pldm_pdr_hdr);

    rec->entity_type = json.value("entityType", 0);
    rec->entity_instance_num = json.value("entityInstanceNumber", 0);
    rec->container_id = json.value("containerID", 0);

    rec->terminus_handle = json.value("terminusHandle", 0);

    uint16_t sensorId = json.value("id", 1);
    rec->sensor_id = sensorId;

    rec->sensor_init = parseEffecterInit(json.value("sensorInit", "noInit"));
    rec->sensor_auxiliary_names_pdr =
        json.value("sensorAuxiliaryNamesPDR", false);
    rec->base_unit = json.value("baseUnit", 0);
    rec->unit_modifier = json.value("unitModifier", 1);
    std::string rateUnitStr = json.value("rateUnit", "None");
    if (rateUnitStr == "None")
    {
        rec->rate_unit = 0;
    }
    else
    {
        rec->rate_unit = std::stoi(rateUnitStr);
    }
    rec->base_oem_unit_handle = json.value("base_oem_unit_handle", 0);
    rec->aux_unit = json.value("aux_unit", 0);
    rec->aux_unit_modifier = json.value("aux_unit_modifier", 0);
    std::string auxRateUnitStr = json.value("aux_rate_unit", "None");
    if (auxRateUnitStr == "None")
    {
        rec->aux_rate_unit = 0;
    }
    else
    {
        rec->aux_rate_unit = std::stoi(auxRateUnitStr);
    }
    rec->aux_oem_unit_handle = json.value("aux_oem_unit_handle", 0);
    rec->is_linear = json.value("is_linear", 0);
    rec->sensor_data_size = PLDM_SENSOR_DATA_SIZE_UINT32;
    rec->resolution = json.value("resolution", 1);
    rec->offset = json.value("offset", 0);
    rec->accuracy = json.value("accuracy", 0);
    rec->plus_tolerance = json.value("plus_tolerance", 0);
    rec->minus_tolerance = json.value("minus_tolerance", 0);
    rec->state_transition_interval = json.value("state_transition_interval", 0);
    rec->range_field_format = PLDM_RANGE_FIELD_FORMAT_UINT32;
    rec->range_field_support.byte = 0;

    auto s = std::make_shared<Sensor>(sensorId, server);
    sensors.emplace_back(s);

    pldm_pdr_add((::pldm_pdr*)pdrRepo, pdr.data(), pdr.size(), 0, false);
}

void PdrJsonParser::parseEntityAssociation(Json& json, ::pldm_pdr* pdrRepo)
{
    json = json["set"];

    auto containedEntityCount = json.value("containedEntityCount", 0);

    assert(containedEntityCount <= 255);
    uint8_t count = containedEntityCount;

    std::vector<uint8_t> pdr(sizeof(pldm_pdr_hdr) +
                             sizeof(pldm_pdr_entity_association) +
                             sizeof(pldm_entity) * containedEntityCount);

    size_t pdrSize = pdr.size();
    auto hdr = reinterpret_cast<pldm_pdr_hdr*>(pdr.data());
    auto rec = reinterpret_cast<pldm_pdr_entity_association*>(
        pdr.data() + sizeof(pldm_pdr_hdr));

    hdr->type = PLDM_PDR_ENTITY_ASSOCIATION;
    hdr->record_handle = 0;
    hdr->version = 1;
    hdr->record_change_num = 0;
    hdr->length = pdrSize - sizeof(pldm_pdr_hdr);

    rec->container_id = json.value("containerId", 0);
    rec->association_type = json.value("associationType", 0);
    rec->container.entity_type = json.value("containerEntityType", 0);
    rec->container.entity_instance_num =
        json.value("containerEntityInstanceNumber", 0);
    rec->container.entity_container_id =
        json.value("containerEntityContainerId", 0);
    rec->num_children = count;

    auto containedEntityIdentificationInfos =
        json.value("containedEntityIdentificationInfo", emptyList);
    assert(containedEntityIdentificationInfos.size() == count);

    auto entities = reinterpret_cast<pldm_entity*>(rec->children);
    for (const auto& e : containedEntityIdentificationInfos)
    {
        entities->entity_type = e.value("containedEntityType", 0);
        entities->entity_instance_num =
            e.value("containedEntityInstanceNumber", 0);
        entities->entity_container_id =
            e.value("containedEntityContainerId", 0);
        entities++;
    }

    pldm_pdr_add((::pldm_pdr*)pdrRepo, pdr.data(), pdr.size(), 0, false);
}

void PdrJsonParser::parseEntry(::pldm_pdr* pdrRepo, Json json)
{
    size_t pdrSize = 0;
    auto effecters = json.value("effecters", emptyList);
    for (const auto& effecter : effecters)
    {
        auto set = effecter.value("set", empty);
        auto statesSize = set.value("size", 0);
        if (!statesSize)
        {
            lg2::error("Malformed PDR JSON return "
                       "pdrEntry;- no state set "
                       "info");
            throw InternalFailure();
        }
        pdrSize += sizeof(state_effecter_possible_states) -
                   sizeof(bitfield8_t) + (sizeof(bitfield8_t) * statesSize);
    }

    pdrSize += sizeof(pldm_state_effecter_pdr) - sizeof(uint8_t);

    std::vector<uint8_t> entry{};
    entry.resize(pdrSize);

    std::vector<uint8_t> pdr(sizeof(struct pldm_state_effecter_pdr) -
                             sizeof(uint8_t) +
                             sizeof(struct state_effecter_possible_states));

    pldm_state_effecter_pdr* rec =
        reinterpret_cast<pldm_state_effecter_pdr*>(entry.data());

    if (!rec)
    {
        lg2::error("Failed to get state effecter PDR.");
        return;
    }

    rec->hdr.type = PLDM_STATE_EFFECTER_PDR;
    rec->hdr.record_handle = 0;
    rec->hdr.version = 1;
    rec->hdr.record_change_num = 0;
    rec->hdr.length = pdrSize - sizeof(pldm_pdr_hdr);
    rec->terminus_handle = 1;
    rec->effecter_id = ++currentEffecterId;

    pldm_pdr_add((::pldm_pdr*)pdrRepo, entry.data(), entry.size(), 0, false);
}