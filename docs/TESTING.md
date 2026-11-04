# Testing

All tests run on Linux against disk images, using the reference tools of e2fsprogs
(`mkfs.ext4`, `e2fsck`, `debugfs`) as independent judges. No test ever touches a real disk.

```bash
tests/run_all.sh            # everything (about an hour)
QUICK=1 tests/run_all.sh    # smaller numbers
make asan && tests/run_all.sh   # with AddressSanitizer + UBSan
```

| Suite | What it proves |
|---|---|
| `test_read.sh` | 20 file system flavours made by `mkfs.ext4 -d` from a tree with large htree directories, long/unicode names, all symlink lengths, hard links, sparse and odd-sized files, deep nesting, permissions and xattrs are listed **byte-identically** (content checksums, modes, link targets, xattrs); `l2m verify` is clean; a read-only mount leaves the image **bit-identical**. |
| directory hashes | 180 names (incl. multi-byte UTF-8) × legacy / half-MD4 / TEA match `debugfs dx_hash`; the unsigned variants are validated by e2fsck on images switched to unsigned hashing and re-indexed with `e2fsck -D`. |
| `test_write.sh` | 16 writable flavours: copy a full tree in, run thousands of random operations mirrored on the host, delete everything again. After each phase `e2fsck -fn` must report **nothing**, and both l2m and `debugfs rdump` must show exactly the host's content. |
| `test_crash.sh` | power loss at a random write; every write since the last flush is kept or dropped at random (4 KiB granularity). Then recovery by l2m and, separately, by e2fsck must give a clean file system, both recoveries must agree, and the content must equal the state after some prefix of the operations, with exactly the exceptions of Linux' ordered mode: the file of the operation in flight may differ, and files that operations after the last commit overwrote in place may already show new content (with size, mode and everything else of the matched state), because several operations share one transaction and in-place data is not journaled. |
| `stress.c` | 8 threads create/write/verify/rename/truncate/link/delete in parallel while another lists directories, the volume runs full repeatedly; half of the files are written through the kernel-offloaded I/O calls; then e2fsck. Also run under ThreadSanitizer (no races) and AddressSanitizer/UBSan. |
| `bigfile.c` | big files on a fragmented volume (filled with small files, every second one deleted), so a file needs thousands of extents and a depth-2 extent tree. Written like the macOS kernel does with kernel-offloaded I/O: map for writing (unwritten extents), put the data on the device, report the range as written — front to back, after a preallocation, in random order, and with big mappings completed in smaller pieces. A `boundary` mode builds 28 targeted cases where data lands at the start, middle or end of a long unwritten extent in a full leaf. Read-back must match, `l2m verify` and e2fsck must be clean. |
| `koio.c` | browser-style downloads through the macOS kernel-offloaded I/O protocol as observed with FSKit on macOS 26: a simulated kernel keeps a page cache for four files growing at the same time and writes it out in numbered operations (write-behind of 64 MB windows, syncs up to the end of the file, 1 MB pageouts, a final flush), maps at most 16 extents per call like FSKit's packer, writes straight onto the device and reports one summary range per operation (operation 0 for writes through kept mappings). Modes: `download` (the observed protocol), `misreport` (reports that leave out the beginning of what was written), `lost` (reports that never come, failed operations written again later; also with lost reports swept after 0 ms), `stale` (a preallocated file with old data on the disk, written in two places by one operation whose report spans the gap: the gap must read as zeros), `merge` (one file in order: at most one extent per 128 MB). Every file must read back exactly and contain no zero holes, `l2m verify` and e2fsck must be clean. With the conversion of 0.9.2 (exactly the reported range) `misreport` and `lost` leave zero holes and `stale` shows old disk contents. |
| `l2m verify` | besides checksums and extent trees it cross-checks every block the metadata, the journal and the files use against the block bitmaps (like e2fsck pass 5): blocks in use but marked free, blocks used twice, and the inodes concerned. |
| macOS | the core is built on Apple silicon (hardware crc32c) and reads the test images identically; an image written on the Mac is clean in e2fsck and read identically on Linux. |

## Bugs the suites found during development (all fixed)

* htree node checksum computed without the 4-byte zero checksum field → e2fsck "root node invalid".
* rewriting `..` in a renamed indexed directory re-verified the old checksum and fell back to the
  linear-block path, destroying the index tail.
* partial write into a block allocated by the same call did a read-modify-write of old disk data
  instead of zero-filling (stale bytes became visible after a later extend).
* deleting a large file: a commit while freeing blocks could happen after the inode had been taken
  off the orphan list (power loss there left blocks allocated) → the inode now stays on the orphan
  list until everything is freed, like Linux.
* the "journal is empty" superblock written after a checkpoint was not made durable before the
  next transaction reused the log area; a power cut in that window could replay an already
  checkpointed transaction over newer metadata. Now flushed first (jbd2 does the same with FUA).
* left shift of a negative block delta (UBSan).
* not a bug, but a false alarm of the power-cut check (3 Nov 2026, also with 0.9.2): it allowed only the
  file of the operation in flight to differ. Without sync, several operations share one transaction,
  and data written into existing blocks (`write` in place, `truncate` zeroing the tail of the last block)
  can reach the disk before the commit that a power cut then loses — Linux behaves the same. The check
  now gets the number of started operations and allows exactly that: content only, for the files those
  operations overwrote (followed through renames and hard links). The exception for a half-done
  operation got narrower instead: a `put`, which syncs at its end, only while it is in flight.
  `crash_match.py --selftest` covers the rules, and a deliberately broken build that reuses blocks freed
  in the running transaction is still caught.
* **found on a real disk (1 Nov 2026), not by the suites:** converting part of an unwritten extent to written data needs up to two more entries in its leaf. When the leaf was full and the extent was its last entry, the leaf was split without moving anything and the new, empty leaf was indexed at the write position, i.e. inside the extent. The conversion then saw a hole, the data stayed "unwritten" (read as zeros) and the tree was inconsistent. It showed up when the Finder copied a 10 GB file onto a fragmented 4 TB disk. Now the leaf is split at an extent boundary (the extent moves along if it is the last one), `split_node` refuses to start an empty leaf inside an extent, and `bigfile.c` covers big files, full leaves and partial completions.
* **found on a real disk (3 Nov 2026), not by the suites:** 4 of 44 browser downloads of 10 GB each
  had one piece of 80–128 KB that read as zeros: an unwritten extent ending exactly at a 64 MB
  boundary, with the right data in its blocks. The I/O trace of the extension showed how macOS
  reports kernel writes: one summary range per operation (first page to end of file), spanning
  gaps between the mapped ranges, writes through kept mappings reported as operation 0. 0.9.2
  converted exactly the reported range, so any data a report left out stayed unwritten, and a
  report spanning space reserved on Linux would have shown old disk contents. The core now
  converts what it handed to each operation (`l2m_koio_*`), and `koio.c` simulates the protocol
  including imperfect reports.
