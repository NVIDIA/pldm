project(
    'pldm',
    'cpp',
    version: '0.1',
    meson_version: '>=1.1.1',
    default_options: [
        'warning_level=3',
        'default_library=shared',
        'werror=true',
        'cpp_std=c++23',
        'buildtype=debugoptimized',
    ],
)

# Wno-psabi reduces the number of "Note:" messages when cross-compiling some STL
# stuff for ARM. See https://stackoverflow.com/questions/48149323/strange-gcc-warning-when-compiling-qt-project
# Basically, gcc 6 and gcc 7 are not ABI compatible, but since the whole OpenBMC
# project uses the same compiler, we can safely ignmore these info notes.
add_project_arguments('-Wno-psabi', language: 'cpp')

# Disable subobject-linkage warning for coroutine frames with lambda expressions
# This warning occurs when using lambda expressions with stdexec::then() in coroutines
add_project_arguments('-Wno-subobject-linkage', language: 'cpp')

# Disable FORTIFY_SOURCE when compiling with no optimization
if (get_option('optimization') == '0')
    add_project_arguments('-U_FORTIFY_SOURCE', language: 'cpp')
    message('Disabling FORTIFY_SOURCE as optimization is set to 0')
endif

if get_option('use_fuzz').enabled()
    add_global_arguments('-DFUZZ_TEST', language: ['c', 'cpp'])
endif

package_datadir = join_paths(
    get_option('prefix'),
    get_option('datadir'),
    meson.project_name(),
)
package_localstatedir = join_paths(
    get_option('prefix'),
    get_option('localstatedir'),
    meson.project_name(),
)

conf_data = configuration_data()
cpp = meson.get_compiler('cpp')

# Enable POSIX poll APIs in libpldm
if cpp.has_header('poll.h')
    conf_data.set('PLDM_HAS_POLL', 1)
endif

conf_data.set_quoted('HOST_JSONS_DIR', join_paths(package_datadir, 'host'))

if get_option('omit-heartbeat').enabled()
    add_project_arguments('-DOMIT_HEARTBEAT', language: 'cpp')
endif

if get_option('non-pldm').enabled()
    add_project_arguments('-DNON_PLDM', language: ['c', 'cpp'])
endif
if get_option('fw-update-skip-package-size-check').enabled()
    add_project_arguments('-DSKIP_PACKAGE_SIZE_CHECK', language: ['c', 'cpp'])
endif

if (get_option('pldm-package-verification') == 'authentication')
    add_project_arguments('-DPLDM_PACKAGE_VERIFICATION', language: ['c', 'cpp'])
endif
if get_option('pldm-package-verification-must-be-signed').enabled()
    add_project_arguments(
        '-DPLDM_PACKAGE_VERIFICATION_MUST_BE_SIGNED',
        language: ['c', 'cpp'],
    )
endif
if (get_option('pldm-package-verification') == 'integrity')
    add_project_arguments(
        '-DPLDM_PACKAGE_INTEGRITY_CHECK',
        language: ['c', 'cpp'],
    )
endif

conf_data.set(
    'CALCULATE_DIGEST_CHUNK_SIZE',
    get_option('pldm-package-verification-calculate-digest-chunk-size'),
)
conf_data.set(
    'CALCULATE_DIGEST_TIMER_INTERVAL',
    get_option('pldm-package-verification-timer-interval'),
)

if get_option('pldm-type2').enabled()
    add_project_arguments('-DPLDM_TYPE2', language: ['c', 'cpp'])
endif
if get_option('oem-nvidia').enabled()
    add_project_arguments('-DOEM_NVIDIA', language: ['c', 'cpp'])
endif
if get_option('debug-token').enabled()
    add_project_arguments('-DDEBUG_TOKEN', language: ['c', 'cpp'])
endif
conf_data.set('PLDM_VERBOSITY', get_option('verbosity'))
if get_option('fw-debug').enabled()
    conf_data.set('FWDEBUG', '--fw-debug')
endif
conf_data.set(
    'NUMBER_OF_REQUEST_RETRIES',
    get_option('number-of-request-retries'),
)
conf_data.set(
    'NUMBER_OF_COMMAND_ATTEMPTS',
    get_option('number-of-command-attempts'),
)
conf_data.set(
    'INSTANCE_ID_EXPIRATION_INTERVAL',
    get_option('instance-id-expiration-interval'),
)
conf_data.set('RESPONSE_TIME_OUT', get_option('response-time-out'))
conf_data.set(
    'FLIGHT_RECORDER_MAX_ENTRIES',
    get_option('flightrecorder-max-entries'),
)
conf_data.set('FIRMWARE_UPDATE_TIME', get_option('firmware-update-time'))
if get_option('firmware-package-staging-dir').endswith('/')
    conf_data.set_quoted(
        'FIRMWARE_PACKAGE_STAGING_DIR',
        get_option('firmware-package-staging-dir').substring(0, -1),
    )
else
    conf_data.set_quoted(
        'FIRMWARE_PACKAGE_STAGING_DIR',
        get_option('firmware-package-staging-dir'),
    )
endif
conf_data.set_quoted(
    'FIRMWARE_PACKAGE_STAGING_DIR_MOUNT_SERVICE',
    get_option('firmware-package-staging-dir-mount-service'),
)
conf_data.set(
    'PROGRESS_UPDATE_INTERVAL',
    get_option('progress-percent-updater-interval'),
)

if get_option('libpldmresponder').allowed()
    conf_data.set_quoted('BIOS_JSONS_DIR', join_paths(package_datadir, 'bios'))
    conf_data.set(
        'SYSTEM_SPECIFIC_BIOS_JSON',
        get_option('system-specific-bios-json').allowed(),
    )
    conf_data.set_quoted(
        'BIOS_TABLES_DIR',
        join_paths(package_localstatedir, 'bios'),
    )
    conf_data.set_quoted('PDR_JSONS_DIR', join_paths(package_datadir, 'pdr'))
    conf_data.set_quoted('FRU_JSONS_DIR', join_paths(package_datadir, 'fru'))
    conf_data.set_quoted(
        'FRU_MASTER_JSON',
        join_paths(package_datadir, 'fru_master.json'),
    )
    conf_data.set_quoted(
        'ENTITY_MAP_JSON',
        join_paths(package_datadir, 'entityMap.json'),
    )

    conf_data.set_quoted(
        'EVENTS_JSONS_DIR',
        join_paths(package_datadir, 'events'),
    )
    conf_data.set('TERMINUS_ID', get_option('terminus-id'))
    conf_data.set('TERMINUS_HANDLE', get_option('terminus-handle'))
    add_project_arguments('-DLIBPLDMRESPONDER', language: 'cpp')
endif
conf_data.set('HEARTBEAT_TIMEOUT', get_option('heartbeat-timeout-seconds'))
conf_data.set('DBUS_TIMEOUT', get_option('dbus-timeout-value'))
if get_option('softoff').allowed()
    conf_data.set(
        'SOFTOFF_TIMEOUT_SECONDS',
        get_option('softoff-timeout-seconds'),
    )
    conf_data.set_quoted(
        'SOFTOFF_CONFIG_JSON',
        join_paths(package_datadir, 'softoff'),
    )
endif
if get_option('oem-ibm').allowed()
    conf_data.set_quoted(
        'FILE_TABLE_JSON',
        join_paths(package_datadir, 'fileTable.json'),
    )
    conf_data.set_quoted(
        'LID_RUNNING_DIR',
        '/var/lib/phosphor-software-manager/hostfw/running',
    )
    conf_data.set_quoted(
        'LID_ALTERNATE_DIR',
        '/var/lib/phosphor-software-manager/hostfw/alternate',
    )
    conf_data.set_quoted(
        'LID_STAGING_DIR',
        '/var/lib/phosphor-software-manager/hostfw/staging',
    )
    conf_data.set_quoted(
        'LID_RUNNING_PATCH_DIR',
        '/usr/local/share/hostfw/running',
    )
    conf_data.set_quoted(
        'LID_ALTERNATE_PATCH_DIR',
        '/usr/local/share/hostfw/alternate',
    )
    conf_data.set('DMA_MAXSIZE', get_option('oem-ibm-dma-maxsize'))
    add_project_arguments('-DOEM_IBM', language: 'cpp')
endif
conf_data.set(
    'NUMBER_OF_REQUEST_RETRIES',
    get_option('number-of-request-retries'),
)
conf_data.set(
    'INSTANCE_ID_EXPIRATION_INTERVAL',
    get_option('instance-id-expiration-interval'),
)
conf_data.set('RESPONSE_TIME_OUT', get_option('response-time-out'))
conf_data.set(
    'FLIGHT_RECORDER_MAX_ENTRIES',
    get_option('flightrecorder-max-entries'),
)
conf_data.set_quoted('HOST_EID_PATH', join_paths(package_datadir, 'host_eid'))
conf_data.set('MAXIMUM_TRANSFER_SIZE', get_option('maximum-transfer-size'))

conf_data.set('UPDATE_MODE_IDLE_TIMEOUT', get_option('update-mode-idle-timeout'))
conf_data.set_quoted(
    'FW_UPDATE_CONFIG_JSON',
    join_paths(package_datadir, 'fw_update_config.json'),
)
conf_data.set_quoted(
    'STATIC_EID_TABLE_PATH',
    join_paths(package_datadir, 'static_eid_table.json'),
)
conf_data.set_quoted(
    'PLDM_T2_CONFIG_JSON',
    join_paths(package_datadir, 'pldm_t2_config.json'),
)
conf_data.set_quoted(
    'PLDM_PACKAGE_VERIFICATION_KEY',
    get_option('pldm-package-verification-key'),
)
conf_data.set('LOCAL_EID_OVER_I2C', get_option('local-eid-over-i2c'))
conf_data.set('LOCAL_EID_OVER_PCIE', get_option('local-eid-over-pcie'))
conf_data.set(
    'STALE_SENSOR_UPPER_LIMITS_POLLING_TIME',
    get_option('stale-sensor-upper-limitms-polling-time'),
)

if get_option('transport-implementation') == 'mctp-demux'
    conf_data.set('PLDM_TRANSPORT_WITH_MCTP_DEMUX', 1)
elif get_option('transport-implementation') == 'af-mctp'
    conf_data.set('PLDM_TRANSPORT_WITH_AF_MCTP', 1)
endif

conf_data.set(
    'DEFAULT_SENSOR_UPDATER_INTERVAL',
    get_option('default-sensor-update-interval'),
)
conf_data.set('SENSOR_POLLING_TIME', get_option('sensor-polling-time'))
conf_data.set('UPDATE_TIMEOUT_SECONDS', get_option('update-timeout-seconds'))

if get_option('platform-prefix') != ''
    conf_data.set_quoted('PLATFORM_PREFIX', get_option('platform-prefix'))
endif
conf_data.set_quoted(
    'PLATFORM_CHASSIS_PATH',
    get_option('platform-chassis-object-path'),
)
if get_option('nvlink-c2c-fabric-object').enabled()
    add_project_arguments('-DNVLINK_C2C_FABRIC_OBJECT', language: ['c', 'cpp'])
endif
if get_option('nvidia-disable-edpp-effecters').enabled()
    conf_data.set('NVIDIA_DISABLE_EDPP_EFFECTERS', 1)
endif

conf_data.set('DEFAULT_RR_REFRESH_LIMIT_IN_MS', get_option('rr-refresh-limit'))
conf_data.set_quoted(
    'PLATFORM_CHASSIS_PATH',
    get_option('platform-chassis-object-path'),
)

configure_file(output: 'config.h', configuration: conf_data)

add_project_arguments(
    '-include',
    '@0@'.format(meson.current_build_dir() / 'config.h'),
    language: 'cpp',
)

filesystem = import('fs')

phosphor_dbus_interfaces = dependency('phosphor-dbus-interfaces')
sdbusplus = dependency('sdbusplus')
sdeventplus = dependency('sdeventplus')
stdplus = dependency('stdplus')
phosphor_logging_dep = dependency('phosphor-logging')

nlohmann_json_dep = dependency('nlohmann_json', include_type: 'system')

if cpp.has_header('CLI/CLI.hpp')
    CLI11_dep = declare_dependency()
else
    CLI11_dep = dependency('CLI11')
endif

if get_option('tests').allowed()
    add_project_arguments('-DPLDM_PACKAGE_VERIFICATION', language: ['c', 'cpp'])
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
                ],
            )
            gmock = gtest_proj.dependency('gmock')
        else
            assert(
                not get_option('tests').allowed(),
                'Googletest is required if tests are enabled',
            )
        endif
    endif
endif

libpldm_dep = dependency(
    'libpldm',
    fallback: ['libpldm', 'libpldm_dep'],
    include_type: 'system',
)

openssl = dependency('openssl', required: true)

if get_option('oem-ampere').allowed()
    add_project_arguments('-DOEM_AMPERE', language: 'cpp')
endif

libpldmutils_headers = ['.']
libpldmutils = library(
    'pldmutils',
    'common/transport.cpp',
    'common/utils.cpp',
    version: meson.project_version(),
    dependencies: [
        libpldm_dep,
        phosphor_dbus_interfaces,
        phosphor_logging_dep,
        nlohmann_json_dep,
        sdbusplus,
    ],
    install: true,
    include_directories: include_directories(libpldmutils_headers),
)

libpldmutils = declare_dependency(
    include_directories: include_directories(libpldmutils_headers),
    link_with: libpldmutils,
)

nvidia_tal = dependency('nvidia-tal', required: true)

deps = [
    libpldm_dep,
    libpldmutils,
    nlohmann_json_dep,
    sdbusplus,
    sdeventplus,
    phosphor_dbus_interfaces,
    phosphor_logging_dep,
    openssl,
    nvidia_tal,
]

oem_files = []
if get_option('oem-ampere').allowed()
    subdir('oem/ampere')
endif

if get_option('libpldmresponder').allowed()
    subdir('libpldmresponder')
    deps += [libpldmresponder_dep]
endif

source_files = [
    'pldmd/pldmd.cpp',
    'fw-update/activation.cpp',
    'fw-update/fw_update_utility.cpp',
    'fw-update/inventory_manager.cpp',
    'fw-update/package_parser.cpp',
    'fw-update/component_updater.cpp',
    'fw-update/device_updater.cpp',
    'fw-update/update.cpp',
    'fw-update/update_manager.cpp',
    'fw-update/other_device_update_manager.cpp',
    'fw-update/config.cpp',
    'fw-update/device_inventory.cpp',
    'fw-update/firmware_inventory.cpp',
    'fw-update/package_signature.cpp',
    oem_files,
    'requester/mctp_endpoint_discovery.cpp',
]

if get_option('pldm-type2').enabled()
    source_files += [
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
        'platform-mc/smbios_mdr.cpp',
        'pldmd/dbus_impl_pdr.cpp',
    ]

    if get_option('oem-nvidia').enabled()
        source_files += [
            'oem/nvidia/platform-mc/oem_nvidia.cpp',
            'oem/nvidia/platform-mc/derived_sensor/switchBandwidthSensor.cpp',
        ]
    endif

endif

if get_option('debug-token').enabled()
    source_files += ['fw-update/oem-nvidia/debug_token.cpp']
endif

executable(
    'pldmd',
    sources: source_files,
    implicit_include_directories: false,
    dependencies: deps,
    install: true,
    install_dir: get_option('bindir'),
    cpp_args: ['-fno-lto'],
)

if get_option('mockup-responder').enabled()
    subdir('mockup-responder')
endif

if get_option('systemd').allowed()
    systemd_system_unit_dir = dependency('systemd').get_variable(
        'systemd_system_unit_dir',
    )
    filesystem.copyfile(
        'pldmd/service_files/pldmd.service',
        'pldmd.service',
        install: true,
        install_dir: systemd_system_unit_dir,
    )

    if get_option('oem-ibm').allowed()
        subdir('oem/ibm/service_files')
    endif

    configure_file(
        input: 'pldmd/verbosity/verbosity',
        output: 'pldm_verbosity',
        configuration: conf_data,
        install: true,
        install_dir: join_paths(get_option('sysconfdir'), 'default'),
    )
endif

subdir('pldmtool')
subdir('configurations')

if get_option('utilities').allowed()
    subdir('utilities')
endif

if get_option('softoff').allowed()
    subdir('softoff')
endif

if get_option('tests').enabled()
    subdir('common/test')
    subdir('fw-update/test')
    subdir('requester/test')
    if get_option('libpldmresponder').enabled()
        # subdir('host-bmc/test')
        # subdir('platform-mc/test')
    endif
    subdir('test')
    if get_option('mockup-responder').enabled()
        subdir('mockup-responder/test')
    endif
endif
