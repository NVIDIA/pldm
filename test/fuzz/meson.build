pldm_headers = [
  '.',
  '..'
]

deps = [
  fmt_dep,
  function2_dep,
  libpldm_dep,
  libpldmutils,
  nlohmann_json,
  sdbusplus,
  sdeventplus,
  phosphor_dbus_interfaces,  
  phosphor_logging,
  openssl

]

#executable(
#  'device-updater-fuzz-test',
#  sources: source_files,
#  implicit_include_directories: false,
#  link_args: dynamic_linker,
#  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
#  dependencies: deps,
#  install: false
#)


source_files_request_update = [
    'request_update_fuzz_test.cpp'
]

executable(
  'request_update_fuzz_test',
  sources: source_files_request_update,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)

sources_files_request_firmware_data = [
    'request_firmware_data_fuzz_test.cpp'
]

executable(
  'request_firmware_data_fuzz_test',
  sources: sources_files_request_firmware_data,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)

sources_files_transfer_complete = [
    'transfer_complete_fuzz_test.cpp'
]

executable(
  'transfer_complete_fuzz_test',
  sources: sources_files_transfer_complete,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)

sources_files_verify_complete = [
    'verify_complete_fuzz_test.cpp'
]

executable(
  'verify_complete_fuzz_test',
  sources: sources_files_verify_complete,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)

sources_files_apply_complete = [
    'apply_complete_fuzz_test.cpp'
]

executable(
  'apply_complete_fuzz_test',
  sources: sources_files_apply_complete,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)

sources_files_pdr_update = [
    'pdr_update_fuzz_test.cpp'
]

executable(
  'pdr_update_fuzz_test',
  sources: sources_files_pdr_update,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)

sources_files_pdr_access = [
    'pdr_access_fuzz_test.cpp'
]

executable(
  'pdr_access_fuzz_test',
  sources: sources_files_pdr_access,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)

sources_files_bios_get_date_time = [
    'bios_get_date_time_fuzz_test.cpp'
]

executable(
  'bios_get_date_time_fuzz_test',
  sources: sources_files_bios_get_date_time,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)


sources_files_set_date_time = [
    'bios_set_date_time_fuzz_test.cpp'
]

executable(
  'bios_set_date_time_fuzz_test',
  sources: sources_files_set_date_time,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)

sources_files_bios_get_set_table = [
    'bios_get_set_table_fuzz_test.cpp'
]

executable(
  'bios_get_set_table_fuzz_test',
  sources: sources_files_bios_get_set_table,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)


sources_files_bios_get_set_attr = [
    'bios_get_set_attr_fuzz_test.cpp'
]

executable(
  'bios_get_set_attr_fuzz_test',
  sources: sources_files_bios_get_set_attr,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)


sources_files_platform_set_state_effecter_states = [
    'platform_set_state_effecter_states_fuzz_test.cpp'
]

executable(
  'platform_set_state_effecter_states_fuzz_test',
  sources: sources_files_platform_set_state_effecter_states,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)



sources_files_platform_get_pdr = [
    'platform_get_pdr_fuzz_test.cpp'
]

executable(
  'platform_get_pdr_fuzz_test',
  sources: sources_files_platform_get_pdr,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)

sources_files_get_pdr_repository_info = [
    'get_pdr_repository_info_fuzz_test.cpp'
]

executable(
  'get_pdr_repository_info_fuzz_test',
  sources: sources_files_get_pdr_repository_info,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)

sources_files_get_set_numeric_effecter_value = [
    'get_set_numeric_effecter_value_fuzz_test.cpp'
]

executable(
  'get_set_numeric_effecter_value_fuzz_test',
  sources: sources_files_get_set_numeric_effecter_value,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)

sources_files_platform_event_message = [
    'platform_event_message_fuzz_test.cpp'
]

executable(
  'platform_event_message_fuzz_test',
  sources: sources_files_platform_event_message,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)

sources_files_pldm_pdr_repository_chg_event_event = [
    'pldm_pdr_repository_chg_event_event_fuzz_test.cpp'
]

executable(
  'pldm_pdr_repository_chg_event_event_fuzz_test',
  sources: sources_files_pldm_pdr_repository_chg_event_event,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)

sources_files_get_sensor_reading = [
    'get_sensor_reading_fuzz_test.cpp'
]

executable(
  'get_sensor_reading_fuzz_test',
  sources: sources_files_get_sensor_reading,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)

sources_files_event_message_supported = [
    'event_message_supported_fuzz_test.cpp'
]

executable(
  'event_message_supported_fuzz_test',
  sources: sources_files_event_message_supported,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)

sources_files_time_legal_test = [
    'time_legal_test_fuzz_test.cpp'
]

executable(
  'time_legal_test_fuzz_test',
  sources: sources_files_time_legal_test,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)

sources_files_mock_numeric_sensor = [
    'mock_numeric_sensor_fuzz_test.cpp'
]

executable(
  'mock_numeric_sensor_fuzz_test',
  sources: sources_files_mock_numeric_sensor,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)

sources_files_mock_sensor_manager = [
    'mock_sensor_manager_fuzz_test.cpp'
]

executable(
  'mock_sensor_manager_fuzz_test',
  sources: sources_files_mock_sensor_manager,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)

sources_files_mock_terminus_manager = [
    'mock_terminus_manager_fuzz_test.cpp'
]

executable(
  'mock_terminus_manager_fuzz_test',
  sources: sources_files_mock_terminus_manager,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)

sources_files_mctp_endpoint_discovery = [
    'mctp_endpoint_discovery_fuzz_test.cpp'
]

executable(
  'mctp_endpoint_discovery_fuzz_test',
  sources: sources_files_mctp_endpoint_discovery,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)

sources_files_pldm_registration_test = [
    'pldm_registration_test_fuzz_test.cpp'
]

executable(
  'pldm_registration_test_fuzz_test',
  sources: sources_files_pldm_registration_test,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)

sources_files_get_state_sensor_readings_fuzz_test = [
    'get_state_sensor_readings_fuzz_test.cpp'
]

executable(
  'get_state_sensor_readings_fuzz_test',
  sources: sources_files_get_state_sensor_readings_fuzz_test,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)


sources_files_pdr_entities_fuzz_test = [
    'pdr_entities_fuzz_test.cpp'
]

executable(
  'pdr_entities_fuzz_test',
  sources: sources_files_pdr_entities_fuzz_test,
  implicit_include_directories: false,
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: false
)

#sources_pldmd_mockup = [
#'../../inventory_manager.cpp',
#    '../../package_parser.cpp',
#    '../../../fw-update/component_updater.cpp',
#    '../../../fw-update/package_signature.cpp',
#    '../../device_updater.cpp',
#    '../../update_manager.cpp',
#    '../../../requester/mctp_endpoint_discovery.cpp',
#    '../../../pldmd/socket_handler.cpp',
#    '../../config.cpp',
#    '../../device_inventory.cpp',
#     '../../firmware_inventory.cpp',
#    '../../../common/utils.cpp',
#    '../../../pldmd/dbus_impl_requester.cpp',
#    '../../../pldmd/instance_id.cpp',
#    '../../other_device_update_manager.cpp',
#    '../../watch.cpp',
#     'pldmd_mockup.cpp'
#]

#executable(
#  'pldmd_mockup',
#  sources: sources_pldmd_mockup,
#  implicit_include_directories: false,
#  link_args: dynamic_linker,
#  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
#  cpp_args: '-DENABLE_FUZZ_TESTS',
#  dependencies: deps,
#  install: false
#)

