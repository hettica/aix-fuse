#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Prove that default fuse_main dispatch overlaps callbacks, honors its worker
#   bound, retains -s single-thread behavior, and shuts down cleanly.
#
# Usage:
#   Run through `make test-e2e` in the AIX CMake binary directory.
#   On a prepared AIX host: bash tests/e2e/multithread.sh

set -u

# This is an unattended gate; never let child utilities prompt on the SSH TTY.
exec </dev/null
. "${AIX_FUSE_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=${TMPDIR:-/tmp}/aix-fuse-mt.$$
MOUNT=$BASE/mount
STATUS=$BASE/status
DAEMON=${AIX_FUSE_MT_DAEMON:?}
DAEMON_PID=
FAILURES=0

say()
{
    echo "$*"
}

fail()
{
    FAILURES=$((FAILURES + 1))
    say "FAIL: $*"
}

is_mounted()
{
    mount 2>/dev/null | grep -q " $MOUNT "
}

wait_mounted()
{
    deadline=$(( $(date +%s) + 30 ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        is_mounted && return 0
        sleep 1
    done
    return 1
}

stop_daemon()
{
    [ -n "$DAEMON_PID" ] || return 0
    local stopped_pid=$DAEMON_PID
    local stop_result=0

    kill -TERM "$DAEMON_PID" 2>/dev/null || true
    deadline=$(( $(date +%s) + 30 ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if ! ps -p "$DAEMON_PID" >/dev/null 2>&1; then
            wait "$stopped_pid" 2>/dev/null || stop_result=$?
            DAEMON_PID=

            if [ "$stop_result" -ne 0 ]; then
                fail "daemon $stopped_pid returned status $stop_result during normal shutdown"
                return 1
            fi

            return 0
        fi
        sleep 1
    done
    fail "daemon $DAEMON_PID did not stop"
    return 1
}

cleanup()
{
    local cleanup_failed=0

    [ -n "${USFS_MT_HELD_READ:-}" ] && : >"$USFS_MT_HELD_READ"

    if ! stop_daemon; then
        if [ -n "$DAEMON_PID" ]; then
            kill -KILL "$DAEMON_PID" 2>/dev/null || true
            wait "$DAEMON_PID" 2>/dev/null || true
        fi

        cleanup_failed=1
    fi

    aix_fuse_owned_cleanup || cleanup_failed=1

    if is_mounted; then
        umount "$MOUNT" 2>/dev/null || umount -f "$MOUNT" 2>/dev/null || cleanup_failed=1
    fi

    if is_mounted; then
        cleanup_failed=1
    elif [ "$cleanup_failed" -eq 0 ]; then
        rm -rf "$BASE"
    fi

    return "$cleanup_failed"
}
trap 'journey_result=$?; trap - 0; cleanup || journey_result=1; exit "$journey_result"' 0
trap 'exit 1' 1 2 15

run_readers()
{
    count=$1
    pids=
    i=1
    while [ "$i" -le "$count" ]; do
        if [ "$((i % 2))" -eq 0 ]; then read_path=other; else read_path=slow; fi
        cat "$MOUNT/$read_path" >"$BASE/read.$i" 2>"$BASE/read.$i.err" &
        pids="$pids $!"
        aix_fuse_track_pid "$!"
        i=$((i + 1))
    done

    deadline=$(( $(date +%s) + 20 ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        alive=0
        for pid in $pids; do
            if ps -p "$pid" >/dev/null 2>&1; then
                alive=1
            fi
        done
        [ "$alive" -eq 0 ] && break
        sleep 1
    done

    for pid in $pids; do
        if ps -p "$pid" >/dev/null 2>&1; then
            kill -KILL "$pid" 2>/dev/null || true
            fail "reader $pid exceeded its deadline"
        fi
        wait "$pid" 2>/dev/null || fail "reader $pid failed"
        aix_fuse_forget_pid "$pid"
    done

    i=1
    while [ "$i" -le "$count" ]; do
        if [ "$(cat "$BASE/read.$i" 2>/dev/null)" != "parallel-dispatch" ]; then
            fail "reader $i returned incorrect content"
        fi
        i=$((i + 1))
    done
}

start_daemon()
{
    rm -f "$STATUS" "$BASE"/read.*
    USFS_MT_STATUS=$STATUS "$DAEMON" "$@" "$MOUNT" \
        >"$BASE/daemon.log" 2>&1 &
    DAEMON_PID=$!
    if ! wait_mounted; then
        fail "daemon $DAEMON_PID did not mount"
        return 1
    fi
}

mkdir -p "$MOUNT"
say "STEP bounded multithreaded dispatch"
export USFS_MT_READ_BARRIER=1
if start_daemon -f -o max_threads=2; then
    run_readers 4
    maximum=$(sed -n 's/^max_active=//p' "$STATUS" 2>/dev/null)
    collision=$(sed -n 's/^context_collision=//p' "$STATUS" 2>/dev/null)
    if [ "$maximum" = 2 ] && [ "$collision" = 0 ]; then
        say "OK   callbacks overlap with private contexts and respect max_threads=2"
    else
        fail "expected max_active=2/context_collision=0, got ${maximum:-missing}/${collision:-missing}"
    fi
    stop_daemon
    is_mounted && fail "multithreaded mount remained after daemon exit"
fi
unset USFS_MT_READ_BARRIER

say "STEP disk-backed large directory enumeration"
export USFS_MT_LARGE_DIRECTORY=1
if start_daemon -f -s; then
    if "${AIX_FUSE_IO_PROBE:?}" readdir-large "$MOUNT" >"$BASE/large-directory.out" 2>&1; then
        say "OK   large directory retains ordered cursors across native windows"
    else
        fail "large directory walk failed: $(cat "$BASE/large-directory.out")"
    fi
    stop_daemon
    callback_count=$(grep -c USFS_LARGE_READDIR_CALLBACK "$BASE/daemon.log")
    [ "$callback_count" -eq 1 ] || fail "large directory used $callback_count backend enumerations"
    is_mounted && fail "large directory mount remained after daemon exit"
fi
unset USFS_MT_LARGE_DIRECTORY

say "STEP explicit single-threaded dispatch"
if start_daemon -f -s; then
    run_readers 2
    maximum=$(sed -n 's/^max_active=//p' "$STATUS" 2>/dev/null)
    collision=$(sed -n 's/^context_collision=//p' "$STATUS" 2>/dev/null)
    if [ "$maximum" = 1 ] && [ "$collision" = 0 ]; then
        say "OK   -s keeps callbacks single-threaded"
    else
        fail "expected single-thread max_active=1, got ${maximum:-missing}"
    fi
    stop_daemon
    is_mounted && fail "single-threaded mount remained after daemon exit"
fi

say "STEP ancestor rename with an outstanding child operation"
if aix_fuse_namespace_case; then
    say "OK   held child observes the published ancestor rename"
else
    fail "ancestor rename exposed an inconsistent child path"
fi

say "STEP callback-requested exit with an idle reader"
export USFS_MT_EXIT_FROM_READ=1
if start_daemon -f -o max_threads=2; then
    cat "$MOUNT/slow" >"$BASE/exit-read" 2>"$BASE/exit-read.err" &
    reader=$!
    aix_fuse_track_pid "$reader"
    deadline=$(( $(date +%s) + 15 ))
    while ps -p "$DAEMON_PID" >/dev/null 2>&1 && [ "$(date +%s)" -lt "$deadline" ]; do
        sleep 1
    done
    if ps -p "$DAEMON_PID" >/dev/null 2>&1; then
        fail "callback exit did not wake the idle dispatch reader"
    else
        wait "$DAEMON_PID" || fail "callback exit reported failed cleanup"
        DAEMON_PID=
        is_mounted && fail "callback exit left its mount behind"
    fi
    stop_daemon
    if ps -p "$reader" >/dev/null 2>&1; then
        kill -KILL "$reader" 2>/dev/null || true
    fi
    wait "$reader" 2>/dev/null || true
    aix_fuse_forget_pid "$reader"
fi
unset USFS_MT_EXIT_FROM_READ

say "STEP thread-requested idle exit, repeated notifications, and exec isolation"
for mode in single multi; do
    extra=
    [ "$mode" = single ] && extra=-s
    USFS_MT_IDLE_EXIT=1 "$DAEMON" -f $extra "$MOUNT" >"$BASE/idle.$mode.log" 2>&1 &
    DAEMON_PID=$!
    deadline=$(( $(date +%s) + 20 ))
    while ps -p "$DAEMON_PID" >/dev/null 2>&1 && [ "$(date +%s)" -lt "$deadline" ]; do
        sleep 1
    done
    if ps -p "$DAEMON_PID" >/dev/null 2>&1; then
        fail "$mode idle fuse_exit did not finish"
        kill -KILL "$DAEMON_PID" 2>/dev/null || true
    fi
    wait "$DAEMON_PID" || fail "$mode idle exit or exec isolation failed"
    DAEMON_PID=
    grep -q USFS_IDLE_EXIT_AND_EXEC_ISOLATION_OK "$BASE/idle.$mode.log" ||
        fail "$mode exit did not complete destruction and helper validation"
    is_mounted && fail "$mode idle exit left its mount behind"
done

say "STEP exit with outstanding requests and held callbacks"
export USFS_MT_HELD_READ=$BASE/held.release
rm -f "$USFS_MT_HELD_READ"
if start_daemon -f -o max_threads=2; then
    held_readers=
    for path in slow other; do
        cat "$MOUNT/$path" >"$BASE/held.$path" 2>&1 &
        held_readers="$held_readers $!"
        aix_fuse_track_pid "$!"
    done
    deadline=$(( $(date +%s) + 10 ))
    while [ "$(grep -c USFS_CALLBACK_HELD "$BASE/daemon.log")" -lt 2 ] &&
          [ "$(date +%s)" -lt "$deadline" ]; do sleep 1; done
    [ "$(grep -c USFS_CALLBACK_HELD "$BASE/daemon.log")" -eq 2 ] || fail "both workers did not enter held callbacks"
    queued_readers=
    for index in 1 2 3 4 5 6 7 8 9 10 11 12; do
        "${AIX_FUSE_IO_PROBE:?}" stat "$MOUNT/pending-$index" >"$BASE/queued.$index" 2>&1 &
        queued_readers="$queued_readers $!"
        aix_fuse_track_pid "$!"
    done
    # Public filesystem callers remain pending while both callbacks are held.
    sleep 1
    for reader in $queued_readers; do
        kill -0 "$reader" 2>/dev/null || fail "metadata reader $reader completed while workers were held"
    done
    callbacks_before=$(grep -c USFS_GETATTR_ENTERED "$BASE/daemon.log")
    kill -TERM "$DAEMON_PID"
    sleep 2
    ps -p "$DAEMON_PID" >/dev/null 2>&1 || fail "exit freed a held callback"
    grep -q USFS_DESTROY_CALLED "$BASE/daemon.log" && fail "exit destroyed live callback state"
    touch "$USFS_MT_HELD_READ"
    deadline=$(( $(date +%s) + 20 ))
    while ps -p "$DAEMON_PID" >/dev/null 2>&1 && [ "$(date +%s)" -lt "$deadline" ]; do sleep 1; done
    if ps -p "$DAEMON_PID" >/dev/null 2>&1; then
        fail "held callback exit did not join completed callbacks"
        kill -KILL "$DAEMON_PID" 2>/dev/null || true
    fi
    wait "$DAEMON_PID" 2>/dev/null || fail "held callback daemon returned failure"
    DAEMON_PID=
    [ "$(grep -c USFS_GETATTR_ENTERED "$BASE/daemon.log")" -eq "$callbacks_before" ] ||
        fail "queued metadata callbacks ran after exit"
    [ "$(grep -c USFS_CALLBACK_HELD "$BASE/daemon.log")" -eq 2 ] || fail "new read callback ran after exit"
    [ "$(grep -c USFS_DESTROY_CALLED "$BASE/daemon.log")" -eq 1 ] || fail "held callback cleanup did not destroy exactly once"
    for reader in $held_readers $queued_readers; do
        kill -KILL "$reader" 2>/dev/null || true
        wait "$reader" 2>/dev/null || true
    done
    AIX_FUSE_TEST_PIDS=
    is_mounted && fail "held callback exit left a mount"
fi
unset USFS_MT_HELD_READ

say "STEP supervisor boundary for a callback that never returns"
export USFS_MT_STUCK_READ=1
if start_daemon -f -o max_threads=2; then
    stuck_readers=
    for index in 1 2 3 4 5 6 7 8; do
        cat "$MOUNT/slow" >"$BASE/stuck.$index" 2>&1 &
        stuck_readers="$stuck_readers $!"
        aix_fuse_track_pid "$!"
    done
    deadline=$(( $(date +%s) + 10 ))
    while ! grep -q USFS_CALLBACK_STUCK "$BASE/daemon.log" && [ "$(date +%s)" -lt "$deadline" ]; do sleep 1; done
    grep -q USFS_CALLBACK_STUCK "$BASE/daemon.log" || fail "stuck callback never started"
    kill -TERM "$DAEMON_PID"
    sleep 2
    ps -p "$DAEMON_PID" >/dev/null 2>&1 || fail "library terminated the owner of a live callback"
    grep -q USFS_DESTROY_CALLED "$BASE/daemon.log" && fail "library destroyed state still used by a callback"
    # The supervisor owns this deliberately stuck test process.
    kill -KILL "$DAEMON_PID" 2>/dev/null || true
    wait "$DAEMON_PID" 2>/dev/null || true
    DAEMON_PID=
    for reader in $stuck_readers; do
        kill -KILL "$reader" 2>/dev/null || true
        wait "$reader" 2>/dev/null || true
    done
    AIX_FUSE_TEST_PIDS=
    umount "$MOUNT" 2>/dev/null || umount -f "$MOUNT" 2>/dev/null || fail "stuck callback recovery left its mount"
fi
unset USFS_MT_STUCK_READ

say "STEP external ordinary unmount wakes idle single and multithreaded daemons"
export USFS_MT_EXTERNAL_UNMOUNT=1
for mode in single multi; do
    extra=
    [ "$mode" = single ] && extra=-s
    if start_daemon -f $extra; then
        rm -f "$BASE/cwd.ready"
        (cd "$MOUNT" && touch "$BASE/cwd.ready" && exec sleep 30) &
        holder=$!
        aix_fuse_track_pid "$holder"
        aix_fuse_wait_until "busy unmount holder" 10 test -f "$BASE/cwd.ready" || fail "$mode holder did not start"
        umount "$MOUNT" >"$BASE/busy.log" 2>&1 && fail "$mode busy unmount succeeded"
        is_mounted || fail "$mode busy unmount removed the mount"
        [ "$(cat "$MOUNT/slow" 2>/dev/null)" = parallel-dispatch ] || fail "$mode busy unmount stopped service"
        kill -TERM "$holder" 2>/dev/null || true
        wait "$holder" 2>/dev/null || true
        aix_fuse_forget_pid "$holder"
        umount "$MOUNT" >"$BASE/unmount.log" 2>&1 || fail "$mode ordinary unmount failed"
        deadline=$(( $(date +%s) + 15 ))
        while ps -p "$DAEMON_PID" >/dev/null 2>&1 && [ "$(date +%s)" -lt "$deadline" ]; do sleep 1; done
        if ps -p "$DAEMON_PID" >/dev/null 2>&1; then
            fail "$mode daemon did not exit after external unmount"
            stop_daemon
        else
            wait "$DAEMON_PID" || fail "$mode external unmount returned a fatal session result"
            DAEMON_PID=
        fi
        [ "$(grep -c USFS_DESTROY_CALLED "$BASE/daemon.log")" -eq 1 ] ||
            fail "$mode external unmount did not destroy exactly once"
    fi
done
unset USFS_MT_EXTERNAL_UNMOUNT

if [ "$FAILURES" -ne 0 ]; then
    say "multithreaded dispatch test failed: $FAILURES error(s)"
    exit 1
fi
say "multithreaded dispatch test passed"
exit 0
