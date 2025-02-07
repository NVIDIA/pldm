#!/bin/bash

SCRIPT_NAME=$(realpath "$0")
SCRIPT_PATH=$(dirname "${SCRIPT_NAME}")
PROJ_PATH=$(dirname "${SCRIPT_PATH}")

pushd ${SCRIPT_PATH} &> /dev/null

source 00-projdefs.sh
source 01-common.sh

cleanup_containers

eval "HCMD=\"${HOST_PREPARATION_COMMAND:-':'}\""
/bin/bash -c "${HCMD}"

eval "DCMD=\"${DOCKER_PREPARATION_COMMAND:-':'}\""
docker run \
    --interactive --net=host --privileged --tty \
    --volume "${PROJ_PATH}:${PROJ_PATH}" \
    --workdir ${PROJ_PATH}/${WORK_DIR}/ \
    ${IMAGE} \
    /bin/bash -c "${DCMD}; /bin/bash"

popd &> /dev/null

