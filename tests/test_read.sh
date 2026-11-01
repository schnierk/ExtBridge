#!/bin/bash
# Read compatibility: images made by mkfs.ext4 -d with many feature sets
# must be listed byte-identically by l2m.
. "$(dirname "$0")/lib.sh"
src_tree
echo "== read tests"
variants=(
	"ext4-default|-t ext4"
	"ext4-1k|-t ext4 -b 1024"
	"ext4-2k|-t ext4 -b 2048"
	"ext4-nocsum|-t ext4 -O ^metadata_csum"
	"ext4-nocsum-nouninit|-t ext4 -O ^metadata_csum,^uninit_bg"
	"ext4-no64bit|-t ext4 -O ^64bit"
	"ext4-nojournal|-t ext4 -O ^has_journal"
	"ext4-noflex|-t ext4 -O ^flex_bg"
	"ext4-128inode|-t ext4 -I 128"
	"ext4-csumseed|-t ext4 -O metadata_csum_seed"
	"ext4-largedir|-t ext4 -O large_dir"
	"ext4-tea|-t ext4|tea"
	"ext4-legacy|-t ext4|legacy"
	"ext4-inline|-t ext4 -O inline_data"
	"ext4-sparse2|-t ext4 -O sparse_super2"
	"ext4-nodirindex|-t ext4 -O ^dir_index"
	"ext4-bigalloc|-t ext4 -O bigalloc -C 16384"
	"ext3|-t ext3"
	"ext2|-t ext2"
	"ext2-1k|-t ext2 -b 1024"
)
for v in "${variants[@]}"; do
	IFS='|' read -r name opts hash <<<"$v"
	img="$WORK/$name.img"
	rm -f "$img"
	conf=/etc/mke2fs.conf
	if [ -n "$hash" ]; then
		conf="$WORK/mke2fs.$hash.conf"
		sed "s/^\[defaults\]/[defaults]\n\thash_alg = $hash/" /etc/mke2fs.conf > "$conf"
	fi
	if ! MKE2FS_CONFIG=$conf mkfs.ext4 -q -F $opts -d "$WORK/src" "$img" 300M >/dev/null 2>"$WORK/mkfs.err"; then
		bad "$name: mkfs failed: $(head -1 "$WORK/mkfs.err")"
		continue
	fi
	if "$L2M" "$img" tree > "$WORK/$name.tree" 2>"$WORK/$name.err" && diff -q "$WORK/src.ref" "$WORK/$name.tree" >/dev/null; then
		ok "$name: tree identical"
	else
		bad "$name: tree differs"; diff "$WORK/src.ref" "$WORK/$name.tree" | head -5; head -3 "$WORK/$name.err"
	fi
	# xattrs
	v1=$("$L2M" "$img" getxattr /xattr/file0 small 2>&1)
	n=$("$L2M" "$img" listxattr /xattr/file4 2>&1 | LC_ALL=C sort | tr '\n' ' ')
	if [ "$v1" = "v" ] && [ "$n" = "sized small " ]; then ok "$name: xattrs"; else bad "$name: xattrs ($v1 / $n)"; fi
	if "$L2M" "$img" verify > "$WORK/$name.verify" 2>&1; then ok "$name: verify"; else bad "$name: verify"; grep -E "PROBLEM|RESULT" "$WORK/$name.verify" | head -5; fi
	# the image must be untouched by read-only use
	if [ -n "$hash" ]; then
		h=$(dumpe2fs -h "$img" 2>/dev/null | awk -F: '/Default directory hash/ {gsub(/ /,"",$2); print $2}')
		[ "$h" = "$hash" ] && ok "$name: image really uses $hash" || bad "$name: hash is $h"
	fi
	sum_before=$(md5sum < "$img")
	"$L2M" "$img" tree >/dev/null 2>&1
	[ "$(md5sum < "$img")" = "$sum_before" ] && ok "$name: read-only use left the image byte-identical" || bad "$name: image changed by read-only use"
done
summary
