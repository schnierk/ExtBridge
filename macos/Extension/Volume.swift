//
//  ExtBridge - an ext4 volume for FSKit.
//
//  All file system logic lives in the C core (libl2m); this class only
//  translates between FSKit's objects and the core's inode numbers.
//  File contents are moved by the kernel itself ("kernel offloaded I/O"):
//  FSKit asks blockmapFile() where a file's bytes live on the disk and then
//  reads/writes them directly, which is what makes it as fast as a native
//  volume.
//
//  SPDX-License-Identifier: MIT
//

import FSKit
import L2MCore

final class Ext4Item: FSItem {
    let ino: UInt32
    var parent: UInt32
    init(ino: UInt32, parent: UInt32) {
        self.ino = ino
        self.parent = parent
        super.init()
    }
}

@inline(__always) func posix(_ r: Int32) -> Error {
    var code = -r
    if code == ENODATA { code = ENOATTR }
    if code == 117 /* EUCLEAN */ { code = EIO }
    return POSIXError(POSIXErrorCode(rawValue: code) ?? .EIO)
}

@inline(__always) func posix(_ r: Int64) -> Error { posix(Int32(truncatingIfNeeded: r)) }

/// true if an `any Error` coming from Objective-C actually holds nil
@inline(never) func isNilError(_ e: Error) -> Bool {
    MemoryLayout<Error>.size == MemoryLayout<UInt>.size && withUnsafeBytes(of: e) { $0.load(as: UInt.self) } == 0
}

private func tupleString<T>(_ t: T) -> String {
    withUnsafeBytes(of: t) { raw in
        let bytes = raw.bindMemory(to: UInt8.self)
        let n = bytes.firstIndex(of: 0) ?? bytes.count
        return String(decoding: bytes[0..<n], as: UTF8.self)
    }
}

final class Ext4Volume: FSVolume, FSVolume.Operations, FSVolume.PathConfOperations,
                        FSVolume.ReadWriteOperations, FSVolume.XattrOperations,
                        FSVolume.RenameOperations, FSVolume.ItemDeactivation,
                        FSVolumeKernelOffloadedIOOperations {

    private let fs: OpaquePointer
    private let device: BlockDevice
    private let bsdName: String
    private var prefs: L2MPrefs
    private let uuid: String
    private let blockSize: UInt32
    private var items: [UInt32: Ext4Item] = [:]
    private let itemLock = NSLock()
    private var timer: DispatchSourceTimer?
    private let background = DispatchQueue(label: "extbridge.background", qos: .utility)
    private var verifying = false
    private var closed = false

    /// Why the volume is read-only when the core does not know it ("check":
    /// the check before the mount found problems).
    private let readOnlyNote: String?

    init(fs: OpaquePointer, device: BlockDevice, bsdName: String, prefs: L2MPrefs, requestedReadOnly: Bool,
         readOnlyNote: String? = nil) {
        self.readOnlyNote = readOnlyNote
        self.fs = fs
        self.device = device
        self.bsdName = bsdName
        self.prefs = prefs
        var info = l2m_info()
        l2m_get_info(fs, &info)
        uuid = tupleString(info.uuid)
        blockSize = info.block_size
        var label = tupleString(info.label)
        if label.isEmpty { label = "Linux" }
        super.init(volumeID: FSVolume.Identifier(uuid: UUID(uuidString: uuid) ?? UUID()),
                   volumeName: FSFileName(string: label))
        // tells FSKit (macOS 26.4+) that the mount is read-only, so Finder
        // does not offer to write
        if l2m_is_readonly(fs) != 0 { mountOptionsRaw = 1 }
    }

    private var mountOptionsRaw: UInt = 0
    @available(macOS 26.4, *)
    var requestedMountOptions: FSVolume.MountOptions {
        get { FSVolume.MountOptions(rawValue: mountOptionsRaw) }
        set { mountOptionsRaw = newValue.rawValue }
    }

    var isReadOnly: Bool { l2m_is_readonly(fs) != 0 }

    func withCore(_ body: (OpaquePointer) -> Void) { body(fs) }

    // MARK: items

    private func item(_ ino: UInt32, parent: UInt32) -> Ext4Item {
        itemLock.lock()
        defer { itemLock.unlock() }
        if let it = items[ino] {
            if parent != 0 { it.parent = parent }
            return it
        }
        let it = Ext4Item(ino: ino, parent: parent)
        items[ino] = it
        return it
    }

    private func ino(_ item: FSItem) -> UInt32 {
        (item as? Ext4Item)?.ino ?? UInt32(L2M_ROOT_INO)
    }

    private func nameBytes(_ name: FSFileName) -> [CChar] {
        name.data.map { CChar(bitPattern: $0) }
    }

    // MARK: ownership mapping

    private func toMac(uid: UInt32) -> UInt32 { prefs.mapOwners && uid == prefs.linuxUID ? prefs.macUID : uid }
    private func toMac(gid: UInt32) -> UInt32 { prefs.mapOwners && gid == prefs.linuxGID ? prefs.macGID : gid }
    private func toLinux(uid: UInt32) -> UInt32 { prefs.mapOwners && uid == prefs.macUID ? prefs.linuxUID : uid }
    private func toLinux(gid: UInt32) -> UInt32 { prefs.mapOwners && gid == prefs.macGID ? prefs.linuxGID : gid }

    // MARK: attributes

    private func type(of mode: UInt32) -> FSItem.ItemType {
        switch mode & 0o170000 {
        case 0o100000: return .file
        case 0o040000: return .directory
        case 0o120000: return .symlink
        case 0o010000: return .fifo
        case 0o020000: return .charDevice
        case 0o060000: return .blockDevice
        case 0o140000: return .socket
        default: return .unknown
        }
    }

    private func type(ofDtype t: UInt32) -> FSItem.ItemType {
        switch Int32(t) {
        case L2M_DT_REG: return .file
        case L2M_DT_DIR: return .directory
        case L2M_DT_LNK: return .symlink
        case L2M_DT_FIFO: return .fifo
        case L2M_DT_CHR: return .charDevice
        case L2M_DT_BLK: return .blockDevice
        case L2M_DT_SOCK: return .socket
        default: return .unknown
        }
    }

    private static let EXT4_IMMUTABLE_FL: UInt32 = 0x10
    private static let EXT4_APPEND_FL: UInt32 = 0x20
    private static let EXT4_NODUMP_FL: UInt32 = 0x40
    private static let EXT4_EXTENTS_FL: UInt32 = 0x80000
    private static let EXT4_INLINE_DATA_FL: UInt32 = 0x10000000
    private static let EXT4_ENCRYPT_FL: UInt32 = 0x800
    private static let EXT4_VERITY_FL: UInt32 = 0x100000

    private func attributes(_ st: l2m_stat, parent: UInt32) -> FSItem.Attributes {
        let a = FSItem.Attributes()
        a.type = type(of: st.mode)
        a.mode = st.mode & 0o7777
        a.linkCount = st.nlink
        a.uid = toMac(uid: st.uid)
        a.gid = toMac(gid: st.gid)
        a.size = st.size
        a.allocSize = st.alloc
        a.fileID = FSItem.Identifier(rawValue: UInt64(st.ino))!
        if parent != 0 {
            a.parentID = FSItem.Identifier(rawValue: st.ino == UInt32(L2M_ROOT_INO) ? 1 : UInt64(parent))!
        }
        var bsd: UInt32 = 0
        if st.flags & Self.EXT4_IMMUTABLE_FL != 0 { bsd |= UInt32(UF_IMMUTABLE) }
        if st.flags & Self.EXT4_APPEND_FL != 0 { bsd |= UInt32(UF_APPEND) }
        if st.flags & Self.EXT4_NODUMP_FL != 0 { bsd |= UInt32(UF_NODUMP) }
        a.flags = bsd
        a.accessTime = timespec(tv_sec: Int(st.atime.sec), tv_nsec: Int(st.atime.nsec))
        a.modifyTime = timespec(tv_sec: Int(st.mtime.sec), tv_nsec: Int(st.mtime.nsec))
        a.changeTime = timespec(tv_sec: Int(st.ctime.sec), tv_nsec: Int(st.ctime.nsec))
        a.birthTime = timespec(tv_sec: Int(st.crtime.sec), tv_nsec: Int(st.crtime.nsec))
        // files the kernel cannot read straight from the disk
        let special = Self.EXT4_INLINE_DATA_FL | Self.EXT4_ENCRYPT_FL | Self.EXT4_VERITY_FL
        let noExtents = st.flags & Self.EXT4_EXTENTS_FL == 0 && !isReadOnly
        a.inhibitKernelOffloadedIO = (st.flags & special) != 0 || (a.type == .file && noExtents)
        a.supportsLimitedXAttrs = false
        return a
    }

    private func stat(_ ino: UInt32) throws -> l2m_stat {
        var st = l2m_stat()
        let r = l2m_getattr(fs, ino, &st)
        if r != 0 { throw posix(r) }
        return st
    }

    // MARK: capabilities

    var supportedVolumeCapabilities: FSVolume.SupportedCapabilities {
        let c = FSVolume.SupportedCapabilities()
        var info = l2m_info()
        l2m_get_info(fs, &info)
        c.supportsPersistentObjectIDs = true
        c.supportsSymbolicLinks = true
        c.supportsHardLinks = true
        c.supportsJournal = info.has_journal != 0
        c.supportsActiveJournal = info.has_journal != 0 && !isReadOnly
        c.supportsSparseFiles = true
        c.supportsZeroRuns = true
        c.supportsFastStatFS = true
        c.supports2TBFiles = true
        c.supports64BitObjectIDs = true
        c.supportsHiddenFiles = false
        c.doesNotSupportImmutableFiles = false
        c.caseFormat = .sensitive
        return c
    }

    var volumeStatistics: FSStatFSResult {
        var st = l2m_statfs()
        l2m_statfs_get(fs, &st)
        let r = FSStatFSResult(fileSystemTypeName: L2MShared.fsShortName)
        r.blockSize = Int(st.block_size)
        r.ioSize = 1 << 20
        r.totalBlocks = st.blocks
        r.availableBlocks = st.bavail
        r.freeBlocks = st.bfree
        r.usedBlocks = st.blocks > st.bfree ? st.blocks - st.bfree : 0
        r.totalFiles = st.files
        r.freeFiles = st.ffree
        return r
    }

    var maximumLinkCount: Int { 65000 }
    var maximumNameLength: Int { 255 }
    var restrictsOwnershipChanges: Bool { true }
    var truncatesLongNames: Bool { false }
    var maximumXattrSize: Int { Int(blockSize) - 64 }
    var maximumFileSize: UInt64 { UInt64(blockSize) * 0xFFFF_FFFF }

    // MARK: volume name

    var isVolumeRenameInhibited: Bool {
        get { isReadOnly }
        set {}
    }

    func setVolumeName(_ name: FSFileName, replyHandler reply: @escaping (FSFileName?, Error?) -> Void) {
        // ext4 labels are at most 16 bytes; cut at a character boundary
        var s = name.string ?? ""
        while s.utf8.count > 16 { s.removeLast() }
        let r = l2m_set_label(fs, s)
        if r != 0 {
            reply(nil, posix(r))
            return
        }
        let n = FSFileName(string: s)
        self.name = n
        publishStatus(mounted: true)
        reply(n, nil)
    }

    // MARK: lifecycle

    func activate(options: FSTaskOptions, replyHandler reply: @escaping (FSItem?, Error?) -> Void) {
        reply(item(UInt32(L2M_ROOT_INO), parent: UInt32(L2M_ROOT_INO)), nil)
    }

    func deactivate(options: FSDeactivateOptions = [], replyHandler reply: @escaping (Error?) -> Void) {
        shutdown()
        reply(nil)
    }

    func mount(options: FSTaskOptions, replyHandler reply: @escaping (Error?) -> Void) {
        startTimer()
        publishStatus(mounted: true)
        l2mLog.info("mounted \(self.bsdName, privacy: .public) \(self.isReadOnly ? "read-only" : "read-write", privacy: .public)")
        reply(nil)
    }

    func unmount(replyHandler reply: @escaping () -> Void) {
        let r = l2m_sync(fs)
        if r != 0 { l2mLog.error("sync at unmount failed: \(r)") }
        timer?.cancel()
        timer = nil
        publishStatus(mounted: false)
        reply()
    }

    func synchronize(flags: FSSyncFlags, replyHandler reply: @escaping (Error?) -> Void) {
        let r = l2m_sync(fs)
        reply(r == 0 ? nil : posix(r))
    }

    /// Final unmount of the core (flushes, marks the volume clean).
    func shutdown() {
        guard !closed else { return }
        closed = true
        timer?.cancel()
        timer = nil
        let r = l2m_umount(fs)
        if r != 0 { l2mLog.error("final unmount reported \(r)") }
        publishStatus(mounted: false, closedState: true)
    }

    private func startTimer() {
        let t = DispatchSource.makeTimerSource(queue: background)
        t.schedule(deadline: .now() + 1, repeating: 1)
        var ticks = 0
        t.setEventHandler { [weak self] in
            guard let self, !self.closed else { return }
            ticks += 1
            // commit like Linux does: at the latest every 5 seconds
            _ = l2m_sync_if_older(self.fs, 5000)
            if ticks % 5 == 0 { self.publishStatus(mounted: true) }
            self.checkCommands()
        }
        t.resume()
        timer = t
    }

    // MARK: status / commands (shared with the app)

    private var statusURL: URL? { L2MShared.dir("status")?.appendingPathComponent("\(uuid).json") }

    private func date(_ t: Int64) -> Date? { t > 0 ? Date(timeIntervalSince1970: TimeInterval(t)) : nil }

    private func roReason(_ r: Int32) -> String {
        switch r {
        case Int32(L2M_RO_NONE): return ""
        case Int32(L2M_RO_REQUESTED): return "requested"
        case Int32(L2M_RO_FEATURE): return "feature"
        case Int32(L2M_RO_ERRORS): return "errors"
        case Int32(L2M_RO_JOURNAL): return "journal"
        case Int32(L2M_RO_NOT_CLEAN): return "notClean"
        default: return "unknown"
        }
    }

    func publishStatus(mounted: Bool, closedState: Bool = false) {
        guard let url = statusURL else { return }
        if closedState {
            if var s = L2MShared.readJSON(L2MVolumeStatus.self, from: url) {
                s.mounted = false
                s.updated = Date()
                L2MShared.writeJSON(s, to: url)
            }
            return
        }
        var i = l2m_info()
        l2m_get_info(fs, &i)
        let state: String
        if i.state & 2 != 0 { state = "errors" } else if i.state & 1 != 0 { state = "clean" } else { state = "notClean" }
        let s = L2MVolumeStatus(
            uuid: uuid, label: tupleString(i.label), bsdName: bsdName, mounted: mounted,
            readOnly: i.readonly != 0, readOnlyReason: i.readonly != 0 ? (readOnlyNote ?? roReason(i.ro_reason)) : "",
            features: tupleString(i.features).split(separator: " ").map(String.init),
            unsupported: tupleString(i.unsupported).split(separator: " ").map(String.init),
            blockSize: i.block_size, blocks: i.blocks_count, freeBlocks: i.free_blocks, reservedBlocks: i.reserved_blocks,
            inodes: i.inodes_count, freeInodes: i.free_inodes, inodeSize: i.inode_size, groups: i.groups,
            state: state, mountCount: UInt32(i.mount_count), maxMountCount: Int32(i.max_mount_count), errorCount: i.error_count,
            created: date(i.mkfs_time), lastMount: date(i.mount_time), lastWrite: date(i.write_time),
            lastCheck: date(i.lastcheck_time), lastError: date(i.last_error_time), kbytesWritten: i.kbytes_written,
            hasJournal: i.has_journal != 0, journalBlocks: i.journal_blocks, journalReplayed: i.journal_needed_recovery != 0,
            journalTransactions: i.journal_replayed_transactions, journalBlocksReplayed: i.journal_replayed_blocks,
            metadataChecksums: i.metadata_csum != 0, hashAlgorithm: tupleString(i.hash_algo),
            lastMountedOn: tupleString(i.last_mounted), bytesReadSinceMount: 0, bytesWrittenSinceMount: 0, updated: Date())
        L2MShared.writeJSON(s, to: url)
    }

    private func checkCommands() {
        guard let dir = L2MShared.dir("commands") else { return }
        let verify = dir.appendingPathComponent("\(uuid).verify")
        if FileManager.default.fileExists(atPath: verify.path) {
            try? FileManager.default.removeItem(at: verify)
            runVerify()
        }
        let refresh = dir.appendingPathComponent("\(uuid).refresh")
        if FileManager.default.fileExists(atPath: refresh.path) {
            try? FileManager.default.removeItem(at: refresh)
            prefs = L2MShared.loadPrefs()
            publishStatus(mounted: true)
        }
    }

    private final class VerifyContext {
        let url: URL
        var result: L2MVerifyResult
        var lastWrite = Date.distantPast
        init(url: URL, result: L2MVerifyResult) {
            self.url = url
            self.result = result
        }
    }

    private func runVerify() {
        guard !verifying, let dir = L2MShared.dir("results") else { return }
        verifying = true
        let url = dir.appendingPathComponent("\(uuid).verify.json")
        let ctx = VerifyContext(url: url, result: L2MVerifyResult(uuid: uuid, running: true, fraction: 0, problems: 0,
                                                                  report: "", started: Date(), finished: nil))
        L2MShared.writeJSON(ctx.result, to: url)
        DispatchQueue.global(qos: .utility).async { [self] in
            var report = [CChar](repeating: 0, count: 1 << 16)
            let unmanaged = Unmanaged.passRetained(ctx)
            let r = l2m_verify(fs, { c, done, total in
                let ctx = Unmanaged<VerifyContext>.fromOpaque(c!).takeUnretainedValue()
                ctx.result.fraction = total > 0 ? min(1, Double(done) / Double(total)) : 0
                if Date().timeIntervalSince(ctx.lastWrite) > 0.25 {
                    ctx.lastWrite = Date()
                    L2MShared.writeJSON(ctx.result, to: ctx.url)
                }
                // the app cancels by deleting the result file
                return FileManager.default.fileExists(atPath: ctx.url.path) ? 0 : 1
            }, unmanaged.toOpaque(), &report, report.count)
            unmanaged.release()
            ctx.result.running = false
            ctx.result.fraction = 1
            ctx.result.problems = Int(max(r, 0))
            ctx.result.report = String(cString: report)
            ctx.result.finished = Date()
            L2MShared.writeJSON(ctx.result, to: url)
            verifying = false
        }
    }

    // MARK: lookup / attributes

    func lookupItem(named name: FSFileName, inDirectory directory: FSItem, replyHandler reply: @escaping (FSItem?, FSFileName?, Error?) -> Void) {
        let dir = ino(directory)
        var n = nameBytes(name)
        var child: UInt32 = 0
        let r = l2m_lookup(fs, dir, &n, n.count, &child)
        if r != 0 {
            reply(nil, nil, posix(r))
            return
        }
        reply(item(child, parent: dir), name, nil)
    }

    func getAttributes(_ desiredAttributes: FSItem.GetAttributesRequest, of item: FSItem, replyHandler reply: @escaping (FSItem.Attributes?, Error?) -> Void) {
        do {
            let st = try stat(ino(item))
            reply(attributes(st, parent: (item as? Ext4Item)?.parent ?? 0), nil)
        } catch {
            reply(nil, error)
        }
    }

    func setAttributes(_ req: FSItem.SetAttributesRequest, on item: FSItem, replyHandler reply: @escaping (FSItem.Attributes?, Error?) -> Void) {
        let i = ino(item)
        var st = l2m_stat()
        var mask: UInt32 = 0
        var consumed: FSItem.Attribute = []
        if req.isValid(.mode) { st.mode = req.mode; mask |= UInt32(L2M_SET_MODE); consumed.insert(.mode) }
        if req.isValid(.uid) { st.uid = toLinux(uid: req.uid); mask |= UInt32(L2M_SET_UID); consumed.insert(.uid) }
        if req.isValid(.gid) { st.gid = toLinux(gid: req.gid); mask |= UInt32(L2M_SET_GID); consumed.insert(.gid) }
        if req.isValid(.size) { st.size = req.size; mask |= UInt32(L2M_SET_SIZE); consumed.insert(.size) }
        if req.isValid(.accessTime) {
            st.atime = l2m_time(sec: Int64(req.accessTime.tv_sec), nsec: UInt32(req.accessTime.tv_nsec))
            mask |= UInt32(L2M_SET_ATIME); consumed.insert(.accessTime)
        }
        if req.isValid(.modifyTime) {
            st.mtime = l2m_time(sec: Int64(req.modifyTime.tv_sec), nsec: UInt32(req.modifyTime.tv_nsec))
            mask |= UInt32(L2M_SET_MTIME); consumed.insert(.modifyTime)
        }
        if req.isValid(.birthTime) {
            st.crtime = l2m_time(sec: Int64(req.birthTime.tv_sec), nsec: UInt32(req.birthTime.tv_nsec))
            mask |= UInt32(L2M_SET_CRTIME); consumed.insert(.birthTime)
        }
        if req.isValid(.changeTime) {
            st.ctime = l2m_time(sec: Int64(req.changeTime.tv_sec), nsec: UInt32(req.changeTime.tv_nsec))
            mask |= UInt32(L2M_SET_CTIME); consumed.insert(.changeTime)
        }
        if req.isValid(.flags) {
            var f: UInt32 = 0
            if req.flags & UInt32(UF_IMMUTABLE) != 0 { f |= Self.EXT4_IMMUTABLE_FL }
            if req.flags & UInt32(UF_APPEND) != 0 { f |= Self.EXT4_APPEND_FL }
            if req.flags & UInt32(UF_NODUMP) != 0 { f |= Self.EXT4_NODUMP_FL }
            st.flags = f
            mask |= UInt32(L2M_SET_FLAGS); consumed.insert(.flags)
        }
        if mask != 0 {
            let r = l2m_setattr(fs, i, mask, &st)
            if r != 0 {
                reply(nil, posix(r))
                return
            }
        }
        req.consumedAttributes = consumed
        do {
            reply(attributes(try stat(i), parent: (item as? Ext4Item)?.parent ?? 0), nil)
        } catch {
            reply(nil, error)
        }
    }

    func reclaimItem(_ item: FSItem, replyHandler reply: @escaping (Error?) -> Void) {
        let i = ino(item)
        // frees the inode if it was unlinked (no-op if already freed)
        let r = l2m_release(fs, i)
        itemLock.lock()
        if items[i] === item { items[i] = nil } // the number may already be reused
        itemLock.unlock()
        reply(r == 0 ? nil : posix(r))
    }

    // deleted files give their space back at the last close, like Linux
    var itemDeactivationPolicy: FSVolume.ItemDeactivationOptions { [.forRemovedItems] }

    func deactivateItem(_ item: FSItem, replyHandler reply: @escaping (Error?) -> Void) {
        let r = l2m_release(fs, ino(item))
        reply(r == 0 ? nil : posix(r))
    }

    // MARK: namespace

    private func blocked(_ name: FSFileName) -> Bool {
        guard let s = name.string else { return false }
        if prefs.blockDSStore && s == ".DS_Store" { return true }
        if prefs.blockAppleDouble && s.hasPrefix("._") { return true }
        if prefs.blockSpotlight && (s == ".Spotlight-V100" || s == ".fseventsd" || s == ".TemporaryItems") { return true }
        if prefs.blockTrash && (s == ".Trashes" || s == ".Trash") { return true }
        return false
    }

    private func modeBits(for type: FSItem.ItemType) -> UInt32 {
        switch type {
        case .directory: return 0o040000
        case .symlink: return 0o120000
        case .fifo: return 0o010000
        case .charDevice: return 0o020000
        case .blockDevice: return 0o060000
        case .socket: return 0o140000
        default: return 0o100000
        }
    }

    private func create(_ name: FSFileName, type: FSItem.ItemType, in directory: FSItem, attrs: FSItem.SetAttributesRequest) throws -> Ext4Item {
        if blocked(name) { throw POSIXError(.EPERM) }
        let dir = ino(directory)
        var n = nameBytes(name)
        let perm: UInt32 = attrs.isValid(.mode) ? attrs.mode & 0o7777 : (type == .directory ? 0o755 : 0o644)
        let uid = attrs.isValid(.uid) ? toLinux(uid: attrs.uid) : prefs.linuxUID
        let gid = attrs.isValid(.gid) ? toLinux(gid: attrs.gid) : prefs.linuxGID
        var newIno: UInt32 = 0
        let r = l2m_create(fs, dir, &n, n.count, modeBits(for: type) | perm, uid, gid, 0, &newIno)
        if r != 0 { throw posix(r) }
        var consumed: FSItem.Attribute = [.mode, .uid, .gid]
        // apply the remaining requested attributes (times, size)
        var st = l2m_stat()
        var mask: UInt32 = 0
        if attrs.isValid(.modifyTime) {
            st.mtime = l2m_time(sec: Int64(attrs.modifyTime.tv_sec), nsec: UInt32(attrs.modifyTime.tv_nsec))
            mask |= UInt32(L2M_SET_MTIME); consumed.insert(.modifyTime)
        }
        if attrs.isValid(.accessTime) {
            st.atime = l2m_time(sec: Int64(attrs.accessTime.tv_sec), nsec: UInt32(attrs.accessTime.tv_nsec))
            mask |= UInt32(L2M_SET_ATIME); consumed.insert(.accessTime)
        }
        if attrs.isValid(.birthTime) {
            st.crtime = l2m_time(sec: Int64(attrs.birthTime.tv_sec), nsec: UInt32(attrs.birthTime.tv_nsec))
            mask |= UInt32(L2M_SET_CRTIME); consumed.insert(.birthTime)
        }
        if attrs.isValid(.size) && type == .file && attrs.size > 0 {
            st.size = attrs.size
            mask |= UInt32(L2M_SET_SIZE); consumed.insert(.size)
        }
        if mask != 0 { _ = l2m_setattr(fs, newIno, mask, &st) }
        attrs.consumedAttributes = consumed
        return item(newIno, parent: dir)
    }

    func createItem(named name: FSFileName, type: FSItem.ItemType, inDirectory directory: FSItem, attributes newAttributes: FSItem.SetAttributesRequest, replyHandler reply: @escaping (FSItem?, FSFileName?, Error?) -> Void) {
        do {
            reply(try create(name, type: type, in: directory, attrs: newAttributes), name, nil)
        } catch {
            reply(nil, nil, error)
        }
    }

    func createSymbolicLink(named name: FSFileName, inDirectory directory: FSItem, attributes newAttributes: FSItem.SetAttributesRequest, linkContents contents: FSFileName, replyHandler reply: @escaping (FSItem?, FSFileName?, Error?) -> Void) {
        if blocked(name) {
            reply(nil, nil, POSIXError(.EPERM))
            return
        }
        let dir = ino(directory)
        var n = nameBytes(name)
        var target = contents.data.map { CChar(bitPattern: $0) }
        target.append(0)
        let uid = newAttributes.isValid(.uid) ? toLinux(uid: newAttributes.uid) : prefs.linuxUID
        let gid = newAttributes.isValid(.gid) ? toLinux(gid: newAttributes.gid) : prefs.linuxGID
        var newIno: UInt32 = 0
        let r = l2m_symlink(fs, dir, &n, n.count, &target, uid, gid, &newIno)
        if r != 0 {
            reply(nil, nil, posix(r))
            return
        }
        newAttributes.consumedAttributes = [.uid, .gid, .mode]
        reply(item(newIno, parent: dir), name, nil)
    }

    func readSymbolicLink(_ item: FSItem, replyHandler reply: @escaping (FSFileName?, Error?) -> Void) {
        var buf = [CChar](repeating: 0, count: 65537)
        let r = l2m_readlink(fs, ino(item), &buf, buf.count)
        if r < 0 {
            reply(nil, posix(r))
            return
        }
        let data = Data(buf[0..<Int(r)].map { UInt8(bitPattern: $0) })
        reply(FSFileName(data: data), nil)
    }

    func createLink(to item: FSItem, named name: FSFileName, inDirectory directory: FSItem, replyHandler reply: @escaping (FSFileName?, Error?) -> Void) {
        var n = nameBytes(name)
        let r = l2m_link(fs, ino(item), ino(directory), &n, n.count)
        reply(r == 0 ? name : nil, r == 0 ? nil : posix(r))
    }

    func removeItem(_ item: FSItem, named name: FSFileName, fromDirectory directory: FSItem, replyHandler reply: @escaping (Error?) -> Void) {
        var n = nameBytes(name)
        let isDir = ((try? stat(ino(item)))?.mode ?? 0) & 0o170000 == 0o040000
        var orphan: Int32 = 0
        let r = l2m_unlink(fs, ino(directory), &n, n.count, isDir ? 1 : 0, &orphan)
        // an orphan is freed in reclaimItem() once nobody uses it anymore
        reply(r == 0 ? nil : posix(r))
    }

    func renameItem(_ item: FSItem, inDirectory sourceDirectory: FSItem, named sourceName: FSFileName, to destinationName: FSFileName, inDirectory destinationDirectory: FSItem, overItem: FSItem?, replyHandler reply: @escaping (FSFileName?, Error?) -> Void) {
        if blocked(destinationName) {
            reply(nil, POSIXError(.EPERM))
            return
        }
        var s = nameBytes(sourceName)
        var d = nameBytes(destinationName)
        var replaced: UInt32 = 0
        var orphan: Int32 = 0
        let r = l2m_rename(fs, ino(sourceDirectory), &s, s.count, ino(destinationDirectory), &d, d.count, 0, &replaced, &orphan)
        if r != 0 {
            reply(nil, posix(r))
            return
        }
        (item as? Ext4Item)?.parent = ino(destinationDirectory)
        reply(destinationName, nil)
    }

    // MARK: directory enumeration

    private struct Entry {
        var name: [UInt8]
        var ino: UInt32
        var dtype: UInt32
        var next: UInt64
    }

    private final class Collector {
        var entries: [Entry] = []
        let limit: Int
        let skipDots: Bool
        init(limit: Int, skipDots: Bool) {
            self.limit = limit
            self.skipDots = skipDots
        }
    }

    func enumerateDirectory(_ directory: FSItem, startingAt cookie: FSDirectoryCookie, verifier: FSDirectoryVerifier, attributes: FSItem.GetAttributesRequest?, packer: FSDirectoryEntryPacker, replyHandler reply: @escaping (FSDirectoryVerifier, Error?) -> Void) {
        let dir = ino(directory)
        let current = FSDirectoryVerifier(rawValue: l2m_dir_version(fs, dir))
        var pos = cookie.rawValue
        let parent = (directory as? Ext4Item)?.parent ?? UInt32(L2M_ROOT_INO)
        while true {
            let col = Collector(limit: 256, skipDots: attributes != nil)
            let unmanaged = Unmanaged.passRetained(col)
            let r = l2m_readdir(fs, dir, &pos, { ctx, name, len, ino, dtype, next in
                let col = Unmanaged<Collector>.fromOpaque(ctx!).takeUnretainedValue()
                if col.entries.count >= col.limit { return 1 }
                let bytes = Array(UnsafeRawBufferPointer(start: name, count: len))
                if col.skipDots && (bytes == [0x2E] || bytes == [0x2E, 0x2E]) { return 0 }
                col.entries.append(Entry(name: bytes, ino: ino, dtype: UInt32(dtype), next: next))
                return 0
            }, unmanaged.toOpaque())
            unmanaged.release()
            if r != 0 {
                reply(current, posix(r))
                return
            }
            if col.entries.isEmpty { break }
            for e in col.entries {
                var t = type(ofDtype: e.dtype)
                var attrs: FSItem.Attributes? = nil
                if attributes != nil || t == .unknown {
                    guard let st = try? stat(e.ino) else { continue }
                    if t == .unknown { t = type(of: st.mode) }
                    if attributes != nil { attrs = self.attributes(st, parent: dir) }
                }
                let isDots = e.name == [0x2E] || e.name == [0x2E, 0x2E]
                let id: UInt64 = isDots && e.name.count == 2 && dir == UInt32(L2M_ROOT_INO) ? 1 : UInt64(e.ino)
                _ = parent
                let ok = packer.packEntry(name: FSFileName(data: Data(e.name)), itemType: t,
                                          itemID: FSItem.Identifier(rawValue: id)!,
                                          nextCookie: FSDirectoryCookie(rawValue: e.next), attributes: attrs)
                if !ok {
                    reply(current, nil)
                    return
                }
            }
            if col.entries.count < col.limit { break }
        }
        reply(current, nil)
    }

    // MARK: data (fallback path for files the kernel cannot map)

    func read(from item: FSItem, at offset: off_t, length: Int, into buffer: FSMutableFileDataBuffer, replyHandler reply: @escaping (Int, Error?) -> Void) {
        let i = ino(item)
        let n: Int64 = buffer.withUnsafeMutableBytes { raw in
            l2m_read(fs, i, UInt64(offset), raw.baseAddress, min(length, raw.count))
        }
        if n < 0 { reply(0, posix(n)) } else { reply(Int(n), nil) }
    }

    func write(contents: Data, to item: FSItem, at offset: off_t, replyHandler reply: @escaping (Int, Error?) -> Void) {
        let i = ino(item)
        let n: Int64 = contents.withUnsafeBytes { raw in
            l2m_write(fs, i, UInt64(offset), raw.baseAddress, raw.count)
        }
        if n < 0 { reply(0, posix(n)) } else { reply(Int(n), nil) }
    }

    // MARK: extended attributes

    func getXattr(named name: FSFileName, of item: FSItem, replyHandler reply: @escaping (Data?, Error?) -> Void) {
        guard let s = name.string else {
            reply(nil, POSIXError(.EINVAL))
            return
        }
        let i = ino(item)
        let size = l2m_getxattr(fs, i, s, nil, 0)
        if size < 0 {
            reply(nil, posix(size))
            return
        }
        var buf = Data(count: Int(size))
        let n: Int64 = buf.withUnsafeMutableBytes { l2m_getxattr(fs, i, s, $0.baseAddress, Int(size)) }
        if n < 0 { reply(nil, posix(n)) } else { reply(buf.prefix(Int(n)), nil) }
    }

    func setXattr(named name: FSFileName, to value: Data?, on item: FSItem, policy: FSVolume.SetXattrPolicy, replyHandler reply: @escaping (Error?) -> Void) {
        guard let s = name.string else {
            reply(POSIXError(.EINVAL))
            return
        }
        let i = ino(item)
        let r: Int32
        switch policy {
        case .delete:
            r = l2m_removexattr(fs, i, s)
        default:
            var flags: UInt32 = 0
            if policy == .mustCreate { flags = UInt32(L2M_XATTR_CREATE) }
            if policy == .mustReplace { flags = UInt32(L2M_XATTR_REPLACE) }
            let v = value ?? Data()
            r = v.withUnsafeBytes { l2m_setxattr(fs, i, s, $0.baseAddress, $0.count, flags) }
        }
        reply(r == 0 ? nil : posix(r))
    }

    func listXattrs(of item: FSItem, replyHandler reply: @escaping ([FSFileName]?, Error?) -> Void) {
        let i = ino(item)
        let size = l2m_listxattr(fs, i, nil, 0)
        if size < 0 {
            reply(nil, posix(size))
            return
        }
        var buf = [CChar](repeating: 0, count: Int(size) + 1)
        let n = l2m_listxattr(fs, i, &buf, buf.count)
        if n < 0 {
            reply(nil, posix(n))
            return
        }
        var names: [FSFileName] = []
        var start = 0
        for k in 0..<Int(n) where buf[k] == 0 {
            if k > start {
                names.append(FSFileName(data: Data(buf[start..<k].map { UInt8(bitPattern: $0) })))
            }
            start = k + 1
        }
        reply(names, nil)
    }

    // MARK: kernel offloaded I/O

    private func pack(_ packer: FSExtentPacker, ino: UInt32, offset: off_t, length: Int, allocate: Bool, forWrite: Bool) -> Int32 {
        let bs = UInt64(blockSize)
        let first = UInt64(offset) / bs
        let last = (UInt64(offset) + UInt64(max(length, 1)) + bs - 1) / bs
        var lblk = first
        var exts = [l2m_extent](repeating: l2m_extent(), count: 64)
        while lblk < last {
            let n = l2m_map(fs, ino, lblk, last - lblk, allocate ? UInt32(L2M_MAP_ALLOCATE) : 0, &exts, Int32(exts.count))
            if n < 0 { return n }
            if n == 0 { break }
            for e in exts[0..<Int(n)] {
                let type: FSExtentType
                switch Int32(e.type) {
                case L2M_EXT_DATA: type = .data
                case L2M_EXT_UNWRITTEN: type = forWrite ? .data : .zeroFill
                default: type = .zeroFill
                }
                let phys = type == .zeroFill && e.pblk == 0 ? 0 : off_t(e.pblk * bs)
                if !packer.packExtent(resource: device.resource, type: type, logicalOffset: off_t(e.lblk * bs),
                                      physicalOffset: phys, length: Int(UInt64(e.len) * bs)) {
                    return 0 // packer full: the kernel asks again for the rest
                }
                lblk = e.lblk + UInt64(e.len)
            }
        }
        return 0
    }

    func blockmapFile(_ file: FSItem, offset: off_t, length: Int, flags: FSBlockmapFlags, operationID: FSOperationID, packer: FSExtentPacker, replyHandler reply: @escaping (Error?) -> Void) {
        let write = flags.contains(.write)
        if write && isReadOnly {
            reply(POSIXError(.EROFS))
            return
        }
        let r = pack(packer, ino: ino(file), offset: offset, length: length, allocate: write, forWrite: write)
        reply(r == 0 ? nil : posix(r))
    }

    func completeIO(for file: FSItem, offset: off_t, length: Int, status: Error, flags: FSCompleteIOFlags, operationID: FSOperationID, replyHandler reply: @escaping (Error?) -> Void) {
        // FSKit passes nil for "no error" although the header declares the
        // parameter nonnull. Bridging a nil Error crashes, so look at the
        // raw reference first.
        let failed = !isNilError(status) && (status as NSError).code != 0
        guard flags.contains(.write), !failed, length > 0 else {
            reply(nil)
            return
        }
        // the data is on the disk: unwritten extents become real data now
        // and the file grows to the end of the written range (Apple: update
        // size and modification time here)
        let end = UInt64(offset) + UInt64(length)
        let r = l2m_write_done(fs, ino(file), UInt64(offset), UInt64(length), end)
        reply(r == 0 ? nil : posix(r))
    }

    func createFile(name: FSFileName, in directory: FSItem, attributes: FSItem.SetAttributesRequest, packer: FSExtentPacker, replyHandler reply: @escaping (FSItem?, FSFileName?, Error?) -> Void) {
        do {
            reply(try create(name, type: .file, in: directory, attrs: attributes), name, nil)
        } catch {
            reply(nil, nil, error)
        }
    }

    func lookupItem(name: FSFileName, in directory: FSItem, packer: FSExtentPacker, replyHandler reply: @escaping (FSItem?, FSFileName?, Error?) -> Void) {
        lookupItem(named: name, inDirectory: directory) { [self] it, n, err in
            if let it = it as? Ext4Item, let st = try? stat(it.ino), st.mode & 0o170000 == 0o100000, st.size > 0 {
                // hand the first extents to the kernel right away (saves a round trip)
                _ = pack(packer, ino: it.ino, offset: 0, length: Int(min(st.size, 64 << 20)), allocate: false, forWrite: false)
            }
            reply(it, n, err)
        }
    }
}
