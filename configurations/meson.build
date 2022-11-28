if get_option('libpldmresponder').enabled()
    install_subdir('pdr', install_dir: package_datadir)
    install_subdir('host', install_dir: package_datadir)
    install_subdir('events', install_dir: package_datadir)
endif

if get_option('oem-ibm').disabled()
    if get_option('libpldmresponder').enabled()
        install_data('fru_master.json', install_dir: package_datadir)
    endif
endif

if get_option('oem-ibm').enabled()
    install_subdir('../oem/ibm/configurations/fru', install_dir: package_datadir)
    install_subdir('../oem/ibm/configurations/events', install_dir: package_datadir)
    install_subdir('../oem/ibm/configurations/bios', install_dir: package_datadir)
    install_data('../oem/ibm/configurations/fru_master.json',install_dir: package_datadir)
    install_data('../oem/ibm/configurations/fileTable.json',
        install_dir: package_datadir)
    install_data('../oem/ibm/configurations/host_eid', install_dir: package_datadir)
endif

if get_option('oem-nvidia').enabled()
    install_data('../oem/nvidia/configurations/fw_update_config.json', install_dir: package_datadir)
endif

