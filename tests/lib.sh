# shared helpers for the shell tests
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
L2M="$ROOT/build/l2m"
WORK=${L2M_TEST_DIR:-$(mktemp -d /tmp/l2m-test.XXXXXX)}
mkdir -p "$WORK"
PASS=0
FAIL=0
export PATH="$PATH:/sbin:/usr/sbin"

ok() { PASS=$((PASS + 1)); printf '  \033[32mok\033[0m   %s\n' "$*"; }
bad() { FAIL=$((FAIL + 1)); printf '  \033[31mFAIL\033[0m %s\n' "$*"; }

# e2fsck in check-only mode must report nothing (exit 0)
fsck_clean() {
	local img=$1 what=$2 out
	out=$(e2fsck -fn "$img" 2>&1)
	local rc=$?
	if [ $rc -eq 0 ]; then ok "e2fsck clean: $what"; else bad "e2fsck ($rc): $what"; echo "$out" | head -30 | sed 's/^/        /'; fi
}

src_tree() {
	if [ ! -f "$WORK/src.ref" ]; then
		python3 "$ROOT/tests/make_tree.py" "$WORK/src"
		python3 "$ROOT/tests/ref_tree.py" "$WORK/src" > "$WORK/src.ref"
	fi
}

summary() {
	echo
	echo "passed: $PASS  failed: $FAIL"
	[ $FAIL -eq 0 ]
}
