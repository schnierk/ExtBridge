//
//  ExtBridge - main window, menu bar and settings.
//
//  SPDX-License-Identifier: MIT
//

import SwiftUI

/// Window sizes: the first start opens at `standard` (shrunk on small
/// screens), smaller than `minimum` the window cannot be made.
enum WindowSize {
    static let standard = CGSize(width: 1180, height: 800)
    static let minimum = CGSize(width: 900, height: 620)
}

@main
struct ExtBridgeApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    // screenshot runs must not touch any disk: the app's own store stays empty
    @StateObject private var store = DiskStore(demo: CommandLine.arguments.contains("--demo") || Snapshot.requested)

    init() {
        Snapshot.runIfRequested()
        #if APPSTORE
        TipJar.shared.start()
        #endif
    }

    var body: some Scene {
        WindowGroup("ExtBridge", id: "main") {
            RootView()
                .environmentObject(store)
                .onAppear { if CommandLine.arguments.contains("--demo") { store.loadDemo() } }
                .frame(minWidth: WindowSize.minimum.width, minHeight: WindowSize.minimum.height)
        }
        .windowStyle(.hiddenTitleBar)
        .defaultWindowPlacement { _, context in
            // only used when there is no remembered window frame yet
            let screen = context.defaultDisplay.visibleRect.size
            return WindowPlacement(.center, size: CGSize(width: min(WindowSize.standard.width, screen.width * 0.92),
                                                         height: min(WindowSize.standard.height, screen.height * 0.92)))
        }
        .commands {
            CommandGroup(replacing: .newItem) {}
        }

        MenuBarExtra {
            MenuBarView().environmentObject(store)
        } label: {
            MenuBarLabel(store: store)
        }
        .menuBarExtraStyle(.window)

        Settings {
            TabView {
                SettingsView().environmentObject(store)
                    .tabItem { Label(L("Einstellungen"), systemImage: "gearshape") }
                AboutView()
                    .tabItem { Label(L("Über ExtBridge"), systemImage: "info.circle") }
            }
            .frame(width: 540)
        }
    }
}

// MARK: root

struct RootView: View {
    @EnvironmentObject var store: DiskStore

    var body: some View {
        NavigationSplitView {
            Sidebar()
                .navigationSplitViewColumnWidth(min: 230, ideal: 260)
        } detail: {
            ZStack {
                if store.extensionState == .notInstalled || store.extensionState == .disabled {
                    SetupView()
                        .transition(.opacity.combined(with: .scale(scale: 0.97)))
                } else if let sel = store.selection, let disk = store.disks.first(where: { $0.bsdName == sel }) {
                    DiskDetailView(disk: disk)
                        .id(disk.bsdName)
                        .transition(.asymmetric(insertion: .move(edge: .trailing).combined(with: .opacity), removal: .opacity))
                } else {
                    EmptyStateView()
                        .transition(.opacity)
                }
            }
            .animation(.spring(response: 0.45, dampingFraction: 0.85), value: store.selection)
            .animation(.easeInOut, value: store.extensionState)
        }
        .overlay(alignment: .bottom) {
            if let m = store.lastMessage {
                Toast(text: m) { store.lastMessage = nil }
                    .padding(.bottom, 18)
                    .transition(.move(edge: .bottom).combined(with: .opacity))
            }
        }
        .animation(.spring(), value: store.lastMessage)
        .onAppear { MainWindow.hideIfLoginLaunch() }
    }
}

struct Toast: View {
    var text: String
    var close: () -> Void

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(Theme.warn)
            Text(text).lineLimit(3)
            Button(action: close) { Image(systemName: "xmark.circle.fill") }.buttonStyle(.borderless)
        }
        .padding(12)
        .background(.thickMaterial, in: Capsule())
        .shadow(radius: 12)
        .frame(maxWidth: 560)
    }
}

struct Sidebar: View {
    @EnvironmentObject var store: DiskStore

    var body: some View {
        List(selection: $store.selection) {
            Section("Linux-Laufwerke") {
                if store.disks.isEmpty {
                    Label("Keine angeschlossen", systemImage: "cable.connector.slash")
                        .foregroundStyle(.secondary)
                }
                ForEach(store.disks) { d in
                    DiskRow(disk: d, status: store.status(for: d), busy: store.busy.contains(d.bsdName),
                            attention: store.attention[d.bsdName])
                        .tag(d.bsdName)
                }
            }
        }
        .listStyle(.sidebar)
        .safeAreaInset(edge: .bottom) {
            VStack(alignment: .leading, spacing: 10) {
                CoffeeButton(compact: true)
                HStack {
                    ExtensionBadge(state: store.extensionState)
                    Spacer()
                    SettingsLink { Image(systemName: "gearshape") }.buttonStyle(.borderless)
                }
            }
            .padding(12)
        }
    }
}

struct DiskRow: View {
    var disk: LinuxDisk
    var status: L2MVolumeStatus?
    var busy: Bool
    var attention: DiskAttention? = nil

    var color: Color {
        if attention == .needsReplug || attention == .needsRestart { return Theme.warn }
        guard disk.isMounted else { return .gray }
        return (status?.readOnly ?? false) ? Theme.accent2 : Theme.ok
    }

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: "externaldrive.fill")
                .font(.title2)
                .foregroundStyle(Theme.gradient)
                .symbolEffect(.pulse, isActive: busy)
            VStack(alignment: .leading, spacing: 2) {
                Text(disk.volumeName ?? status?.label.nilIfEmpty ?? disk.mediaName.nilIfEmpty ?? disk.model.nilIfEmpty ?? disk.bsdName)
                    .font(.body.weight(.medium))
                    .lineLimit(1)
                switch attention {
                case .needsReplug:
                    Text("Bitte neu anstecken").font(.caption.weight(.medium)).foregroundStyle(.orange)
                case .unsupported:
                    Text("Kein ext2/ext3/ext4").font(.caption).foregroundStyle(.secondary)
                case .needsRestart:
                    Text("Mac neu starten").font(.caption.weight(.medium)).foregroundStyle(.orange)
                case nil:
                    Text(verbatim: "\(formatBytes(disk.size)) · \(disk.busProtocol)")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            Spacer()
            PulseDot(color: color, pulsing: disk.isMounted || attention == .needsReplug)
        }
        .padding(.vertical, 4)
    }
}

extension String {
    var nilIfEmpty: String? { isEmpty ? nil : self }
}

struct ExtensionBadge: View {
    var state: ExtensionState

    var body: some View {
        HStack(spacing: 6) {
            PulseDot(color: state == .enabled ? Theme.ok : Theme.warn, pulsing: state != .enabled)
            Text(L(state == .enabled ? "Treiber aktiv" : state == .disabled ? "Treiber aus" : state == .notInstalled ? "Treiber fehlt" : "…"))
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }
}

// MARK: empty / setup

struct EmptyStateView: View {
    @State private var bob = false

    var body: some View {
        ZStack {
            AuroraBackground().opacity(0.6)
            VStack(spacing: 18) {
                Image(systemName: "externaldrive.badge.plus")
                    .font(.system(size: 72, weight: .light))
                    .foregroundStyle(Theme.gradient)
                    .offset(y: bob ? -8 : 8)
                    .animation(.easeInOut(duration: 2).repeatForever(autoreverses: true), value: bob)
                Text("Schließe eine Linux-Festplatte an")
                    .font(.title2.weight(.semibold))
                Text("ext2, ext3 und ext4 erscheinen dann automatisch hier und im Finder.")
                    .foregroundStyle(.secondary)
            }
        }
        .background(Color(nsColor: .windowBackgroundColor))
        .onAppear { bob = true }
    }
}

struct SetupView: View {
    @EnvironmentObject var store: DiskStore
    @State private var step = 0

    var body: some View {
        ZStack {
            AuroraBackground()
            VStack(alignment: .leading, spacing: 22) {
                Text("Fast geschafft!")
                    .font(.system(size: 34, weight: .bold, design: .rounded))
                Text("macOS muss den ExtBridge-Treiber einmal erlauben. Danach erscheinen Linux-Platten ganz normal im Finder.")
                    .font(.title3)
                    .foregroundStyle(.secondary)
                VStack(alignment: .leading, spacing: 14) {
                    SetupStep(n: 1, text: "Klicke auf „Systemeinstellungen öffnen“.", done: step > 0)
                    SetupStep(n: 2, text: "Wähle „Nach Kategorie“ → „Dateisystem-Erweiterungen“ (ⓘ).", done: step > 0)
                    SetupStep(n: 3, text: "Schalte „ExtBridge ext4“ ein.", done: store.extensionState == .enabled)
                }
                .padding(20)
                .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 18))
                Button {
                    step = 1
                    store.openExtensionSettings()
                } label: {
                    Label("Systemeinstellungen öffnen", systemImage: "gearshape.2.fill")
                        .padding(.horizontal, 10).padding(.vertical, 4)
                }
                .buttonStyle(.borderedProminent)
                .tint(Theme.accent)
                .controlSize(.large)
                if store.extensionState == .notInstalled {
                    Text("Hinweis: Der Treiber wurde noch nicht registriert. Liegt ExtBridge im Ordner „Programme“? Starte die App nach dem Verschieben neu.")
                        .font(.callout).foregroundStyle(.secondary)
                }
            }
            .padding(48)
            .frame(maxWidth: 680)
        }
        .background(Color(nsColor: .windowBackgroundColor))
    }
}

struct SetupStep: View {
    var n: Int
    var text: String
    var done: Bool

    var body: some View {
        HStack(spacing: 12) {
            ZStack {
                Circle().fill(done ? Theme.ok : Theme.accent.opacity(0.2)).frame(width: 30, height: 30)
                if done {
                    Image(systemName: "checkmark").font(.headline).foregroundStyle(.white).transition(.scale)
                } else {
                    Text("\(n)").font(.headline)
                }
            }
            .animation(.spring(response: 0.4, dampingFraction: 0.6), value: done)
            Text(L(text))
        }
    }
}

// MARK: menu bar

struct MenuBarView: View {
    @EnvironmentObject var store: DiskStore
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("ExtBridge").font(.headline)
                Spacer()
                ExtensionBadge(state: store.extensionState)
            }
            Divider()
            if store.disks.isEmpty {
                Text("Keine Linux-Laufwerke").foregroundStyle(.secondary)
            }
            ForEach(store.disks) { d in
                let st = store.status(for: d)
                let attention = store.attention[d.bsdName]
                HStack {
                    DiskRow(disk: d, status: st, busy: store.busy.contains(d.bsdName), attention: attention)
                    if d.isMounted {
                        Button { store.reveal(d) } label: { Image(systemName: "folder") }.help("Im Finder zeigen")
                        Button { store.unmount(d) } label: { Image(systemName: "eject.fill") }.help("Auswerfen")
                    } else if let attention {
                        Button { open(d) } label: {
                            Image(systemName: attention.symbol)
                                .foregroundStyle(attention == .unsupported ? Color.secondary : Theme.warn)
                        }
                        .help(L(attention.hint))
                    } else {
                        Button { store.mount(d) } label: { Image(systemName: "play.fill") }.help("Einhängen")
                    }
                }
                .buttonStyle(.borderless)
            }
            Divider()
            HStack {
                Button("ExtBridge öffnen") { open(nil) }
                Spacer()
                Button("Beenden") { NSApp.terminate(nil) }
            }
        }
        .padding(14)
        .frame(width: 340)
    }

    private func open(_ disk: LinuxDisk?) {
        if let disk { store.selection = disk.bsdName }
        if !MainWindow.bringToFront() { openWindow(id: "main") }
    }
}

/// The menu bar icon; it also opens the window when a notification is
/// clicked and no window exists.
struct MenuBarLabel: View {
    @ObservedObject var store: DiskStore
    @Environment(\.openWindow) private var openWindow

    var symbol: String {
        if store.attention.values.contains(where: { $0 != .unsupported }) { return "externaldrive.badge.exclamationmark" }
        return store.status.values.contains { $0.mounted } ? "externaldrive.fill.badge.checkmark" : "externaldrive"
    }

    var body: some View {
        Image(systemName: symbol)
            .onChange(of: store.windowRequest) { _, _ in
                openWindow(id: "main")
                NSApp.activate(ignoringOtherApps: true)
            }
    }
}

// MARK: settings

struct SettingsView: View {
    @EnvironmentObject var store: DiskStore

    var body: some View {
        Form {
            Section("Beim Anmelden") {
                Toggle("ExtBridge beim Anmelden starten (nur in der Menüleiste)",
                       isOn: Binding(get: { store.launchAtLogin }, set: { store.setLaunchAtLogin($0) }))
                Text("Dann erinnert dich ExtBridge mit einer Mitteilung, wenn eine Platte nach dem Hochfahren einmal neu angesteckt werden muss.")
                    .font(.caption).foregroundStyle(.secondary)
                if store.loginNeedsApproval {
                    Button("In den Systemeinstellungen erlauben") { LoginItem.openSettings() }
                }
            }
            Section("Sicherheit") {
                Toggle("Neue Laufwerke zuerst nur lesend einhängen", isOn: $store.prefs.defaultReadOnly)
                Text("Empfohlen, wenn du wertvolle Daten hast. Pro Laufwerk änderbar.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Besitzer (Linux ↔ Mac)") {
                Toggle("Linux-Benutzer als meinen Mac-Benutzer zeigen", isOn: $store.prefs.mapOwners)
                HStack {
                    TextField("Linux-Benutzer-ID", value: $store.prefs.linuxUID, format: .number.grouping(.never))
                    TextField("Linux-Gruppen-ID", value: $store.prefs.linuxGID, format: .number.grouping(.never))
                }
                .disabled(!store.prefs.mapOwners)
                Text("Unter Linux hat der erste Benutzer meist die ID 1000. Neue Dateien vom Mac bekommen dann automatisch diese ID.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Linux-Platte sauber halten") {
                Toggle("Keine .DS_Store-Dateien anlegen", isOn: $store.prefs.blockDSStore)
                Toggle("Keine ._-Hilfsdateien anlegen", isOn: $store.prefs.blockAppleDouble)
                Toggle("Keine Spotlight-/fseventsd-Ordner anlegen", isOn: $store.prefs.blockSpotlight)
                Toggle("Keinen Papierkorb-Ordner (.Trashes) anlegen", isOn: $store.prefs.blockTrash)
                Text("Ohne Papierkorb-Ordner löscht der Finder sofort endgültig.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .onChange(of: store.prefs) { _, _ in store.savePrefs() }
        .padding()
    }
}
