project('pldm', ['c', 'cpp'],
        version: '0.1', meson_version: '>=0.57.0',
        default_options: [
          'warning_level=3',
          'default_library=shared',
          'werror=true',
          'cpp_std=c++20',
          'buildtype=debugoptimized'
        ])

# Wno-psabi reduces the number of "Note:" messages when cross-compiling some STL
# stuff for ARM. See https://stackoverflow.com/questions/48149323/strange-gcc-warning-when-compiling-qt-project
# Basically, gcc 6 and gcc 7 are not ABI compatible, but since the whole OpenBMC
# project uses the same compiler, we can safely ignmore these info notes.
add_project_arguments('-Wno-psabi', language: 'cpp')

# Enable support for the C++ coroutines extension
add_project_arguments('-fcoroutines',language : ['cpp'])

# Disable FORTIFY_SOURCE when compiling with no optimization
if(get_option('optimization') == '0')
  add_project_arguments('-U_FORTIFY_SOURCE',language:['cpp','c'])
  message('Disabling FORTIFY_SOURCE as optimization is set to 0')
endif

package_datadir = join_paths(get_option('prefix'), get_option('datadir'), meson.project_name())
package_localstatedir = join_paths(get_option('prefix'), get_option('localstatedir'), meson.project_name())

conf_data = configuration_data()
if get_option('libpldmresponder').enabled()
conf_data.set_quoted('BIOS_JSONS_DIR', join_paths(package_datadir, 'bios'))
conf_data.set_quoted('BIOS_TABLES_DIR', join_paths(package_localstatedir, 'bios'))
conf_data.set_quoted('PDR_JSONS_DIR', join_paths(package_datadir, 'pdr'))
conf_data.set_quoted('FRU_JSONS_DIR', join_paths(package_datadir, 'fru'))
conf_data.set_quoted('FRU_MASTER_JSON', join_paths(package_datadir, 'fru_master.json'))
conf_data.set_quoted('HOST_JSONS_DIR', join_paths(package_datadir, 'host'))
conf_data.set_quoted('EVENTS_JSONS_DIR', join_paths(package_datadir, 'events'))
conf_data.set('HEARTBEAT_TIMEOUT', get_option('heartbeat-timeout-seconds'))
conf_data.set('TERMINUS_ID', get_option('terminus-id'))
conf_data.set('TERMINUS_HANDLE',get_option('terminus-handle'))
add_project_arguments('-DLIBPLDMRESPONDER', language : ['c','cpp'])
endif
if get_option('softoff').enabled()
  conf_data.set('SOFTOFF_TIMEOUT_SECONDS', get_option('softoff-timeout-seconds'))
endif
if get_option('oem-ibm').enabled()
  conf_data.set_quoted('FILE_TABLE_JSON', join_paths(package_datadir, 'fileTable.json'))
  conf_data.set_quoted('LID_RUNNING_DIR', '/var/lib/phosphor-software-manager/hostfw/running')
  conf_data.set_quoted('LID_ALTERNATE_DIR', '/var/lib/phosphor-software-manager/hostfw/alternate')
  conf_data.set_quoted('LID_STAGING_DIR', '/var/lib/phosphor-software-manager/hostfw/staging')
  conf_data.set_quoted('LID_RUNNING_PATCH_DIR', '/usr/local/share/hostfw/running')
  conf_data.set_quoted('LID_ALTERNATE_PATCH_DIR', '/usr/local/share/hostfw/alternate')
  conf_data.set('DMA_MAXSIZE', get_option('oem-ibm-dma-maxsize'))
  add_project_arguments('-DOEM_IBM', language : 'c')
  add_project_arguments('-DOEM_IBM', language : 'cpp')
endif
if get_option('oem-nvidia').enabled()
  add_project_arguments('-DOEM_NVIDIA', language : 'c')
  add_project_arguments('-DOEM_NVIDIA', language : 'cpp')
endif

if get_option('non-pldm').enabled()
  add_project_arguments('-DNON_PLDM', language : ['c','cpp'])
endif
if get_option('fw-update-skip-package-size-check').enabled()
    add_project_arguments('-DSKIP_PACKAGE_SIZE_CHECK', language : ['c','cpp'])
endif

if(get_option('pldm-package-verification') == 'authentication')
    add_project_arguments('-DPLDM_PACKAGE_VERIFICATION', language : ['c','cpp'])
endif
if get_option('pldm-package-verification-must-be-signed').enabled()
    add_project_arguments('-DPLDM_PACKAGE_VERIFICATION_MUST_BE_SIGNED', language : ['c','cpp'])
endif
if(get_option('pldm-package-verification') == 'integrity')
    add_project_arguments('-DPLDM_PACKAGE_INTEGRITY_CHECK', language : ['c','cpp'])
endif

if get_option('pldm-type2').enabled()
  add_project_arguments('-DPLDM_TYPE2', language : ['c','cpp'])
endif
if get_option('oem-nvidia').enabled()
    add_project_arguments('-DOEM_NVIDIA', language : ['c','cpp'])
endif
if get_option('debug-token').enabled()
    add_project_arguments('-DDEBUG_TOKEN', language : ['c','cpp'])
endif
conf_data.set('PLDM_VERBOSITY',get_option('verbosity'))
if get_option('fw-debug').enabled()
  conf_data.set('FWDEBUG', '--fw-debug')
endif
conf_data.set('NUMBER_OF_REQUEST_RETRIES', get_option('number-of-request-retries'))
conf_data.set('NUMBER_OF_COMMAND_ATTEMPTS', get_option('number-of-command-attempts'))
conf_data.set('INSTANCE_ID_EXPIRATION_INTERVAL',get_option('instance-id-expiration-interval'))
conf_data.set('RESPONSE_TIME_OUT',get_option('response-time-out'))
conf_data.set('FLIGHT_RECORDER_MAX_ENTRIES',get_option('flightrecorder-max-entries'))
conf_data.set('FIRMWARE_UPDATE_TIME', get_option('firmware-update-time'))
if get_option('firmware-package-staging-dir').endswith('/')
  conf_data.set_quoted('FIRMWARE_PACKAGE_STAGING_DIR', get_option('firmware-package-staging-dir').substring(0, -1))
else
  conf_data.set_quoted('FIRMWARE_PACKAGE_STAGING_DIR', get_option('firmware-package-staging-dir'))
endif
if get_option('firmware-package-split-staging-dir').endswith('/')
  conf_data.set_quoted('FIRMWARE_PACKAGE_SPLIT_STAGING_DIR', get_option('firmware-package-split-staging-dir').substring(0, -1))
else
  conf_data.set_quoted('FIRMWARE_PACKAGE_SPLIT_STAGING_DIR', get_option('firmware-package-split-staging-dir'))
endif
conf_data.set_quoted('FIRMWARE_PACKAGE_STAGING_DIR_MOUNT_SERVICE', get_option('firmware-package-staging-dir-mount-service'))
conf_data.set_quoted('FIRMWARE_PACKAGE_SPLIT_STAGING_DIR_MOUNT_SERVICE', get_option('firmware-package-split-staging-dir-mount-service'))
conf_data.set('PROGRESS_UPDATE_INTERVAL', get_option('progress-percent-updater-interval'))
if get_option('libpldm-only').disabled()
  conf_data.set_quoted('HOST_EID_PATH', join_paths(package_datadir, 'host_eid'))
endif
conf_data.set('MAXIMUM_TRANSFER_SIZE', get_option('maximum-transfer-size'))
conf_data.set_quoted('FW_UPDATE_CONFIG_JSON', join_paths(package_datadir, 'fw_update_config.json'))
conf_data.set_quoted('STATIC_EID_TABLE_PATH', join_paths(package_datadir, 'static_eid_table.json'))
conf_data.set_quoted('PLDM_T2_CONFIG_JSON', join_paths(package_datadir, 'pldm_t2_config.json'))
conf_data.set_quoted('PLDM_PACKAGE_VERIFICATION_KEY', get_option('pldm-package-verification-key'))
conf_data.set('SENSOR_POLLING_TIME', get_option('sensor-polling-time'))
conf_data.set('LOCAL_EID_OVER_I2C', get_option('local-eid-over-i2c'))
conf_data.set('LOCAL_EID_OVER_PCIE', get_option('local-eid-over-pcie'))
conf_data.set('STALE_SENSOR_UPPER_LIMITS_POLLING_TIME', get_option('stale-sensor-upper-limitms-polling-time'))
if get_option('platform-prefix') != ''
  conf_data.set_quoted('PLATFORM_PREFIX', get_option('platform-prefix'))
endif

configure_file(output: 'config.h',
  configuration: conf_data
)

cpp = meson.get_compiler('cpp')

if get_option('libpldm-only').disabled()
  phosphor_dbus_interfaces = dependency('phosphor-dbus-interfaces')
  sdbusplus = dependency('sdbusplus')
  sdeventplus = dependency('sdeventplus')
  phosphor_logging = dependency('phosphor-logging')

  if cpp.has_header('nlohmann/json.hpp')
    nlohmann_json = declare_dependency()
  else
    nlohmann_json = dependency('nlohmann_json')
  endif

  if cpp.has_header('CLI/CLI.hpp')
    CLI11_dep = declare_dependency()
  else
    CLI11_dep = dependency('CLI11')
  endif

  if cpp.has_header_symbol('function2/function2.hpp', 'fu2::unique_function')
    function2_dep = declare_dependency()
  else
    subproject('function2')
    function2_dep = declare_dependency(
      include_directories: [
        'subprojects/function2/include/function2'
      ]
    )
  endif

  fmt_dep = dependency('fmt', required: false)
  if not fmt_dep.found()
    fmt_opts = import('cmake').subproject_options()
    fmt_opts.add_cmake_defines({
      'CMAKE_POSITION_INDEPENDENT_CODE': 'ON',
      'MASTER_PROJECT': 'OFF',
    })
    fmt_proj = import('cmake').subproject(
      'fmt',
      options: fmt_opts,
      required: false)
    assert(fmt_proj.found(), 'fmtlib is required')
    fmt_dep = fmt_proj.dependency('fmt')
  endif
endif

if get_option('oe-sdk').enabled()
  # Setup OE SYSROOT
  OECORE_TARGET_SYSROOT = run_command('sh', '-c', 'echo $OECORE_TARGET_SYSROOT').stdout().strip()
  if OECORE_TARGET_SYSROOT == ''
      error('Unable to get $OECORE_TARGET_SYSROOT, check your environment.')
  endif
  message('OE_SYSROOT: ' + OECORE_TARGET_SYSROOT)
  rpath = ':'.join([OECORE_TARGET_SYSROOT + '/lib', OECORE_TARGET_SYSROOT + '/usr/lib'])
  ld_so = run_command('sh', '-c', 'find ' + OECORE_TARGET_SYSROOT + '/lib/ld-*.so | sort -r -n | head -n1').stdout().strip()
  dynamic_linker = ['-Wl,-dynamic-linker,' + ld_so]
else
  dynamic_linker = []
endif

if get_option('tests').enabled()
    add_project_arguments('-DPLDM_PACKAGE_VERIFICATION', language : ['c','cpp'])
    gtest = dependency('gtest', main: true, disabler: true, required: false)
    gmock = dependency('gmock', disabler: true, required: false)
    if not gtest.found() or not gmock.found()
        gtest_proj = import('cmake').subproject('googletest', required: false)
        if gtest_proj.found()
            gtest = declare_dependency(
                dependencies: [
                    dependency('threads'),
                    gtest_proj.dependency('gtest'),
                    gtest_proj.dependency('gtest_main'),
                ]
            )
            gmock = gtest_proj.dependency('gmock')
        else
            assert(
                not get_option('tests').enabled(),
                'Googletest is required if tests are enabled'
            )
        endif
    endif
endif

subdir('libpldm')

if get_option('libpldm-only').disabled()

openssl = dependency('openssl', required : true)

libpldmutils_headers = ['.']
libpldmutils = library(
  'pldmutils',
  'common/utils.cpp',
  version: meson.project_version(),
  dependencies: [
      libpldm_dep,
      phosphor_dbus_interfaces,
      nlohmann_json,
      sdbusplus,
      phosphor_logging,
  ],
  install: true,
  include_directories: include_directories(libpldmutils_headers),
)

libpldmutils = declare_dependency(
  include_directories: include_directories(libpldmutils_headers),
  link_with: libpldmutils)

deps = [
  fmt_dep,
  function2_dep,
  libpldm_dep,
  libpldmutils,
  nlohmann_json,
  sdbusplus,
  sdeventplus,
  phosphor_dbus_interfaces,
  phosphor_logging,
  openssl,
]

if get_option('libpldmresponder').enabled()
subdir('libpldmresponder')
deps += [
  libpldmresponder
]
endif
source_files = [
  'pldmd/pldmd.cpp',
  'pldmd/dbus_impl_requester.cpp',
  'pldmd/instance_id.cpp',
  'pldmd/dbus_impl_pdr.cpp',
  'pldmd/socket_handler.cpp',
  'fw-update/inventory_manager.cpp',
  'fw-update/package_parser.cpp',
  'fw-update/component_updater.cpp',
  'fw-update/device_updater.cpp',
  'fw-update/watch.cpp',
  'fw-update/update_manager.cpp',
  'fw-update/other_device_update_manager.cpp',
  'fw-update/config.cpp',
  'fw-update/device_inventory.cpp',
  'fw-update/firmware_inventory.cpp',
  'fw-update/package_signature.cpp',
  'platform-mc/terminus_manager.cpp',
  'platform-mc/terminus.cpp',
  'platform-mc/platform_manager.cpp',
  'platform-mc/sensor_manager.cpp',
  'platform-mc/numeric_sensor.cpp',
  'platform-mc/numeric_effecter.cpp',
  'platform-mc/state_sensor.cpp',
  'platform-mc/event_manager.cpp',
  'platform-mc/state_set.cpp',
  'platform-mc/state_effecter.cpp',
  'platform-mc/state_set.cpp',
  'requester/mctp_endpoint_discovery.cpp',
]

if get_option('oem-nvidia').enabled()
  source_files += [
    'oem/nvidia/platform-mc/oem_nvidia.cpp'
  ]
endif

if get_option('debug-token').enabled()
  source_files += [
    'fw-update/oem-nvidia/debug_token.cpp'
  ]
endif


executable(
  'pldmd',
  sources: source_files,
  implicit_include_directories: false,
  dependencies: deps,
  install: true,
  install_dir: get_option('bindir'))

if get_option('systemd').enabled()
  systemd_system_unit_dir = dependency('systemd').get_variable(
          pkgconfig: 'systemdsystemunitdir')

  configure_file(
    copy: true,
    input: 'pldmd/service_files/pldmd.service',
    install: true,
    install_dir: systemd_system_unit_dir,
    output: 'pldmd.service',
  )

  configure_file(
    input: 'pldmd/verbosity/verbosity',
    output: 'pldm_verbosity',
    configuration: conf_data,
    install: true,
    install_dir: join_paths(get_option('sysconfdir'), 'default'))

  if get_option('oem-ibm').enabled()
    subdir('oem/ibm/service_files')
  endif
endif

subdir('pldmtool')

subdir('configurations')

if get_option('utilities').enabled()
  subdir('utilities')
endif

if get_option('softoff').enabled()
  subdir('softoff')
endif

if get_option('tests').enabled()
  subdir('common/test')
  subdir('fw-update/test')
  subdir('host-bmc/test')
  subdir('requester/test')
  subdir('platform-mc/test')
  subdir('test')
endif

endif # pldm-only
