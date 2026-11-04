# ExtBridge – User guide

ExtBridge lets you use Linux disks (ext2, ext3, ext4) on your Mac like any other disk: the disk
shows up in Finder and you can read **and** write.

*Deutsche Fassung: [ANLEITUNG.md](ANLEITUNG.md)*

## Requirements

* macOS 15.4 or later (Apple silicon or Intel)
* a disk, USB stick or SD card with a Linux file system (ext2/ext3/ext4)

## Installation

1. Download the latest `ExtBridge-….dmg` from the
   [releases page](https://github.com/schnierk/ExtBridge/releases/latest) and open it.
2. Drag **ExtBridge** into **Applications**.
3. Open ExtBridge from the Applications folder.
4. The app says "Almost done!". Click **Open System Settings**, choose
   **By Category → File System Extensions** (ⓘ) and turn on **ExtBridge ext4**.
   You only need to do this once.
5. Attach your Linux disk. It appears in Finder and in the app.

## Everyday use

* In **Finder** you work with the disk like with any other: open, copy, rename, delete.
* **Always eject before unplugging** (Finder: ⏏ next to the name, or in the app).
* For every disk the **app** shows:
  * **Overview** – usage, health, journal, mode (read-only or read & write)
  * **Diagnostics** – a file system check that changes nothing, and a speed test. The check also
    lists files with *zero holes*: a piece of the file reads as zeros where data belongs.
    ExtBridge 0.9.2 could leave such pieces in very large files written on the Mac – copy or
    download those files again.
  * **Information** – all technical details
  * **Settings** – *Always mount read-only*, *Do not mount automatically*
* A small drive icon in the **menu bar** mounts and ejects quickly.
* **ExtBridge → Settings** (⌘ ,) decides who owns new files (Linux user ↔ Mac user), whether
  Mac helper files such as `.DS_Store` may be written to the disk, and whether ExtBridge starts
  **at login**.

## Safety

* Every change goes through the **journal** first, exactly like on Linux. If power fails or the
  cable is pulled, the file system stays consistent.
* Before macOS mounts a disk, ExtBridge checks it. If the check finds problems, the disk is mounted
  **read-only**: your files stay accessible but nothing is changed. Repair such a disk on Linux
  with `e2fsck -f`.
* Disks that use Linux features ExtBridge can only read are mounted read-only as well. The app
  always tells you why.
* For precious collections, turn on *Always mount read-only* in the app.
* ExtBridge is a **beta**. Keep a backup of important data.

## Troubleshooting

* **The disk does not show up:** the app must say "Driver active" (bottom left). Otherwise turn
  the extension on as in step 4.
* **The disk was attached while the Mac started:** macOS checks it before ExtBridge runs. Unplug
  and re-attach it once after logging in; the app reminds you.
* **After an update** the extension is sometimes switched off again – turn it on as during
  installation.
* **"Restart your Mac"**: when macOS still remembers an old mount, only a restart helps. The app
  tells you.
* **Log** (for bug reports, in Terminal):
  `log stream --predicate 'subsystem == "de.nexami.extbridge"' --level info`

Report bugs: [issues](https://github.com/schnierk/ExtBridge/issues)

## Support

ExtBridge is free and open source. If it helps you, I'd appreciate a coffee – with the button in
the app or directly: [PayPal](https://paypal.me/popierk) · [Card / Apple Pay](https://buy.stripe.com/aFa00l8Rm1Hl5wbauk1B601)
