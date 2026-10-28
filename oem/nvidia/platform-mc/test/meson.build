# Tests for the NVIDIA OEM platform-mc sources. Processed after
# platform-mc/test, whose test_src dependency and test_cpp_args (-DOEM_NVIDIA)
# already carry the OEM sources and include paths.

oem_nvidia_platform_mc_tests = ['entity_tree_test', 'inventory_manager_test']

foreach t : oem_nvidia_platform_mc_tests
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
        workdir: meson.current_source_dir(),
        suite: non_valgrind_test_suites,
    )
endforeach
