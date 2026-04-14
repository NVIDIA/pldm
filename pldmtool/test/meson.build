tests = [
    'pldmtool_cmd_helper_test',
    'pldmtool_base_cmd_test',
    'pldmtool_fw_update_cmd_test',
    'pldmtool_platform_cmd_test',
    'pldmtool_main_test',
]

pldmtool_main_preload = shared_library(
    'pldmtool_main_preload',
    'pldmtool_main_preload.cpp',
    implicit_include_directories: false,
    include_directories: [
        include_directories('..'),
        include_directories('../..'),
    ],
    dependencies: [libpldm_dep],
    install: false,
)

foreach t : tests
    link_args = ['-Wl,--wrap=pldm_instance_db_init_default']
    test_depends = []
    test_tmpdir = join_paths(meson.project_build_root(), 'pldmtool', 'test-tmp')
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

    test_env = {'TEST_TMPDIR': test_tmpdir}
    if t == 'pldmtool_main_test'
        test_env += {
            'PLDMTOOL_BINARY': pldmtool_exe.full_path(),
            'PLDMTOOL_PRELOAD': pldmtool_main_preload.full_path(),
            'PLDMTOOL_INSTANCE_DB_DIR': join_paths(
                test_tmpdir,
                'pldmtool-instance-db',
            ),
        }
        test_depends += [pldmtool_exe, pldmtool_main_preload]
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
        depends: test_depends,
        is_parallel: t != 'pldmtool_cmd_helper_test',
        workdir: meson.current_source_dir(),
        env: test_env,
        suite: non_valgrind_test_suites,
    )
endforeach
