#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT

# Purpose: Run adapter units or filesystem journeys on a prepared AIX host.
# Usage: bash scripts/aix/run-target.sh test|test-e2e source build
set -eu
exec </dev/null

mode=$1
AIX_FUSE_SOURCE_DIR=$2
AIX_FUSE_BINARY_DIR=$3
export AIX_FUSE_SOURCE_DIR AIX_FUSE_BINARY_DIR

run_units()
{
    while IFS= read -r script; do
        bash -n "$script"
    done < <(find "$AIX_FUSE_SOURCE_DIR/scripts" "$AIX_FUSE_SOURCE_DIR/tests" \
        -type f -name '*.sh' -print)

    found=0
    for program in "$AIX_FUSE_BINARY_DIR"/tests/unit/*; do
        [ -x "$program" ] || continue
        [ -f "$program" ] || continue
        found=1
        echo "== $(basename "$program")"
        "$program"
    done
    [ "$found" -eq 1 ] || {
        echo "Failed to run adapter units: no test executables were built" >&2
        return 1
    }
}

run_journeys()
{
    AIX_FUSE_MIRROR="$AIX_FUSE_BINARY_DIR/bin/aix_fuse_mirror"
    AIX_FUSE_MEMFS="$AIX_FUSE_BINARY_DIR/bin/aix_fuse_memfs"
    AIX_FUSE_MT_DAEMON="$AIX_FUSE_BINARY_DIR/tests/helpers/mt_daemon"
    AIX_FUSE_PATH_DAEMON="$AIX_FUSE_BINARY_DIR/tests/helpers/path_daemon"
    AIX_FUSE_IO_PROBE="$AIX_FUSE_BINARY_DIR/tests/helpers/io_probe"
    export AIX_FUSE_MIRROR AIX_FUSE_MEMFS AIX_FUSE_MT_DAEMON
    export AIX_FUSE_PATH_DAEMON AIX_FUSE_IO_PROBE

    for executable in "$AIX_FUSE_MIRROR" "$AIX_FUSE_MEMFS" \
        "$AIX_FUSE_MT_DAEMON" "$AIX_FUSE_PATH_DAEMON" "$AIX_FUSE_IO_PROBE"; do
        [ -x "$executable" ] || {
            echo "Failed to run adapter journeys: missing executable $executable" >&2
            return 1
        }
    done

    for journey in mirror memfs multithread pathname_lifetime; do
        echo "== $journey"
        bash "$AIX_FUSE_SOURCE_DIR/tests/e2e/$journey.sh"
    done
}

case "$mode" in
    test) run_units ;;
    test-e2e) run_journeys ;;
    *) echo "Failed to run adapter target: unknown mode $mode" >&2; exit 2 ;;
esac
