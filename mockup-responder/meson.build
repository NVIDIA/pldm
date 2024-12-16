configure_file(output: '..config.h',
  configuration: conf_data
)


pldm_headers = [
  '.',
  '..',
  '../libpldm',
  '../common',
  '../fw-update',
  '../libpldmresponder'
]


deps = [
  sdbusplus,
  sdeventplus,
  phosphor_dbus_interfaces,  
  phosphor_logging
] 

sources_mockup_responder = [
    '../fw-update/config.cpp',
    '../libpldm/requester/pldm.c',
    '../libpldm/base.c',
    '../fw-update/../common/utils.cpp',
    '../libpldmresponder/base.cpp',
    '../pldmd/handler.hpp',
    '../libpldm/platform.c',
    '../libpldm/pdr.c',
    'pldm_mockup_responder.cpp',
    'mockup_responder.cpp',
    'pdr_json_parser.cpp',
    'sensor_to_dbus.cpp',
    '../pldmd/dbus_impl_requester.cpp',
    '../pldmd/instance_id.cpp'
]

executable(
  'pldm_mockup_responder',
  sources: sources_mockup_responder,
  implicit_include_directories: false,
  include_directories: include_directories(pldm_headers),
  link_args: dynamic_linker,
  build_rpath: get_option('oe-sdk').enabled() ? rpath : '',
  dependencies: deps,
  install: true
)