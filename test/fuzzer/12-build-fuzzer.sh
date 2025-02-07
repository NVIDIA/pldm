#!/bin/bash

SCRIPT_NAME=$(realpath "$0")
SCRIPT_PATH=$(dirname "${SCRIPT_NAME}")
PROJ_PATH=$(dirname "${SCRIPT_PATH}")

pushd ${SCRIPT_PATH} &> /dev/null

source 00-projdefs.sh
source 01-common.sh

cleanup_containers

rm -rf ${PROJ_PATH}/builddir

CXXFLAGS=${CXXFLAGS:-"-std=c++20 -Wno-maybe-uninitialized"}
CC=${CC:-"afl-gcc"}
CXX=${CXX:-"afl-g++"}

DOCKER_ENV=""
for ARG in "$@"; do
    DOCKER_ENV="${DOCKER_ENV} --env ${ARG}"
done

docker run \
    --interactive --net=host --privileged --tty \
    --volume "${PROJ_PATH}:${PROJ_PATH}" \
    --workdir ${PROJ_PATH} \
    --env CXXFLAGS="${CXXFLAGS}" --env CC="${CC}" --env CXX="${CXX}" ${DOCKER_ENV} \
    ${IMAGE} \
    /bin/bash -c "${BUILD_COMMAND}"

popd &> /dev/null

