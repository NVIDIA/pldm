pldmd_inc = include_directories('../')
test_src = declare_dependency(include_directories: pldmd_inc)

if get_option('use_fuzz').enabled()
    subdir('fuzz')
endif

tests = ['pldmd_registration_test']

foreach t : tests
    test(
        t,
        executable(
            t.underscorify(),
            t + '.cpp',
            implicit_include_directories: false,
            dependencies: [
                gtest,
                libpldm_dep,
                nlohmann_json_dep,
                phosphor_logging_dep,
                test_src,
            ],
        ),
        workdir: meson.current_source_dir(),
    )
endforeach

# pldmd_internal_test includes pldmd/pldmd.cpp directly (via #define main trick)
# and provides fake PldmTransport implementations, so common/transport.cpp must
# NOT be in the sources (it would cause duplicate-symbol link errors).
pldmd_internal_test_src = declare_dependency(
    sources: [
        '../common/utils.cpp',
        '../common/mctp_error_handling.cpp',
        '../requester/mctp_endpoint_discovery.cpp',
        '../fw-update/activation.cpp',
        '../fw-update/fw_update_utility.cpp',
        '../fw-update/inventory_manager.cpp',
        '../fw-update/package_parser.cpp',
        '../fw-update/component_updater.cpp',
        '../fw-update/device_updater.cpp',
        '../fw-update/update_manager.cpp',
        '../fw-update/update.cpp',
        '../fw-update/config.cpp',
        '../fw-update/device_inventory.cpp',
        '../fw-update/firmware_inventory.cpp',
        '../fw-update/other_device_update_manager.cpp',
        '../fw-update/package_signature.cpp',
        '../fw-update/oem-nvidia/debug_token.cpp',
    ],
    include_directories: pldmd_inc,
)

test(
    'pldmd_internal_test',
    executable(
        'pldmd_internal_test',
        'pldmd_internal_test.cpp',
        implicit_include_directories: false,
        dependencies: [
            gtest,
            libpldm_dep,
            nlohmann_json_dep,
            phosphor_dbus_interfaces,
            phosphor_logging_dep,
            sdbusplus,
            sdeventplus,
            stdplus,
            openssl,
            nvidia_tal,
            pldmd_internal_test_src,
        ],
    ),
    workdir: meson.current_source_dir(),
)
