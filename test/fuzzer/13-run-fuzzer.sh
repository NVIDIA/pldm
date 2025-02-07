#!/bin/bash

SCRIPT_NAME=$(realpath "$0")
SCRIPT_PATH=$(dirname "${SCRIPT_NAME}")

pushd ${SCRIPT_PATH} &> /dev/null

source 00-projdefs.sh
source 01-common.sh

cleanup_containers

COMMAND="$@"
FUZZ_TIME=${DEFAULT_FUZZ_TIME}

if [[ ! -d ${PROJ_PATH}/${WORK_DIR}/${SEED_DIR} ]]; then
    mkdir -p ${PROJ_PATH}/${WORK_DIR}/${SEED_DIR}
    for i in $(seq 5); do
        dd if=/dev/urandom of=${PROJ_PATH}/${WORK_DIR}/${SEED_DIR}/seed_${i} bs=64 count=10;
    done
fi

eval "HCMD=\"${HOST_PREPARATION_COMMAND:-':'}\""
/bin/bash -c "${HCMD}"

eval "DCMD=\"${DOCKER_PREPARATION_COMMAND:-':'}\""
if [[ -z "$COMMAND" ]]; then
    docker run \
        --interactive --net=host --privileged --tty \
        --volume "${PROJ_PATH}:${PROJ_PATH}" \
        --workdir ${PROJ_PATH}/${WORK_DIR}/ \
        ${IMAGE} \
        ./${FUZZ_EXEC} -h
else
    OUTPUT_DIR=${PROJ_PATH}/aflOut$(date +%s)
    mkdir -p ${OUTPUT_DIR}
    realpath ${OUTPUT_DIR}
    docker run \
        --interactive --net=host --privileged --tty \
        --volume "${PROJ_PATH}:${PROJ_PATH}" \
        --workdir ${PROJ_PATH}/${WORK_DIR}/ --env AFL_SKIP_CPUFREQ=1 \
        ${IMAGE} \
        /bin/bash -c "${DCMD}; afl-fuzz -o ${OUTPUT_DIR} -i ./${SEED_DIR}/ -V ${FUZZ_TIME} -- ./${FUZZ_EXEC} ${COMMAND}"
fi

popd &> /dev/null

