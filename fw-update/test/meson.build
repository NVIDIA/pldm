fw_update_test_sources = [
    'fake_dbusutil.cpp',
    '../activation.cpp',
    '../aggregate_update_manager.cpp',
    '../inventory_manager.cpp',
    '../package_parser.cpp',
    '../component_updater.cpp',
    '../device_updater.cpp',
    '../firmware_inventory_manager.cpp',
    '../update_manager.cpp',
    '../update.cpp',
    '../config.cpp',
    '../device_inventory.cpp',
    '../firmware_inventory.cpp',
    '../package_signature.cpp',
    '../../common/utils.cpp',
    '../../common/test/mocked_utils.hpp',
    '../other_device_update_manager.cpp',
    '../fw_update_utility.cpp',
    '../oem-nvidia/debug_token.cpp',
]

fw_update_test_sources_no_config = []
fw_update_test_sources_no_device_inventory = []
fw_update_test_sources_no_firmware_inventory = []
foreach src : fw_update_test_sources
    if src != '../config.cpp'
        fw_update_test_sources_no_config += src
    endif
    if src != '../device_inventory.cpp'
        fw_update_test_sources_no_device_inventory += src
    endif
    if src != '../firmware_inventory.cpp'
        fw_update_test_sources_no_firmware_inventory += src
    endif
endforeach

fw_update_test_src = declare_dependency(sources: fw_update_test_sources)
fw_update_test_src_no_config = declare_dependency(
    sources: fw_update_test_sources_no_config,
)
fw_update_test_src_no_device_inventory = declare_dependency(
    sources: fw_update_test_sources_no_device_inventory,
)
fw_update_test_src_no_firmware_inventory = declare_dependency(
    sources: fw_update_test_sources_no_firmware_inventory,
)

tests = [
    'activation_real_update_manager_test',
    'activation_test',
    'component_updater_test',
    'config_internal_test',
    'config_test',
    'dbusutil_internal_test',
    'debug_token_internal_test',
    'device_inventory_internal_thread_test',
    'device_inventory_test',
    'device_updater_firmware_update_test',
    'device_updater_test',
    'error_handling_test',
    'firmware_inventory_internal_thread_test',
    'firmware_inventory_manager_test',
    'firmware_inventory_test',
    'inventory_manager_internal_test',
    'inventory_manager_test',
    'manager_internal_test',
    'other_device_update_manager_test',
    'package_parser_stub_error_test',
    'package_parser_test',
    'package_signature_test',
    'update_and_utility_test',
    'update_manager_pkg_association_test',
    'update_manager_test',
]

openssl = dependency('openssl', required: true)

foreach t : tests
    test_fw_update_src = fw_update_test_src
    test_cpp_args = []
    test_include_dirs = ['../../pldmd']
    if t == 'config_internal_test'
        test_fw_update_src = fw_update_test_src_no_config
    elif t == 'device_inventory_internal_thread_test'
        test_fw_update_src = fw_update_test_src_no_device_inventory
    elif t == 'firmware_inventory_internal_thread_test'
        test_fw_update_src = fw_update_test_src_no_firmware_inventory
    elif t == 'debug_token_internal_test' or t == 'other_device_update_manager_test'
        test_cpp_args = ['-fno-access-control']
    endif

    if t != 'dbusutil_internal_test'
        test_include_dirs += 'include'
    endif

    test(
        t,
        executable(
            t.underscorify(),
            t + '.cpp',
            cpp_args: test_cpp_args,
            implicit_include_directories: false,
            include_directories: test_include_dirs,
            dependencies: [
                test_fw_update_src,
                gmock,
                gtest,
                libpldm_dep,
                libpldmutils,
                nlohmann_json_dep,
                phosphor_dbus_interfaces,
                phosphor_logging_dep,
                sdbusplus,
                sdeventplus,
                openssl,
            ],
        ),
        workdir: meson.current_source_dir(),
    )
endforeach
