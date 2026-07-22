#!/bin/sh
set -eu

B=${B:-./build}
STRESS_TIMEOUT=${STRESS_TIMEOUT:-30}
if ! command -v timeout >/dev/null 2>&1; then
    echo 'stress: the Linux/coreutils timeout command is required' >&2
    exit 1
fi
umask 077
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/threads-stress.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

run_with_deadline()
{
    timeout "$STRESS_TIMEOUT" "$@"
}

run_with_deadline "$B/showcase" --serial --width 96 --height 64 \
    --output "$tmpdir/serial.ppm" >"$tmpdir/serial.log"

iteration=0
while [ "$iteration" -lt 20 ]; do
    run_with_deadline "$B/03_invariant" --check >/dev/null
    run_with_deadline "$B/04_transfers" --check >/dev/null
    run_with_deadline "$B/05_bounded_queue" --quiet
    run_with_deadline "$B/06_atomic_claim" --quiet
    run_with_deadline "$B/07_atomic_publish" --check >/dev/null
    run_with_deadline "$B/09_reduction" --quiet
    run_with_deadline "$B/10_barrier" --check >/dev/null
    run_with_deadline "$B/11_lane_program" --lanes 64 >/dev/null

    case $((iteration % 4)) in
        0) lanes=2 ;;
        1) lanes=3 ;;
        2) lanes=8 ;;
        *) lanes=101 ;;
    esac
    run_with_deadline "$B/showcase" --threads "$lanes" --width 96 --height 64 \
        --output "$tmpdir/threaded.ppm" >"$tmpdir/threaded.log"
    cmp "$tmpdir/serial.ppm" "$tmpdir/threaded.ppm"

    iteration=$((iteration + 1))
done

echo 'stress: 20 iterations passed'
