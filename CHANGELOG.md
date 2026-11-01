# Changelog

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
