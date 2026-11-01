//
//  ExtBridge - start at login and macOS notifications.
//
//  Started at login, ExtBridge waits in the menu bar only. If a disk was
//  probed before the driver ran (see Replug.swift), a notification asks
//  for the one re-plug; clicking it opens the window at that disk.
//
//  SPDX-License-Identifier: MIT
//

import AppKit
import OSLog
import ServiceManagement
@preconcurrency import UserNotifications

enum LoginItem {
    static var isEnabled: Bool { SMAppService.mainApp.status == .enabled }
    static var needsApproval: Bool { SMAppService.mainApp.status == .requiresApproval }

    static func set(_ on: Bool) throws {
        if on { try SMAppService.mainApp.register() } else { try SMAppService.mainApp.unregister() }
    }

    static func openSettings() { SMAppService.openSystemSettingsLoginItems() }

    /// true when macOS started ExtBridge as a login item (set at launch)
    static private(set) var launchedAtLogin = false

    static func detectLaunch() {
        if CommandLine.arguments.contains("--login") { launchedAtLogin = true; return }
        let ev = NSAppleEventManager.shared().currentAppleEvent
        if ev?.eventID == fourCC("oapp"),
           ev?.paramDescriptor(forKeyword: fourCC("prdt"))?.enumCodeValue == fourCC("lgit") {
            launchedAtLogin = true
            return
        }
        // not every macOS version marks SMAppService launches: started
        // within 90 seconds after logging in counts as well
        launchedAtLogin = isEnabled && (secondsSinceConsoleLogin().map { $0 < 90 } ?? false)
    }

    private static func fourCC(_ s: String) -> UInt32 { s.utf8.reduce(0) { $0 << 8 | UInt32($1) } }

    private static func secondsSinceConsoleLogin() -> TimeInterval? {
        setutxent()
        defer { endutxent() }
        let me = NSUserName()
        while let p = getutxent() {
            var e = p.pointee
            guard Int32(e.ut_type) == USER_PROCESS else { continue }
            let user = withUnsafeBytes(of: &e.ut_user) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
            let line = withUnsafeBytes(of: &e.ut_line) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
            if user == me && line == "console" {
                return Date().timeIntervalSince1970 - TimeInterval(e.ut_tv.tv_sec)
            }
        }
        return nil
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationWillFinishLaunching(_ notification: Notification) {
        LoginItem.detectLaunch()
        if LoginItem.launchedAtLogin { NSApp.setActivationPolicy(.accessory) }
    }
}

@MainActor
enum MainWindow {
    private static var hiddenForLogin = false

    static func find() -> NSWindow? {
        NSApp.windows.first { $0.identifier?.rawValue.hasPrefix("main") == true }
    }

    /// Called when the window first appears: a login launch keeps only
    /// the menu bar icon.
    static func hideIfLoginLaunch() {
        guard LoginItem.launchedAtLogin, !hiddenForLogin else { return }
        hiddenForLogin = true
        DispatchQueue.main.async { find()?.orderOut(nil) }
    }

    /// Brings an existing window to the front; false if there is none.
    @discardableResult
    static func bringToFront() -> Bool {
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
        guard let w = find() else { return false }
        w.makeKeyAndOrderFront(nil)
        return true
    }
}

@MainActor
final class Notifier: NSObject, UNUserNotificationCenterDelegate {
    static let shared = Notifier()
    private weak var store: DiskStore?
    nonisolated private static let log = Logger(subsystem: "de.nexami.ExtBridge", category: "notify")

    func attach(_ store: DiskStore) {
        self.store = store
        UNUserNotificationCenter.current().delegate = self
    }

    func requestPermission() {
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { ok, error in
            Self.log.info("notification permission: \(ok) \(String(describing: error), privacy: .public)")
        }
    }

    private static func id(_ bsd: String) -> String { "replug-" + bsd }

    /// Posts the re-plug notification. Without permission (macOS asks
    /// only while ExtBridge is in front) the window opens instead.
    func replug(_ disk: LinuxDisk) {
        let c = UNMutableNotificationContent()
        c.title = LF("„%@“ bitte neu anstecken", disk.displayName)
        c.body = L("macOS hat die Linux-Platte noch nicht erkannt. Zieh sie einmal ab und steck sie wieder an – dann erscheint sie im Finder.")
        c.sound = .default
        c.userInfo = ["bsd": disk.bsdName]
        let request = UNNotificationRequest(identifier: Self.id(disk.bsdName), content: c, trigger: nil)
        let bsd = disk.bsdName
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { ok, error in
            if ok {
                UNUserNotificationCenter.current().add(request)
                return
            }
            Self.log.info("no notification permission (\(String(describing: error), privacy: .public)), showing the window")
            Task { @MainActor in
                guard let store = Notifier.shared.store else { return }
                store.selection = bsd
                if !MainWindow.bringToFront() { store.windowRequest += 1 }
            }
        }
    }

    func removeReplug(_ bsd: String) {
        UNUserNotificationCenter.current().removeDeliveredNotifications(withIdentifiers: [Self.id(bsd)])
    }

    nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse,
                                            withCompletionHandler completionHandler: @escaping () -> Void) {
        let bsd = response.notification.request.content.userInfo["bsd"] as? String
        Task { @MainActor in
            if let store {
                if let bsd, store.disks.contains(where: { $0.bsdName == bsd }) { store.selection = bsd }
                if !MainWindow.bringToFront() { store.windowRequest += 1 }
            }
            completionHandler()
        }
    }

    nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter, willPresent notification: UNNotification,
                                            withCompletionHandler completionHandler: @escaping (UNNotificationPresentationOptions) -> Void) {
        completionHandler([.banner, .sound])
    }
}
