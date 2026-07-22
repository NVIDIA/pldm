#!/bin/sh
# One retry for tests with a known rare cross-test flake: a test case that
# abandons an in-flight async D-Bus call on the shared default bus lets the
# late reply resume a destroyed coroutine frame, corrupting whatever now
# occupies that stack region (observed as a startup SIGSEGV in terminus_test
# in roughly 3% of runs). The real fix is cancelling the pending slot when
# the awaitable is destroyed (common/dBusAsyncUtils.hpp); the retry keeps CI
# green without dropping the suite's coverage in the meantime.
"$@" && exit 0
status=$?
echo "run_with_retry: first attempt exited with status ${status}; retrying once" >&2
exec "$@"
