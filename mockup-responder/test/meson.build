dep_src_files = [
    '../../fw-update/config.cpp',
    '../../libpldm/requester/pldm.c',
    '../../libpldm/base.c',
    '../../common/utils.cpp',
    '../../libpldmresponder/base.cpp',
    '../../pldmd/handler.hpp',
    '../../libpldm/platform.c',
    '../../libpldm/pdr.c',
    # '../pldm_mockup_responder.cpp',
    '../mockup_responder.cpp',
    '../pdr_json_parser.cpp',
    '../sensor_to_dbus.cpp',
    '../../pldmd/dbus_impl_requester.cpp',
    '../../pldmd/instance_id.cpp',
]

dep_src_headers = [
    '.',
    '..',
    '../../',
    '../../common',
    '../../libpldm',
    '../../fw-update',
    '../../libpldmresponder',
]

test_src = declare_dependency(
    sources: dep_src_files,
    include_directories: dep_src_headers,
)

tests = ['mockup_responder_test']

foreach t : tests
    test(
        t,
        executable(
            t.underscorify(),
            t + '.cpp',
            implicit_include_directories: false,
            dependencies: [
                test_src,
                CLI11_dep,
                nlohmann_json_dep,
                sdbusplus,
                sdeventplus,
                phosphor_logging_dep,
                gtest,
                gmock,
                phosphor_dbus_interfaces,
            ],
        ),
        workdir: meson.current_source_dir(),
        suite: non_valgrind_test_suites,
    )
endforeach
