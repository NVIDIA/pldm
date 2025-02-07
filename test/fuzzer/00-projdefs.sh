IMAGE=afl:pldm
CONTAINER=afl-pldm-container

PROJECT_NAME="pldm"

BUILD_COMMAND="meson builddir --wrap-mode nodownload -Dtests=disabled -Duse_fuzz=enabled -Dprefix=/usr/local && ninja -C builddir"

DEFAULT_FUZZ_TIME="1200"
WORK_DIR="builddir/test/fuzz"
SEED_DIR="seeds"
FUZZ_EXEC="pldmd_mockup"

HOST_PREPARATION_COMMAND='cp -f ${SCRIPT_PATH}/fuzz_test_fw_update_config.json ${PROJ_PATH}/${WORK_DIR}'
DOCKER_PREPARATION_COMMAND="service dbus restart"

