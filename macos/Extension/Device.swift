//
//  ExtBridge - glue between the C core's block device and FSKit.
//
//  All I/O uses the resource's direct read/write calls (the kernel's
//  offloaded file I/O goes straight to the device as well, so everything
//  stays coherent). The kernel buffer cache calls (metadataRead/Write)
//  reject the 1 KiB superblock reads ext4 needs and are not used.
//  Unaligned requests are bounced through sector aligned buffers.
//
//  SPDX-License-Identifier: MIT
//

import FSKit
import L2MCore
import os

let l2mLog = Logger(subsystem: "de.nexami.extbridge", category: "fs")

final class BlockDevice {
    let resource: FSBlockDeviceResource
    let sector: Int
    private(set) var dev = l2m_dev()

    init(_ resource: FSBlockDeviceResource) {
        self.resource = resource
        let pbs = Int(resource.physicalBlockSize)
        let lbs = Int(resource.blockSize)
        sector = max(512, max(pbs, lbs))
        dev.ctx = Unmanaged.passUnretained(self).toOpaque()
        dev.size = resource.blockSize * resource.blockCount
        dev.sector_size = UInt32(sector)
        dev.read = { ctx, off, buf, len in
            BlockDevice.from(ctx).io(off, UnsafeMutableRawPointer(buf!), len, write: false, meta: true)
        }
        dev.write = { ctx, off, buf, len in
            BlockDevice.from(ctx).io(off, UnsafeMutableRawPointer(mutating: buf!), len, write: true, meta: true)
        }
        dev.read_data = { ctx, off, buf, len in
            BlockDevice.from(ctx).io(off, UnsafeMutableRawPointer(buf!), len, write: false, meta: false)
        }
        dev.write_data = { ctx, off, buf, len in
            BlockDevice.from(ctx).io(off, UnsafeMutableRawPointer(mutating: buf!), len, write: true, meta: false)
        }
        dev.flush = { ctx in
            BlockDevice.from(ctx).flush()
        }
    }

    static func from(_ ctx: UnsafeMutableRawPointer?) -> BlockDevice {
        Unmanaged<BlockDevice>.fromOpaque(ctx!).takeUnretainedValue()
    }

    private func errno(of error: Error) -> Int32 {
        let ns = error as NSError
        if ns.domain == NSPOSIXErrorDomain { return Int32(ns.code) }
        return EIO
    }

    private var loggedFallback = false

    /// one direct transfer; returns 0 or a positive errno
    private func transfer(_ off: UInt64, _ p: UnsafeMutableRawPointer, _ len: Int, write: Bool) throws {
        var done = 0
        while done < len {
            let n: Int
            if write {
                n = try resource.write(from: UnsafeRawBufferPointer(start: p + done, count: len - done),
                                       startingAt: off_t(off) + off_t(done), length: len - done)
            } else {
                n = try resource.read(into: UnsafeMutableRawBufferPointer(start: p + done, count: len - done),
                                      startingAt: off_t(off) + off_t(done), length: len - done)
            }
            if n <= 0 { throw POSIXError(.EIO) }
            done += n
        }
    }

    private func raw(_ off: UInt64, _ p: UnsafeMutableRawPointer, _ len: Int, write: Bool, meta: Bool) -> Int32 {
        // The kernel side of FSKit transfers straight into our memory; page
        // aligned buffers are the safe choice, so unaligned callers go
        // through an aligned bounce buffer.
        let aligned = Int(bitPattern: p) % 4096 == 0
        var firstError: Error?
        do {
            if aligned {
                try transfer(off, p, len, write: write)
                return 0
            }
        } catch {
            firstError = error
        }
        let bounce = UnsafeMutableRawPointer.allocate(byteCount: len, alignment: 16384)
        defer { bounce.deallocate() }
        do {
            if write { bounce.copyMemory(from: p, byteCount: len) }
            try transfer(off, bounce, len, write: write)
            if !write { p.copyMemory(from: bounce, byteCount: len) }
            if firstError != nil && !loggedFallback {
                loggedFallback = true
                l2mLog.info("device: aligned bounce buffer worked after a direct transfer failed")
            }
            return 0
        } catch {
            firstError = firstError ?? error
        }
        // last resort: sector by sector (isolates a single bad sector)
        if !write {
            var off2 = off
            var q = p
            var left = len
            let chunk = sector
            let one = UnsafeMutableRawPointer.allocate(byteCount: chunk, alignment: 16384)
            defer { one.deallocate() }
            do {
                while left > 0 {
                    let n = min(chunk, left)
                    try transfer(off2, one, n, write: false)
                    q.copyMemory(from: one, byteCount: n)
                    q += n; off2 += UInt64(n); left -= n
                }
                l2mLog.info("device: read at \(off) (\(len) bytes) only worked sector by sector")
                return 0
            } catch {
                firstError = error
            }
        }
        let e = firstError ?? POSIXError(.EIO)
        let ns = e as NSError
        l2mLog.error("device \(write ? "write" : "read", privacy: .public) at \(off) (\(len) bytes) failed: \(ns.domain, privacy: .public) \(ns.code) \(ns.localizedDescription, privacy: .public)")
        return errno(of: e)
    }

    /// Returns 0 or -errno (the convention of the C core).
    func io(_ off: UInt64, _ p: UnsafeMutableRawPointer, _ len: Int, write: Bool, meta: Bool) -> Int32 {
        let s = UInt64(sector)
        if off % s == 0 && UInt64(len) % s == 0 {
            return -raw(off, p, len, write: write, meta: meta)
        }
        // bounce: cover the request with whole sectors
        let start = off / s * s
        let end = (off + UInt64(len) + s - 1) / s * s
        let blen = Int(end - start)
        let bounce = UnsafeMutableRawPointer.allocate(byteCount: blen, alignment: 4096)
        defer { bounce.deallocate() }
        var r = raw(start, bounce, blen, write: false, meta: meta)
        if r != 0 { return -r }
        if write {
            bounce.advanced(by: Int(off - start)).copyMemory(from: p, byteCount: len)
            r = raw(start, bounce, blen, write: true, meta: meta)
        } else {
            p.copyMemory(from: bounce.advanced(by: Int(off - start)), byteCount: len)
        }
        return -r
    }

    func flush() -> Int32 {
        // FSKit has no explicit cache flush for direct writes; the calls are
        // synchronous (the buffer cache API is not used, see above).
        return 0
    }
}
