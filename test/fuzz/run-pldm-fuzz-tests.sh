BUILD_TYPES=( \
    "AFL_USE_ASAN=1" \
    "AFL_USE_TSAN=1" \
    "AFL_USE_UBSAN=1" \
    )

COMMANDS=( \
    "-b" \
    "-i" \
    "-a" \
    )

for TYPE in ${BUILD_TYPES[@]}; do
    ./fuzzer/12-build-fuzzer.sh ${TYPE}
    for CMD in ${COMMANDS[@]}; do
        ./fuzzer/13-run-fuzzer.sh ${CMD}
    done
done

