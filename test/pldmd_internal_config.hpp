#pragma once

#ifdef BIOS_JSONS_DIR
#undef BIOS_JSONS_DIR
#endif
#define BIOS_JSONS_DIR "../libpldmresponder/test/bios_jsons"

#ifdef BIOS_TABLES_DIR
#undef BIOS_TABLES_DIR
#endif
#define BIOS_TABLES_DIR "pldmd_internal_bios_tables"

#ifdef PDR_JSONS_DIR
#undef PDR_JSONS_DIR
#endif
#define PDR_JSONS_DIR "../configurations/pdr"

#ifdef FRU_JSONS_DIR
#undef FRU_JSONS_DIR
#endif
#define FRU_JSONS_DIR "../libpldmresponder/test/fru_jsons/good"

#ifdef FRU_MASTER_JSON
#undef FRU_MASTER_JSON
#endif
#define FRU_MASTER_JSON                                                        \
    "../libpldmresponder/test/fru_jsons/fru_master/fru_master.json"

#ifdef EVENTS_JSONS_DIR
#undef EVENTS_JSONS_DIR
#endif
#define EVENTS_JSONS_DIR "../libpldmresponder/test/event_jsons/good"

#ifdef HOST_JSONS_DIR
#undef HOST_JSONS_DIR
#endif
#define HOST_JSONS_DIR "../configurations/host"

#ifdef HOST_EID_PATH
#undef HOST_EID_PATH
#endif
#define HOST_EID_PATH "pldmd_internal_host_eid"
