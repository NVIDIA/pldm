host_bmc_test_src = declare_dependency(
    sources: ['../dbus_to_host_effecters.cpp'],
    include_directories: '../../requester',
)

tests = ['dbus_to_host_effecter_test']

foreach t : tests
    test(
        t,
        executable(
            t.underscorify(),
            t + '.cpp',
            implicit_include_directories: false,
            dependencies: [
                gtest,
                gmock,
                host_bmc_test_src,
                libpldm_dep,
                libpldmutils,
                nlohmann_json_dep,
                phosphor_dbus_interfaces,
                phosphor_logging_dep,
                sdbusplus,
                sdeventplus,
            ],
        ),
        workdir: meson.current_source_dir(),
        suite: non_valgrind_test_suites,
    )
endforeach
