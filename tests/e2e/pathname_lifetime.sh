#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose: Qualify libfuse hidden-file lifetime against a pathname-only backend.
# Usage: Run through make test-e2e in an AIX CMake binary directory.
set -u
. "${AIX_FUSE_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/aix-fuse-path-lifetime.$$
DAEMON=${AIX_FUSE_PATH_DAEMON:?}
DAEMON_PID=0
MOUNT=$BASE/mount
BACKING=$BASE/backing
DAEMON_LOG=$BASE/daemon.log
PROBE_LOG=$BASE/probe.log
MOUNT_WAIT_SECONDS=30
EXIT_WAIT_SECONDS=30
EXPECTED_CLEANUP_FAILURE=10

is_mounted()
{
    mount 2>/dev/null | grep -q " $MOUNT "
}

daemon_has_exited()
{
    ! kill -0 "$DAEMON_PID" 2>/dev/null
}

start_path_daemon()
{
    case_name=$1
    MOUNT=$BASE/$case_name/mount
    BACKING=$BASE/$case_name/backing
    DAEMON_LOG=$BASE/$case_name/daemon.log
    PROBE_LOG=$BASE/$case_name/probe.log
    mkdir -p "$MOUNT" "$BACKING"
    USFS_PATH_BACKING_DIRECTORY="$BACKING" "$DAEMON" -f "$MOUNT" >"$DAEMON_LOG" 2>&1 &
    DAEMON_PID=$!
    aix_fuse_track_pid "$DAEMON_PID"
    aix_fuse_track_mount "$MOUNT"
    aix_fuse_wait_until "pathname backend mount" "$MOUNT_WAIT_SECONDS" is_mounted
}

stop_path_daemon()
{
    expected=$1
    umount "$MOUNT" || return 1
    aix_fuse_wait_until "pathname backend exit" "$EXIT_WAIT_SECONDS" daemon_has_exited || return 1
    wait "$DAEMON_PID"
    result=$?
    aix_fuse_forget_pid "$DAEMON_PID"
    DAEMON_PID=0
    [ "$result" -eq "$expected" ]
}

cleanup_case()
{
    cleanup_mode=$1
    failure=0
    start_path_daemon "$cleanup_mode" || failure=1
    "$DAEMON" --cleanup-probe "$MOUNT" "$BACKING" >"$PROBE_LOG" 2>&1 || failure=1
    grep -q 'retained for shutdown retry' "$DAEMON_LOG" || failure=1
    if [ "$cleanup_mode" = transient ]; then
        rm -f "$BACKING/.fault-cleanup"
        stop_path_daemon 0 || failure=1
        remaining=$(find "$BACKING" -name '.fuse_hidden*' -print | wc -l)
        [ "$remaining" -eq 0 ] || failure=1
    else
        stop_path_daemon "$EXPECTED_CLEANUP_FAILURE" || failure=1
        grep -q 'shutdown cleanup unresolved' "$DAEMON_LOG" || failure=1
    fi
    grep 'Failed to remove owned hidden file' "$DAEMON_LOG" | sed 's/^/# /'
    [ "$failure" -eq 0 ] || cat "$PROBE_LOG" "$DAEMON_LOG"
    tap_ok "$failure" "$cleanup_mode cleanup failure is tracked, retried and reported without losing unrelated I/O"
}

cleanup()
{
    aix_fuse_owned_cleanup || return 1
    rm -rf "$BASE"
}

trap 'journey_result=$?; trap - 0; cleanup || journey_result=1; exit "$journey_result"' 0
trap 'exit 1' 1 2 15
tap_plan 3
failure=0
start_path_daemon normal || failure=1
"$DAEMON" --probe "$MOUNT" "$BACKING" >"$PROBE_LOG" 2>&1 || failure=1
stop_path_daemon 0 || failure=1
grep 'Failed to replace' "$DAEMON_LOG" | sed 's/^/# /'
[ "$failure" -eq 0 ] || cat "$PROBE_LOG" "$DAEMON_LOG"
tap_ok "$failure" "pathname backend passes unlink, replacement, identity, hard-link and rollback journeys"
cleanup_case transient
cleanup_case persistent
tap_finish
