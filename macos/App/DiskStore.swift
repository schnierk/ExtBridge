//
//  ExtBridge - model of the app: finds Linux disks (DiskArbitration),
//  reads the live status published by the extension and runs actions.
//
//  SPDX-License-Identifier: MIT
//

import AppKit
import Combine
import DiskArbitration
import Foundation
import FSKit

let linuxPartitionGUID = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"

struct LinuxDisk: Identifiable, Equatable {
    var id: String { bsdName }
    var bsdName: String
    var wholeBSDName: String
    var mediaName: String
    var size: UInt64
    var busProtocol: String
    var vendor: String
    var model: String
    var isInternal: Bool
    var ejectable: Bool
    var volumeName: String?
    var volumeKind: String?
    var mountPoint: URL?
    var partitionUUID: String?

    var isMounted: Bool { mountPoint != nil }
}

enum ExtensionState: Equatable {
    case unknown, notInstalled, disabled, enabled
}

@MainActor
final class DiskStore: ObservableObject {
    @Published var disks: [LinuxDisk] = []
    @Published var status: [String: L2MVolumeStatus] = [:]   // by bsdName
    @Published var verify: [String: L2MVerifyResult] = [:]   // by uuid
    @Published var prefs = L2MShared.loadPrefs()
    @Published var extensionState: ExtensionState = .unknown
    @Published var busy: Set<String> = []
    @Published var lastMessage: String?
    @Published var selection: String?
    /// Linux disks macOS did not recognise, by bsdName (see Replug.swift)
    @Published var attention: [String: DiskAttention] = [:]
    @Published var launchAtLogin = LoginItem.isEnabled
    @Published var loginNeedsApproval = LoginItem.needsApproval
    /// bumped when the window must be opened and none exists (menu bar label opens it)
    @Published var windowRequest = 0

    private var session: DASession?
    private var timer: Timer?
    private var extTimer: Timer?
    private let startedAt = Date()
    private var firstSeen: [String: Date] = [:]
    /// appeared while the driver was already running
    private var seenWithDriver: Set<String> = []
    private var notified: Set<String> = []
    private var autoMountTried: Set<String> = []
    /// mount refused because of a stale fskitd record (see isStaleMountRecord)
    private var restartNeeded: Set<String> = []

    static let extensionID: String = {
        let app = Bundle.main.bundleIdentifier ?? "de.nexami.ExtBridge"
        return app + ".FSModule"
    }()

    /// `passive`: only watches (live screenshots): never mounts, never
    /// notifies, never writes settings
    private let passive: Bool

    init(demo: Bool = false, passive: Bool = false) {
        self.passive = passive
        if demo { return }
        if !passive {
            Notifier.shared.attach(self)
            firstRunDefaults()
            applyLaunchArguments()
        }
        startDiskArbitration()
        refreshStatus()
        timer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refreshStatus() }
        }
        checkExtension()
        extTimer = Timer.scheduledTimer(withTimeInterval: 4.0, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.checkExtension() }
        }
    }

    // MARK: preferences

    private func firstRunDefaults() {
        guard L2MShared.prefsURL.map({ !FileManager.default.fileExists(atPath: $0.path) }) ?? false else { return }
        var p = L2MPrefs()
        p.macUID = getuid()
        p.macGID = getgid()
        L2MShared.savePrefs(p)
        prefs = p
    }

    /// `--read-only-uuid UUID` (repeatable): protect a volume before it is
    /// ever mounted, e.g. a precious archive disk.
    private func applyLaunchArguments() {
        let args = CommandLine.arguments
        var changed = false
        for (i, a) in args.enumerated() where a == "--read-only-uuid" && i + 1 < args.count {
            let uuid = args[i + 1].lowercased()
            var v = prefs.prefs(for: uuid)
            if !v.readOnly {
                v.readOnly = true
                prefs.volumes[uuid] = v
                changed = true
            }
        }
        if changed { L2MShared.savePrefs(prefs) }
    }

    func savePrefs() {
        L2MShared.savePrefs(prefs)
        // tell mounted volumes to pick up the new settings
        for s in status.values where s.mounted {
            touchCommand(uuid: s.uuid, command: "refresh")
        }
    }

    func volumePrefs(_ uuid: String) -> L2MVolumePrefs { prefs.prefs(for: uuid) }

    func setVolumePrefs(_ uuid: String, _ v: L2MVolumePrefs) {
        prefs.volumes[uuid] = v
        savePrefs()
    }

    // MARK: DiskArbitration

    private func startDiskArbitration() {
        guard let s = DASessionCreate(kCFAllocatorDefault) else { return }
        session = s
        DASessionSetDispatchQueue(s, DispatchQueue.main)
        let ctx = Unmanaged.passUnretained(self).toOpaque()
        DARegisterDiskAppearedCallback(s, nil, { disk, ctx in
            let store = Unmanaged<DiskStore>.fromOpaque(ctx!).takeUnretainedValue()
            MainActor.assumeIsolated { store.update(disk) }
        }, ctx)
        DARegisterDiskDisappearedCallback(s, nil, { disk, ctx in
            let store = Unmanaged<DiskStore>.fromOpaque(ctx!).takeUnretainedValue()
            MainActor.assumeIsolated { store.remove(disk) }
        }, ctx)
        DARegisterDiskDescriptionChangedCallback(s, nil, nil, { disk, _, ctx in
            let store = Unmanaged<DiskStore>.fromOpaque(ctx!).takeUnretainedValue()
            MainActor.assumeIsolated { store.update(disk) }
        }, ctx)
    }

    private func describe(_ disk: DADisk) -> LinuxDisk? {
        guard let d = DADiskCopyDescription(disk) as? [String: Any],
              let bsd = d[kDADiskDescriptionMediaBSDNameKey as String] as? String else { return nil }
        let content = d[kDADiskDescriptionMediaContentKey as String] as? String ?? ""
        let kind = d[kDADiskDescriptionVolumeKindKey as String] as? String
        let isLinux = content.uppercased() == linuxPartitionGUID || content == "Linux" || kind == L2MShared.fsShortName
        guard isLinux else { return nil }
        var whole = bsd
        if let w = DADiskCopyWholeDisk(disk), let wn = DADiskGetBSDName(w) { whole = String(cString: wn) }
        var puuid: String?
        if let u = d[kDADiskDescriptionMediaUUIDKey as String] {
            puuid = CFUUIDCreateString(nil, (u as! CFUUID)) as String?
        }
        return LinuxDisk(
            bsdName: bsd, wholeBSDName: whole,
            mediaName: d[kDADiskDescriptionMediaNameKey as String] as? String ?? "",
            size: (d[kDADiskDescriptionMediaSizeKey as String] as? NSNumber)?.uint64Value ?? 0,
            busProtocol: d[kDADiskDescriptionDeviceProtocolKey as String] as? String ?? "",
            vendor: (d[kDADiskDescriptionDeviceVendorKey as String] as? String ?? "").trimmingCharacters(in: .whitespaces),
            model: (d[kDADiskDescriptionDeviceModelKey as String] as? String ?? "").trimmingCharacters(in: .whitespaces),
            isInternal: d[kDADiskDescriptionDeviceInternalKey as String] as? Bool ?? false,
            ejectable: d[kDADiskDescriptionMediaEjectableKey as String] as? Bool ?? true,
            volumeName: d[kDADiskDescriptionVolumeNameKey as String] as? String,
            volumeKind: kind,
            mountPoint: d[kDADiskDescriptionVolumePathKey as String] as? URL,
            partitionUUID: puuid)
    }

    private func update(_ disk: DADisk) {
        guard let ld = describe(disk) else { return }
        if let i = disks.firstIndex(where: { $0.bsdName == ld.bsdName }) {
            if disks[i] != ld { disks[i] = ld }
        } else {
            disks.append(ld)
            disks.sort { $0.bsdName < $1.bsdName }
            if selection == nil { selection = ld.bsdName }
            firstSeen[ld.bsdName] = Date()
            if extensionState == .enabled && Date().timeIntervalSince(startedAt) > 3 { seenWithDriver.insert(ld.bsdName) }
        }
    }

    private func remove(_ disk: DADisk) {
        guard let n = DADiskGetBSDName(disk) else { return }
        let bsd = String(cString: n)
        disks.removeAll { $0.bsdName == bsd }
        if selection == bsd { selection = disks.first?.bsdName }
        firstSeen[bsd] = nil
        seenWithDriver.remove(bsd)
        notified.remove(bsd)
        autoMountTried.remove(bsd)
        restartNeeded.remove(bsd)
        if attention.removeValue(forKey: bsd) != nil { Notifier.shared.removeReplug(bsd) }
    }

    // MARK: disks macOS did not recognise

    /// A Linux partition without a recognised file system while the driver
    /// is running. Present since before the app started (boot, update):
    /// probed too early, a re-plug helps. Attached later: not ext2/3/4.
    private func updateAttention() {
        var a: [String: DiskAttention] = [:]
        // after a login launch give macOS a little time to settle first
        let settled = !LoginItem.launchedAtLogin || Date().timeIntervalSince(startedAt) > 20
        if extensionState == .enabled && settled {
            for d in disks where d.volumeKind == nil && !d.isMounted {
                guard let t = firstSeen[d.bsdName], Date().timeIntervalSince(t) > 4 else { continue }
                a[d.bsdName] = seenWithDriver.contains(d.bsdName) ? .unsupported : .needsReplug
            }
        }
        for d in disks where restartNeeded.contains(d.bsdName) {
            if d.isMounted { restartNeeded.remove(d.bsdName) } else { a[d.bsdName] = .needsRestart }
        }
        guard a != attention else { return }
        for bsd in attention.keys where a[bsd] == nil { Notifier.shared.removeReplug(bsd) }
        attention = a
        for (bsd, kind) in a where kind == .needsReplug && !notified.contains(bsd) {
            notified.insert(bsd)
            if !passive && !NSApp.isActive, let d = disks.first(where: { $0.bsdName == bsd }) { Notifier.shared.replug(d) }
        }
    }

    /// Disks macOS recognised but did not mount (an ExtBridge update
    /// unmounts them, for example): mount them once when the app starts.
    private func autoMountAtStart() {
        guard !passive, extensionState == .enabled, Date().timeIntervalSince(startedAt) < 120 else { return }
        for d in disks where d.volumeKind == L2MShared.fsShortName && !d.isMounted
            && !busy.contains(d.bsdName) && !autoMountTried.contains(d.bsdName) {
            // only disks that were there at start and stayed unmounted for a moment
            guard let t = firstSeen[d.bsdName], t.timeIntervalSince(startedAt) < 3,
                  Date().timeIntervalSince(t) > 3 else { continue }
            if let uuid = status(for: d)?.uuid, volumePrefs(uuid).noAutoMount { continue }
            autoMountTried.insert(d.bsdName)
            mount(d, quiet: true)
        }
    }

    // MARK: start at login

    func setLaunchAtLogin(_ on: Bool) {
        do {
            try LoginItem.set(on)
            if on { Notifier.shared.requestPermission() }
        } catch {
            lastMessage = LF("Beim Anmelden starten: %@", error.localizedDescription)
        }
        refreshLoginState()
    }

    func refreshLoginState() {
        let on = LoginItem.isEnabled, approval = LoginItem.needsApproval
        if launchAtLogin != on { launchAtLogin = on }
        if loginNeedsApproval != approval { loginNeedsApproval = approval }
    }

    // MARK: status from the extension

    func refreshStatus() {
        if let dir = L2MShared.dir("status"),
           let files = try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil) {
            var m: [String: L2MVolumeStatus] = [:]
            for f in files where f.pathExtension == "json" {
                if let s = L2MShared.readJSON(L2MVolumeStatus.self, from: f) {
                    if let old = m[s.bsdName], old.updated > s.updated { continue }
                    m[s.bsdName] = s
                }
            }
            if m != status { status = m }
        }
        updateAttention()
        autoMountAtStart()
        if let dir = L2MShared.dir("results") {
            for s in status.values {
                let u = dir.appendingPathComponent("\(s.uuid).verify.json")
                if let r = L2MShared.readJSON(L2MVerifyResult.self, from: u), verify[s.uuid] != r { verify[s.uuid] = r }
            }
        }
    }

    func status(for disk: LinuxDisk) -> L2MVolumeStatus? {
        guard let s = status[disk.bsdName] else { return nil }
        return s
    }

    // MARK: extension state

    func checkExtension() {
        let id = Self.extensionID
        Task.detached {
            let st: ExtensionState
            if let mods = try? await FSClient.shared.installedExtensions {
                // FSKit's own list (also works in the App Store sandbox)
                if let m = mods.first(where: { $0.bundleIdentifier == id }) { st = m.isEnabled ? .enabled : .disabled }
                else { st = .notInstalled }
            } else {
                let out = (try? Shell.run("/usr/bin/pluginkit", ["-m", "-i", id])) ?? ""
                if out.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty { st = .notInstalled }
                else if out.contains("+") { st = .enabled }
                else { st = .disabled }
            }
            await MainActor.run {
                if self.extensionState != st { self.extensionState = st }
                self.refreshLoginState()
            }
        }
    }

    func openExtensionSettings() {
        let urls = ["x-apple.systempreferences:com.apple.LoginItems-Settings.extension",
                    "x-apple.systempreferences:com.apple.ExtensionsPreferences"]
        for u in urls { if let url = URL(string: u), NSWorkspace.shared.open(url) { return } }
    }

    // MARK: actions
    //
    // Everything goes through DiskArbitration, like the Finder does.
    // `diskutil` asks storagekitd, which does not know third-party FSKit
    // file systems: it reports "no file system" and refuses to mount,
    // unmount or eject them.

    /// `quiet`: no message if it fails (automatic attempts)
    func mount(_ disk: LinuxDisk, readOnly: Bool? = nil, quiet: Bool = false) {
        let uuid = status(for: disk)?.uuid
        if let uuid, volumePrefs(uuid).noAutoMount {
            prefs.mountRequests.append(uuid)
            savePrefs()
        } else if uuid == nil {
            // not mounted yet: we may not know the ext4 UUID; allow the
            // extension to mount any volume the user asks for
        }
        let ro = readOnly ?? (uuid.map { volumePrefs($0).readOnly } ?? prefs.defaultReadOnly)
        busy.insert(disk.bsdName)
        daMount(disk.bsdName, readOnly: ro) { status in
            self.busy.remove(disk.bsdName)
            guard let status else { return }
            if Self.isStaleMountRecord(status) { self.restartNeeded.insert(disk.bsdName) }
            if !quiet { self.lastMessage = Self.mountFailed(status) }
        }
    }

    func unmount(_ disk: LinuxDisk) {
        busy.insert(disk.bsdName)
        daUnmount(disk.bsdName, whole: false) { status in
            self.busy.remove(disk.bsdName)
            self.lastMessage = status.map { LF("%@: %@", L("Auswerfen"), Self.daMessage($0)) }
        }
    }

    /// Unmounts every partition of the device and ejects it.
    /// `replugHint`: say what to do next (from the re-plug card)
    func eject(_ disk: LinuxDisk, replugHint: Bool = false) {
        busy.insert(disk.bsdName)
        daUnmount(disk.wholeBSDName, whole: true) { _ in
            // an unmount problem shows up again, more precisely, when ejecting
            self.daEject(disk.wholeBSDName) { status in
                self.busy.remove(disk.bsdName)
                if let status {
                    self.lastMessage = LF("%@: %@", L("Gerät trennen"), Self.daMessage(status))
                } else if replugHint {
                    self.lastMessage = L(disk.isDiskImage ? "Ausgeworfen – jetzt das Abbild neu öffnen."
                                                          : "Getrennt – jetzt das Kabel abziehen und wieder anstecken.")
                }
            }
        }
    }

    func remount(_ disk: LinuxDisk, readOnly: Bool) {
        busy.insert(disk.bsdName)
        daUnmount(disk.bsdName, whole: false) { _ in
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.7) {
                self.daMount(disk.bsdName, readOnly: readOnly) { status in
                    self.busy.remove(disk.bsdName)
                    self.lastMessage = status.map { LF("Neu einhängen: %@", Self.daMessage($0)) }
                }
            }
        }
    }

    // MARK: DiskArbitration requests

    /// fskitd still has a mount record for the volume ("file exists"):
    /// left behind when the driver ended while the volume was mounted.
    /// macOS drops these records only when it restarts.
    private static func isStaleMountRecord(_ status: DAReturn) -> Bool { status == 0xC204 || status == 516 }

    private static func mountFailed(_ status: DAReturn) -> String {
        if isStaleMountRecord(status) {
            return L("macOS hält das Laufwerk noch für eingehängt. Ein Neustart des Macs behebt das.")
        }
        return LF("Einhängen hat nicht geklappt (%@). Zieh die Platte einmal ab und steck sie wieder an.", daMessage(status))
    }

    static func daMessage(_ status: DAReturn) -> String {
        switch UInt32(bitPattern: status) {
        // DiskArbitration reports errno values as 0xC000 | errno
        case 0xF8DA_0002, 0xC010:            // kDAReturnBusy, EBUSY
            return L("Das Laufwerk wird gerade benutzt – schließe Programme, die Dateien darauf geöffnet haben.")
        case 0xF8DA_0008, 0xF8DA_0009, 0xC001, 0xC00D:   // not permitted / privileged, EPERM, EACCES
            return L("macOS hat das nicht erlaubt.")
        case 0xF8DA_0006:
            return L("Laufwerk nicht gefunden")
        default:
            return String(format: "0x%08x", UInt32(bitPattern: status))
        }
    }

    private func daDisk(_ bsd: String) -> DADisk? {
        guard let s = session else { return nil }
        return DADiskCreateFromBSDName(kCFAllocatorDefault, s, bsd)
    }

    private func daMount(_ bsd: String, readOnly: Bool, done: @escaping @MainActor (DAReturn?) -> Void) {
        guard let d = daDisk(bsd) else { done(DAReturn(bitPattern: 0xF8DA_0006)); return }
        let ctx = DAReply.context(done)
        let options = DADiskMountOptions(kDADiskMountOptionDefault)
        if readOnly {
            let rdonly = "rdonly" as CFString
            var args: [Unmanaged<CFString>?] = [Unmanaged.passUnretained(rdonly), nil]
            withExtendedLifetime(rdonly) {
                args.withUnsafeMutableBufferPointer { DADiskMountWithArguments(d, nil, options, DAReply.callback, ctx, $0.baseAddress) }
            }
        } else {
            DADiskMount(d, nil, options, DAReply.callback, ctx)
        }
    }

    private func daUnmount(_ bsd: String, whole: Bool, done: @escaping @MainActor (DAReturn?) -> Void) {
        guard let d = daDisk(bsd) else { done(DAReturn(bitPattern: 0xF8DA_0006)); return }
        let options = DADiskUnmountOptions(whole ? kDADiskUnmountOptionWhole : kDADiskUnmountOptionDefault)
        DADiskUnmount(d, options, DAReply.callback, DAReply.context(done))
    }

    private func daEject(_ bsd: String, done: @escaping @MainActor (DAReturn?) -> Void) {
        guard let d = daDisk(bsd) else { done(DAReturn(bitPattern: 0xF8DA_0006)); return }
        DADiskEject(d, DADiskEjectOptions(kDADiskEjectOptionDefault), DAReply.callback, DAReply.context(done))
    }

    func reveal(_ disk: LinuxDisk) {
        if let m = disk.mountPoint { NSWorkspace.shared.open(m) }
    }

    private func touchCommand(uuid: String, command: String) {
        guard let dir = L2MShared.dir("commands") else { return }
        FileManager.default.createFile(atPath: dir.appendingPathComponent("\(uuid).\(command)").path, contents: Data())
    }

    func startVerify(_ s: L2MVolumeStatus) {
        verify[s.uuid] = L2MVerifyResult(uuid: s.uuid, running: true, fraction: 0, problems: 0, report: "", started: Date(), finished: nil)
        touchCommand(uuid: s.uuid, command: "verify")
    }

    func cancelVerify(_ s: L2MVolumeStatus) {
        if let dir = L2MShared.dir("results") {
            try? FileManager.default.removeItem(at: dir.appendingPathComponent("\(s.uuid).verify.json"))
        }
        verify[s.uuid] = nil
    }
}

/// Carries a completion through DiskArbitration's C callback (the session
/// runs on the main queue). nil means success.
private final class DAReply {
    let done: @MainActor (DAReturn?) -> Void
    init(_ done: @escaping @MainActor (DAReturn?) -> Void) { self.done = done }

    static func context(_ done: @escaping @MainActor (DAReturn?) -> Void) -> UnsafeMutableRawPointer {
        Unmanaged.passRetained(DAReply(done)).toOpaque()
    }

    static let callback: @convention(c) (DADisk, DADissenter?, UnsafeMutableRawPointer?) -> Void = { _, dissenter, ctx in
        let reply = Unmanaged<DAReply>.fromOpaque(ctx!).takeRetainedValue()
        let status = dissenter.map { DADissenterGetStatus($0) }
        MainActor.assumeIsolated { reply.done(status) }
    }
}

enum Shell {
    struct Failure: LocalizedError {
        var message: String
        var errorDescription: String? { message }
    }

    @discardableResult
    static func run(_ tool: String, _ args: [String]) throws -> String {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: tool)
        p.arguments = args
        let out = Pipe(), err = Pipe()
        p.standardOutput = out
        p.standardError = err
        try p.run()
        let o = out.fileHandleForReading.readDataToEndOfFile()
        let e = err.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        let so = String(decoding: o, as: UTF8.self)
        if p.terminationStatus != 0 {
            let se = String(decoding: e, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
            throw Failure(message: se.isEmpty ? so.trimmingCharacters(in: .whitespacesAndNewlines) : se)
        }
        return so
    }
}

// MARK: speed test

@MainActor
final class SpeedTest: ObservableObject {
    enum Phase: Equatable { case idle, waiting, searching, reading, writing, done, failed(String) }
    @Published var phase: Phase = .idle
    @Published var readMBs: Double = 0
    @Published var writeMBs: Double = 0
    @Published var current: Double = 0
    @Published var fileName = ""
    /// only small files to read: the read value says little
    @Published var smallFile = false
    private var task: Task<Void, Never>?

    var running: Bool { [.waiting, .searching, .reading, .writing].contains(phase) }

    /// `busy` tells whether a consistency scan is running on the disk; the test waits for it,
    /// because the scan holds the file system and would make the disk look slow.
    func start(volume: URL, write: Bool, busy: @escaping @MainActor @Sendable () -> Bool = { false }) {
        task?.cancel()
        readMBs = 0
        writeMBs = 0
        current = 0
        fileName = ""
        smallFile = false
        phase = .searching
        task = Task.detached(priority: .userInitiated) { [weak self] in
            await self?.runTest(volume: volume, write: write, busy: busy)
        }
    }

    func cancel() {
        task?.cancel()
        phase = .idle
    }

    nonisolated private func biggestFile(in root: URL) -> (URL, Int)? {
        let fm = FileManager.default
        guard let e = fm.enumerator(at: root, includingPropertiesForKeys: [.fileSizeKey, .isRegularFileKey],
                                    options: [.skipsPackageDescendants]) else { return nil }
        var best: (URL, Int)? = nil
        let start = Date()
        for case let u as URL in e {
            // folders full of small files come first on many disks: look a while, not just at the first entries
            if Task.isCancelled || Date().timeIntervalSince(start) > 4 { break }
            guard let v = try? u.resourceValues(forKeys: [.fileSizeKey, .isRegularFileKey]), v.isRegularFile == true,
                  let s = v.fileSize else { continue }
            if s > (best?.1 ?? 0) { best = (u, s) }
            if s >= 1 << 30 { break }
        }
        return best
    }

    nonisolated private func runTest(volume: URL, write: Bool, busy: @escaping @MainActor @Sendable () -> Bool) async {
        while !Task.isCancelled, await busy() {
            await MainActor.run { self.phase = .waiting }
            try? await Task.sleep(nanoseconds: 500_000_000)
        }
        let chunk = 8 << 20
        let buf = UnsafeMutableRawPointer.allocate(byteCount: chunk, alignment: 4096)
        defer { buf.deallocate() }
        // page-aligned buffers + F_NOCACHE: the data goes straight to and from the disk, not the RAM.
        // The gauges are updated a few times a second only; waiting for the UI after every
        // chunk made the disk look several times slower than it is.
        var testFile: URL? = nil
        if write && !Task.isCancelled {
            await MainActor.run { self.phase = .writing; self.current = 0; self.fileName = L("eigene 1-GB-Testdatei") }
            let tmp = volume.appendingPathComponent(".extbridge-speedtest-\(getpid())")
            let wfd = open(tmp.path, O_WRONLY | O_CREAT | O_TRUNC, 0o600)
            if wfd >= 0 {
                _ = fcntl(wfd, F_NOCACHE, 1)
                memset(buf, 0x5A, chunk)
                let start = Date()
                var shown = start
                var total = 0
                while !Task.isCancelled && total < (1 << 30) && Date().timeIntervalSince(start) < 8 {
                    let n = Darwin.write(wfd, buf, chunk)
                    if n <= 0 { break }
                    total += n
                    if Date().timeIntervalSince(shown) > 0.2 {
                        shown = Date()
                        let mbs = Double(total) / 1e6 / max(shown.timeIntervalSince(start), 0.001)
                        await MainActor.run { self.current = mbs; self.writeMBs = mbs }
                    }
                }
                _ = fcntl(wfd, F_FULLFSYNC)
                let all = Double(total) / 1e6 / max(Date().timeIntervalSince(start), 0.001)
                close(wfd)
                await MainActor.run { self.writeMBs = all }
                // read the test file back: it is big and certainly not in the cache
                if total >= 256 << 20 { testFile = tmp } else { unlink(tmp.path) }
            }
        }
        defer { if let t = testFile { unlink(t.path) } }
        var file = testFile
        if file == nil && !Task.isCancelled {
            await MainActor.run { self.phase = .searching }
            guard let best = biggestFile(in: volume) else {
                await MainActor.run { self.phase = .failed("Keine Datei zum Lesen gefunden.") }
                return
            }
            let (f, size) = best
            file = f
            await MainActor.run { self.fileName = f.lastPathComponent; self.smallFile = size < 256 << 20 }
        }
        if let file, !Task.isCancelled {
            await MainActor.run { self.phase = .reading; self.current = 0 }
            let fd = open(file.path, O_RDONLY)
            if fd >= 0 {
                _ = fcntl(fd, F_NOCACHE, 1)
                let start = Date()
                var shown = start
                var total = 0
                while !Task.isCancelled && Date().timeIntervalSince(start) < 8 {
                    let n = read(fd, buf, chunk)
                    if n <= 0 { break }
                    total += n
                    if Date().timeIntervalSince(shown) > 0.2 {
                        shown = Date()
                        let mbs = Double(total) / 1e6 / max(shown.timeIntervalSince(start), 0.001)
                        await MainActor.run { self.current = mbs; self.readMBs = mbs }
                    }
                }
                let all = Double(total) / 1e6 / max(Date().timeIntervalSince(start), 0.001)
                close(fd)
                await MainActor.run { self.readMBs = all }
            }
        }
        await MainActor.run { self.phase = Task.isCancelled ? .idle : .done }
    }
}
