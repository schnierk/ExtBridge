//
//  ExtBridge - Linux disks macOS did not recognise.
//
//  FSKit drivers start with the user session. A disk that is already
//  attached while the Mac boots is probed before ExtBridge runs, and
//  macOS repeats that probe only when the disk is attached again (it is
//  reserved for macOS itself, even an administrator cannot trigger it).
//  These views explain the one re-plug that is needed.
//
//  SPDX-License-Identifier: MIT
//

import SwiftUI

enum DiskAttention: Equatable {
    /// probed before the driver was running: one re-plug fixes it
    case needsReplug
    /// attached while the driver was running and still not recognised:
    /// probably Btrfs, XFS, swap, LUKS ...
    case unsupported
    /// fskitd kept a mount record from a driver that ended while the
    /// volume was mounted; only a restart of the Mac clears it
    case needsRestart
}

extension DiskAttention {
    var symbol: String {
        switch self {
        case .needsReplug: return "arrow.triangle.2.circlepath"
        case .unsupported: return "questionmark.circle"
        case .needsRestart: return "restart.circle"
        }
    }

    var hint: String {
        switch self {
        case .needsReplug: return "Bitte einmal abziehen und wieder anstecken"
        case .unsupported: return "Kein ext2/ext3/ext4 gefunden"
        case .needsRestart: return "Bitte den Mac neu starten"
        }
    }
}

extension LinuxDisk {
    var displayName: String { volumeName ?? mediaName.nilIfEmpty ?? model.nilIfEmpty ?? bsdName }
    var isDiskImage: Bool { busProtocol == "Disk Image" || busProtocol == "Virtual Interface" }
}

// MARK: animation

private enum ReplugPhase: CaseIterable {
    case done, unplug, wait, plug

    var offset: CGFloat { self == .unplug || self == .wait ? 54 : 0 }
    var connected: Bool { self == .done || self == .plug }

    var caption: String {
        switch self {
        case .unplug, .wait: return "1 · abziehen"
        case .plug: return "2 · wieder anstecken"
        case .done: return "3 · fertig"
        }
    }

    var animation: Animation {
        switch self {
        case .unplug: return .spring(response: 0.55, dampingFraction: 0.8)
        case .wait: return .linear(duration: 0.9)
        case .plug: return .spring(response: 0.38, dampingFraction: 0.55)
        case .done: return .linear(duration: 1.6)
        }
    }
}

/// Endless little film: the drive's plug leaves the Mac, waits a moment,
/// clicks back in and a tick appears.
struct ReplugAnimation: View {
    var body: some View {
        PhaseAnimator(ReplugPhase.allCases) { phase in
            VStack(spacing: 10) {
                HStack(spacing: 0) {
                    ZStack(alignment: .trailing) {
                        Image(systemName: "laptopcomputer")
                            .font(.system(size: 50, weight: .light))
                            .foregroundStyle(.secondary)
                        // the port
                        RoundedRectangle(cornerRadius: 2)
                            .fill(Color.primary.opacity(0.55))
                            .frame(width: 5, height: 12)
                            .offset(x: 2, y: -2)
                    }
                    ZStack(alignment: .leading) {
                        HStack(spacing: 0) {
                            Plug()
                            Capsule().fill(Color.primary.opacity(0.35)).frame(width: 46, height: 4)
                            Image(systemName: "externaldrive.fill")
                                .font(.system(size: 38))
                                .foregroundStyle(Theme.gradient)
                        }
                        .offset(x: phase.offset)
                        Image(systemName: phase.connected ? "checkmark.circle.fill" : "bolt.horizontal.circle")
                            .font(.title2)
                            .foregroundStyle(phase.connected ? Theme.ok : Theme.warn)
                            .opacity(phase == .plug ? 0 : 1)
                            .scaleEffect(phase == .done ? 1 : 0.8)
                            .offset(x: 18, y: -32)
                            .animation(.spring(response: 0.3, dampingFraction: 0.5), value: phase)
                    }
                    .frame(width: 170, alignment: .leading)
                }
                Text(L(phase.caption))
                    .font(.caption.weight(.medium))
                    .foregroundStyle(.secondary)
                    .contentTransition(.opacity)
                    .animation(.easeInOut(duration: 0.25), value: phase)
            }
            // room for the status symbol above the gap
            .padding(.top, 34)
        } animation: { $0.animation }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(L("Platte abziehen und wieder anstecken"))
    }
}

private struct Plug: View {
    var body: some View {
        HStack(spacing: 0) {
            RoundedRectangle(cornerRadius: 1.5)
                .fill(Color.primary.opacity(0.6))
                .frame(width: 7, height: 9)
            RoundedRectangle(cornerRadius: 4)
                .fill(Theme.accent2)
                .frame(width: 22, height: 16)
        }
    }
}

// MARK: cards

/// Shown instead of the usual "not mounted" card when macOS probed the
/// disk too early.
struct ReplugCard: View {
    @EnvironmentObject var store: DiskStore
    var disk: LinuxDisk
    @State private var why = false

    var body: some View {
        Card(title: "Einmal neu anstecken", icon: "arrow.triangle.2.circlepath") {
            VStack(alignment: .leading, spacing: 16) {
                ViewThatFits(in: .horizontal) {
                    HStack(alignment: .center, spacing: 28) {
                        ReplugAnimation().fixedSize()
                        message
                    }
                    VStack(alignment: .leading, spacing: 16) {
                        ReplugAnimation().fixedSize()
                        message
                    }
                }
                HStack(spacing: 10) {
                    if disk.ejectable {
                        Button { store.eject(disk, replugHint: true) } label: {
                            Label(disk.isDiskImage ? "Abbild auswerfen" : "Gerät trennen", systemImage: "eject.fill")
                        }
                        .buttonStyle(.borderedProminent).tint(Theme.accent)
                    }
                    Spacer()
                }
                if !store.launchAtLogin {
                    HStack(spacing: 10) {
                        Image(systemName: "bell.badge.fill").foregroundStyle(Theme.accent2)
                        Text("Tipp: ExtBridge kann beim Anmelden starten und dich dann sofort mit einer Mitteilung daran erinnern.")
                            .font(.callout).foregroundStyle(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                        Spacer(minLength: 8)
                        Button("Einschalten") { store.setLaunchAtLogin(true) }
                    }
                    .padding(12)
                    .background(Theme.accent2.opacity(0.08), in: RoundedRectangle(cornerRadius: 12))
                }
                DisclosureGroup(L("Warum ist das nötig?"), isExpanded: $why) {
                    Text("Platten, die beim Einschalten des Macs schon angesteckt sind, prüft macOS, bevor der ExtBridge-Treiber läuft – er startet erst mit deiner Anmeldung. Auch direkt nach einer Aktualisierung von ExtBridge kann das passieren. Diese Prüfung wiederholt macOS nur beim Anstecken, nicht einmal ein Administrator kann sie anders auslösen.")
                        .font(.callout).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                        .padding(.top, 6)
                }
            }
        }
    }

    private var message: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(disk.isDiskImage ? "Bitte das Abbild einmal auswerfen und neu öffnen"
                                  : "Bitte die Platte einmal abziehen und wieder anstecken")
                .font(.title3.weight(.semibold))
                .fixedSize(horizontal: false, vertical: true)
            Text("macOS hat dieses Laufwerk noch nicht als Linux-Platte erkannt. Danach erscheint es von selbst im Finder.")
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            if disk.ejectable && !disk.isDiskImage {
                Label("Am sichersten erst auf „Gerät trennen“ klicken – dann kann das Kabel gefahrlos ab.", systemImage: "checkmark.shield.fill")
                    .font(.callout)
                    .foregroundStyle(Theme.ok)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .frame(maxWidth: 460, alignment: .leading)
    }
}

/// macOS refuses to mount because it still has a record of the volume
/// being mounted.
struct RestartCard: View {
    var body: some View {
        Card(title: "Bitte den Mac neu starten", icon: "restart.circle.fill") {
            VStack(alignment: .leading, spacing: 8) {
                Text("macOS hält dieses Laufwerk noch für eingehängt, weil der ExtBridge-Treiber beendet wurde, während es eingehängt war – zum Beispiel bei einer Aktualisierung. Diesen Eintrag löscht macOS erst beim nächsten Neustart.")
                    .fixedSize(horizontal: false, vertical: true)
                Label("Die Daten auf der Platte sind davon nicht betroffen.", systemImage: "checkmark.shield.fill")
                    .foregroundStyle(Theme.ok)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}

/// A Linux partition that macOS probed with the driver running and still
/// did not recognise.
struct UnsupportedCard: View {
    var body: some View {
        Card(title: "Kein ext2/ext3/ext4 gefunden", icon: "questionmark.folder.fill") {
            VStack(alignment: .leading, spacing: 8) {
                Text("Auf dieser Linux-Partition hat macOS kein Dateisystem gefunden, das ExtBridge öffnen kann – zum Beispiel Btrfs, XFS, Linux-Swap oder eine mit LUKS verschlüsselte Partition. ExtBridge verändert daran nichts.")
                    .fixedSize(horizontal: false, vertical: true)
                Text("Bist du sicher, dass es ext4 ist? Dann zieh die Platte einmal ab und steck sie wieder an.")
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}
