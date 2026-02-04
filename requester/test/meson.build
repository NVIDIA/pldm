test_src = declare_dependency(
    sources: [
        '../mctp_endpoint_discovery.cpp',
        '../../common/utils.cpp',
        '../../common/transport.cpp',
        '../../common/mctp_error_handling.cpp',
        '../../fw-update/activation.cpp',
        '../../fw-update/fw_update_utility.cpp',
        '../../fw-update/inventory_manager.cpp',
        '../../fw-update/package_parser.cpp',
        '../../fw-update/component_updater.cpp',
        '../../fw-update/device_updater.cpp',
        '../../fw-update/update_manager.cpp',
        '../../fw-update/update.cpp',
        '../../fw-update/config.cpp',
        '../../fw-update/device_inventory.cpp',
        '../../fw-update/firmware_inventory.cpp',
        '../../fw-update/other_device_update_manager.cpp',
        '../../fw-update/package_signature.cpp',
        '../../fw-update/oem-nvidia/debug_token.cpp',
    ],
)

tests = ['handler_test', 'request_test', 'mctp_endpoint_discovery_test']

openssl = dependency('openssl', required: true)

foreach t : tests
    test(
        t,
        executable(
            t.underscorify(),
            t + '.cpp',
            implicit_include_directories: false,
            include_directories: ['../../', '../../pldmd'],
            dependencies: [
                gtest,
                gmock,
                libpldm_dep,
                nlohmann_json_dep,
                phosphor_dbus_interfaces,
                phosphor_logging_dep,
                sdbusplus,
                sdeventplus,
                test_src,
                openssl,
            ],
        ),
        workdir: meson.current_source_dir(),
    )
endforeach
