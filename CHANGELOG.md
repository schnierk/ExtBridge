# Changelog

## 0.9.3

* **Fixed: zero holes in big files.** When macOS writes file data itself (kernel-offloaded I/O),
  it reports every finished write operation with one byte range. ExtBridge 0.9.2 marked exactly
  that range as written. The range is only a summary, though: it can span gaps and, rarely, it
  missed data the operation had written. That data then stayed marked "unwritten" and read back
  as zeros, although the right bytes were on the disk. Seen 4 times in 44 browser downloads of
  10 GB each, always a piece of 80–128 KB that ended exactly at a 64 MB boundary.
  Now the core remembers which blocks it handed to which write operation and converts exactly
  those when the operation reports back, wherever its reported range lies. Blocks the kernel
  wrote without any report are converted when they are a minute old, at the latest when the file
  is closed for good or the disk is ejected. Space reserved on Linux (fallocate) is no longer
  marked written just because a reported range spans it, so old disk contents never show up in
  a file. New test program `tests/koio.c` simulates the macOS protocol, including the imperfect
  reports; with the logic of 0.9.2 it finds the zero holes.
* **Fewer, longer extents:** data written piece by piece joins its neighbour extent when the two
  are adjacent on the disk, as on Linux. A 10 GB download had about 1,400 extents with 0.9.2
  (e2fsck: "extent tree could be narrower"), now it gets a few dozen to a few hundred, depending
  on how many files are written at the same time.
* **Consistency check finds zero holes:** it lists files with zero holes by path, so files damaged
  by 0.9.2 can be copied again. `l2m IMAGE zeroholes` does the same from the command line,
  `l2m IMAGE extents PATH` lists the extents of a file, and `l2m` now opens whole-disk images with
  a GPT partition table directly.
* Read mappings no longer pass the disk position of space that reads as zeros.
* An I/O trace for debugging (off by default): while the file `commands/trace-io` exists in the
  app's group container, every mapping, completion and size change goes to `trace/`.

## 0.9.2

* **Speed test measures the disk, not the app:** the test no longer waits for the gauges after
  every 8 MB (they are updated a few times a second), which could make fast disks look much
  slower (81 MB/s shown for a disk that writes about 800 MB/s). With the write test, reading uses the freshly written 1 GB test file, so
  the read value no longer depends on finding a big file; without it, the app looks for a big
  file for up to 4 seconds and says when it only found small ones. The test waits while a
  consistency check is running on the disk.

## 0.9.1 – first public beta

* **Fixed:** writing into a long unwritten extent that was the last entry of a full extent leaf
  split the leaf inside that extent. The data then stayed marked "unwritten" (read back as zeros)
  and the extent tree became inconsistent. It needed a big file (thousands of extents) on a disk
  with fragmented free space, e.g. copying a 10 GB file in Finder. New test program
  `tests/bigfile.c` covers big files, full leaves and partial completions.
* The check before mounting no longer locks you out: a disk with problems is mounted read-only and
  the app says why. The consistency check now cross-checks every used block against the block
  bitmaps and describes damaged extent trees.
* Settings written by older versions are read field by field (a missing key no longer makes every
  disk read-only).
* "Buy me a coffee" offers PayPal or card/Apple Pay.

## 0.9.0

* First complete version: ext2/3/4 read, ext4 read/write with journal, FSKit module with kernel
  offloaded I/O, SwiftUI app with diagnostics, Disk Utility First Aid, English and German UI.
