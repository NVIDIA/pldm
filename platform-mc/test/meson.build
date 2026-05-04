dep_src_files = [
    '../terminus_manager.cpp',
    '../terminus.cpp',
    '../platform_manager.cpp',
    '../sensor_manager.cpp',
    '../numeric_sensor.cpp',
    '../state_sensor.cpp',
    '../state_effecter.cpp',
    '../state_set.cpp',
    '../event_manager.cpp',
    '../oem_events.cpp',
    '../smbios_mdr.cpp',
    '../state_effecter.cpp',
    '../numeric_effecter.cpp',
    '../state_sensor.cpp',
    '../event_manager.cpp',
    '../../test/test_instance_id.hpp',
    '../../fw-update/component_updater.cpp',
    '../../fw-update/device_updater.cpp',
    '../../fw-update/other_device_update_manager.cpp',
    '../../fw-update/update_manager.cpp',
    '../../fw-update/update.cpp',
    '../../fw-update/activation.cpp',
    '../../fw-update/config.cpp',
    '../../fw-update/firmware_inventory.cpp',
    '../../fw-update/package_parser.cpp',
    '../../fw-update/device_inventory.cpp',
    '../../fw-update/inventory_manager.cpp',
    '../../fw-update/package_signature.cpp',
    '../../fw-update/fw_update_utility.cpp',
    '../../fw-update/inventory_manager.cpp',
    '../../fw-update/firmware_inventory.cpp',
]

dep_src_files += [
    '../../oem/nvidia/platform-mc/oem_nvidia.cpp',
    '../../oem/nvidia/platform-mc/derived_sensor/switchBandwidthSensor.cpp',
]

dep_src_headers = ['../../requester', '../../oem/nvidia']

test_cpp_args = ['-DOEM_NVIDIA']

test_src = declare_dependency(
    sources: dep_src_files,
    include_directories: dep_src_headers,
)

tests = [
    'platform_sensor_getter_cross_tu_test',
    'platform_inline_coverage_test',
    'terminus_manager_test',
    'terminus_manager_private_test',
    'terminus_test',
    'sensor_manager_test',
    'numeric_sensor_test',
    'numeric_effecter_test',
    'state_effecter_test',
    'state_sensor_test',
    'state_set_nvidia_internal_test',
    'state_set_test',
    'oem_events_test',
]

private_tests = ['oem_events_private_test']

openssl = dependency('openssl', required: true)

foreach t : tests
    test(
        t,
        executable(
            t.underscorify(),
            t + '.cpp',
            implicit_include_directories: false,
            cpp_args: test_cpp_args,
            dependencies: [
                gtest,
                gmock,
                libpldm_dep,
                libpldmutils,
                nlohmann_json_dep,
                phosphor_dbus_interfaces,
                phosphor_logging_dep,
                sdbusplus,
                sdeventplus,
                test_src,
                nvidia_tal,
                openssl,
            ],
        ),
        is_parallel: t != 'terminus_test',
        workdir: meson.current_source_dir(),
        suite: non_valgrind_test_suites,
    )
endforeach

foreach t : private_tests
    test(
        t,
        executable(
            t.underscorify(),
            t + '.cpp',
            implicit_include_directories: false,
            cpp_args: test_cpp_args,
            include_directories: [
                '../..',
                '../../requester',
                '../../oem/nvidia',
            ],
            dependencies: [
                gtest,
                gmock,
                libpldm_dep,
                libpldmutils,
                nlohmann_json_dep,
                phosphor_dbus_interfaces,
                phosphor_logging_dep,
                sdbusplus,
                sdeventplus,
                nvidia_tal,
                openssl,
            ],
        ),
        workdir: meson.current_source_dir(),
        suite: non_valgrind_test_suites,
    )
endforeach
