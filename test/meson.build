test_src = declare_dependency(
          sources: [
            '../pldmd/instance_id.cpp'])

tests = [
  'pldmd_instanceid_test',
  'pldmd_registration_test',
]

foreach t : tests
  test(t, executable(t.underscorify(), t + '.cpp',
                     implicit_include_directories: false,
                     dependencies: [
                         libpldm_dep,
                         libpldmutils,
                         nlohmann_json,
                         gtest,
                         test_src]),
       workdir: meson.current_source_dir())
endforeach
