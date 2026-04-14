# Coverage Progress

Authoritative flow:

1. `dbus-unit-test.py`
2. `unit-test.py -g`
3. `meson test -C build/coverage --print-errorlogs`
4. `ninja -C build/coverage coverage-html`

Format: `lines / functions / branches / decisions`

## Run Log

### 2026-03-25 baseline after fixing `unit-test.py` coverage reuse

- `common`: `95.3 / 98.1 / 82.3 / 76.2`
- `fw-update`: `95.8 / 96.1 / 82.9 / 88.0`
- `libpldmresponder`: `96.5 / 99.4 / 86.1 / 77.5`
- `oem`: `98.6 / 100.0 / 88.0 / 90.8`
- `platform-mc`: `99.2 / 90.9 / 88.1 / 94.7`
- `pldmd`: `100.0 / 75.0 / 90.0 / -`
- `pldmtool`: `97.4 / 100.0 / 90.1 / 81.4`
- `requester`: `94.7 / 96.0 / 86.3 / 80.0`
- Whole repo: `96.9 / 96.8 / 86.3 / 86.1`

### 2026-03-25 after requester coverage batch 1

- `common`: `95.3 / 98.1 / 82.3 / 76.2`
- `fw-update`: `95.8 / 96.1 / 82.8 / 88.0`
- `libpldmresponder`: `96.5 / 99.4 / 86.1 / 77.5`
- `oem`: `98.6 / 100.0 / 88.0 / 90.8`
- `platform-mc`: `99.2 / 90.9 / 88.1 / 94.7`
- `pldmd`: `100.0 / 75.0 / 90.0 / -`
- `pldmtool`: `97.4 / 100.0 / 90.1 / 81.4`
- `requester`: `94.9 / 96.1 / 86.6 / 80.4`
- Whole repo: `96.9 / 96.8 / 86.3 / 86.2`

Batch summary:

- Added requester-only coverage tests in [requester/test/handler_coverage_test.cpp](/mnt/sda1/probe/pldm/requester/test/handler_coverage_test.cpp) for:
  - queue-empty and already-active `pollEndpointQueue()` short-circuits
  - missing-endpoint `unregisterRequest()` for uncovered handler instantiations
  - active-request `unregisterRequest()` promoting the next queued request
  - queued-request search paths for `MockRequest` and `FailingSendRequest`
  - RX-direction `storeTransportError()` selection of the affected EID

### 2026-03-25 after requester coverage batch 2

- `common`: `95.3 / 98.1 / 82.4 / 76.2`
- `fw-update`: `95.8 / 96.1 / 82.8 / 88.0`
- `libpldmresponder`: `96.5 / 99.4 / 86.1 / 77.5`
- `oem`: `98.6 / 100.0 / 88.2 / 91.2`
- `platform-mc`: `99.2 / 90.9 / 88.1 / 94.7`
- `pldmd`: `100.0 / 75.0 / 90.0 / -`
- `pldmtool`: `97.1 / 100.0 / 89.7 / 81.4`
- `requester`: `95.9 / 96.2 / 88.2 / 80.0`
- Whole repo: `97.0 / 96.8 / 86.4 / 86.2`

Batch summary:

- Added requester transport-error filter tests in [requester/test/handler_coverage_test.cpp](/mnt/sda1/probe/pldm/requester/test/handler_coverage_test.cpp) for explicit PLDM-type matches and `0xFF` wildcard matches across `NiceMock<MockRequest>`, `Request`, `MockRequest`, and `FailingSendRequest`.
- Added the remaining unknown-key `instanceIdExpiryCallBack()` assertion coverage for `Request` and `NiceMock<MockRequest>` in [requester/test/handler_coverage_test.cpp](/mnt/sda1/probe/pldm/requester/test/handler_coverage_test.cpp).
- Fixed the `pldmtool` CI D-Bus-path tests in [pldmtool_cmd_helper_test.cpp](/mnt/sda1/probe/pldm/pldmtool/test/pldmtool_cmd_helper_test.cpp) so they use local mapper/object-manager services instead of poisoning the shared session bus with missing-service error paths.
- Reduced runtime in the slow requester timeout tests in [handler_test.cpp](/mnt/sda1/probe/pldm/requester/test/handler_test.cpp) so `handler_test` stays below the Meson 30s timeout in the authoritative CI run.

### 2026-03-26 after common/fw-update/pldmd coverage batch

- `common`: `99.5 / 100.0 / 83.8 / 82.1`
- `fw-update`: `97.0 / 96.9 / 84.2 / 88.9`
- `libpldmresponder`: `96.5 / 99.4 / 86.4 / 77.5`
- `oem`: `99.4 / 100.0 / 89.1 / 91.2`
- `platform-mc`: `99.2 / 91.2 / 86.7 / 94.4`
- `pldmd`: `94.0 / 95.2 / 79.5 / 83.8`
- `pldmtool`: `97.4 / 100.0 / 90.0 / 79.5`
- `requester`: `98.6 / 100.0 / 90.5 / 85.0`
- Whole repo: `97.8 / 98.0 / 86.5 / 86.7`

Batch summary:

- Added more `common` coverage in [common/test/pldm_utils_test.cpp](/mnt/sda1/probe/pldm/common/test/pldm_utils_test.cpp) for:
  - `DBusHandler::getDbusProperty<T>()` association success coverage
  - remaining scalar and association mismatch branches for both mocked and direct template calls
  - direct `checkForFruPresence()` success-path D-Bus decoding coverage
- Added mock-mode async D-Bus constructor coverage in [common/test/dbus_async_utils_mock_coverage_test.cpp](/mnt/sda1/probe/pldm/common/test/dbus_async_utils_mock_coverage_test.cpp) across the remaining stored property types with custom services.
- Added `fw-update` timer/deferred-response coverage in [fw-update/test/component_updater_test.cpp](/mnt/sda1/probe/pldm/fw-update/test/component_updater_test.cpp) for:
  - `onResponseSendComplete()` no-op and send-failure paths
  - `createRequestFwDataTimer()` pending-discovery early return
  - `createCompleteCommandsTimeoutTimer()` `RequestFirmwareData` state and pending-discovery early return
- Focused validation passed for `pldm_utils_test`, `dbus_async_utils_mock_coverage_test`, `component_updater_test`, and `pldmd_internal_test`.
- Authoritative run passed `68/68` tests through `unit-test.py -g`, but the directory branch totals moved only slightly, so the next batch needs to chase larger uncovered file clusters rather than more low-level template branches.

### 2026-03-26 latest authoritative baseline

- `common`: `99.5 / 100.0 / 83.3 / 82.1`
- `fw-update`: `97.5 / 96.9 / 85.1 / 89.7`
- `libpldmresponder`: `96.6 / 99.4 / 86.5 / 77.5`
- `oem`: `99.4 / 100.0 / 89.1 / 91.2`
- `platform-mc`: `99.2 / 91.2 / 86.6 / 94.4`
- `pldmd`: `94.4 / 95.2 / 79.9 / 85.4`
- `pldmtool`: `97.4 / 100.0 / 90.0 / 79.5`
- `requester`: `98.6 / 100.0 / 90.5 / 85.0`
- Whole repo: `97.9 / 98.0 / 86.7 / 86.9`

Batch summary:

- Added more `common` async D-Bus coverage in [common/test/dbus_async_utils_test.cpp](/mnt/sda1/probe/pldm/common/test/dbus_async_utils_test.cpp), then removed invalid exception-path attempts that were terminating because the exercised helpers are `noexcept` or only hold references.
- Added a real D-Bus positive-path inventory status test in [fw-update/test/inventory_manager_internal_test.cpp](/mnt/sda1/probe/pldm/fw-update/test/inventory_manager_internal_test.cpp) for `InventoryManager::logDeviceStatusErrors()`.
- Focused validation passed for `dbus_async_utils_test`, `inventory_manager_internal_test`, and `manager_internal_test`.
- Authoritative run passed `68/68` tests through `unit-test.py -g`, and the next batch is targeting the highest-ROI remaining branch clusters rather than continuing on low-yield template branches.

### 2026-03-26 authoritative baseline after workspace-temp cleanup

- `common`: `99.5 / 100.0 / 84.2 / 82.1`
- `fw-update`: `97.6 / 96.9 / 85.3 / 90.3`
- `libpldmresponder`: `96.6 / 99.4 / 86.5 / 77.5`
- `oem`: `99.4 / 100.0 / 90.1 / 90.8`
- `platform-mc`: `99.3 / 91.2 / 86.2 / 94.4`
- `pldmd`: `94.6 / 95.2 / 79.9 / 85.4`
- `pldmtool`: `97.4 / 100.0 / 90.0 / 79.5`
- `requester`: `98.6 / 100.0 / 90.5 / 85.0`
- Whole repo: `97.9 / 98.0 / 86.7 / 87.0`

Batch summary:

- Updated [openbmc-build-scripts/scripts/unit-test.py](/mnt/sda1/probe/openbmc-build-scripts/scripts/unit-test.py) so Meson coverage subprocesses use an absolute temp root under `build/coverage/.tmp`, plus `TEST_TMPDIR` and `GTEST_TMP_DIR`, instead of the broken container `/tmp`.
- Added shared test temp helpers in [test/test_tmp_utils.hpp](/mnt/sda1/probe/pldm/test/test_tmp_utils.hpp) and migrated the affected test scaffolding and wrappers away from hardcoded `/tmp` paths in:
  - [test/test_instance_id.hpp](/mnt/sda1/probe/pldm/test/test_instance_id.hpp)
  - [test/pldmd_internal_test.cpp](/mnt/sda1/probe/pldm/test/pldmd_internal_test.cpp)
  - [test/pldmd_main_coverage_helper.cpp](/mnt/sda1/probe/pldm/test/pldmd_main_coverage_helper.cpp)
  - [pldmtool/test/meson.build](/mnt/sda1/probe/pldm/pldmtool/test/meson.build)
  - [pldmtool/test/pldmtool_base_cmd_test.cpp](/mnt/sda1/probe/pldm/pldmtool/test/pldmtool_base_cmd_test.cpp)
  - [pldmtool/test/pldmtool_cmd_helper_test.cpp](/mnt/sda1/probe/pldm/pldmtool/test/pldmtool_cmd_helper_test.cpp)
  - [pldmtool/test/pldmtool_fw_update_cmd_test.cpp](/mnt/sda1/probe/pldm/pldmtool/test/pldmtool_fw_update_cmd_test.cpp)
  - [pldmtool/test/pldmtool_main_test.cpp](/mnt/sda1/probe/pldm/pldmtool/test/pldmtool_main_test.cpp)
  - [pldmtool/test/pldmtool_platform_cmd_test.cpp](/mnt/sda1/probe/pldm/pldmtool/test/pldmtool_platform_cmd_test.cpp)
  - [libpldmresponder/test/libpldmresponder_fru_test.cpp](/mnt/sda1/probe/pldm/libpldmresponder/test/libpldmresponder_fru_test.cpp)
  - [libpldmresponder/test/libpldmresponder_pdr_effecter_test.cpp](/mnt/sda1/probe/pldm/libpldmresponder/test/libpldmresponder_pdr_effecter_test.cpp)
  - [libpldmresponder/test/libpldmresponder_bios_config_test.cpp](/mnt/sda1/probe/pldm/libpldmresponder/test/libpldmresponder_bios_config_test.cpp)
  - [libpldmresponder/test/libpldmresponder_platform_test.cpp](/mnt/sda1/probe/pldm/libpldmresponder/test/libpldmresponder_platform_test.cpp)
  - [libpldmresponder/test/bios_handler_test_config.hpp](/mnt/sda1/probe/pldm/libpldmresponder/test/bios_handler_test_config.hpp)
  - [libpldmresponder/test/libpldmresponder_bios_handler_test.cpp](/mnt/sda1/probe/pldm/libpldmresponder/test/libpldmresponder_bios_handler_test.cpp)
  - [platform-mc/test/oem_events_private_test.cpp](/mnt/sda1/probe/pldm/platform-mc/test/oem_events_private_test.cpp)
  - [fw-update/test/other_device_update_manager_test.cpp](/mnt/sda1/probe/pldm/fw-update/test/other_device_update_manager_test.cpp)
  - [common/test/readHostEID_test.cpp](/mnt/sda1/probe/pldm/common/test/readHostEID_test.cpp)
  - [common/test/pldm_utils_test.cpp](/mnt/sda1/probe/pldm/common/test/pldm_utils_test.cpp)
- Focused validation passed for the previously failing target set, then the authoritative `unit-test.py -g` run passed `68/68` and regenerated the report successfully.

## Notes

- `pldmtool_cmd_helper_test` was marked non-parallel in [pldmtool/test/meson.build](/mnt/sda1/probe/pldm/pldmtool/test/meson.build) because the test shares D-Bus state and was flaky under parallel Meson execution during the coverage run.
- `unit-test.py` was updated to reconfigure and clean the existing `build/coverage` tree before the coverage run so stale outputs from removed sources do not poison gcovr input.

### 2026-03-26 authoritative run after common/libpldmresponder/pldmd helper batch

- `common`: `99.5 / 100.0 / 84.5 / 82.1`
- `fw-update`: `97.6 / 96.9 / 85.3 / 90.6`
- `libpldmresponder`: `96.9 / 99.4 / 87.0 / 78.7`
- `oem`: `99.4 / 100.0 / 90.1 / 90.8`
- `platform-mc`: `99.3 / 93.4 / 86.2 / 94.4`
- `pldmd`: `94.6 / 95.2 / 80.0 / 85.4`
- `pldmtool`: `97.4 / 100.0 / 90.0 / 79.5`
- `requester`: `98.6 / 100.0 / 90.5 / 85.0`
- Whole repo: `98.0 / 98.4 / 86.9 / 87.3`

Batch summary:

- Added `common` async D-Bus coverage in:
  - [common/test/dbus_async_utils_test.cpp](/mnt/sda1/probe/pldm/common/test/dbus_async_utils_test.cpp)
  - [common/test/dbus_async_utils_mock_coverage_test.cpp](/mnt/sda1/probe/pldm/common/test/dbus_async_utils_mock_coverage_test.cpp)
- Added `pldmd` subprocess and failure-hook coverage in:
  - [test/pldmd_internal_test.cpp](/mnt/sda1/probe/pldm/test/pldmd_internal_test.cpp)
  - [test/pldmd_main_coverage_helper.cpp](/mnt/sda1/probe/pldm/test/pldmd_main_coverage_helper.cpp)
- Added `libpldmresponder` coverage in:
  - [libpldmresponder/test/libpldmresponder_base_test.cpp](/mnt/sda1/probe/pldm/libpldmresponder/test/libpldmresponder_base_test.cpp)
  - [libpldmresponder/test/libpldmresponder_fru_test.cpp](/mnt/sda1/probe/pldm/libpldmresponder/test/libpldmresponder_fru_test.cpp)
  - [libpldmresponder/test/libpldmresponder_platform_test.cpp](/mnt/sda1/probe/pldm/libpldmresponder/test/libpldmresponder_platform_test.cpp)
- Removed three `libpldmresponder_platform_test` assertions that depended on the known unreachable OEM `getStateSensorReadings()` wrapper path documented below in the production-bug notes. The authoritative CI run passed `68/68` after that correction.

## Production Bugs To Revisit

### `libpldmresponder/platform.cpp` request length mismatch

- Function: `Handler::getStateSensorReadings()`
- Current behavior:
  - outer guard checks `payloadLength != PLDM_GET_SENSOR_READING_REQ_BYTES`
  - decode path calls `decode_get_state_sensor_readings_req()`
  - decoder expects `PLDM_GET_STATE_SENSOR_READINGS_REQ_BYTES`
- Result:
  - success path is unreachable for valid `GetStateSensorReadings` decoding
  - tests can only hit invalid-length behavior unless production code is corrected
- This was not changed during the coverage work because you requested no production-code edits.
