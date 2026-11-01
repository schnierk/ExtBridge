//
//  ExtBridge - demo data and an off-screen snapshot mode used for the
//  README screenshots and visual checks:
//
//    ExtBridge.app/Contents/MacOS/ExtBridge --snapshot /tmp/shots
//    ExtBridge.app/Contents/MacOS/ExtBridge --snapshot /tmp/shots --size 900x620
//
//  `--snapshot-live DIR` renders the real disks instead of the demo (one
//  picture per disk plus the menu bar panel), e.g. for bug reports. It only
//  watches: nothing is mounted and no setting is written.
//
//  SPDX-License-Identifier: MIT
//

import AppKit
import SwiftUI

extension DiskStore {
    func loadDemo() {
        let disk = LinuxDisk(bsdName: "disk5s1", wholeBSDName: "disk5", mediaName: "Linux Filesystem", size: 2_000_398_934_016,
                             busProtocol: "USB", vendor: "Example", model: "Portable SSD", isInternal: false, ejectable: true,
                             volumeName: "Archive", volumeKind: L2MShared.fsShortName, mountPoint: URL(fileURLWithPath: "/Volumes/Archive"),
                             partitionUUID: "5A1E0B2C-3D4F-4E6A-8B9C-0D1E2F3A4B5C")
        let disk2 = LinuxDisk(bsdName: "disk6s2", wholeBSDName: "disk6", mediaName: "Backup", size: 64_000_000_000,
                              busProtocol: "USB", vendor: "Example", model: "USB Stick", isInternal: false, ejectable: true,
                              volumeName: nil, volumeKind: nil, mountPoint: nil, partitionUUID: nil)
        disks = [disk, disk2]
        let now = Date()
        status = ["disk5s1": L2MVolumeStatus(
            uuid: "0f1e2d3c-4b5a-4968-8776-655443322110", label: "Archive", bsdName: "disk5s1", mounted: true, readOnly: true,
            readOnlyReason: "requested",
            features: "has_journal ext_attr resize_inode dir_index filetype extent 64bit flex_bg sparse_super large_file huge_file dir_nlink extra_isize metadata_csum".split(separator: " ").map(String.init),
            unsupported: [], blockSize: 4096, blocks: 488_378_646, freeBlocks: 150_610_056, reservedBlocks: 24_418_932,
            inodes: 122_101_760, freeInodes: 121_877_418, inodeSize: 256, groups: 14_905, state: "clean", mountCount: 412,
            maxMountCount: -1, errorCount: 0, created: now.addingTimeInterval(-86400 * 400), lastMount: now,
            lastWrite: now.addingTimeInterval(-3600 * 20), lastCheck: now.addingTimeInterval(-86400 * 2), lastError: nil,
            kbytesWritten: 6_100_000_000, hasJournal: true, journalBlocks: 262_144, journalReplayed: true,
            journalTransactions: 3, journalBlocksReplayed: 41, metadataChecksums: true, hashAlgorithm: "half_md4",
            lastMountedOn: "/mnt/archive", bytesReadSinceMount: 0, bytesWrittenSinceMount: 0, updated: now)]
        attention = ["disk6s2": .needsReplug]
        extensionState = .enabled
        selection = "disk5s1"
        verify["0f1e2d3c-4b5a-4968-8776-655443322110"] = L2MVerifyResult(uuid: "0f1e2d3c-4b5a-4968-8776-655443322110", running: false,
            fraction: 1, problems: 0, report: "ExtBridge consistency scan\nRESULT: no problems found\n", started: now, finished: now)
    }
}

@MainActor
enum Snapshot {
    static var requested: Bool {
        CommandLine.arguments.contains("--snapshot") || CommandLine.arguments.contains("--snapshot-live")
    }

    private static func directory(_ flag: String) -> URL? {
        let args = CommandLine.arguments
        guard let i = args.firstIndex(of: flag), i + 1 < args.count else { return nil }
        let dir = URL(fileURLWithPath: args[i + 1])
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    /// window size to render at (default: the size of the first start)
    private static var size: CGSize {
        let args = CommandLine.arguments
        if let j = args.firstIndex(of: "--size"), j + 1 < args.count {
            let wh = args[j + 1].split(separator: "x").compactMap { Double($0) }
            if wh.count == 2 { return CGSize(width: wh[0], height: wh[1]) }
        }
        return WindowSize.standard
    }

    static func runIfRequested() {
        if let dir = directory("--snapshot-live") { runLive(dir) }
        guard let dir = directory("--snapshot") else { return }
        let size = Self.size
        let store = DiskStore(demo: true)
        store.loadDemo()
        var shots: [(String, AnyView)] = []
        for (t, name) in [(0, "overview"), (1, "diagnostics"), (2, "details"), (3, "settings")] {
            shots.append((name, AnyView(DemoWindow(tab: t).environmentObject(store))))
        }
        shots.append(("replug", AnyView(DemoWindow(tab: 0, disk: 1).environmentObject(store))))
        shots.append(("restart", AnyView(VStack(spacing: 18) { RestartCard(); UnsupportedCard() }.padding(24)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top).background(Color(nsColor: .windowBackgroundColor)))))
        store.extensionState = .disabled
        shots.append(("setup", AnyView(SetupView().environmentObject(store))))
        shots.append(("empty", AnyView(EmptyStateView())))
        shots.append(("about", AnyView(AboutView().frame(width: 540, height: 520).background(Color(nsColor: .windowBackgroundColor)))))
        shots.append(("coffee", AnyView(PaymentChoice {}.background(Color(nsColor: .windowBackgroundColor)))))
        store.extensionState = .enabled
        func next(_ k: Int) {
            guard k < shots.count else { exit(0) }
            if shots[k].0 == "setup" { store.extensionState = .disabled } else { store.extensionState = .enabled }
            let host = NSHostingView(rootView: shots[k].1.frame(width: size.width, height: size.height))
            host.appearance = NSAppearance(named: CommandLine.arguments.contains("--light") ? .aqua : .darkAqua)
            host.frame = NSRect(origin: .zero, size: size)
            let w = NSWindow(contentRect: host.frame, styleMask: [.borderless], backing: .buffered, defer: false)
            w.contentView = host
            w.setFrameOrigin(NSPoint(x: -5000, y: -5000))
            w.orderFront(nil)
            DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) {
                if let rep = host.bitmapImageRepForCachingDisplay(in: host.bounds) {
                    host.cacheDisplay(in: host.bounds, to: rep)
                    try? rep.representation(using: .png, properties: [:])?.write(to: dir.appendingPathComponent("\(shots[k].0).png"))
                }
                w.orderOut(nil)
                next(k + 1)
            }
        }
        next(0)
    }

    private static var liveStore: DiskStore?

    private static func runLive(_ dir: URL) {
        let store = DiskStore(passive: true)
        liveStore = store
        // give DiskArbitration, the status files and the driver check time
        DispatchQueue.main.asyncAfter(deadline: .now() + 8) {
            var shots: [(String, AnyView)] = []
            for (i, d) in store.disks.enumerated() {
                shots.append(("disk-\(d.bsdName)", AnyView(LiveWindow(disk: i).environmentObject(store))))
            }
            if store.disks.isEmpty { shots.append(("empty", AnyView(EmptyStateView()))) }
            shots.append(("menubar", AnyView(MenuBarView().environmentObject(store).background(Color(nsColor: .windowBackgroundColor)))))
            render(shots, at: 0, dir: dir, store: store)
        }
    }

    private static func render(_ shots: [(String, AnyView)], at k: Int, dir: URL, store: DiskStore) {
        guard k < shots.count else { exit(0) }
        let isMenu = shots[k].0 == "menubar"
        let host = NSHostingView(rootView: shots[k].1.frame(width: isMenu ? 340 : size.width, height: isMenu ? nil : size.height))
        host.appearance = NSAppearance(named: CommandLine.arguments.contains("--light") ? .aqua : .darkAqua)
        host.frame = NSRect(origin: .zero, size: isMenu ? host.fittingSize : size)
        let w = NSWindow(contentRect: host.frame, styleMask: [.borderless], backing: .buffered, defer: false)
        w.contentView = host
        w.setFrameOrigin(NSPoint(x: -5000, y: -5000))
        w.orderFront(nil)
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) {
            if let rep = host.bitmapImageRepForCachingDisplay(in: host.bounds) {
                host.cacheDisplay(in: host.bounds, to: rep)
                try? rep.representation(using: .png, properties: [:])?.write(to: dir.appendingPathComponent("\(shots[k].0).png"))
            }
            w.orderOut(nil)
            render(shots, at: k + 1, dir: dir, store: store)
        }
    }
}

/// Sidebar plus the page of one real disk.
private struct LiveWindow: View {
    @EnvironmentObject var store: DiskStore
    var disk: Int

    var body: some View {
        HStack(spacing: 0) {
            Sidebar().frame(width: 260)
            Divider()
            if disk < store.disks.count { DiskDetailView(disk: store.disks[disk]) }
        }
        .background(Color(nsColor: .windowBackgroundColor))
    }
}

/// Same layout as the real window, with a fixed tab.
struct DemoWindow: View {
    @EnvironmentObject var store: DiskStore
    var tab: Int
    var disk = 0

    var body: some View {
        HStack(spacing: 0) {
            Sidebar().frame(width: 260)
            Divider()
            DiskDetailView(disk: store.disks[disk], initialTab: tab)
        }
        .background(Color(nsColor: .windowBackgroundColor))
    }
}
