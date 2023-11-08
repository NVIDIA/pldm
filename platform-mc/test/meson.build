dep_src_files = [
  '../terminus_manager.cpp',
  '../terminus.cpp',
  '../platform_manager.cpp',
  '../sensor_manager.cpp',
  '../numeric_sensor.cpp',
  '../state_sensor.cpp',
  '../state_effecter.cpp',
  '../state_set.cpp',
  '../event_manager.cpp',
  '../state_effecter.cpp',
  '../numeric_effecter.cpp',
  '../state_sensor.cpp',
  '../event_manager.cpp',
  '../../pldmd/dbus_impl_requester.cpp',
  '../../pldmd/instance_id.cpp',
]


dep_src_files += [
  '../../oem/nvidia/platform-mc/oem_nvidia.cpp',
  '../../oem/nvidia/libpldm/energy_count_numeric_sensor_oem.c'
]

dep_src_headers = [
  '../../requester',
  '../../oem/nvidia',
]

test_cpp_args = ['-DOEM_NVIDIA']

test_src = declare_dependency(sources: dep_src_files,
                              include_directories: dep_src_headers)

tests = [
  'terminus_manager_test',
  'terminus_test',
  'sensor_manager_test',
  'numeric_sensor_test',
  'event_manager_test',
  'state_effecter_test',
  'state_sensor_test',
]

openssl = dependency('openssl', required : true)

foreach t : tests
  test(t, executable(t.underscorify(), t + '.cpp',
                     implicit_include_directories: false,
                     link_args: dynamic_linker,
                     build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
                     cpp_args: test_cpp_args,
                     dependencies: [
                         gtest,
                         gmock,
                         libpldm_dep,
                         libpldmutils,
                         nlohmann_json,
                         phosphor_dbus_interfaces,
                         phosphor_logging,
                         sdbusplus,
                         sdeventplus,
                         test_src,
                         openssl]),
       workdir: meson.current_source_dir())
endforeach
