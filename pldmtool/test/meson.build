tests = [
    'pldmtool_cmd_helper_test',
    'pldmtool_base_cmd_test',
    'pldmtool_fw_update_cmd_test',
    'pldmtool_platform_cmd_test',
    'pldmtool_main_test',
]

foreach t : tests
    link_args = ['-Wl,--wrap=pldm_instance_db_init_default']
    if t == 'pldmtool_cmd_helper_test'
        link_args += [
            '-Wl,--wrap=pldm_transport_af_mctp_init',
            '-Wl,--wrap=pldm_transport_af_mctp_destroy',
            '-Wl,--wrap=pldm_transport_af_mctp_core',
            '-Wl,--wrap=pldm_transport_af_mctp_init_pollfd',
            '-Wl,--wrap=pldm_transport_af_mctp_map_tid',
            '-Wl,--wrap=pldm_transport_af_mctp_bind',
            '-Wl,--wrap=pldm_transport_send_recv_msg',
        ]
    endif

    test(
        t,
        executable(
            t.underscorify(),
            t + '.cpp',
            implicit_include_directories: false,
            include_directories: [
                include_directories('..'),
                include_directories('../..'),
            ],
            link_args: link_args,
            dependencies: [
                CLI11_dep,
                gmock,
                gtest,
                libpldm_dep,
                libpldmutils,
                nlohmann_json_dep,
                phosphor_dbus_interfaces,
                phosphor_logging_dep,
                sdbusplus,
            ],
        ),
        workdir: meson.current_source_dir(),
        env: {'TEST_TMPDIR': '/tmp/claude'},
    )
endforeach
