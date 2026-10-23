#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT

# Shared TAP, bounded wait, and owned-resource helpers for adapter journeys.
# Tests are unattended even when their parent is an interactive SSH session.
# Utilities such as AIX rm otherwise read the inherited terminal for a
# confirmation while their prompt is commonly redirected into a test log.
exec </dev/null

AIX_FUSE_TAP_PLANNED=0
AIX_FUSE_TAP_EXECUTED=0
AIX_FUSE_TAP_FAILED=0
AIX_FUSE_TEST_PIDS=""
AIX_FUSE_TEST_MOUNTS=""

tap_plan()
{
    AIX_FUSE_TAP_PLANNED=$1
    echo "TAP version 13"
    echo "1..$1"
}

tap_diag()
{
    printf '# %s\n' "$*"
}

tap_ok()
{
    AIX_FUSE_TAP_EXECUTED=$((AIX_FUSE_TAP_EXECUTED + 1))
    if [ "$1" -eq 0 ]; then
        printf 'ok %d - %s\n' "$AIX_FUSE_TAP_EXECUTED" "$2"
    else
        printf 'not ok %d - %s\n' "$AIX_FUSE_TAP_EXECUTED" "$2"
        AIX_FUSE_TAP_FAILED=$((AIX_FUSE_TAP_FAILED + 1))
    fi
}

tap_is()
{
    if [ "$1" = "$2" ]; then
        tap_ok 0 "$3"
    else
        tap_diag "$3: actual='$1' expected='$2'"
        tap_ok 1 "$3"
    fi
}

tap_finish()
{
    if [ "$AIX_FUSE_TAP_EXECUTED" -ne "$AIX_FUSE_TAP_PLANNED" ]; then
        tap_diag "plan mismatch: planned=$AIX_FUSE_TAP_PLANNED executed=$AIX_FUSE_TAP_EXECUTED"
        return 2
    fi
    [ "$AIX_FUSE_TAP_FAILED" -eq 0 ]
}

aix_fuse_track_pid()
{
    AIX_FUSE_TEST_PIDS="$AIX_FUSE_TEST_PIDS $1"
}

aix_fuse_forget_pid()
{
    local retained_pids=
    local tracked_pid

    for tracked_pid in $AIX_FUSE_TEST_PIDS; do
        [ "$tracked_pid" = "$1" ] || retained_pids="$retained_pids $tracked_pid"
    done

    AIX_FUSE_TEST_PIDS=$retained_pids
}

aix_fuse_track_mount()
{
    AIX_FUSE_TEST_MOUNTS="$AIX_FUSE_TEST_MOUNTS $1"
}

aix_fuse_owned_cleanup()
{
    local cleanup_failed=0

    for pid in $AIX_FUSE_TEST_PIDS; do
        kill -TERM "$pid" 2>/dev/null || true
    done

    for pid in $AIX_FUSE_TEST_PIDS; do
        if ! aix_fuse_wait_until "owned process $pid" 10 aix_fuse_process_gone "$pid"; then
            kill -KILL "$pid" 2>/dev/null || true
            cleanup_failed=1
        fi

        wait "$pid" 2>/dev/null || true
    done

    for mnt in $AIX_FUSE_TEST_MOUNTS; do
        aix_fuse_mount_exists "$mnt" || continue
        umount "$mnt" 2>/dev/null || umount -f "$mnt" 2>/dev/null || cleanup_failed=1
        aix_fuse_mount_exists "$mnt" && cleanup_failed=1
    done

    return "$cleanup_failed"
}

aix_fuse_process_gone()
{
    ! kill -0 "$1" 2>/dev/null
}

aix_fuse_mount_exists()
{
    mount 2>/dev/null | awk -v path="$1" '$2 == path { found=1 } END { exit !found }'
}

# File-backed ownership survives the subshell used to collect a new mount's
# PID and pathname. Only this journey writes its registry.
aix_fuse_record_example()
{
    printf '%s %s\n' "$1" "$2" >>"$AIX_FUSE_DAEMON_FILE"
}

aix_fuse_forget_example()
{
    awk -v pid="$1" '$1 != pid' "$AIX_FUSE_DAEMON_FILE" >"$AIX_FUSE_DAEMON_FILE.next" &&
        mv "$AIX_FUSE_DAEMON_FILE.next" "$AIX_FUSE_DAEMON_FILE"
}

aix_fuse_stop_example()
{
    local example_pid=$1
    local example_mount=$2
    local stop_failed=0

    kill -TERM "$example_pid" 2>/dev/null || true

    if ! aix_fuse_wait_until "example process $example_pid" "$STOP_WAIT" aix_fuse_process_gone "$example_pid"; then
        kill -KILL "$example_pid" 2>/dev/null || true
        aix_fuse_wait_until "killed example process $example_pid" 10 aix_fuse_process_gone "$example_pid" || true
        stop_failed=1
    fi

    if aix_fuse_mount_exists "$example_mount"; then
        umount "$example_mount" 2>/dev/null || umount -f "$example_mount" 2>/dev/null || true
        stop_failed=1
    fi

    if aix_fuse_process_gone "$example_pid" && ! aix_fuse_mount_exists "$example_mount"; then
        aix_fuse_forget_example "$example_pid" || stop_failed=1
        rmdir "$example_mount" "/mnt/$example_pid" 2>/dev/null || true
    fi

    return "$stop_failed"
}

aix_fuse_examples_unmounted()
{
    local example_pid example_mount
    local remaining_mounts=0

    while read -r example_pid example_mount; do
        if aix_fuse_mount_exists "$example_mount"; then
            tap_diag "owned example mount remains: $example_mount"
            remaining_mounts=1
        fi
    done <"$AIX_FUSE_DAEMON_FILE"

    return "$remaining_mounts"
}

aix_fuse_example_snapshot()
{
    local example_pid example_mount
    local daemon_count=0
    local mount_count=0

    while read -r example_pid example_mount; do
        if ! aix_fuse_process_gone "$example_pid"; then
            daemon_count=$((daemon_count + 1))
        fi

        if aix_fuse_mount_exists "$example_mount"; then
            mount_count=$((mount_count + 1))
        fi
    done <"$AIX_FUSE_DAEMON_FILE"

    say "STATE owned_daemons=$daemon_count owned_mounts=$mount_count ($*)"
}

aix_fuse_cleanup_examples()
{
    local example_pid example_mount
    local cleanup_failed=0

    aix_fuse_owned_cleanup || cleanup_failed=1
    cp "$AIX_FUSE_DAEMON_FILE" "$AIX_FUSE_DAEMON_FILE.cleanup" || return 1

    while read -r example_pid example_mount; do
        aix_fuse_stop_example "$example_pid" "$example_mount" || cleanup_failed=1
    done <"$AIX_FUSE_DAEMON_FILE.cleanup"

    rm -f "$AIX_FUSE_DAEMON_FILE.cleanup"
    tap_diag "Journey logs: $WORK_ROOT"

    return "$cleanup_failed"
}

aix_fuse_wait_until()
{
    description=$1
    timeout=$2
    shift 2
    deadline=$(( $(date +%s) + timeout ))
    next_heartbeat=$(( $(date +%s) + 15 ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if "$@"; then
            return 0
        fi
        now=$(date +%s)
        if [ "$now" -ge "$next_heartbeat" ]; then
            tap_diag "heartbeat: waiting for $description"
            next_heartbeat=$((now + 15))
        fi
        sleep 1
    done
    tap_diag "timeout waiting for $description after ${timeout}s"
    return 1
}

# Pause a backend ancestor rename after mutation, then issue fstat through an
# already open child. The probe marks issuance before its ordinary fstat call;
# it must remain outstanding until the backend publishes the renamed path.
aix_fuse_namespace_case()
(
    ns_base="${TMPDIR:-/tmp}/aix-fuse-namespace.$$"
    ns_mount="$ns_base/mnt"
    ns_pids=""
    ns_mounted() { mount | grep -q " $ns_mount "; }
    ns_gone() { ! kill -0 "$1" 2>/dev/null; }
    ns_wait_owned()
    {
        local completed_pid=$1
        local retained_pids=
        local tracked_pid
        local result=0

        wait "$completed_pid" || result=$?

        for tracked_pid in $ns_pids; do
            [ "$tracked_pid" = "$completed_pid" ] || retained_pids="$retained_pids $tracked_pid"
        done

        ns_pids=$retained_pids
        return "$result"
    }

    ns_cleanup()
    {
        ns_cleanup_status=0
        # Release the callback before stopping its worker, including failures
        # before the normal publication step. A regular file never blocks here.
        if [ -d "$ns_base" ]; then
            : >"$ns_base/rename.release" || ns_cleanup_status=1
        fi

        for ns_pid in $ns_pids; do kill -TERM "$ns_pid" 2>/dev/null || true; done
        for ns_pid in $ns_pids; do
            if ! aix_fuse_wait_until "namespace process cleanup" 5 ns_gone "$ns_pid"; then
                kill -KILL "$ns_pid" 2>/dev/null || true
                ns_cleanup_status=1
            fi
            wait "$ns_pid" 2>/dev/null || true
        done
        if ns_mounted; then
            umount "$ns_mount" || umount -f "$ns_mount" || ns_cleanup_status=1
        fi
        if ns_mounted; then ns_cleanup_status=1; else rm -rf "$ns_base"; fi
        return "$ns_cleanup_status"
    }
    trap 'ns_status=$?; trap - 0; if [ "$ns_status" -ne 0 ]; then cat "$ns_base"/*.log 2>/dev/null; fi; ns_cleanup || ns_status=1; exit "$ns_status"' 0
    trap 'exit 1' 1 2 15
    mkdir -p "$ns_mount" || exit 1
    mkfifo "$ns_base/stat" || exit 1
    USFS_MT_NAMESPACE_RELEASE="$ns_base/rename.release" "${AIX_FUSE_MT_DAEMON:?}" \
        -f -o max_threads=4 "$ns_mount" >"$ns_base/daemon.log" 2>&1 &
    ns_daemon=$!; ns_pids="$ns_daemon"
    aix_fuse_wait_until "namespace mount" 30 ns_mounted || exit 1
    "${AIX_FUSE_IO_PROBE:?}" stat-held "$ns_mount/old/file" "$ns_base/ready" \
        "$ns_base/stat" >"$ns_base/stat.log" 2>&1 &
    ns_holder=$!; ns_pids="$ns_pids $ns_holder"
    aix_fuse_wait_until "held child descriptor" 15 test -f "$ns_base/ready" || exit 1
    "${AIX_FUSE_IO_PROBE:?}" rename "$ns_mount/old" "$ns_mount/new" >"$ns_base/rename.log" 2>&1 &
    ns_rename=$!; ns_pids="$ns_pids $ns_rename"
    aix_fuse_wait_until "backend rename commit" 15 grep -q USFS_RENAME_COMMITTED "$ns_base/daemon.log" || exit 1

    # Exercise a background sync while the ancestor rename is held.
    sync &
    ns_sync=$!; ns_pids="$ns_pids $ns_sync"

    printf x >"$ns_base/stat" &
    ns_command=$!; ns_pids="$ns_pids $ns_command"
    aix_fuse_wait_until "held stat command" 10 ns_gone "$ns_command" || exit 1
    ns_wait_owned "$ns_command" || exit 1
    aix_fuse_wait_until "held stat issuance" 10 grep -q '^issued$' "$ns_base/ready" || exit 1
    sleep 1
    kill -0 "$ns_holder" 2>/dev/null || exit 1
    grep -q '^held-stat=' "$ns_base/stat.log" && exit 1
    : >"$ns_base/rename.release" || exit 1
    aix_fuse_wait_until "ancestor rename completion" 15 ns_gone "$ns_rename" || exit 1
    ns_wait_owned "$ns_rename" || exit 1
    aix_fuse_wait_until "held child stat completion" 15 ns_gone "$ns_holder" || exit 1
    ns_wait_owned "$ns_holder" || exit 1
    aix_fuse_wait_until "concurrent sync completion" 15 ns_gone "$ns_sync" || exit 1
    ns_wait_owned "$ns_sync" || exit 1
    grep -q '^held-stat=valid$' "$ns_base/stat.log" || exit 1
    grep -q '^NAMESPACE_GETATTR /new/file valid=1$' "$ns_base/daemon.log" || exit 1
    kill -TERM "$ns_daemon" || exit 1
    aix_fuse_wait_until "namespace daemon exit" 15 ns_gone "$ns_daemon" || exit 1
    ns_wait_owned "$ns_daemon" || exit 1
    ! ns_mounted
)

