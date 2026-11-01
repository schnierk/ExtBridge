//
//  ExtBridge - the FSKit module: recognises ext2/3/4 disks and creates
//  volumes for them.
//
//  SPDX-License-Identifier: MIT
//

import FSKit
import L2MCore

final class Ext4FileSystem: FSUnaryFileSystem, FSUnaryFileSystemOperations {

    private var volume: Ext4Volume?
    /// last block device we saw (needed for Disk Utility's First Aid)
    private var lastResource: FSBlockDeviceResource?

    override init() {
        super.init()
        l2m_set_log { level, msg in
            guard let msg else { return }
            let s = String(cString: msg)
            switch level {
            case 0: l2mLog.error("\(s, privacy: .public)")
            case 1: l2mLog.warning("\(s, privacy: .public)")
            default: l2mLog.info("\(s, privacy: .public)")
            }
        }
    }

    // MARK: probe

    func probeResource(resource: FSResource, replyHandler reply: @escaping (FSProbeResult?, Error?) -> Void) {
        guard let blk = resource as? FSBlockDeviceResource else {
            reply(.notRecognized, nil)
            return
        }
        lastResource = blk
        let dev = BlockDevice(blk)
        var d = dev.dev
        var label = [CChar](repeating: 0, count: 17)
        var uuid = [CChar](repeating: 0, count: 37)
        var size: UInt64 = 0
        let r = l2m_probe(&d, &label, &uuid, &size)
        guard r == 1, let u = UUID(uuidString: String(cString: uuid)) else {
            reply(.notRecognized, nil)
            return
        }
        var name = String(cString: label)
        if name.isEmpty { name = "Linux" }
        let prefs = L2MShared.loadPrefs()
        let uuidString = String(cString: uuid)
        let container = FSContainerIdentifier(uuid: u)
        if prefs.prefs(for: uuidString).noAutoMount && !prefs.mountRequests.contains(uuidString) {
            // known, but the user does not want it mounted automatically
            reply(.recognized(name: name, containerID: container), nil)
            return
        }
        reply(.usable(name: name, containerID: container), nil)
    }

    // MARK: load / unload

    func loadResource(resource: FSResource, options: FSTaskOptions, replyHandler reply: @escaping (FSVolume?, Error?) -> Void) {
        guard let blk = resource as? FSBlockDeviceResource else {
            reply(nil, POSIXError(.EINVAL))
            return
        }
        lastResource = blk
        // Safety: until the app has saved settings (or if they cannot be
        // read) every volume is mounted read-only.
        let stored = L2MShared.storedPrefs()
        var prefs = stored ?? L2MPrefs()
        if stored == nil {
            l2mLog.warning("no settings from the app yet: mounting read-only")
        }
        let dev = BlockDevice(blk)
        var d = dev.dev
        var uuidBuf = [CChar](repeating: 0, count: 37)
        _ = l2m_probe(&d, nil, &uuidBuf, nil)
        let uuid = String(cString: uuidBuf)
        let vp = prefs.prefs(for: uuid)
        // one-shot manual mount request is used up now
        if let i = prefs.mountRequests.firstIndex(of: uuid) {
            prefs.mountRequests.remove(at: i)
            L2MShared.savePrefs(prefs)
        }
        // "-r", "-o rdonly" / "-o ro" (mount -r, diskutil mount readOnly)
        let opts = options.taskOptions
        let optionReadOnly = opts.contains { $0 == "-r" || $0 == "rdonly" || $0 == "ro" || $0.contains("rdonly") }
        l2mLog.info("load \(blk.bsdName, privacy: .public) options: \(opts.joined(separator: " "), privacy: .public)")
        // the check right before this mount found problems: read-only (macOS
        // already passes a read-only device then; this is a second guard)
        let checkProblems = CheckMarker.recentProblems(uuid: uuid) && !prefs.hasCheckException(uuid)
        if checkProblems {
            l2mLog.warning("check found problems on \(blk.bsdName, privacy: .public): mounting read-only")
        }
        let readOnly = stored == nil || vp.readOnly || optionReadOnly || !blk.isWritable || checkProblems
        if readOnly {
            l2mLog.info("read-only because: settings \(stored == nil ? "missing" : "ok", privacy: .public), setting \(vp.readOnly, privacy: .public), option \(optionReadOnly, privacy: .public), writable device \(blk.isWritable, privacy: .public), check \(checkProblems, privacy: .public)")
        }
        var flags: UInt32 = 0
        if readOnly { flags |= UInt32(L2M_MOUNT_RDONLY) }
        var fs: OpaquePointer?
        var err = [CChar](repeating: 0, count: 256)
        let r = l2m_mount(&d, flags, &fs, &err, err.count)
        guard r == 0, let fs else {
            let msg = String(cString: err)
            l2mLog.error("mount of \(blk.bsdName, privacy: .public) failed: \(msg, privacy: .public) (\(r))")
            reply(nil, POSIXError(POSIXErrorCode(rawValue: -r) ?? .EIO))
            return
        }
        let vol = Ext4Volume(fs: fs, device: dev, bsdName: blk.bsdName, prefs: prefs, requestedReadOnly: readOnly,
                             readOnlyNote: checkProblems && !vp.readOnly && !optionReadOnly ? "check" : nil)
        volume = vol
        containerStatus = .ready
        reply(vol, nil)
    }

    func unloadResource(resource: FSResource, options: FSTaskOptions, replyHandler reply: @escaping (Error?) -> Void) {
        volume?.shutdown()
        volume = nil
        reply(nil)
    }

    func didFinishLoading() {}

    /// Whether the checked disk may be mounted despite problems: it will be
    /// mounted read-only anyway (its setting, or no settings at all), or the
    /// user made an exception for it.
    func mayMountDespiteProblems(_ resource: FSBlockDeviceResource?) -> Bool {
        guard let res = resource else { return false }
        // (the device of a check is always read-only, so that says nothing)
        guard let prefs = L2MShared.storedPrefs() else { return true }
        guard let u = volumeUUID(res) else { return false }
        return prefs.prefs(for: u).readOnly || prefs.hasCheckException(u)
    }

    func volumeUUID(_ resource: FSBlockDeviceResource?) -> String? {
        guard let res = resource else { return nil }
        let dev = BlockDevice(res)
        var d = dev.dev
        var uuid = [CChar](repeating: 0, count: 37)
        guard l2m_probe(&d, nil, &uuid, nil) == 1 else { return nil }
        return withExtendedLifetime(dev) { String(cString: uuid) }
    }

    func verifyMounted(_ body: (OpaquePointer) -> Void) -> Bool {
        guard let v = volume else { return false }
        v.withCore(body)
        return true
    }
}

// MARK: Disk Utility "First Aid" (fsck_fskit -t extbridge)

/// The result of the last check of a volume, so the mount that follows the
/// check (macOS checks before mounting) can fall back to read-only.
enum CheckMarker {
    private struct Entry: Codable {
        var date: Date
        var problems: Bool
    }

    private static func url(_ uuid: String) -> URL? {
        L2MShared.dir("results")?.appendingPathComponent("\(uuid).check.json")
    }

    static func record(uuid: String, problems: Bool) {
        guard let u = url(uuid) else { return }
        L2MShared.writeJSON(Entry(date: Date(), problems: problems), to: u)
    }

    /// Problems found by a check in the last few minutes.
    static func recentProblems(uuid: String) -> Bool {
        guard let u = url(uuid), let e = L2MShared.readJSON(Entry.self, from: u) else { return false }
        return e.problems && Date().timeIntervalSince(e.date) < 600
    }
}

private final class CheckContext {
    let progress: Progress
    init(_ p: Progress) { progress = p }
}

extension Ext4FileSystem: FSManageableResourceMaintenanceOperations {

    func startCheck(task: FSTask, options: FSTaskOptions) throws -> Progress {
        let progress = Progress(totalUnitCount: 1000)
        let resource = lastResource
        DispatchQueue.global(qos: .userInitiated).async { [self] in
            task.logMessage("ExtBridge: Das ext4-Dateisystem wird geprüft (nur lesend, es wird nichts verändert) …")
            var report = [CChar](repeating: 0, count: 1 << 16)
            let ctx = CheckContext(progress)
            let box = Unmanaged.passRetained(ctx)
            var result: Int32 = -1
            let run: (OpaquePointer) -> Void = { fs in
                result = l2m_verify(fs, { c, done, total in
                    let ctx = Unmanaged<CheckContext>.fromOpaque(c!).takeUnretainedValue()
                    if total > 0 { ctx.progress.completedUnitCount = Int64(Double(done) / Double(total) * 1000) }
                    return ctx.progress.isCancelled ? 1 : 0
                }, box.toOpaque(), &report, report.count)
            }
            if !self.verifyMounted(run) {
                if let res = resource {
                    // not mounted: open it read-only just for the check
                    let dev = BlockDevice(res)
                    var d = dev.dev
                    var fs: OpaquePointer?
                    var err = [CChar](repeating: 0, count: 256)
                    let r = l2m_mount(&d, UInt32(L2M_MOUNT_RDONLY), &fs, &err, err.count)
                    if r == 0, let fs {
                        run(fs)
                        l2m_umount(fs)
                    } else {
                        task.logMessage("ExtBridge: Das Laufwerk kann nicht geöffnet werden: \(String(cString: err))")
                        result = r
                    }
                    withExtendedLifetime(dev) {}
                }
            }
            box.release()
            let text = String(cString: report)
            for line in text.split(separator: "\n") {
                task.logMessage(String(line))
            }
            // keep the last report for the app and for support questions
            if let dir = L2MShared.dir("results") {
                let stamp = ISO8601DateFormatter().string(from: Date())
                try? "\(stamp) result \(result)\n\(text)".write(to: dir.appendingPathComponent("last-check.txt"),
                                                              atomically: true, encoding: .utf8)
            }
            progress.completedUnitCount = 1000
            let uuid = self.volumeUUID(resource)
            if let uuid { CheckMarker.record(uuid: uuid, problems: result > 0) }
            // macOS checks before mounting ("-q", then "-y" to repair). When
            // the check fails it mounts the disk read-only by itself, so
            // nobody is locked out. A disk that stays read-only anyway, or
            // that the user made an exception for, passes.
            if result > 0 && self.mayMountDespiteProblems(resource) {
                task.logMessage("ExtBridge: Das Laufwerk wird trotzdem eingebunden (nur lesend oder ausdrücklich erlaubt).")
                task.didComplete(error: nil)
            } else if result == 0 {
                task.didComplete(error: nil)
            } else if result > 0 {
                task.logMessage("ExtBridge: Bitte das Laufwerk unter Linux mit „e2fsck -f“ reparieren.")
                task.didComplete(error: POSIXError(.EIO))
            } else {
                task.didComplete(error: POSIXError(POSIXErrorCode(rawValue: -result) ?? .EIO))
            }
        }
        return progress
    }

    func startFormat(task: FSTask, options: FSTaskOptions) throws -> Progress {
        // Creating ext4 file systems is left to Linux (mkfs.ext4).
        throw POSIXError(.ENOTSUP)
    }
}
