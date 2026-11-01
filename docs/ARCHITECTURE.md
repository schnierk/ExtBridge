# Architecture

```
 Finder / apps
      │  POSIX calls
 macOS kernel (VFS) ──────── file data: read/write directly to the disk ───────┐
      │  FSKit XPC (metadata only)                                              │
 ExtBridgeFS.appex  (Swift, sandboxed)                                          │
      │  C calls                                                                │
 libl2m (C core) ── metadata via kernel buffer cache ── FSBlockDeviceResource ──┘
```

## Layers

| Layer | Files | Job |
|---|---|---|
| On-disk format | `core/src/ext4_disk.h` | ext4 + jbd2 structures, verified with `_Static_assert` |
| Checksums | `crc.c`, `hash.c` | crc32c (ARMv8 / SSE4.2 hardware, slicing-by-8 fallback), crc16, htree hashes |
| Block cache | `cache.c` | metadata only; dirty buffers belong to the running transaction |
| Mount / groups | `super.c` | geometry checks, feature policy (rw / ro / refuse), group descriptors, bitmaps incl. `BLOCK_UNINIT` |
| Journal | `journal.c` | jbd2 recovery (scan, revoke, replay; csum v2/v3) and transaction commit |
| Inodes | `inode.c` | inode table, checksums, 34-bit timestamps with nanoseconds |
| Block maps | `extent.c` | extent trees (lookup, insert with merge/split/grow, truncate, unwritten → written), legacy indirect read |
| Allocation | `alloc.c` | goal-based first fit for long contiguous extents; inode allocation spreading top-level dirs |
| Directories | `dir.c` | linear + htree lookup, insert with leaf/index splits and level growth, conversion to htree, inline dirs (read) |
| Xattrs | `xattr.c` | in-inode and block attributes, copy-on-write for shared blocks |
| API | `ops.c`, `l2m.h` | the operations FSKit needs; one lock per volume |
| Diagnostics | `verify.c` | read-only consistency scan used by the app |

## Write safety

1. Every metadata change is made in cached buffers that join the *running transaction*.
2. A transaction is committed after an operation when it is 5 s old or large, on `sync` and unmount:
   * write a descriptor block + copies of all changed blocks to the journal, **flush**;
   * write the commit block and the journal superblock (`s_start`), **flush** – now durable;
   * write the blocks to their home locations ("checkpoint"), **flush**;
   * mark the journal empty again.
3. A crash before step 2's second flush leaves the old state, afterwards Linux, e2fsck or
   ExtBridge replays the transaction. The on-disk format is exactly jbd2, so Linux can do it.
4. Blocks freed in a transaction are not reused before it is committed – otherwise new data could
   land in a block the still-valid old metadata points to.
5. File data is written before the metadata that references it is committed (ordered mode).
   Space handed to the kernel for writing is allocated as *unwritten* extents and only marked
   written in `completeIO` after the kernel wrote the data – no stale data can appear after a crash.
6. Unlinked-but-open files and interrupted truncates are on the ext4 orphan list until finished.
7. If the device reports a write error the volume switches to read-only immediately.

## Read-only safety

* A read-only mount never calls the write function (`dev_write_blocks` refuses and logs a bug).
* A journal that needs recovery is replayed into *overlay buffers* in memory.
* Checksums are verified on every metadata read; corrupt structures return errors instead of guesses.

## FSKit glue (`macos/Extension`)

* `FileSystem.swift` – probe (superblock check, `usable` / `recognized`), load (mount the core,
  read-only decisions), unload.
* `Volume.swift` – `FSVolume` with Operations, PathConf, ReadWrite (fallback), Xattr and
  KernelOffloadedIO (`blockmapFile`, `completeIO`, extent-supplying `lookupItem`/`createFile`).
* `Device.swift` – maps the core's device callbacks to `FSBlockDeviceResource`; metadata through
  `metadataRead/Write` (kernel buffer cache), file data through `read/write`; bounce buffers for
  unaligned requests.
* App ↔ extension communication: JSON files in the shared app group container
  (`prefs.json`, `status/<uuid>.json`, `commands/`, `results/`).
