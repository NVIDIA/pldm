pldmd_inc = include_directories('../')
pldmd_test_inc = declare_dependency(include_directories: pldmd_inc)

pldmd_internal_test_sources = [
    '../requester/mctp_endpoint_discovery.cpp',
    '../pldmd/dbus_impl_pdr.cpp',
]
pldmd_internal_test_responder_sources = [
    '../libpldmresponder/base.cpp',
    '../libpldmresponder/bios.cpp',
    '../libpldmresponder/bios_table.cpp',
    '../libpldmresponder/bios_attribute.cpp',
    '../libpldmresponder/bios_string_attribute.cpp',
    '../libpldmresponder/bios_integer_attribute.cpp',
    '../libpldmresponder/bios_enum_attribute.cpp',
    '../libpldmresponder/bios_config.cpp',
    '../libpldmresponder/pdr_utils.cpp',
    '../libpldmresponder/pdr.cpp',
    '../libpldmresponder/platform.cpp',
    '../libpldmresponder/fru_parser.cpp',
    '../libpldmresponder/fru.cpp',
    '../host-bmc/host_pdr_handler.cpp',
    '../host-bmc/utils.cpp',
    '../host-bmc/dbus_to_event_handler.cpp',
    '../host-bmc/dbus_to_host_effecters.cpp',
    '../host-bmc/host_condition.cpp',
    '../host-bmc/dbus/asset.cpp',
    '../host-bmc/dbus/availability.cpp',
    '../host-bmc/dbus/cable.cpp',
    '../host-bmc/dbus/chassis.cpp',
    '../host-bmc/dbus/cpu_core.cpp',
    '../host-bmc/dbus/custom_dbus.cpp',
    '../host-bmc/dbus/inventory_item.cpp',
    '../host-bmc/dbus/pcie_device.cpp',
    '../host-bmc/dbus/pcie_slot.cpp',
    '../libpldmresponder/event_parser.cpp',
]

if get_option('oem-meta').allowed()
    pldmd_internal_test_responder_sources += oem_files
endif

pldmd_internal_test_dependencies = []
pldmd_internal_test_cpp_args = [
    '-include',
    '../../test/pldmd_internal_config.hpp',
]

if get_option('pldm-type2').enabled()
    platform_mc_test_src = test_src
    pldmd_internal_test_dependencies += [platform_mc_test_src]
else
    pldmd_internal_test_dependencies += [fw_update_test_src]
endif

pldmd_internal_test_dep = declare_dependency(
    sources: [
        pldmd_internal_test_sources,
        pldmd_internal_test_responder_sources,
    ],
)

pldmd_internal_test_target_deps = [pldmd_internal_test_dep]
pldmd_internal_test_target_deps += pldmd_internal_test_dependencies
pldmd_internal_test_target_deps += [
    libpldm_dep,
    libpldmutils,
    nlohmann_json_dep,
    phosphor_dbus_interfaces,
    phosphor_logging_dep,
    sdbusplus,
    sdeventplus,
    openssl,
    nvidia_tal,
    pldmd_test_inc,
]

pldmd_main_coverage_helper = executable(
    'pldmd_main_coverage_helper',
    'pldmd_main_coverage_helper.cpp',
    implicit_include_directories: false,
    build_by_default: true,
    cpp_args: pldmd_internal_test_cpp_args,
    link_args: ['-Wl,--wrap=pldm_instance_db_init_default'],
    dependencies: pldmd_internal_test_target_deps,
)

if get_option('use_fuzz').enabled()
    subdir('fuzz')
endif

tests = ['pldmd_registration_test']

foreach t : tests
    test_deps = [deps, gtest, pldmd_test_inc]
    test_link_args = []
    test_cpp_args = []
    test_env = environment()
    test_target_deps = []
    test_is_parallel = true
    if t == 'pldmd_internal_test'
        test_link_args = ['-Wl,--wrap=pldm_instance_db_init_default']
        test_cpp_args = pldmd_internal_test_cpp_args
        test_deps = pldmd_internal_test_target_deps + [gtest]
        test_env.set(
            'PLDMD_MAIN_COVERAGE_HELPER',
            pldmd_main_coverage_helper.full_path(),
        )
        test_target_deps = [pldmd_main_coverage_helper]
        test_is_parallel = false
    endif

    test(
        t,
        executable(
            t.underscorify(),
            t + '.cpp',
            implicit_include_directories: false,
            cpp_args: test_cpp_args,
            link_args: test_link_args,
            dependencies: test_deps,
        ),
        workdir: meson.current_source_dir(),
        env: test_env,
        depends: test_target_deps,
        is_parallel: test_is_parallel,
        suite: non_valgrind_test_suites,
    )
endforeach
