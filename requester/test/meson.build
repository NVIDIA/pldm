test_src = declare_dependency(
          sources: [
            '../../pldmd/dbus_impl_requester.cpp',
            '../../pldmd/socket_manager.hpp',
            '../../pldmd/instance_id.cpp',
            '../mctp_endpoint_discovery.cpp',
            '../../pldmd/socket_handler.cpp',
            '../../pldmd/dbus_impl_requester.cpp',
            '../../common/utils.cpp',
            '../../fw-update/inventory_manager.cpp',
            '../../fw-update/package_parser.cpp',
            '../../fw-update/component_updater.cpp',
            '../../fw-update/device_updater.cpp',
            '../../fw-update/update_manager.cpp',
            '../../fw-update/config.cpp',
            '../../fw-update/device_inventory.cpp',
            '../../fw-update/firmware_inventory.cpp',
            '../../fw-update/other_device_update_manager.cpp',
            '../../fw-update/watch.cpp',
            '../../fw-update/package_signature.cpp'])

tests = [
  'handler_test',
  'request_test',
  'mctp_endpoint_discovery_test',
]

openssl = dependency('openssl', required : true)

foreach t : tests
  test(t, executable(t.underscorify(), t + '.cpp',
                     implicit_include_directories: false,
                     link_args: dynamic_linker,
                     build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
                     dependencies: [
                         gtest,
                         gmock,
                         libpldm_dep,
                         nlohmann_json,
                         phosphor_dbus_interfaces,
                         phosphor_logging,
                         sdbusplus,
                         sdeventplus,
                         test_src,
                         openssl]),
       workdir: meson.current_source_dir())
endforeach
