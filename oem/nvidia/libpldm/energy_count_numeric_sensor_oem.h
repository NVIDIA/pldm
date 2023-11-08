#ifndef ENERGY_COUNT_NUMERIC_SENSOR_H
#define ENERGY_COUNT_NUMERIC_SENSOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "libpldm/base.h"
#include "libpldm/pdr.h"
#include "libpldm//platform.h"

// Maximum size for request
#define PLDM_GET_OEM_ENERGYCOUNT_SENSOR_READING_REQ_BYTES 2

// Minimum response length
#define PLDM_GET_OEM_ENERGYCOUNT_SENSOR_READING_MIN_RESP_BYTES 4

// Minimum length of OEM energyCount numeric sensor PDR
#define PLDM_PDR_OEM_ENERGYCOUNT_NUMERIC_SENSOR_PDR_FIXED_LENGTH 19
#define PLDM_PDR_OEM_ENERGYCOUNT_NUMERIC_SENSOR_PDR_VARIED_MIN_LENGTH 2
#define PLDM_PDR_OEM_ENERGYCOUNT_NUMERIC_SENSOR_PDR_MIN_LENGTH                                 \
	(PLDM_PDR_OEM_ENERGYCOUNT_NUMERIC_SENSOR_PDR_FIXED_LENGTH +                            \
	 PLDM_PDR_OEM_ENERGYCOUNT_NUMERIC_SENSOR_PDR_VARIED_MIN_LENGTH)

/** @brief PLDM OEM type supported commands
 */
enum pldm_oem_commands {
	PLDM_OEM_GET_ENERGYCOUNT_SENSOR_READING = 0x11
};

/** @struct pldm_oem_energycount_numeric_sensor_value_pdr
 *
 *  Structure representing PLDM OEM Energy Count Numeric Sensor PDR
 */
struct pldm_oem_energycount_numeric_sensor_value_pdr {
	uint16_t terminus_handle;
	uint8_t nvidia_oem_pdr_type;
	uint16_t sensor_id;
	uint16_t entity_type;
	uint16_t entity_instance_num;
	uint16_t container_id;
	bool8_t sensor_auxiliary_names_pdr;
	uint8_t base_unit;
	int8_t unit_modifier;
	uint8_t sensor_data_size;
	real32_t update_interval;
	union_sensor_data_size max_readable;
	union_sensor_data_size min_readable;
} __attribute__((packed));

/** @struct pldm_get_oem_energycount_sensor_reading_req
 *
 *  Structure representing PLDM get oem energy count sensor reading request
 */
struct pldm_get_oem_energycount_sensor_reading_req {
	uint16_t sensor_id;
} __attribute__((packed));

/** @struct pldm_get_oem_energycount_sensor_reading_resp
 *
 *  Structure representing PLDM get oem energy count sensor reading response
 */
struct pldm_get_oem_energycount_sensor_reading_resp {
	uint8_t completion_code;
	uint8_t sensor_data_size;
	uint8_t sensor_operational_state;
	uint8_t present_reading[1];
} __attribute__((packed));

/* GetOEMEnergyCountSensorReading */

/** @brief Encode GetOEMEnergyCountSensorReading request data
 *
 *  @param[in] instance_id - Message's instance id
 *  @param[in] sensor_id - A handle that is used to identify and access the
 *         sensor
 *  @param[out] msg - Message will be written to this
 *  @return pldm_completion_codes
 *  @note	Caller is responsible for memory alloc and dealloc of param
 * 		'msg.payload'
 */
int encode_get_oem_enegy_count_sensor_reading_req(uint8_t instance_id, uint16_t sensor_id,
				  struct pldm_msg *msg);

/** @brief Decode GetOEMEnergyCountSensorReading response data
 *
 *  @param[in] msg - Request message
 *  @param[in] payload_length - Length of response message payload
 *  @param[out] completion_code - PLDM completion code
 *  @param[out] sensor_data_size - The bit width and format of reading and
 *         threshold values
 *  @param[out] sensor_operational_state - The state of the sensor itself
 *  @param[out] present_reading - The present value indicated by the sensor
 *  @return pldm_completion_codes
 */

int decode_get_oem_energy_count_sensor_reading_resp(
    const struct pldm_msg *msg, size_t payload_length, uint8_t *completion_code,
    uint8_t *sensor_data_size, uint8_t *sensor_operational_state,
    uint8_t *present_reading);

#ifdef __cplusplus
}
#endif

#endif /* ENERGY_COUNT_NUMERIC_SENSOR_H */