common_test_src = declare_dependency(sources: ['../utils.cpp'])

tests = ['pldm_utils_test']
pldm_utils_extra_sources = ['types_match_entry_coverage.cpp']

foreach t : tests
    test(
        t,
        executable(
            t.underscorify(),
            [t + '.cpp'] + pldm_utils_extra_sources,
            implicit_include_directories: false,
            dependencies: [
                common_test_src,
                gmock,
                gtest,
                libpldm_dep,
                nlohmann_json_dep,
                phosphor_dbus_interfaces,
                phosphor_logging_dep,
                libpldmutils,
                sdbusplus,
            ],
        ),
        workdir: meson.current_source_dir(),
        suite: non_valgrind_test_suites,
    )
endforeach

test(
    'dbus_async_utils_test',
    executable(
        'dbus_async_utils_test',
        'dbus_async_utils_test.cpp',
        implicit_include_directories: false,
        dependencies: [
            gtest,
            libpldm_dep,
            nlohmann_json_dep,
            phosphor_dbus_interfaces,
            phosphor_logging_dep,
            sdbusplus,
        ],
    ),
    workdir: meson.current_source_dir(),
    suite: non_valgrind_test_suites,
)

test(
    'dbus_async_utils_mock_coverage_test',
    executable(
        'dbus_async_utils_mock_coverage_test',
        'dbus_async_utils_mock_coverage_test.cpp',
        implicit_include_directories: false,
        dependencies: [
            gtest,
            libpldm_dep,
            nlohmann_json_dep,
            phosphor_dbus_interfaces,
            phosphor_logging_dep,
            sdbusplus,
        ],
    ),
    workdir: meson.current_source_dir(),
    suite: non_valgrind_test_suites,
)

test(
    'dbus_async_utils_cross_tu_coverage_test',
    executable(
        'dbus_async_utils_cross_tu_coverage_test',
        'dbus_async_utils_cross_tu_coverage_test.cpp',
        implicit_include_directories: false,
        dependencies: [
            gtest,
            libpldm_dep,
            nlohmann_json_dep,
            phosphor_dbus_interfaces,
            phosphor_logging_dep,
            sdbusplus,
        ],
    ),
    workdir: meson.current_source_dir(),
    suite: non_valgrind_test_suites,
)

test(
    'utils_inline_cross_tu_coverage_test',
    executable(
        'utils_inline_cross_tu_coverage_test',
        'utils_inline_cross_tu_coverage_test.cpp',
        implicit_include_directories: false,
        dependencies: [
            common_test_src,
            gtest,
            libpldm_dep,
            nlohmann_json_dep,
            phosphor_dbus_interfaces,
            phosphor_logging_dep,
            libpldmutils,
            sdbusplus,
        ],
    ),
    workdir: meson.current_source_dir(),
    suite: non_valgrind_test_suites,
)

# readHostEID_test: separate target that #includes utils.cpp with an
# overridden HOST_EID_PATH so the function reads from a temp file we control.
# It must NOT link libpldmutils or use common_test_src to avoid duplicate
# symbols (utils.cpp is compiled inline via the #include).
test(
    'readHostEID_test',
    executable(
        'readHostEID_test',
        'readHostEID_test.cpp',
        implicit_include_directories: false,
        dependencies: [
            gtest,
            libpldm_dep,
            nlohmann_json_dep,
            phosphor_dbus_interfaces,
            phosphor_logging_dep,
            sdbusplus,
        ],
    ),
    workdir: meson.current_source_dir(),
    suite: non_valgrind_test_suites,
)

# mctp_error_handling_test: separate target that #includes
# mctp_error_handling.cpp with mocked external dependencies and recvmsg.
# It must NOT link production objects that already contain the same symbols.
test(
    'mctp_error_handling_test',
    executable(
        'mctp_error_handling_test',
        'mctp_error_handling_test.cpp',
        implicit_include_directories: false,
        include_directories: [include_directories('../..')],
        dependencies: [
            gtest,
            libpldm_dep,
            nlohmann_json_dep,
            phosphor_dbus_interfaces,
            phosphor_logging_dep,
            sdbusplus,
        ],
    ),
    workdir: meson.current_source_dir(),
    suite: non_valgrind_test_suites,
)

test(
    'flight_recorder_disabled_test',
    executable(
        'flight_recorder_disabled_test',
        'flight_recorder_disabled_test.cpp',
        implicit_include_directories: false,
        dependencies: [
            common_test_src,
            gtest,
            libpldm_dep,
            nlohmann_json_dep,
            phosphor_dbus_interfaces,
            phosphor_logging_dep,
            libpldmutils,
            sdbusplus,
        ],
    ),
    workdir: meson.current_source_dir(),
    suite: non_valgrind_test_suites,
)
