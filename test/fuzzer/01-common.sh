function cleanup_containers {
    # check if the container exists, but its status is `exited`
    if [ "$( docker ps --all --quiet --filter ancestor=${IMAGE} --filter status=exited )" ]; then
        # clean it up
        docker rm --force $( docker stop $( docker ps --all --quiet --filter ancestor=${IMAGE} --format="{{.ID}}" ) )
    fi
}

PROJ_PATH=${SCRIPT_PATH}
while [[ "$(basename ${PROJ_PATH})" != "${PROJECT_NAME}" ]]; do
    PROJ_PATH=$(dirname ${PROJ_PATH})
done

