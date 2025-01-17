fw_update_test_src = declare_dependency(
          sources: [
            'fake_dbusutil.cpp',
            '../inventory_manager.cpp',
            '../package_parser.cpp',
            '../component_updater.cpp',
            '../device_updater.cpp',
            '../update_manager.cpp',
            '../config.cpp',
            '../device_inventory.cpp',
            '../firmware_inventory.cpp',
            '../package_signature.cpp',
            '../../common/utils.cpp',
            '../../pldmd/dbus_impl_requester.cpp',
            '../../pldmd/instance_id.cpp',
            '../other_device_update_manager.cpp',
            '../watch.cpp'])

tests = [
  'inventory_manager_test',
  'package_parser_test',
  'device_updater_test',
  'component_updater_test',
  'device_updater_firmware_update_test',
  'error_handling_test',
  'config_test',
  'device_inventory_test',
  'firmware_inventory_test',
  'other_device_update_manager_test',
  'update_manager_pkg_association_test',
  'update_manager_test',
  'activation_test',
  'package_signature_test',
]

cc = meson.get_compiler('c')
libcrypto = cc.find_library('libcrypto', required: true)
openssl = dependency('openssl', required : true)

foreach t : tests
  test(t, executable(t.underscorify(), t + '.cpp',
                     implicit_include_directories: false,
                     link_args: dynamic_linker,
                     build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
                     dependencies: [
                         fw_update_test_src,
                         gmock,
                         gtest,
                         libpldm_dep,
                         nlohmann_json,
                         phosphor_dbus_interfaces,
                         phosphor_logging,
                         sdbusplus,
                         sdeventplus,
                         libcrypto,
                         openssl]),
       workdir: meson.current_source_dir())
endforeach
