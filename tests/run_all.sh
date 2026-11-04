#!/bin/bash
# Runs the complete test suite (Linux, needs e2fsprogs and python3).
#   tests/run_all.sh            full run (~1 hour)
#   QUICK=1 tests/run_all.sh    smaller numbers (~5 minutes)
set -u
cd "$(dirname "$0")/.."
make -s all || exit 1
export L2M_TEST_DIR=${L2M_TEST_DIR:-$(mktemp -d /tmp/l2m-tests.XXXXXX)}
fail=0
if [ "${QUICK:-0}" = 1 ]; then
	export L2M_OPS=120 L2M_CRASH_ROUNDS=8
fi
tests/test_read.sh || fail=1
tests/test_write.sh || fail=1
tests/test_crash.sh || fail=1
echo "== concurrency stress (8 threads)"
rm -f "$L2M_TEST_DIR/stress.img"
mkfs.ext4 -q -F "$L2M_TEST_DIR/stress.img" 1G >/dev/null 2>&1
if build/stress "$L2M_TEST_DIR/stress.img" 8 "${STRESS_SECONDS:-30}" && e2fsck -fn "$L2M_TEST_DIR/stress.img" >/dev/null 2>&1; then
	echo "  ok   stress test + e2fsck clean"
else
	echo "  FAIL stress test"; fail=1
fi
echo "== big files on a fragmented volume (macOS kernel I/O pattern)"
for mode in boundary prealloc map shuffle kernel; do
	img="$L2M_TEST_DIR/bigfile-$mode.img"
	rm -f "$img"
	# boundary: a small default volume; the others need many inodes for the small files
	if [ $mode = boundary ]; then mkfs.ext4 -q -F "$img" 64M; else mkfs.ext4 -q -F -N 80000 "$img" 512M; fi >/dev/null 2>&1
	if out=$(build/bigfile "$img" $mode 2>&1) && e2fsck -fn "$img" >/dev/null 2>&1; then
		echo "  ok   $out + e2fsck clean"
	else
		echo "  FAIL bigfile $mode: $out" | head -20; fail=1
	fi
done
echo "== downloads through macOS kernel I/O (imperfect completion reports, zero holes)"
for mode in download misreport lost stale merge; do
	img="$L2M_TEST_DIR/koio-$mode.img"
	rm -f "$img"
	mkfs.ext4 -q -F "$img" 1G >/dev/null 2>&1
	if out=$(L2M_KOIO_MB=${L2M_KOIO_MB:-160} build/koio "$img" $mode 2>&1) && e2fsck -fn "$img" >/dev/null 2>&1; then
		echo "  ok   $out + e2fsck clean"
	else
		echo "  FAIL koio $mode: $out" | head -20; fail=1
	fi
done
# lost reports made up for while the files are still open (lost after 0 ms)
img="$L2M_TEST_DIR/koio-lost0.img"
rm -f "$img"
mkfs.ext4 -q -F "$img" 1G >/dev/null 2>&1
if out=$(L2M_KOIO_LOST_MS=0 build/koio "$img" lost 7 2>&1) && e2fsck -fn "$img" >/dev/null 2>&1; then
	echo "  ok   $out (lost reports swept early) + e2fsck clean"
else
	echo "  FAIL koio lost (swept early): $out" | head -20; fail=1
fi
echo
[ $fail = 0 ] && echo "ALL TESTS PASSED" || echo "SOME TESTS FAILED"
exit $fail
