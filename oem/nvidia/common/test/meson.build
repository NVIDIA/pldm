# Tests for the NVIDIA OEM common sources. Processed after common/test,
# whose common_test_src dependency carries common/utils.cpp.

oem_nvidia_common_tests = ['em_device_test']

foreach t : oem_nvidia_common_tests
    test(
        t,
        executable(
            t.underscorify(),
            t + '.cpp',
            '../em_device.cpp',
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
