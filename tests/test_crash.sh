#!/bin/bash
# Crash safety: cut the power at a random write (dropping a random subset of
# everything not yet flushed), then
#  1. recovery by l2m itself (journal replay at mount) -> e2fsck clean
#  2. recovery by Linux' e2fsck (replays our journal)  -> clean, same state
#  3. the visible state equals the state after some prefix of operations
. "$(dirname "$0")/lib.sh"
ROUNDS=${L2M_CRASH_ROUNDS:-60}
echo "== crash tests ($ROUNDS power cuts per variant)"
variants=("default|-t ext4|0" "default-sync|-t ext4|1" "1k|-t ext4 -b 1024|1" "nocsum|-t ext4 -O ^metadata_csum|1")
for v in "${variants[@]}"; do
	IFS='|' read -r name opts sync <<<"$v"
	base="$WORK/c-$name.base"
	model="$WORK/c-$name.model"
	rm -rf "$base" "$model" "$WORK/cpayload" "$WORK/snaps"
	mkdir -p "$model"
	mkfs.ext4 -q -F $opts "$base" 256M >/dev/null 2>&1
	XATTR_MAX=500 SNAPDIR="$WORK/snaps" SMALL=1 python3 "$ROOT/tests/gen_ops.py" "$model" "$WORK/cpayload" 77 120 > "$WORK/cops.txt"
	flags=-w
	[ "$sync" = 1 ] && flags="-w -s"
	cp "$base" "$WORK/count.img"
	total=$(L2M_COUNT_WRITES=1 "$L2M" $flags "$WORK/count.img" batch < "$WORK/cops.txt" 2>&1 | sed -n 's/.*writes=//p')
	echo "  $name: $(wc -l < "$WORK/cops.txt") operations, $total device writes"
	fails=0
	for i in $(seq 1 "$ROUNDS"); do
		k=$(( (RANDOM * 32768 + RANDOM) % total + 1 ))
		img="$WORK/crash.img"
		cp "$base" "$img"
		L2M_CRASH_AFTER_WRITES=$k L2M_CRASH_SEED=$i "$L2M" $flags "$img" batch < "$WORK/cops.txt" >/dev/null 2>"$WORK/crash.err"
		cp "$img" "$WORK/crash2.img"
		# 1. our own recovery
		if ! "$L2M" -w "$img" info > "$WORK/info.txt" 2>"$WORK/rec.err"; then
			bad "$name round $i (cut at write $k): l2m could not recover: $(head -2 "$WORK/rec.err")"; fails=$((fails+1)); continue
		fi
		out=$(e2fsck -fn "$img" 2>&1)
		if [ $? -ne 0 ]; then
			bad "$name round $i (cut at write $k): not clean after l2m recovery"; echo "$out" | grep -v "^Durchgang\|^Pass\|^e2fsck" | head -8 | sed 's/^/        /'; fails=$((fails+1)); continue
		fi
		"$L2M" "$img" tree > "$WORK/crash_tree.txt" 2>/dev/null
		if ! m=$(python3 "$ROOT/tests/crash_match.py" "$WORK/crash_tree.txt" "$WORK/snaps" "$WORK/cops.txt"); then
			bad "$name round $i (cut at write $k): $m"; fails=$((fails+1)); continue
		fi
		# 2. Linux' recovery (e2fsck replays the journal we wrote)
		out=$(e2fsck -fy "$WORK/crash2.img" 2>&1); rc=$?
		# Normal after a power cut, exactly as with Linux: journal replay,
		# finishing orphans (files deleted while open / interrupted truncates)
		# and refreshing the lazily kept superblock free counters.
		probs=$(echo "$out" | grep -vE "^(e2fsck|Durchgang|Pass|/|$)|[Jj]ournal|Wiederherstellung|recover|verwaist|orphan|Anzahl freier|freier (Blöcke|Inodes)|count wrong|Reparieren\? ja|Fix\? yes|Zusammengefasste|\*\*\*\*|zusammenhängend|non-contiguous" )
		if [ $rc -gt 1 ] || [ -n "$probs" ]; then
			bad "$name round $i (cut at write $k): e2fsck had to repair ($rc)"; echo "$probs" | head -8 | sed 's/^/        /'; fails=$((fails+1)); continue
		fi
		if ! e2fsck -fn "$WORK/crash2.img" >/dev/null 2>&1; then
			bad "$name round $i: not clean after e2fsck recovery"; fails=$((fails+1)); continue
		fi
		"$L2M" "$WORK/crash2.img" tree > "$WORK/crash_tree2.txt" 2>/dev/null
		if ! cmp -s "$WORK/crash_tree.txt" "$WORK/crash_tree2.txt"; then
			bad "$name round $i: Linux recovery shows a different state than l2m recovery"; fails=$((fails+1)); continue
		fi
	done
	[ $fails -eq 0 ] && ok "$name: $ROUNDS power cuts, every one recovered to a consistent state (by l2m and by e2fsck)"
done
summary
