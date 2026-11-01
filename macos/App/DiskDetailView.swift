//
//  ExtBridge - detail page of one Linux disk: overview, diagnostics,
//  technical information and per-disk settings.
//
//  SPDX-License-Identifier: MIT
//

import SwiftUI

struct DiskDetailView: View {
    @EnvironmentObject var store: DiskStore
    var disk: LinuxDisk
    @State private var tab: Int
    @Namespace private var ns

    init(disk: LinuxDisk, initialTab: Int = 0) {
        self.disk = disk
        _tab = State(initialValue: initialTab)
    }

    var status: L2MVolumeStatus? { store.status(for: disk) }

    var body: some View {
        VStack(spacing: 0) {
            Header(disk: disk, status: status)
            TabBar(tab: $tab, ns: ns)
                .padding(.horizontal, 24)
                .padding(.top, 6)
            ScrollView {
                Group {
                    switch tab {
                    case 0: OverviewTab(disk: disk, status: status)
                    case 1: DiagnosticsTab(disk: disk, status: status)
                    case 2: DetailsTab(disk: disk, status: status)
                    default: DiskSettingsTab(disk: disk, status: status)
                    }
                }
                .padding(24)
                .transition(.opacity.combined(with: .offset(y: 8)))
                .id(tab)
            }
            .animation(.spring(response: 0.4, dampingFraction: 0.85), value: tab)
        }
    }
}

private struct TabBar: View {
    @Binding var tab: Int
    var ns: Namespace.ID
    let items = [("Übersicht", "gauge.with.dots.needle.67percent"), ("Diagnose", "stethoscope"),
                 ("Informationen", "info.circle"), ("Einstellungen", "slider.horizontal.3")]

    var body: some View {
        HStack(spacing: 4) {
            ForEach(items.indices, id: \.self) { i in
                Button {
                    withAnimation(.spring(response: 0.35, dampingFraction: 0.8)) { tab = i }
                } label: {
                    Label(L(items[i].0), systemImage: items[i].1)
                        .padding(.horizontal, 14).padding(.vertical, 7)
                        .foregroundStyle(tab == i ? Color.white : Color.primary)
                        .background {
                            if tab == i {
                                Capsule().fill(Theme.gradient).matchedGeometryEffect(id: "tab", in: ns)
                            }
                        }
                        .contentShape(Capsule())
                }
                .buttonStyle(.plain)
            }
            Spacer()
        }
    }
}

// MARK: header

private struct Header: View {
    @EnvironmentObject var store: DiskStore
    var disk: LinuxDisk
    var status: L2MVolumeStatus?

    var title: String { disk.volumeName ?? status?.label.nilIfEmpty ?? disk.mediaName.nilIfEmpty ?? L("Linux-Laufwerk") }
    var readOnly: Bool { status?.readOnly ?? false }
    var busy: Bool { store.busy.contains(disk.bsdName) }
    var attention: DiskAttention? { store.attention[disk.bsdName] }

    var stateText: String {
        if busy { return "Einen Moment …" }
        if attention == .needsReplug { return "Noch nicht erkannt – bitte neu anstecken" }
        if attention == .unsupported { return "Kein ext2/ext3/ext4 erkannt" }
        if attention == .needsRestart { return "Neustart des Macs nötig" }
        guard disk.isMounted else { return "Nicht eingehängt" }
        return readOnly ? "Eingehängt – nur lesen" : "Eingehängt – lesen und schreiben"
    }

    var body: some View {
        ZStack {
            AuroraBackground(strength: 0.5)
            HStack(spacing: 26) {
                DriveIcon(mounted: disk.isMounted, readOnly: readOnly, busy: busy)
                VStack(alignment: .leading, spacing: 6) {
                    Text(title)
                        .font(.system(size: 30, weight: .bold, design: .rounded))
                        .lineLimit(1)
                    HStack(spacing: 8) {
                        PulseDot(color: attention == .needsReplug || attention == .needsRestart ? Theme.warn
                                     : disk.isMounted ? (readOnly ? Theme.accent2 : Theme.ok) : .gray,
                                 pulsing: disk.isMounted || attention == .needsReplug)
                        Text(L(stateText)).foregroundStyle(.secondary)
                            .contentTransition(.opacity)
                    }
                    Text([disk.vendor, disk.model].filter { !$0.isEmpty }.joined(separator: " ") + " · \(disk.busProtocol) · \(disk.bsdName)")
                        .font(.caption).foregroundStyle(.secondary)
                    // buttons lose their text step by step instead of being cut off
                    ViewThatFits(in: .horizontal) {
                        HeaderActions(disk: disk)
                        HeaderActions(disk: disk, compact: 1)
                        HeaderActions(disk: disk, compact: 2)
                    }
                    .disabled(busy)
                    .padding(.top, 6)
                }
                Spacer()
            }
            .padding(.horizontal, 28)
        }
        .frame(height: 190)
        .clipShape(RoundedRectangle(cornerRadius: 0))
    }
}

private struct HeaderActions: View {
    @EnvironmentObject var store: DiskStore
    var disk: LinuxDisk
    /// 0: all buttons with text, 1: "Gerät trennen" only as symbol, 2: only the first button with text
    var compact = 0

    var body: some View {
        HStack(spacing: 10) {
            if disk.isMounted {
                Button { store.reveal(disk) } label: { Label("Im Finder öffnen", systemImage: "folder.fill") }
                    .buttonStyle(.borderedProminent).tint(Theme.accent2)
                Button { store.unmount(disk) } label: { Label("Auswerfen", systemImage: "eject.fill") }
                    .iconOnly(compact >= 2).help("Auswerfen")
            } else if store.attention[disk.bsdName] != nil {
                // nothing to mount until macOS has recognised the disk
            } else {
                Button { store.mount(disk) } label: { Label("Einhängen", systemImage: "play.fill") }
                    .buttonStyle(.borderedProminent).tint(Theme.accent)
                Button { store.mount(disk, readOnly: true) } label: { Label("Nur lesend einhängen", systemImage: "lock.fill") }
                    .iconOnly(compact >= 2).help("Nur lesend einhängen")
            }
            if disk.ejectable {
                Button { store.eject(disk) } label: { Label("Gerät trennen", systemImage: "cable.connector") }
                    .iconOnly(compact >= 1)
                    .help("Wirft alle Partitionen aus, danach kann das Kabel ab.")
            }
        }
    }
}

private extension View {
    @ViewBuilder func iconOnly(_ on: Bool) -> some View {
        if on { labelStyle(.iconOnly) } else { labelStyle(.titleAndIcon) }
    }
}

// MARK: overview

private struct OverviewTab: View {
    @EnvironmentObject var store: DiskStore
    var disk: LinuxDisk
    var status: L2MVolumeStatus?

    var body: some View {
        if let a = store.attention[disk.bsdName], !disk.isMounted {
            switch a {
            case .needsReplug: ReplugCard(disk: disk)
            case .unsupported: UnsupportedCard()
            case .needsRestart: RestartCard()
            }
        } else if let s = status, disk.isMounted {
            VStack(spacing: 18) {
                // side by side when both cards get their natural width, otherwise one below the other
                ViewThatFits(in: .horizontal) {
                    HStack(alignment: .top, spacing: 18) {
                        StorageCard(status: s)
                        InodeCard(status: s).fixedSize(horizontal: true, vertical: false)
                    }
                    .fixedSize(horizontal: false, vertical: true)
                    VStack(spacing: 18) {
                        StorageCard(status: s)
                        InodeCard(status: s)
                    }
                }
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 170), spacing: 14)], spacing: 14) {
                    StatTile(icon: s.readOnly ? "lock.fill" : "pencil", title: "Modus", value: s.readOnly ? "Nur lesen" : "Lesen & Schreiben",
                             tint: s.readOnly ? Theme.accent2 : Theme.ok)
                    StatTile(icon: s.state == "clean" ? "checkmark.seal.fill" : "exclamationmark.triangle.fill", title: "Zustand",
                             value: s.state == "clean" ? "Sauber" : (s.state == "errors" ? "Fehler gemeldet" : "Nicht sauber getrennt"),
                             tint: s.state == "clean" ? Theme.ok : Theme.warn)
                    StatTile(icon: "book.closed.fill", title: "Journal (Absturzschutz)", value: s.hasJournal ? "Aktiv" : "Keins",
                             tint: s.hasJournal ? Theme.ok : Theme.warn)
                    StatTile(icon: "number", title: "Prüfsummen", value: s.metadataChecksums ? "Ja (crc32c)" : "Nein", tint: Theme.accent2)
                    StatTile(icon: "clock.fill", title: "Zuletzt geschrieben", value: formatDate(s.lastWrite), tint: Theme.accent)
                    StatTile(icon: "arrow.triangle.2.circlepath", title: "Eingehängt (gesamt)", value: "\(s.mountCount)×", tint: Theme.accent)
                }
                if s.readOnly && !s.readOnlyReason.isEmpty && s.readOnlyReason != "requested" {
                    ReadOnlyExplanation(status: s)
                }
                if s.journalReplayed {
                    Card(title: "Journal wurde wiederhergestellt", icon: "bandage.fill") {
                        Text(LF(s.readOnly
                                ? "Das Laufwerk wurde zuletzt nicht sauber getrennt (z. B. Stromausfall). %u offene Vorgänge (%u Blöcke) wurden für die Anzeige im Speicher nachgeholt – auf der Platte selbst wurde nichts verändert."
                                : "Das Laufwerk wurde zuletzt nicht sauber getrennt (z. B. Stromausfall). %u offene Vorgänge (%u Blöcke) wurden sicher auf der Platte nachgeholt.",
                                s.journalTransactions, s.journalBlocksReplayed))
                    }
                }
            }
        } else {
            Card(title: "Noch nicht eingehängt", icon: "info.circle") {
                VStack(alignment: .leading, spacing: 8) {
                    Text(LF("Größe: %@", formatBytes(disk.size)))
                    Text(LF("Anschluss: %@", disk.busProtocol))
                    Text("Sobald das Laufwerk eingehängt ist, siehst du hier Belegung, Zustand und alle Details.")
                        .foregroundStyle(.secondary)
                }
            }
        }
    }
}

private struct StorageCard: View {
    var status: L2MVolumeStatus

    var body: some View {
        let s = status
        let used = s.blocks > s.freeBlocks ? s.blocks - s.freeBlocks : 0
        let fraction = s.blocks > 0 ? Double(used) / Double(s.blocks) : 0
        Card(title: "Speicherplatz", icon: "chart.pie.fill") {
            RingAndLabels {
                UsageRing(fraction: fraction, label: "\(Int((fraction * 100).rounded())) %", sublabel: "belegt")
                    .frame(width: 140, height: 140)
            } labels: {
                VStack(alignment: .leading, spacing: 8) {
                    Legend(color: Theme.ok, text: "Belegt", value: formatBytes(used * UInt64(s.blockSize)))
                    Legend(color: .primary.opacity(0.15), text: "Frei", value: formatBytes(s.freeBlocks * UInt64(s.blockSize)))
                    Legend(color: Theme.warn, text: "Für Linux-root reserviert", value: formatBytes(s.reservedBlocks * UInt64(s.blockSize)))
                    Legend(color: Theme.accent2, text: "Gesamt", value: formatBytes(s.blocks * UInt64(s.blockSize)))
                }
                .frame(maxWidth: 380) // keeps the values near their labels in a wide card
            }
        }
    }
}

private struct InodeCard: View {
    var status: L2MVolumeStatus

    var body: some View {
        let s = status
        let ifrac = s.inodes > 0 ? Double(s.inodes - s.freeInodes) / Double(s.inodes) : 0
        Card(title: "Dateiplätze (Inodes)", icon: "doc.on.doc.fill") {
            RingAndLabels {
                UsageRing(fraction: ifrac, lineWidth: 10, label: "\(Int((ifrac * 100).rounded())) %", sublabel: "genutzt")
                    .frame(width: 110, height: 110)
            } labels: {
                VStack(alignment: .leading, spacing: 6) {
                    Text(LF("%@ Dateien & Ordner", (s.inodes - s.freeInodes).formatted()))
                    Text(LF("%@ frei", s.freeInodes.formatted())).foregroundStyle(.secondary)
                }
            }
        }
    }
}

/// Ring with its labels beside it, or below it when the card is too narrow
/// (the labels would otherwise break letter by letter).
private struct RingAndLabels<Ring: View, Labels: View>: View {
    @ViewBuilder var ring: Ring
    @ViewBuilder var labels: Labels

    var body: some View {
        ViewThatFits(in: .horizontal) {
            HStack(spacing: 24) { ring; labels }
            VStack(spacing: 16) {
                ring
                labels.frame(maxWidth: .infinity, alignment: .leading)
            }
        }
    }
}

private struct Legend: View {
    var color: Color
    var text: String
    var value: String
    var body: some View {
        HStack(spacing: 8) {
            RoundedRectangle(cornerRadius: 3).fill(color).frame(width: 12, height: 12)
            Text(L(text)).foregroundStyle(.secondary).lineLimit(1)
            Spacer(minLength: 12)
            Text(value).font(.body.monospacedDigit()).fixedSize()
        }
    }
}

private struct ReadOnlyExplanation: View {
    var status: L2MVolumeStatus
    var text: String {
        switch status.readOnlyReason {
        case "feature": return LF("Das Laufwerk nutzt ext4-Funktionen, die ExtBridge nur lesen kann: %@. Deine Daten sind sicher; zum Schreiben bitte unter Linux nutzen.", status.unsupported.joined(separator: ", "))
        case "errors": return "Linux hat auf diesem Laufwerk Fehler vermerkt. Zum Schutz deiner Daten nur lesend. Lass unter Linux „e2fsck“ laufen."
        case "check": return "Die Prüfung vor dem Einhängen hat Fehler gefunden (Details unter Diagnose). Zum Schutz deiner Daten nur lesend. Reparatur unter Linux mit „e2fsck -f“."
        case "journal": return "Das Journal konnte nicht sicher wiederhergestellt werden. Zum Schutz nur lesend. Unter Linux mit „e2fsck“ prüfen."
        case "notClean": return "Das Laufwerk (ohne Journal) wurde nicht sauber getrennt. Zum Schutz nur lesend."
        default: return "Nur lesend eingehängt."
        }
    }
    var body: some View {
        Card(title: "Warum nur lesen?", icon: "lock.shield.fill") {
            Text(L(text))
        }
    }
}

// MARK: diagnostics

private struct DiagnosticsTab: View {
    @EnvironmentObject var store: DiskStore
    var disk: LinuxDisk
    var status: L2MVolumeStatus?
    @StateObject private var speed = SpeedTest()
    @State private var allowWrite = false
    @State private var showReport = false

    var body: some View {
        VStack(spacing: 18) {
            Card(title: "Konsistenz-Prüfung", icon: "checkmark.shield.fill") {
                if let s = status, disk.isMounted {
                    let v = store.verify[s.uuid]
                    HStack(alignment: .top, spacing: 22) {
                        ScanOrb(fraction: v?.fraction ?? 0, running: v?.running ?? false,
                                problems: v?.running == false ? v?.problems : nil)
                            .frame(width: 120, height: 120)
                        VStack(alignment: .leading, spacing: 10) {
                            Text("Prüft alle Prüfsummen, Belegungstabellen und Dateibäume – nur lesend, es wird nichts verändert.")
                                .foregroundStyle(.secondary)
                            if let v, !v.running {
                                Text(v.problems == 0 ? L("Keine Probleme gefunden.") : LF("%d Auffälligkeit(en) gefunden.", v.problems))
                                    .font(.headline)
                                    .foregroundStyle(v.problems == 0 ? Theme.ok : Theme.warn)
                                Button(L(showReport ? "Bericht ausblenden" : "Bericht anzeigen")) { withAnimation { showReport.toggle() } }
                            }
                            HStack {
                                if v?.running == true {
                                    Button("Abbrechen") { store.cancelVerify(s) }
                                } else {
                                    Button { store.startVerify(s) } label: { Label("Prüfung starten", systemImage: "play.fill") }
                                        .buttonStyle(.borderedProminent).tint(Theme.accent)
                                }
                            }
                        }
                    }
                    if showReport, let r = v?.report, !r.isEmpty {
                        ScrollView {
                            Text(r).font(.system(.caption, design: .monospaced)).textSelection(.enabled)
                                .frame(maxWidth: .infinity, alignment: .leading)
                        }
                        .frame(height: 180)
                        .padding(10)
                        .background(Color.black.opacity(0.25), in: RoundedRectangle(cornerRadius: 10))
                        .transition(.opacity.combined(with: .move(edge: .top)))
                    }
                } else {
                    Text("Zum Prüfen bitte zuerst einhängen.").foregroundStyle(.secondary)
                }
            }
            // the App Store sandbox cannot read or write test files on the disk
            #if !APPSTORE
            Card(title: "Geschwindigkeitstest", icon: "speedometer") {
                if let mp = disk.mountPoint {
                    let gauges = HStack(spacing: 30) {
                        SpeedGauge(value: speed.phase == .reading ? speed.current : speed.readMBs, title: "Lesen", active: speed.phase == .reading)
                        SpeedGauge(value: speed.phase == .writing ? speed.current : speed.writeMBs, title: "Schreiben", active: speed.phase == .writing)
                    }
                    // text beside the gauges only when it gets a comfortable width, otherwise below them
                    ViewThatFits(in: .horizontal) {
                        HStack(spacing: 30) {
                            gauges
                            speedControls(mp).frame(idealWidth: 300)
                        }
                        VStack(alignment: .leading, spacing: 16) {
                            gauges
                            speedControls(mp)
                        }
                    }
                } else {
                    Text("Zum Messen bitte zuerst einhängen.").foregroundStyle(.secondary)
                }
            }
            #endif
            Card(title: "Gerät", icon: "cpu") {
                InfoRow(label: "Hersteller / Modell", value: [disk.vendor, disk.model].filter { !$0.isEmpty }.joined(separator: " "))
                InfoRow(label: "Anschluss", value: disk.busProtocol)
                InfoRow(label: "Größe (Partition)", value: formatBytes(disk.size))
                InfoRow(label: "BSD-Name", value: LF("%@ (Gerät %@)", disk.bsdName, disk.wholeBSDName), copyable: true)
                if let p = disk.partitionUUID { InfoRow(label: "Partitions-UUID", value: p, copyable: true) }
                InfoRow(label: "SMART-Gesundheit", value: disk.busProtocol == "USB" ? "Über USB von macOS nicht abfragbar" : "–")
            }
        }
    }

    private func speedControls(_ mp: URL) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Liest die größte gefundene Datei direkt von der Platte (am Zwischenspeicher vorbei).")
                .foregroundStyle(.secondary)
            Toggle("Auch Schreiben testen (legt kurz 1 GB Testdatei an)", isOn: $allowWrite)
                .disabled(status?.readOnly ?? true)
            if !speed.fileName.isEmpty {
                Text(LF("Testdatei: %@", speed.fileName)).font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
            if case let .failed(m) = speed.phase { Text(L(m)).foregroundStyle(Theme.bad) }
            HStack {
                if speed.phase == .reading || speed.phase == .writing || speed.phase == .searching {
                    ProgressView().controlSize(.small)
                    Button("Stopp") { speed.cancel() }
                } else {
                    Button { speed.start(volume: mp, write: allowWrite && !(status?.readOnly ?? true)) } label: {
                        Label("Test starten", systemImage: "bolt.fill")
                    }
                    .buttonStyle(.borderedProminent).tint(Theme.accent2)
                }
            }
        }
    }
}

/// Circular scan indicator with a sweeping light.
private struct ScanOrb: View {
    var fraction: Double
    var running: Bool
    var problems: Int?
    @State private var sweep = false

    var body: some View {
        ZStack {
            Circle().stroke(Color.primary.opacity(0.08), lineWidth: 10)
            Circle()
                .trim(from: 0, to: fraction)
                .stroke(Theme.gradient, style: StrokeStyle(lineWidth: 10, lineCap: .round))
                .rotationEffect(.degrees(-90))
                .animation(.easeOut(duration: 0.4), value: fraction)
            if running {
                Circle()
                    .trim(from: 0, to: 0.12)
                    .stroke(Color.white.opacity(0.7), style: StrokeStyle(lineWidth: 4, lineCap: .round))
                    .padding(14)
                    .rotationEffect(.degrees(sweep ? 360 : 0))
                    .animation(.linear(duration: 1.4).repeatForever(autoreverses: false), value: sweep)
                    .onAppear { sweep = true }
                    .onDisappear { sweep = false }
                Text("\(Int(fraction * 100)) %").font(.title3.monospacedDigit().weight(.semibold))
                    .contentTransition(.numericText())
            } else if let p = problems {
                Image(systemName: p == 0 ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
                    .font(.system(size: 44))
                    .foregroundStyle(p == 0 ? Theme.ok : Theme.warn)
                    .symbolEffect(.bounce, value: p)
                    .transition(.scale.combined(with: .opacity))
            } else {
                Image(systemName: "shield.lefthalf.filled").font(.system(size: 38)).foregroundStyle(.secondary)
            }
        }
        .animation(.spring(response: 0.5, dampingFraction: 0.6), value: running)
    }
}

// MARK: details

private struct DetailsTab: View {
    var disk: LinuxDisk
    var status: L2MVolumeStatus?

    var body: some View {
        if let s = status {
            VStack(spacing: 18) {
                Card(title: "Dateisystem", icon: "internaldrive.fill") {
                    InfoRow(label: "Name (Label)", value: s.label.isEmpty ? "–" : s.label)
                    InfoRow(label: "UUID", value: s.uuid, copyable: true)
                    InfoRow(label: "Typ", value: s.features.contains("extent") ? "ext4" : (s.hasJournal ? "ext3" : "ext2"))
                    InfoRow(label: "Blockgröße", value: "\(s.blockSize) Bytes")
                    InfoRow(label: "Blöcke", value: LF("%@ (%@ frei)", s.blocks.formatted(), s.freeBlocks.formatted()))
                    InfoRow(label: "Blockgruppen", value: s.groups.formatted())
                    InfoRow(label: "Inode-Größe", value: "\(s.inodeSize) Bytes")
                    InfoRow(label: "Ordner-Index (Hash)", value: s.hashAlgorithm)
                    InfoRow(label: "Zuletzt eingehängt unter", value: s.lastMountedOn.isEmpty ? "–" : s.lastMountedOn)
                }
                Card(title: "Zeiten & Zähler", icon: "calendar") {
                    InfoRow(label: "Angelegt", value: formatDate(s.created))
                    InfoRow(label: "Zuletzt eingehängt", value: formatDate(s.lastMount))
                    InfoRow(label: "Zuletzt geschrieben", value: formatDate(s.lastWrite))
                    InfoRow(label: "Zuletzt geprüft (e2fsck)", value: formatDate(s.lastCheck))
                    InfoRow(label: "Einhängezähler", value: s.maxMountCount > 0 ? LF("%u von %d", s.mountCount, s.maxMountCount) : "\(s.mountCount)")
                    InfoRow(label: "Insgesamt geschrieben", value: formatBytes(s.kbytesWritten * 1024))
                    InfoRow(label: "Vermerkte Fehler", value: s.errorCount == 0 ? "keine" : LF("%u (zuletzt %@)", s.errorCount, formatDate(s.lastError)))
                }
                Card(title: "Journal", icon: "book.closed") {
                    InfoRow(label: "Vorhanden", value: s.hasJournal ? "ja" : "nein")
                    if s.hasJournal {
                        InfoRow(label: "Größe", value: formatBytes(UInt64(s.journalBlocks) * UInt64(s.blockSize)))
                        InfoRow(label: "Beim Einhängen wiederhergestellt", value: s.journalReplayed ? LF("ja: %u Vorgänge", s.journalTransactions) : "nicht nötig")
                    }
                }
                Card(title: "ext4-Funktionen", icon: "puzzlepiece.extension.fill") {
                    FlowLayout(spacing: 6) {
                        ForEach(s.features, id: \.self) { f in FeatureChip(name: f, bad: s.unsupported.contains(f)) }
                    }
                    if !s.unsupported.isEmpty {
                        Text("Orange: nur lesend unterstützt.").font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
        } else {
            Card(title: "Keine Daten", icon: "info.circle") {
                Text("Informationen über das Dateisystem gibt es, sobald das Laufwerk eingehängt ist.").foregroundStyle(.secondary)
            }
        }
    }
}

/// Simple wrapping layout for chips.
struct FlowLayout: Layout {
    var spacing: CGFloat = 6

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let w = proposal.width ?? 600
        var x: CGFloat = 0, y: CGFloat = 0, row: CGFloat = 0
        for s in subviews {
            let sz = s.sizeThatFits(.unspecified)
            if x + sz.width > w { x = 0; y += row + spacing; row = 0 }
            x += sz.width + spacing
            row = max(row, sz.height)
        }
        return CGSize(width: w, height: y + row)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        var x = bounds.minX, y = bounds.minY, row: CGFloat = 0
        for s in subviews {
            let sz = s.sizeThatFits(.unspecified)
            if x + sz.width > bounds.maxX { x = bounds.minX; y += row + spacing; row = 0 }
            s.place(at: CGPoint(x: x, y: y), proposal: ProposedViewSize(sz))
            x += sz.width + spacing
            row = max(row, sz.height)
        }
    }
}

// MARK: per disk settings

private struct DiskSettingsTab: View {
    @EnvironmentObject var store: DiskStore
    var disk: LinuxDisk
    var status: L2MVolumeStatus?

    var body: some View {
        if let s = status {
            let binding = Binding<L2MVolumePrefs>(get: { store.volumePrefs(s.uuid) }, set: { store.setVolumePrefs(s.uuid, $0) })
            VStack(spacing: 18) {
                Card(title: "Dieses Laufwerk", icon: "slider.horizontal.3") {
                    Toggle("Immer nur lesend einhängen", isOn: binding.readOnly)
                    Text("Dann schreibt der Mac kein einziges Byte auf diese Platte – ideal für wertvolle Sammlungen.")
                        .font(.caption).foregroundStyle(.secondary)
                    Toggle("Nicht automatisch einhängen", isOn: binding.noAutoMount)
                    if disk.isMounted && binding.wrappedValue.readOnly != s.readOnly {
                        Button {
                            store.remount(disk, readOnly: binding.wrappedValue.readOnly)
                        } label: {
                            Label("Jetzt neu einhängen, damit es wirkt", systemImage: "arrow.clockwise")
                        }
                        .buttonStyle(.borderedProminent).tint(Theme.accent)
                        .transition(.scale.combined(with: .opacity))
                    }
                }
                .animation(.spring(), value: binding.wrappedValue)
                Card(title: "Hilfe zur Sicherheit", icon: "lifepreserver.fill") {
                    Text("ExtBridge schreibt jede Änderung zuerst ins Journal – genau wie Linux. Selbst wenn mitten im Kopieren der Strom ausfällt oder das Kabel gezogen wird, bleibt das Dateisystem heil. Trotzdem gilt: vor dem Abziehen immer „Auswerfen“.")
                        .foregroundStyle(.secondary)
                }
            }
        } else {
            Card(title: "Einstellungen", icon: "slider.horizontal.3") {
                Text("Laufwerk-Einstellungen gibt es, sobald das Laufwerk einmal eingehängt war.").foregroundStyle(.secondary)
            }
        }
    }
}
