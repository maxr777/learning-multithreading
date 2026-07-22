#!/bin/sh
set -eu

B=${B:-./build}
CHECK_TIMEOUT=${CHECK_TIMEOUT:-30}
if ! command -v timeout >/dev/null 2>&1; then
    echo 'check: the Linux/coreutils timeout command is required' >&2
    exit 1
fi
umask 077
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/threads-course.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

run_with_deadline()
{
    timeout "$CHECK_TIMEOUT" "$@"
}

expect_pass()
{
    name=$1
    shift
    run_with_deadline "$@" >"$tmpdir/$name.log"
    grep -q 'PASS' "$tmpdir/$name.log"
}

extract_checksum()
{
    log=$1
    destination=$2
    sed -n 's/.*checksum=\([0-9][0-9]*\).*/\1/p' "$log" >"$destination"
    test "$(wc -l <"$destination")" -eq 1
    test -s "$destination"
}

expect_pass create_join "$B/01_create_join" --check
expect_pass race_mutex "$B/02_race_mutex" --check
expect_pass invariant "$B/03_invariant" --check
expect_pass transfers "$B/04_transfers" --check
run_with_deadline "$B/05_bounded_queue" --quiet
run_with_deadline "$B/06_atomic_claim" --quiet
expect_pass atomic_publish "$B/07_atomic_publish" --check
run_with_deadline "$B/08_false_sharing" >"$tmpdir/false-sharing.log"
grep -q 'PASS$' "$tmpdir/false-sharing.log"
run_with_deadline "$B/09_reduction" --quiet
expect_pass barrier "$B/10_barrier" --check
run_with_deadline "$B/11_lane_program" --serial >"$tmpdir/lane-serial.log"
run_with_deadline "$B/11_lane_program" --lanes 4 >"$tmpdir/lane-four.log"
run_with_deadline "$B/11_lane_program" --lanes 64 >"$tmpdir/lane-many.log"
test "$(grep -c ' PASS$' "$tmpdir/lane-serial.log")" -eq 3
test "$(grep -c ' PASS$' "$tmpdir/lane-four.log")" -eq 3
test "$(grep -c ' PASS$' "$tmpdir/lane-many.log")" -eq 3

run_with_deadline "$B/showcase" --serial --width 160 --height 100 \
    --output "$tmpdir/serial.ppm" >"$tmpdir/serial.log"
extract_checksum "$tmpdir/serial.log" "$tmpdir/serial.checksum"

for lanes in 2 3 8 101; do
    run_with_deadline "$B/showcase" --threads "$lanes" --width 160 --height 100 \
        --output "$tmpdir/threaded-$lanes.ppm" >"$tmpdir/threaded-$lanes.log"
    extract_checksum "$tmpdir/threaded-$lanes.log" \
        "$tmpdir/threaded-$lanes.checksum"
    cmp "$tmpdir/serial.checksum" "$tmpdir/threaded-$lanes.checksum"
    cmp "$tmpdir/serial.ppm" "$tmpdir/threaded-$lanes.ppm"
done

if [ -e /dev/full ]; then
    if run_with_deadline "$B/showcase" --serial --width 16 --height 16 \
        --output /dev/full \
        >"$tmpdir/output-failure.log" 2>&1; then
        echo 'check: showcase did not report /dev/full output failure' >&2
        exit 1
    else
        status=$?
        if [ "$status" -eq 124 ]; then
            echo 'check: showcase timed out while testing /dev/full' >&2
            exit 1
        fi
    fi
fi

echo 'check: all safe deterministic checks passed'
