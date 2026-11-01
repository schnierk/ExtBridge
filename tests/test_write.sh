#!/bin/bash
# Write correctness: copy trees in, run random operations, and check that
# (1) e2fsck finds nothing, (2) l2m and debugfs (the reference reader of
# e2fsprogs) both see exactly the expected content.
. "$(dirname "$0")/lib.sh"
src_tree
NOPS=${L2M_OPS:-400}
echo "== write tests ($NOPS random operations per variant)"

compare() { # img model what
	local img=$1 model=$2 what=$3
	python3 "$ROOT/tests/ref_tree.py" "$model" > "$WORK/expect.txt"
	if "$L2M" "$img" tree > "$WORK/got.txt" 2>"$WORK/got.err" && diff -q "$WORK/expect.txt" "$WORK/got.txt" >/dev/null; then
		ok "$what: l2m sees expected content ($(wc -l < "$WORK/expect.txt") entries)"
	else
		bad "$what: l2m content differs"; diff "$WORK/expect.txt" "$WORK/got.txt" | head -8; head -3 "$WORK/got.err"
	fi
	rm -rf "$WORK/dump"; mkdir -p "$WORK/dump"
	debugfs -R "rdump / $WORK/dump" "$img" >/dev/null 2>&1
	chmod -R u+rwX "$WORK/dump" 2>/dev/null
	python3 "$ROOT/tests/ref_tree.py" "$WORK/dump" | grep -v $'\txattr\t' > "$WORK/dump.txt"
	grep -v $'\txattr\t' "$WORK/expect.txt" | sed 's#^\(/[^\t]*\t\(file\|dir\)\t\)[0-7]*#\1MODE#' > "$WORK/e2.txt"
	sed 's#^\(/[^\t]*\t\(file\|dir\)\t\)[0-7]*#\1MODE#' "$WORK/dump.txt" > "$WORK/d2.txt"
	if diff -q "$WORK/e2.txt" "$WORK/d2.txt" >/dev/null; then
		ok "$what: debugfs (e2fsprogs) reads the same content"
	else
		bad "$what: debugfs content differs"; diff "$WORK/e2.txt" "$WORK/d2.txt" | head -8
	fi
}

variants=(
	"default|-t ext4"
	"1k|-t ext4 -b 1024"
	"2k|-t ext4 -b 2048"
	"nocsum|-t ext4 -O ^metadata_csum"
	"nocsum-nouninit|-t ext4 -O ^metadata_csum,^uninit_bg"
	"no64bit|-t ext4 -O ^64bit"
	"nojournal|-t ext4 -O ^has_journal"
	"noflex|-t ext4 -O ^flex_bg"
	"128inode|-t ext4 -I 128"
	"csumseed|-t ext4 -O metadata_csum_seed"
	"largedir|-t ext4 -O large_dir"
	"tea|-t ext4|tea"
	"legacy|-t ext4|legacy"
	"sparse2|-t ext4 -O sparse_super2"
	"nodirindex|-t ext4 -O ^dir_index"
	"unsigned|-t ext4|unsigned"
)
only=${L2M_ONLY:-}
seed=1
for v in "${variants[@]}"; do
	IFS='|' read -r name opts hash <<<"$v"
	seed=$((seed + 1))
	[ -n "$only" ] && [ "$only" != "$name" ] && continue
	img="$WORK/w-$name.img"
	model="$WORK/w-$name.model"
	rm -rf "$img" "$model" "$WORK/payload"
	conf=/etc/mke2fs.conf
	if [ -n "$hash" ] && [ "$hash" != unsigned ]; then
		conf="$WORK/mke2fs.$hash.conf"
		sed "s/^\[defaults\]/[defaults]\n\thash_alg = $hash/" /etc/mke2fs.conf > "$conf"
	fi
	if ! MKE2FS_CONFIG=$conf mkfs.ext4 -q -F $opts -d "$WORK/src" "$img" 700M >/dev/null 2>"$WORK/mkfs.err"; then
		bad "$name: mkfs failed: $(head -1 "$WORK/mkfs.err")"; continue
	fi
	if [ "$hash" = unsigned ]; then
		# switch to unsigned hashing (as made on ARM Linux) and let
		# e2fsck rebuild every index with it
		debugfs -w -R "ssv flags 2" "$img" >/dev/null 2>&1
		e2fsck -fyD "$img" >/dev/null 2>&1
	fi
	cp -a "$WORK/src" "$model"
	# phase 1: copy a full tree in (large directories become htrees)
	"$L2M" -w "$img" copyin "$WORK/src" /copy 2>"$WORK/err.txt" || { bad "$name: copyin failed: $(head -2 "$WORK/err.txt")"; continue; }
	cp -a "$WORK/src" "$model/copy"
	fsck_clean "$img" "$name after copyin"
	compare "$img" "$model" "$name after copyin"
	# phase 2: random operations
	xmax=2000; case "$opts" in *"-b 1024"*) xmax=500;; *"-b 2048"*) xmax=1200;; esac
	XATTR_MAX=$xmax python3 "$ROOT/tests/gen_ops.py" "$model" "$WORK/payload" $seed $NOPS /ops > "$WORK/ops.txt"
	if "$L2M" -w "$img" batch < "$WORK/ops.txt" 2>"$WORK/err.txt"; then
		ok "$name: $(wc -l < "$WORK/ops.txt") operations executed"
	else
		bad "$name: batch failed: $(head -3 "$WORK/err.txt")"
	fi
	fsck_clean "$img" "$name after random operations"
	compare "$img" "$model" "$name after random operations"
	# phase 3: delete everything that was added again
	printf 'rm\t%s\n' $(cd "$model" && find copy ops -depth -not -type d -printf '/%p\n' 2>/dev/null) > /dev/null
	(cd "$model" && find copy ops -depth -printf '%y\t/%p\n') | awk -F'\t' '{ if ($1=="d") print "rmdir\t" $2; else print "rm\t" $2 }' > "$WORK/del.txt"
	"$L2M" -w "$img" batch < "$WORK/del.txt" 2>"$WORK/err.txt" || bad "$name: delete failed: $(head -3 "$WORK/err.txt")"
	rm -rf "$model/copy" "$model/ops"
	fsck_clean "$img" "$name after deleting everything again"
	compare "$img" "$model" "$name after deleting"
	# free space must be back to (almost) the original value
	"$L2M" "$img" verify > "$WORK/verify.txt" 2>&1 && ok "$name: l2m verify" || { bad "$name: l2m verify"; grep -E "PROBLEM" "$WORK/verify.txt" | head -5; }
done
summary
