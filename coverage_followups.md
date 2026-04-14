# Coverage Follow-ups

## Production bugs found while adding coverage

### `libpldmresponder/platform.cpp`

- `Handler::getStateSensorReadings()` validates `payloadLength` against
  `PLDM_GET_SENSOR_READING_REQ_BYTES` (3), but then calls
  `decode_get_state_sensor_readings_req()`, which expects
  `PLDM_GET_STATE_SENSOR_READINGS_REQ_BYTES` (4).
- Result: valid `GetStateSensorReadings` requests are rejected with
  `PLDM_ERROR_INVALID_LENGTH`, and the OEM state-sensor wrapper branch is
  unreachable without a production fix.
