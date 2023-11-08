#include <endian.h>
#include <string.h>

#include "energy_count_numeric_sensor_oem.h"

int encode_get_oem_enegy_count_sensor_reading_req(uint8_t instance_id, uint16_t sensor_id,
				  struct pldm_msg *msg)
{
	if (msg == NULL) {
		return PLDM_ERROR_INVALID_DATA;
	}

	struct pldm_header_info header = {0};
	header.msg_type = PLDM_REQUEST;
	header.instance = instance_id;
	header.pldm_type = PLDM_OEM;
	header.command = PLDM_OEM_GET_ENERGYCOUNT_SENSOR_READING;

	uint8_t rc = pack_pldm_header(&header, &(msg->hdr));
	if (rc != PLDM_SUCCESS) {
		return rc;
	}

	struct pldm_get_oem_energycount_sensor_reading_req *request =
	    (struct pldm_get_oem_energycount_sensor_reading_req *)msg->payload;

	request->sensor_id = htole16(sensor_id);

	return PLDM_SUCCESS;
}

int decode_get_oem_energy_count_sensor_reading_resp(
    const struct pldm_msg *msg, size_t payload_length, uint8_t *completion_code,
    uint8_t *sensor_data_size, uint8_t *sensor_operational_state,
    uint8_t *present_reading)
{
	if (msg == NULL || completion_code == NULL ||
	    sensor_data_size == NULL || sensor_operational_state == NULL ||
	    present_reading == NULL) {
		return PLDM_ERROR_INVALID_DATA;
	}

	*completion_code = msg->payload[0];
	if (PLDM_SUCCESS != *completion_code) {
		return PLDM_SUCCESS;
	}

	if (payload_length < PLDM_GET_OEM_ENERGYCOUNT_SENSOR_READING_MIN_RESP_BYTES) {
		return PLDM_ERROR_INVALID_LENGTH;
	}

	struct pldm_get_oem_energycount_sensor_reading_resp *response =
	    (struct pldm_get_oem_energycount_sensor_reading_resp *)msg->payload;

	if (response->sensor_data_size > PLDM_SENSOR_DATA_SIZE_SINT64) {
		return PLDM_ERROR_INVALID_DATA;
	}
	if (response->sensor_data_size > *sensor_data_size) {
		return PLDM_ERROR_INVALID_LENGTH;
	}

	*sensor_data_size = response->sensor_data_size;
	*sensor_operational_state = response->sensor_operational_state;

	if (*sensor_data_size == PLDM_EFFECTER_DATA_SIZE_UINT8 ||
	    *sensor_data_size == PLDM_EFFECTER_DATA_SIZE_SINT8) {
		if (payload_length != PLDM_GET_OEM_ENERGYCOUNT_SENSOR_READING_MIN_RESP_BYTES) {
			return PLDM_ERROR_INVALID_LENGTH;
		}
		*present_reading = response->present_reading[0];

	} else if (*sensor_data_size == PLDM_EFFECTER_DATA_SIZE_UINT16 ||
		   *sensor_data_size == PLDM_EFFECTER_DATA_SIZE_SINT16) {
		if (payload_length !=
		    PLDM_GET_OEM_ENERGYCOUNT_SENSOR_READING_MIN_RESP_BYTES + 1) {
			return PLDM_ERROR_INVALID_LENGTH;
		}
		memcpy(present_reading, response->present_reading, 2);
		uint16_t *val = (uint16_t *)(present_reading);
		*val = le16toh(*val);

	} else if (*sensor_data_size == PLDM_EFFECTER_DATA_SIZE_UINT32 ||
		   *sensor_data_size == PLDM_EFFECTER_DATA_SIZE_SINT32) {
		if (payload_length !=
		    PLDM_GET_OEM_ENERGYCOUNT_SENSOR_READING_MIN_RESP_BYTES + 3) {
			return PLDM_ERROR_INVALID_LENGTH;
		}
		memcpy(present_reading, response->present_reading, 4);
		uint32_t *val = (uint32_t *)(present_reading);
		*val = le32toh(*val);

	} else if (*sensor_data_size == PLDM_EFFECTER_DATA_SIZE_UINT64 ||
		   *sensor_data_size == PLDM_EFFECTER_DATA_SIZE_SINT64) {
		if (payload_length !=
		    PLDM_GET_OEM_ENERGYCOUNT_SENSOR_READING_MIN_RESP_BYTES + 7) {
			return PLDM_ERROR_INVALID_LENGTH;
		}
		memcpy(present_reading, response->present_reading, 8);
		uint64_t *val = (uint64_t *)(present_reading);
		*val = le64toh(*val);
	}

	return PLDM_SUCCESS;
}