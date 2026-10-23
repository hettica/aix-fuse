#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Start usfs_memfs daemons, run a POSIX filesystem workout covering
#   namespace, data, attributes, capacity, and concurrency, then verify clean
#   shutdown.
#
# Each daemon mounts at /mnt/<pid>/memfs and holds everything in its own RAM,
# so the file system is expected to be empty at mount and gone after exit.
#
# Usage:
#   Run through `make test-e2e` in the AIX CMake binary directory.
#   On a prepared AIX VM:
#     MEMFS_PHASES=rw,truncate SIZE_MB=16 bash tests/e2e/memfs.sh
#
# Every step prints a timestamped progress line, and every wait is bounded by a
# deadline that reports TIMEOUT rather than blocking forever.
#
# Environment knobs:
#   MEMFS_PHASES  comma-separated phases, or "all"        (default all)
#                 createatomic tree rw openhandles durability mmap truncate rename link symlink attrs openunlink
#                 nospc inodecap notempty df concurrent shutdown readonly
#   SIZE_MB       capacity of the file system             (default 8)
#   N_DAEMONS     concurrent daemons in the shutdown phase (default 2)
#   ITERATIONS    shutdown iterations                     (default 2)
#   N_WRITERS     concurrent writers                      (default 2)
#   MOUNT_WAIT / READ_WAIT / STOP_WAIT                    (90 / 240 / 60)
#   LOGFILE       progress log            (default: journey-owned temporary directory)
#
# A small SIZE_MB keeps the capacity phase quick on a slow VM.

set -u

# This is an unattended gate; never let child utilities prompt on the SSH TTY.
exec </dev/null
. "${AIX_FUSE_SOURCE_DIR:?}/tests/helpers/aix.sh"
: "${AIX_FUSE_IO_PROBE:?}"
WORK_ROOT=${TMPDIR:-/tmp}/aix-fuse-memfs.$$
(umask 077; mkdir "$WORK_ROOT") || exit 1
chmod 755 "$WORK_ROOT" || exit 1
AIX_FUSE_DAEMON_FILE=$WORK_ROOT/daemons
: >"$AIX_FUSE_DAEMON_FILE"
trap 'journey_result=$?; trap - 0; aix_fuse_cleanup_examples || journey_result=1; exit "$journey_result"' 0
trap 'exit 1' 1 2 15

MEMFS_PHASES=${MEMFS_PHASES:-all}
SIZE_MB=${SIZE_MB:-8}
N_DAEMONS=${N_DAEMONS:-2}
ITERATIONS=${ITERATIONS:-2}
N_WRITERS=${N_WRITERS:-2}
MOUNT_WAIT=${MOUNT_WAIT:-90}
READ_WAIT=${READ_WAIT:-240}
STOP_WAIT=${STOP_WAIT:-60}
LOGFILE=${LOGFILE:-$WORK_ROOT/journey.log}

DAEMON=${AIX_FUSE_MEMFS:?}
START=$(date +%s)
failures=0

: > "$LOGFILE"

say()
{
    now=$(date +%s)
    printf '[T+%04d] %s\n' "$((now - START))" "$*" | tee -a "$LOGFILE"
}

fail()
{
    failures=$((failures + 1))
    say "FAIL: $*"
}

snapshot()
{
    aix_fuse_example_snapshot "$*"
}

# Matches whole comma-separated names only. A substring test would let
# "openunlink" quietly switch on "link" as well, and the extra phase would then
# be reported as a failure of a selection that never asked for it.
phase_enabled()
{
    case "$MEMFS_PHASES" in
        all) return 0 ;;
    esac

    for _p in $(echo "$MEMFS_PHASES" | tr ',' ' '); do
        if [ "$_p" = "$1" ]; then
            return 0
        fi
    done

    return 1
}

is_mounted()
{
    mount 2>/dev/null | awk '$3 == "usfs" { print $2 }' | grep -qx "$1"
}

wait_mounted()
{
    _deadline=$(( $(date +%s) + $2 ))
    while [ "$(date +%s)" -lt "$_deadline" ]; do
        if is_mounted "$1"; then
            return 0
        fi
        sleep 2
    done
    return 1
}

wait_gone()
{
    _deadline=$(( $(date +%s) + $2 ))
    while [ "$(date +%s)" -lt "$_deadline" ]; do
        if ! ps -p "$1" >/dev/null 2>&1; then
            return 0
        fi
        sleep 2
    done
    return 1
}

# Starts a daemon. Echoes "<pid> <mountpoint>" on success.
start_memfs()
{
    "$DAEMON" -f --size="$SIZE_MB" "$@" </dev/null >>"$LOGFILE" 2>&1 &
    _pid=$!
    _mnt=/mnt/$_pid/memfs
    aix_fuse_record_example "$_pid" "$_mnt"

    if wait_mounted "$_mnt" "$MOUNT_WAIT"; then
        echo "$_pid $_mnt"
        return 0
    fi

    return 1
}

start_memfs_with_inode_limit()
{
    _limit=$1
    "$DAEMON" -f --size="$SIZE_MB" --inodes="$_limit" </dev/null >>"$LOGFILE" 2>&1 &
    _pid=$!
    _mnt=/mnt/$_pid/memfs
    aix_fuse_record_example "$_pid" "$_mnt"
    if wait_mounted "$_mnt" "$MOUNT_WAIT"; then
        echo "$_pid $_mnt"
        return 0
    fi
    return 1
}

stop_memfs()
{
    aix_fuse_stop_example "$1" "$2" || fail "daemon $1 or mount $2 did not stop cleanly"
}

# ---------------------------------------------------------------------------
# Phases
# ---------------------------------------------------------------------------

phase_tree()
{
    say "STEP tree: the file system starts empty"

    entries=$(ls -A "$1" 2>/dev/null | wc -l | tr -d ' ')
    if [ "$entries" -eq 0 ]; then
        say "OK   root is empty"
    else
        fail "root is not empty at mount ($entries entries)"
    fi

    say "STEP tree: creating directories and files"

    if mkdir -p "$1/a/b/c" 2>>"$LOGFILE"; then
        say "OK   mkdir -p a/b/c"
    else
        fail "mkdir -p failed"
        return
    fi

    if touch "$1/a/file1" "$1/a/b/file2" 2>>"$LOGFILE"; then
        say "OK   created files"
    else
        fail "touch failed"
    fi

    got=$( (cd "$1" && find . | sort | tr '\n' ' ') 2>/dev/null)
    want=". ./a ./a/b ./a/b/c ./a/b/file2 ./a/file1 "
    if [ "$got" = "$want" ]; then
        say "OK   tree matches: $got"
    else
        fail "tree is '$got', expected '$want'"
    fi
}

phase_rw()
{
    if "${AIX_FUSE_IO_PROBE:?}" mapped-append-records "$1" >>"$LOGFILE" 2>&1; then
        say "OK   mapped concurrent append and truncate preserve complete records"
    else
        fail "mapped concurrent append or truncate lost data"
    fi
    if "${AIX_FUSE_IO_PROBE:?}" append-records "$1" >>"$LOGFILE" 2>&1; then
        say "OK   concurrent append records stay contiguous through hard-link aliases"
    else
        fail "append split records, lost data, or selected a replacement object"
    fi
    say "STEP rw: write and read back"

    echo "hello memfs" > "$1/greeting" 2>>"$LOGFILE"
    got=$(cat "$1/greeting" 2>/dev/null)
    if [ "$got" = "hello memfs" ]; then
        say "OK   small file round trip"
    else
        fail "small file read back as '$got'"
    fi

    say "STEP rw: appending"
    echo "second line" >> "$1/greeting" 2>>"$LOGFILE"
    got=$(wc -l < "$1/greeting" 2>/dev/null | tr -d ' ')
    if [ "$got" = "2" ]; then
        say "OK   append produced 2 lines"
    else
        fail "append produced $got lines"
    fi

    # A 256 KiB fixture exercises several transfer windows.
    say "STEP rw: file larger than one transfer chunk"
    i=0
    : > $WORK_ROOT/big.src
    while [ $i -lt 200 ]; do
        dd if=/dev/zero bs=1024 count=1 2>/dev/null | tr '\0' 'z' >> $WORK_ROOT/big.src
        i=$((i + 1))
    done

    cp $WORK_ROOT/big.src "$1/big.bin" 2>>"$LOGFILE"
    want=$(cksum < $WORK_ROOT/big.src)
    got=$(cat "$1/big.bin" 2>/dev/null | cksum)

    if [ "$want" = "$got" ]; then
        say "OK   204800-byte file round trip ($got)"
    else
        fail "large file differs: got '$got', want '$want'"
    fi

    rm -f $WORK_ROOT/big.src
}

phase_openhandles()
{
    say "STEP openhandles: simultaneous read-only and write-only descriptions"

    : > "$1/per-open" 2>>"$LOGFILE"
    got=$("${AIX_FUSE_IO_PROBE:?}" dual-open "$1/per-open" 2>>"$LOGFILE")
    content=$(cat "$1/per-open" 2>/dev/null)
    if echo "$got" | grep -q "result=0" &&
       echo "$got" | grep -q "first=80" && [ "$content" = "PQ" ]; then
        say "OK   distinct handles preserve modes and survive independent close"
    else
        fail "per-open routing failed: probe='$got' content='$content'"
    fi
}

phase_createatomic()
{
    say "STEP createatomic: creation preserves access modes and failed create leaves no name"

    if "${AIX_FUSE_IO_PROBE:?}" create-flags "$1" >>"$LOGFILE" 2>&1; then
        say "OK   creation preserves access modes and rejects unsupported flags before publication"
    else
        fail "creation changed access modes or published an unsupported request"
    fi

    rm -f "$1/create-ro" "$1/create-wo" "$1/create-rw" "$1/unsupported"
}

phase_durability()
{
    say "STEP durability: flush, file sync, range sync, and filesystem sync"

    echo "durability" > "$1/durable" 2>>"$LOGFILE"

    got=$("${AIX_FUSE_IO_PROBE:?}" fsync "$1/durable" 2>>"$LOGFILE")
    if echo "$got" | grep -q "result=0 errno=0"; then
        say "OK   whole-file fsync completed through the memfs callback"
    else
        fail "whole-file fsync failed: $got"
    fi

    got=$("${AIX_FUSE_IO_PROBE:?}" fsync-range "$1/durable" 2>>"$LOGFILE")
    if echo "$got" | grep -q "result=0 errno=0"; then
        say "OK   range fsync completed through the memfs callback"
    else
        fail "range fsync failed: $got"
    fi

    got=$("${AIX_FUSE_IO_PROBE:?}" close "$1/durable" 2>>"$LOGFILE")
    if echo "$got" | grep -q "result=0 errno=0"; then
        say "OK   close-time flush completed through the memfs callback"
    else
        fail "close-time flush failed: $got"
    fi

    got=$("${AIX_FUSE_IO_PROBE:?}" syncfs "$1" 2>>"$LOGFILE")
    if echo "$got" | grep -q "result=0 errno=0"; then
        say "OK   filesystem sync completed through the memfs callback"
    else
        fail "filesystem sync failed: $got"
    fi
}

phase_mmap()
{
    say "STEP mmap: read, shared-write, and private-write mappings"

    : > "$1/mapped.bin"
    "${AIX_FUSE_IO_PROBE:?}" write "$1/mapped.bin" 8192 0 >>"$LOGFILE" 2>&1
    got=$("${AIX_FUSE_IO_PROBE:?}" mmap-read "$1/mapped.bin" 8192 0 \
        2>>"$LOGFILE")

    if echo "$got" | grep -q "result=0 errno=0 pattern=1"; then
        say "OK   mmap read returned the complete file after close"
    else
        fail "mmap read failed: $got"
    fi

    got=$("${AIX_FUSE_IO_PROBE:?}" mmap-write-shared \
        "$1/mapped.bin" 8192 0 2>>"$LOGFILE")
    if echo "$got" | grep -q "result=0 errno=0 source=1"; then
        say "OK   shared mmap changes persisted after close and msync"
    else
        fail "shared mmap write failed: $got"
    fi

    "${AIX_FUSE_IO_PROBE:?}" write "$1/mapped.bin" 8192 0 >>"$LOGFILE" 2>&1
    got=$("${AIX_FUSE_IO_PROBE:?}" mmap-write-private \
        "$1/mapped.bin" 8192 0 2>>"$LOGFILE")
    if echo "$got" | grep -q "result=0 errno=0 source=1"; then
        say "OK   private mmap changes remained isolated from the file"
    else
        fail "private mmap write failed: $got"
    fi

    chmod 444 "$1/mapped.bin"
    got=$(su nobody -c "\"${AIX_FUSE_IO_PROBE:?}\" mmap-write-private-readonly \
        '$1/mapped.bin' 4096 0" 2>>"$LOGFILE")
    if echo "$got" | grep -q "result=0 errno=0 source=1"; then
        say "OK   writable private mapping needs only source read access"
    else
        fail "read-only descriptor private mmap failed: $got"
    fi
    got=$(su nobody -c "\"${AIX_FUSE_IO_PROBE:?}\" mmap-write-shared-readonly \
        '$1/mapped.bin' 4096 0" 2>>"$LOGFILE")
    if echo "$got" | grep -q "result=-1 errno=[1-9]"; then
        say "OK   writable shared mapping rejects a read-only descriptor"
    else
        fail "read-only descriptor shared mmap was not rejected: $got"
    fi
    chmod 644 "$1/mapped.bin"

    say "STEP mmap: sustained multi-page page-in and page-out"
    : > "$1/mapped-heavy.bin"
    i=0
    while [ "$i" -lt 8 ]; do
        "${AIX_FUSE_IO_PROBE:?}" pwrite "$1/mapped-heavy.bin" 262144 \
            "$((i * 262144))" >>"$LOGFILE" 2>&1
        i=$((i + 1))
    done
    got=$("${AIX_FUSE_IO_PROBE:?}" mmap-read \
        "$1/mapped-heavy.bin" 2097152 0 2>>"$LOGFILE")
    if echo "$got" | grep -q "result=0 errno=0 pattern=1"; then
        say "OK   two-megabyte mapping paged in with intact contents"
    else
        fail "sustained mapped read failed: $got"
    fi
    got=$("${AIX_FUSE_IO_PROBE:?}" mmap-write-shared \
        "$1/mapped-heavy.bin" 2097152 0 2>>"$LOGFILE")
    if echo "$got" | grep -q "result=0 errno=0 source=1"; then
        say "OK   two-megabyte dirty mapping flushed completely"
    else
        fail "sustained mapped write failed: $got"
    fi

    say "STEP mmap: partial pages, EOF, and mapping lifecycle"

    : > "$1/mapped-tail.bin"
    "${AIX_FUSE_IO_PROBE:?}" write "$1/mapped-tail.bin" 5000 0 \
        >>"$LOGFILE" 2>&1
    got=$("${AIX_FUSE_IO_PROBE:?}" mmap-tail \
        "$1/mapped-tail.bin" 5000 8192 2>>"$LOGFILE")
    if echo "$got" | grep -q "result=0 pattern=1 zero=1 signal=0"; then
        say "OK   final partial page is zero-filled past logical EOF"
    else
        fail "partial-page mmap failed: $got"
    fi

    : > "$1/mapped-unlink.bin"
    "${AIX_FUSE_IO_PROBE:?}" write "$1/mapped-unlink.bin" 8192 0 \
        >>"$LOGFILE" 2>&1
    got=$("${AIX_FUSE_IO_PROBE:?}" mmap-unlink \
        "$1/mapped-unlink.bin" 8192 2>>"$LOGFILE")
    if echo "$got" | grep -q "result=0 pattern=1 signal=0 absent=1"; then
        say "OK   mapping survives descriptor close and unlink"
    else
        fail "unlinked mapping failed: $got"
    fi

    : > "$1/mapped-fork.bin"
    "${AIX_FUSE_IO_PROBE:?}" write "$1/mapped-fork.bin" 8192 0 \
        >>"$LOGFILE" 2>&1
    got=$("${AIX_FUSE_IO_PROBE:?}" mmap-fork \
        "$1/mapped-fork.bin" 8192 2>>"$LOGFILE")
    if echo "$got" | grep -q "result=0 exited=1 status=0"; then
        say "OK   inherited mapping is readable after fork"
    else
        fail "forked mapping failed: $got"
    fi

    got=$("${AIX_FUSE_IO_PROBE:?}" mmap-repeat \
        "$1/mapped-fork.bin" 8192 200 2>>"$LOGFILE")
    if echo "$got" | grep -q "result=0 completed=200 signal=0"; then
        say "OK   200 map/fault/unmap cycles completed"
    else
        fail "repeated mapping failed: $got"
    fi

    got=$("${AIX_FUSE_IO_PROBE:?}" mmap-touch-read \
        "$1/mapped-fork.bin" 4096 1 0 2>>"$LOGFILE")
    if echo "$got" | grep -q "result=-1 errno=[1-9]"; then
        say "OK   unaligned file offset was rejected"
    else
        fail "unaligned mmap was not rejected: $got"
    fi

    got=$("${AIX_FUSE_IO_PROBE:?}" mmap-touch-read \
        "$1/mapped-fork.bin" 12288 0 9000 2>>"$LOGFILE")
    if echo "$got" | grep -q "result=-1 errno=0 signal=[1-9]"; then
        say "OK   access wholly beyond EOF raised a process-local fault"
    else
        fail "beyond-EOF mmap access was unsafe: $got"
    fi

    : > "$1/mapped-truncate.bin"
    "${AIX_FUSE_IO_PROBE:?}" write "$1/mapped-truncate.bin" 8192 0 \
        >>"$LOGFILE" 2>&1
    got=$("${AIX_FUSE_IO_PROBE:?}" mmap-truncate \
        "$1/mapped-truncate.bin" 8192 4096 2>>"$LOGFILE")
    if echo "$got" | grep -q "result=0 fault=1 .* size=4096"; then
        say "OK   truncate invalidated mapped pages beyond new EOF"
    else
        fail "truncate-with-mapping failed: $got"
    fi

    say "STEP mmap: concurrent page faults and teardown"
    mmap_pids=""
    i=0
    while [ "$i" -lt 8 ]; do
        "${AIX_FUSE_IO_PROBE:?}" mmap-repeat \
            "$1/mapped-fork.bin" 8192 25 \
            >"$WORK_ROOT/mmap-repeat.${i}" 2>>"$LOGFILE" &
        mmap_pids="$mmap_pids $!"
        aix_fuse_track_pid "$!"
        i=$((i + 1))
    done
    concurrent_failure=0
    for mmap_pid in $mmap_pids; do
        if ! aix_fuse_wait_until "mapping worker $mmap_pid" "$READ_WAIT" aix_fuse_process_gone "$mmap_pid"; then
            kill -KILL "$mmap_pid" 2>/dev/null || true
            concurrent_failure=1
        fi

        if ! wait "$mmap_pid"; then
            concurrent_failure=1
        fi

        aix_fuse_forget_pid "$mmap_pid"
    done
    i=0
    while [ "$i" -lt 8 ]; do
        if ! grep -q "result=0 completed=25 signal=0" \
            "$WORK_ROOT/mmap-repeat.${i}"; then
            concurrent_failure=1
        fi
        rm -f "$WORK_ROOT/mmap-repeat.${i}"
        i=$((i + 1))
    done
    if [ "$concurrent_failure" -eq 0 ]; then
        say "OK   eight concurrent mapping loops completed cleanly"
    else
        fail "concurrent mapping loops failed"
    fi
}

phase_truncate()
{
    say "STEP truncate: shrink and extend"

    printf '0123456789' > "$1/trunc.txt" 2>>"$LOGFILE"

    # Shrink to 4 bytes.
    if command -v truncate >/dev/null 2>&1; then
        truncate -s 4 "$1/trunc.txt" 2>>"$LOGFILE"
    else
        # No truncate(1) on AIX; use a shell redirect through dd.
        dd if="$1/trunc.txt" of=$WORK_ROOT/truncate.tmp bs=1 count=4 2>/dev/null
        cp $WORK_ROOT/truncate.tmp "$1/trunc.txt" 2>/dev/null
        rm -f $WORK_ROOT/truncate.tmp
    fi

    got=$(cat "$1/trunc.txt" 2>/dev/null)
    size=$(ls -l "$1/trunc.txt" 2>/dev/null | awk '{ print $5 }')
    if [ "$got" = "0123" ] && [ "$size" = "4" ]; then
        say "OK   truncated to 4 bytes"
    else
        fail "after truncate content='$got' size='$size'"
    fi
}

phase_rename()
{
    if "${AIX_FUSE_IO_PROBE:?}" moved-parent "$1" >>"$LOGFILE" 2>&1; then
        say "OK   held working directory follows repeated moves across parents"
    else
        fail "dot-dot used a stale parent or lost directory identity"
    fi
    say "STEP rename: within and across directories"

    if "${AIX_FUSE_IO_PROBE:?}" rename-cycle "$1" >>"$LOGFILE" 2>&1; then
        say "OK   moved directory retains its parent and rejects ancestor cycles"
    else
        fail "moved directory lost its parent relationship or admitted a cycle"
    fi

    mkdir -p "$1/rn/src" "$1/rn/dst" 2>>"$LOGFILE"
    echo payload > "$1/rn/src/f.txt" 2>>"$LOGFILE"

    if mv "$1/rn/src/f.txt" "$1/rn/src/g.txt" 2>>"$LOGFILE"; then
        got=$(cat "$1/rn/src/g.txt" 2>/dev/null)
        if [ "$got" = "payload" ] && [ ! -f "$1/rn/src/f.txt" ]; then
            say "OK   renamed within a directory, content intact"
        else
            fail "rename within a directory lost content or left the old name"
        fi
    else
        fail "rename within a directory failed"
    fi

    if mv "$1/rn/src/g.txt" "$1/rn/dst/h.txt" 2>>"$LOGFILE"; then
        got=$(cat "$1/rn/dst/h.txt" 2>/dev/null)
        if [ "$got" = "payload" ] && [ ! -f "$1/rn/src/g.txt" ]; then
            say "OK   renamed across directories, content intact"
        else
            fail "rename across directories lost content or left the old name"
        fi
    else
        fail "rename across directories failed"
    fi
}

phase_link()
{
    say "STEP link: hard links share one object"

    if "${AIX_FUSE_IO_PROBE:?}" hardlink-mappings "$1" >>"$LOGFILE" 2>&1; then
        say "OK   aliases share mapped pages, writeback, truncation, and final lifetime"
    else
        fail "hard-link aliases diverged in mapping identity or leaked ownership"
    fi

    echo linked > "$1/orig.txt" 2>>"$LOGFILE"

    if ! ln "$1/orig.txt" "$1/hard.txt" 2>>"$LOGFILE"; then
        fail "hard link creation failed"
        return
    fi

    n=$(ls -l "$1/orig.txt" 2>/dev/null | awk '{ print $2 }')
    got=$(cat "$1/hard.txt" 2>/dev/null)

    if [ "$n" = "2" ] && [ "$got" = "linked" ]; then
        say "OK   link count is 2 and both names read the same content"
    else
        fail "after ln: nlink='$n' content='$got'"
    fi

    rm -f "$1/orig.txt" 2>>"$LOGFILE"
    got=$(cat "$1/hard.txt" 2>/dev/null)
    n=$(ls -l "$1/hard.txt" 2>/dev/null | awk '{ print $2 }')

    if [ "$got" = "linked" ] && [ "$n" = "1" ]; then
        say "OK   unlinking one name leaves the other readable, nlink back to 1"
    else
        fail "after removing one link: nlink='$n' content='$got'"
    fi
}

phase_symlink()
{
    say "STEP symlink: create and read back"

    echo target_content > "$1/sym_target.txt" 2>>"$LOGFILE"

    if ! ln -s sym_target.txt "$1/sym.txt" 2>>"$LOGFILE"; then
        fail "symlink creation failed"
        return
    fi

    got=$(ls -l "$1/sym.txt" 2>/dev/null | sed 's/.*-> //')
    if [ "$got" = "sym_target.txt" ]; then
        say "OK   symlink target reads back as $got"
    else
        fail "symlink target is '$got'"
    fi

    got=$(cat "$1/sym.txt" 2>/dev/null)
    if [ "$got" = "target_content" ]; then
        say "OK   symlink resolves to its target"
    else
        fail "reading through the symlink gave '$got'"
    fi

    ln -s nowhere "$1/dangling" 2>>"$LOGFILE"
    if [ -L "$1/dangling" ] && [ ! -e "$1/dangling" ]; then
        say "OK   dangling symlink behaves as one"
    else
        fail "dangling symlink is wrong"
    fi

    rm -f "$1/sym.txt" "$1/dangling"
    if echo accounting-ok >"$1/after-symlink-delete" 2>>"$LOGFILE" &&
       [ "$(cat "$1/after-symlink-delete" 2>/dev/null)" = accounting-ok ]; then
        say "OK   deleting symlinks preserves regular-file byte capacity"
    else
        fail "symlink deletion corrupted regular-file byte accounting"
    fi
}

phase_attrs()
{
    say "STEP attrs: chmod and chown are reflected"
    if "${AIX_FUSE_IO_PROBE:?}" retained-attributes "$1" >>"$LOGFILE" 2>&1; then
        say "OK   retained objects keep their own authorization, modes, owners and times"
    else
        fail "metadata changes affected a replacement or lost the retained object"
    fi

    echo x > "$1/attr.txt" 2>>"$LOGFILE"

    chmod 741 "$1/attr.txt" 2>>"$LOGFILE"
    got=$(ls -l "$1/attr.txt" 2>/dev/null | cut -c1-10)
    if [ "$got" = "-rwxr----x" ]; then
        say "OK   chmod 741 reads back as $got"
    else
        fail "after chmod 741, mode reads as '$got'"
    fi

    chown 2:3 "$1/attr.txt" 2>>"$LOGFILE"
    got=$(ls -n "$1/attr.txt" 2>/dev/null | awk '{ print $3 ":" $4 }')
    if [ "$got" = "2:3" ]; then
        say "OK   chown 2:3 reads back as $got"
    else
        fail "after chown 2:3, ownership reads as '$got'"
    fi
}

phase_openunlink()
{
    "${AIX_FUSE_IO_PROBE:?}" identity-churn "$1" >>"$LOGFILE" 2>&1 ||
        fail "unique-name and hard-link churn lost a held unlinked identity"
    say "STEP openunlink: recreated names have independent objects and mappings"
    if "${AIX_FUSE_IO_PROBE:?}" recreated-identity "$1" >>"$LOGFILE" 2>&1; then
        say "OK   old and new descriptors, mappings, ownership, and types remain independent"
    else
        fail "recreated name reused a detached object or retained its resources"
    fi

    say "STEP openunlink: a file unlinked while open stays readable"

    echo still_here > "$1/doomed.txt" 2>>"$LOGFILE"

    # Hold the file open on a background reader, remove it, then read through
    # the descriptor that is still open.
    ( exec 3< "$1/doomed.txt"
      rm -f "$1/doomed.txt"
      out=$(cat <&3)
      exec 3<&-
      [ "$out" = "still_here" ] ) 2>>"$LOGFILE"

    if [ $? -eq 0 ]; then
        say "OK   read through an open descriptor after unlink"
    else
        fail "reading an unlinked but open file failed"
    fi

    if [ -e "$1/doomed.txt" ]; then
        fail "the unlinked name is still present"
    else
        say "OK   the name is gone"
    fi

    # Writing and truncating through a descriptor whose name is gone: these
    # reach the file system with only a handle to go on, since the path no
    # longer resolves to anything.
    say "STEP openunlink: writing through a descriptor whose name is gone"

    ( exec 3> "$1/doomed2.txt"
      rm -f "$1/doomed2.txt"
      echo written_after_unlink >&3
      exec 3>&- ) 2>>"$LOGFILE"

    if [ $? -eq 0 ]; then
        say "OK   wrote through an open descriptor after unlink"
    else
        fail "writing to an unlinked but open file failed"
    fi
}

phase_notempty()
{
    say "STEP notempty: rmdir of a non-empty directory must fail"

    mkdir -p "$1/ne" 2>>"$LOGFILE"
    touch "$1/ne/child" 2>>"$LOGFILE"

    if err=$(rmdir "$1/ne" 2>&1); then
        fail "rmdir removed a non-empty directory"
    else
        say "OK   refused: $(echo "$err" | head -1)"
    fi

    rm -f "$1/ne/child" 2>>"$LOGFILE"
    if rmdir "$1/ne" 2>>"$LOGFILE"; then
        say "OK   rmdir succeeds once empty"
    else
        fail "rmdir failed on an empty directory"
    fi
}

phase_df()
{
    say "STEP df: usage tracks the data written"

    before=$(df -k "$1" 2>/dev/null | tail -1 | awk '{ print $3 }')

    i=0
    : > "$1/filler.bin"
    while [ $i -lt 512 ]; do
        dd if=/dev/zero bs=1024 count=1 2>/dev/null | tr '\0' 'q' >> "$1/filler.bin"
        i=$((i + 1))
    done

    after=$(df -k "$1" 2>/dev/null | tail -1 | awk '{ print $3 }')

    say "df free before=$before after=$after"
    if [ -n "$after" ] && [ "$after" -lt "$before" ]; then
        say "OK   free space decreased after writing"
    else
        fail "free space did not decrease (before=$before after=$after)"
    fi

    rm -f "$1/filler.bin" 2>>"$LOGFILE"
}

phase_nospc()
{
    say "STEP nospc: filling the file system reports ENOSPC"

    # Write past the capacity; dd should stop with a "file system is full"
    # style error rather than growing without limit.
    err=$(dd if=/dev/zero of="$1/fill.bin" bs=65536 count=$((SIZE_MB * 32)) 2>&1)
    rc=$?

    size=$(ls -l "$1/fill.bin" 2>/dev/null | awk '{ print $5 }')
    say "wrote $size bytes into a ${SIZE_MB}MB file system (dd rc=$rc)"

    if [ "$rc" -ne 0 ] || echo "$err" | grep -qiE "space|full"; then
        say "OK   the write was refused once full"
    else
        fail "filling past the capacity was not refused: $err"
    fi

    if [ -n "$size" ] && [ "$size" -le $((SIZE_MB * 1024 * 1024)) ]; then
        say "OK   stored data stayed within the capacity"
    else
        fail "stored $size bytes, above the ${SIZE_MB}MB capacity"
    fi

    if "${AIX_FUSE_IO_PROBE:?}" append-full "$1/fill.bin" >>"$LOGFILE" 2>&1; then
        say "OK   short append and ENOSPC preserve committed file positions"
    else
        fail "append returned incorrect count, position, or full-filesystem error"
    fi
    rm -f "$1/fill.bin" 2>>"$LOGFILE"

    # Keep each emptied name alive while another file reuses its buffer budget.
    # Unit allocator interposition checks physical buffer sizes; this journey
    # verifies the same lifecycle through AIX truncate and statfs.
    baseline=$(df -k "$1" 2>/dev/null | tail -1 | awk '{ print $3 }')
    cycle=1
    while [ "$cycle" -le 3 ]; do
        path="$1/capacity-cycle-$cycle"
        dd if=/dev/zero of="$path" bs=65536 count=$((SIZE_MB * 16 + 1)) >>"$LOGFILE" 2>&1
        size=$(ls -l "$path" 2>/dev/null | awk '{ print $5 }')
        if [ -z "$size" ] || [ "$size" -le 0 ]; then
            fail "capacity reuse cycle $cycle could not store data"
        fi
        if ! : > "$path"; then
            fail "capacity reuse cycle $cycle could not truncate its file"
        fi
        after=$(df -k "$1" 2>/dev/null | tail -1 | awk '{ print $3 }')
        size=$(ls -l "$path" 2>/dev/null | awk '{ print $5 }')
        if [ -z "$baseline" ] || [ "$after" != "$baseline" ] || [ "$size" != 0 ]; then
            fail "capacity reuse cycle $cycle failed to restore free space (before=$baseline after=$after size=$size)"
        else
            say "OK   capacity reuse cycle $cycle released buffers while retaining the empty name"
        fi
        cycle=$((cycle + 1))
    done
    rm -f "$1/capacity-cycle-1" "$1/capacity-cycle-2" "$1/capacity-cycle-3" 2>>"$LOGFILE"
}

phase_concurrent()
{
    say "STEP concurrent: $N_WRITERS writers on separate files"

    writer_pids=""
    i=1
    while [ "$i" -le "$N_WRITERS" ]; do
        (
            n=0
            while [ $n -lt 20 ]; do
                echo "writer $i line $n" >> "$1/w$i.txt" || exit 1
                n=$((n + 1))
            done
            [ "$(wc -l < "$1/w$i.txt" | tr -d ' ')" = "20" ]
        ) &
        writer_pids="$writer_pids $!"
        aix_fuse_track_pid "$!"
        i=$((i + 1))
    done

    deadline=$(( $(date +%s) + READ_WAIT ))
    for wp in $writer_pids; do
        while ps -p "$wp" >/dev/null 2>&1 && [ "$(date +%s)" -lt "$deadline" ]; do
            sleep 2
        done
        if ps -p "$wp" >/dev/null 2>&1; then
            fail "TIMEOUT writer $wp still running after ${READ_WAIT}s"
            kill -9 "$wp" 2>/dev/null
            wait "$wp" 2>/dev/null || true
        elif ! wait "$wp"; then
            fail "writer $wp reported an error"
        fi

        aix_fuse_forget_pid "$wp"
    done

    say "OK   concurrent writers finished"
}

phase_shutdown()
{
    iter=1
    while [ "$iter" -le "$ITERATIONS" ]; do
        say "=== shutdown iteration $iter/$ITERATIONS"

        pids=""
        mnts=""
        i=1
        while [ "$i" -le "$N_DAEMONS" ]; do
            info=$(start_memfs)
            if [ -z "$info" ]; then
                fail "TIMEOUT daemon $i never mounted"
            else
                pids="$pids ${info% *}"
                mnts="$mnts ${info#* }"
                say "OK   daemon $i mounted ${info#* }"
            fi
            i=$((i + 1))
        done

        snapshot "after mount"

        k=1
        for pid in $pids; do
            mnt=$(echo "$mnts" | awk -v n="$k" '{ print $n }')

            if [ $((k % 2)) -eq 1 ]; then
                say "STEP kill -9 daemon $pid"
                kill -9 "$pid" 2>/dev/null
                wait_gone "$pid" "$STOP_WAIT" || fail "daemon $pid survived SIGKILL"

                if is_mounted "$mnt"; then
                    if ls "$mnt" >/dev/null 2>&1; then
                        fail "$mnt still usable after kill -9"
                    else
                        say "OK   $mnt reports an error as expected"
                    fi

                    if umount "$mnt" >>"$LOGFILE" 2>&1; then
                        say "OK   unmounted $mnt"
                    else
                        fail "cannot umount $mnt after kill -9"
                    fi
                fi
                rmdir "$mnt" "/mnt/$pid" 2>/dev/null
            else
                say "STEP kill -TERM daemon $pid"
                kill -TERM "$pid" 2>/dev/null
                if wait_gone "$pid" "$STOP_WAIT" && ! is_mounted "$mnt"; then
                    say "OK   $mnt self-unmounted"
                else
                    fail "$mnt still mounted after SIGTERM"
                    umount "$mnt" >>"$LOGFILE" 2>&1
                fi
                rmdir "$mnt" "/mnt/$pid" 2>/dev/null
            fi

            if aix_fuse_process_gone "$pid" && ! is_mounted "$mnt"; then
                aix_fuse_forget_example "$pid"
            fi

            k=$((k + 1))
        done

        snapshot "after cleanup"
        iter=$((iter + 1))
    done
}

phase_inodecap()
{
    say "STEP inodecap: object ceiling and recovery"
    info=$(start_memfs_with_inode_limit 32)
    if [ -z "$info" ]; then
        fail "inode-limited memfs never mounted"
        return
    fi
    ipid=${info% *}
    imnt=${info#* }
    created=0
    while [ "$created" -lt 40 ]; do
        if : >"$imnt/object.$created" 2>>"$LOGFILE"; then
            created=$((created + 1))
        else
            break
        fi
    done
    if [ "$created" -eq 31 ] && [ ! -e "$imnt/object.31" ]; then
        say "OK   inode ceiling rejected object 32 with root counted"
    else
        fail "inode ceiling stopped after $created files, expected 31"
    fi
    rm -f "$imnt/object.0"
    if : >"$imnt/recovered" 2>>"$LOGFILE"; then
        say "OK   inode capacity recovered after unlink"
    else
        fail "inode capacity did not recover after unlink"
    fi
    rm -f "$imnt"/object.* "$imnt/recovered"
    # stop_memfs also attempts to remove both possible mount-point paths;
    # rmdir may therefore report a harmless missing path after a clean stop.
    # Judge cleanup by the resources that matter instead of that final status.
    stop_memfs "$ipid" "$imnt"
    if is_mounted "$imnt" || kill -0 "$ipid" 2>/dev/null; then
        fail "inode-limited memfs did not stop"
    else
        say "OK   inode-limited memfs stopped cleanly"
    fi
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

say "=== USFS memfs test (phases: $MEMFS_PHASES, capacity ${SIZE_MB}MB)"
snapshot "before start"

# Phases that share one long-lived file system.
if phase_enabled createatomic || phase_enabled tree || phase_enabled rw ||
   phase_enabled openhandles ||
   phase_enabled durability ||
   phase_enabled mmap ||
   phase_enabled truncate ||
   phase_enabled rename || phase_enabled link || phase_enabled symlink ||
   phase_enabled attrs || phase_enabled openunlink || phase_enabled notempty ||
   phase_enabled df || phase_enabled nospc || phase_enabled concurrent; then

    say "STEP starting a memfs daemon"
    info=$(start_memfs)

    if [ -z "$info" ]; then
        fail "TIMEOUT memfs never mounted"
    else
        mpid=${info% *}
        mmnt=${info#* }
        say "OK   mounted $mmnt (pid $mpid)"

        phase_enabled createatomic && phase_createatomic "$mmnt"
        phase_enabled tree       && phase_tree       "$mmnt"
        phase_enabled rw         && phase_rw         "$mmnt"
        phase_enabled openhandles && phase_openhandles "$mmnt"
        phase_enabled durability && phase_durability "$mmnt"
        phase_enabled mmap       && phase_mmap       "$mmnt"
        phase_enabled truncate   && phase_truncate   "$mmnt"
        phase_enabled rename     && phase_rename     "$mmnt"
        phase_enabled link       && phase_link       "$mmnt"
        phase_enabled symlink    && phase_symlink    "$mmnt"
        phase_enabled attrs      && phase_attrs      "$mmnt"
        phase_enabled openunlink && phase_openunlink "$mmnt"
        phase_enabled notempty   && phase_notempty   "$mmnt"
        phase_enabled df         && phase_df         "$mmnt"
        phase_enabled concurrent && phase_concurrent "$mmnt"
        phase_enabled nospc      && phase_nospc      "$mmnt"

        say "STEP stopping the daemon"
        stop_memfs "$mpid" "$mmnt"

        if is_mounted "$mmnt"; then
            fail "$mmnt still mounted after SIGTERM"
            umount "$mmnt" >>"$LOGFILE" 2>&1
        else
            say "OK   $mmnt self-unmounted"
        fi
    fi
fi

if phase_enabled readonly; then
    say "STEP readonly: writable memfs callbacks obey the requested restriction"
    info=$(start_memfs -o ro)
    if [ -z "$info" ]; then
        fail "read-only memfs never mounted"
    else
        mpid=${info% *}
        mmnt=${info#* }
        "${AIX_FUSE_IO_PROBE:?}" readonly-root "$mmnt" >>"$LOGFILE" 2>&1 ||
            fail "read-only memfs admitted mutation or returned an incorrect error"
        [ -z "$(ls -A "$mmnt")" ] || fail "read-only root contents changed"
        stop_memfs "$mpid" "$mmnt"
    fi
    say "STEP readonly: unsupported restrictions fail before mounting"
    "$DAEMON" -o noexec </dev/null >>"$LOGFILE" 2>&1 &
    mpid=$!
    aix_fuse_record_example "$mpid" "/mnt/$mpid/memfs"
    if ! wait_gone "$mpid" 10; then
        fail "unsupported restriction did not terminate the daemon"
        stop_memfs "$mpid" "/mnt/$mpid/memfs"
    else
        wait "$mpid" && fail "unsupported restriction reported success"
        is_mounted "/mnt/$mpid/memfs" && fail "unsupported restriction published a mount"
        rmdir "/mnt/$mpid/memfs" "/mnt/$mpid" 2>/dev/null
    fi
fi

phase_enabled shutdown && phase_shutdown
phase_enabled inodecap && phase_inodecap

say "=== finished"
snapshot "final"

if ! aix_fuse_examples_unmounted; then
    fail "an owned example mount remains"
fi

if [ "$failures" -eq 0 ]; then
    say "ALL TESTS PASSED"
    exit 0
fi

say "$failures FAILURE(S)"
exit 1
