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
| `test_crash.sh` | power loss at a random write; every write since the last flush is kept or dropped at random (4 KiB granularity). Then recovery by l2m and, separately, by e2fsck must give a clean file system, both recoveries must agree, and the content must equal the state after some prefix of the operations (only the file of the single operation in flight may differ, as with Linux' ordered mode). |
| `stress.c` | 8 threads create/write/verify/rename/truncate/link/delete in parallel while another lists directories, the volume runs full repeatedly; then e2fsck. Also run under ThreadSanitizer (no races) and AddressSanitizer/UBSan. |
| `bigfile.c` | big files on a fragmented volume (filled with small files, every second one deleted), so a file needs thousands of extents and a depth-2 extent tree. Written like the macOS kernel does with kernel-offloaded I/O: map for writing (unwritten extents), put the data on the device, report the range as written — front to back, after a preallocation, in random order, and with big mappings completed in smaller pieces. A `boundary` mode builds 28 targeted cases where data lands at the start, middle or end of a long unwritten extent in a full leaf. Read-back must match, `l2m verify` and e2fsck must be clean. |
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
* **found on a real disk (1 Nov 2026), not by the suites:** converting part of an unwritten extent to written data needs up to two more entries in its leaf. When the leaf was full and the extent was its last entry, the leaf was split without moving anything and the new, empty leaf was indexed at the write position, i.e. inside the extent. The conversion then saw a hole, the data stayed "unwritten" (read as zeros) and the tree was inconsistent. It showed up when the Finder copied a 10 GB file onto a fragmented 4 TB disk. Now the leaf is split at an extent boundary (the extent moves along if it is the last one), `split_node` refuses to start an empty leaf inside an extent, and `bigfile.c` covers big files, full leaves and partial completions.
