//
//  ExtBridge - reusable, animated interface pieces.
//
//  SPDX-License-Identifier: MIT
//

import SwiftUI

enum Theme {
    static let accent = Color(red: 0.98, green: 0.62, blue: 0.16)      // warm Linux orange
    static let accent2 = Color(red: 0.36, green: 0.55, blue: 1.0)      // macOS blue
    static let ok = Color(red: 0.25, green: 0.78, blue: 0.45)
    static let warn = Color(red: 1.0, green: 0.72, blue: 0.2)
    static let bad = Color(red: 0.95, green: 0.33, blue: 0.3)
    static let gradient = LinearGradient(colors: [accent, Color(red: 0.93, green: 0.35, blue: 0.45), accent2],
                                         startPoint: .topLeading, endPoint: .bottomTrailing)
}

/// Localized text: German strings are the keys, en.lproj has the English
/// translations. Unknown strings (numbers, names) come back unchanged.
func L(_ key: String) -> String { NSLocalizedString(key, comment: "") }
func LF(_ key: String, _ args: CVarArg...) -> String { String(format: L(key), locale: Locale.current, arguments: args) }

func formatBytes(_ b: UInt64) -> String {
    ByteCountFormatter.string(fromByteCount: Int64(clamping: b), countStyle: .file)
}

func formatDate(_ d: Date?) -> String {
    guard let d else { return "–" }
    let f = DateFormatter()
    f.locale = Locale.current
    f.dateStyle = .medium
    f.timeStyle = .short
    return f.string(from: d)
}

/// Slowly drifting, soft colour clouds (radial gradients, no blur needed).
struct AuroraBackground: View {
    var colors: [Color] = [Theme.accent, Color.pink, Theme.accent2]
    var strength: Double = 0.45
    @State private var t = false

    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width, h = geo.size.height
            let r = max(w, h) * 0.55
            ZStack {
                ForEach(0..<colors.count, id: \.self) { i in
                    let x = w * (0.2 + 0.32 * CGFloat(i)) + (t ? 1 : -1) * w * 0.08 * CGFloat(i % 2 == 0 ? 1 : -1)
                    let y = h * (i == 1 ? 0.25 : 0.65) + (t ? -1 : 1) * h * 0.12
                    Circle()
                        .fill(RadialGradient(colors: [colors[i].opacity(strength), colors[i].opacity(0)],
                                             center: .center, startRadius: 0, endRadius: r / 2))
                        .frame(width: r, height: r)
                        .position(x: x, y: y)
                }
            }
            .animation(.easeInOut(duration: 9).repeatForever(autoreverses: true), value: t)
        }
        .onAppear { t = true }
        .clipped()
        .allowsHitTesting(false)
    }
}

/// The drive icon: floats gently, glows in the state colour and shows a
/// spinning ring while something happens.
struct DriveIcon: View {
    var mounted: Bool
    var readOnly: Bool
    var busy: Bool
    var size: CGFloat = 92
    @State private var float = false
    @State private var spin = false

    var stateColor: Color { !mounted ? .gray : (readOnly ? Theme.accent2 : Theme.ok) }

    var body: some View {
        ZStack {
            Circle()
                .fill(RadialGradient(colors: [stateColor.opacity(0.45), stateColor.opacity(0)], center: .center,
                                     startRadius: size * 0.2, endRadius: size * 0.8))
                .frame(width: size * 1.6, height: size * 1.6)
                .scaleEffect(mounted ? (float ? 1.06 : 0.94) : 1)
            if busy {
                Circle()
                    .trim(from: 0, to: 0.7)
                    .stroke(AngularGradient(colors: [stateColor.opacity(0), stateColor], center: .center),
                            style: StrokeStyle(lineWidth: 4, lineCap: .round))
                    .frame(width: size * 1.25, height: size * 1.25)
                    .rotationEffect(.degrees(spin ? 360 : 0))
                    .animation(.linear(duration: 1.1).repeatForever(autoreverses: false), value: spin)
                    .onAppear { spin = true }
                    .onDisappear { spin = false }
            }
            RoundedRectangle(cornerRadius: size * 0.22, style: .continuous)
                .fill(LinearGradient(colors: [Color(white: 0.28), Color(white: 0.12)], startPoint: .top, endPoint: .bottom))
                .frame(width: size, height: size * 0.72)
                .overlay(
                    RoundedRectangle(cornerRadius: size * 0.22, style: .continuous)
                        .strokeBorder(Theme.gradient, lineWidth: 2)
                )
                .overlay(alignment: .bottomTrailing) {
                    Circle()
                        .fill(stateColor)
                        .frame(width: size * 0.11, height: size * 0.11)
                        .shadow(color: stateColor, radius: mounted ? 6 : 0)
                        .padding(size * 0.12)
                }
                .overlay(alignment: .topLeading) {
                    Text("ext4")
                        .font(.system(size: size * 0.17, weight: .heavy, design: .rounded))
                        .foregroundStyle(Theme.gradient)
                        .padding(.leading, size * 0.13)
                        .padding(.top, size * 0.1)
                }
                .overlay {
                    Image(systemName: readOnly ? "lock.fill" : "arrow.left.arrow.right")
                        .font(.system(size: size * 0.2, weight: .bold))
                        .foregroundStyle(.white.opacity(mounted ? 0.85 : 0.35))
                        .offset(y: size * 0.06)
                        .contentTransition(.symbolEffect(.replace))
                }
                .offset(y: float ? -3 : 3)
        }
        .animation(.easeInOut(duration: 2.4).repeatForever(autoreverses: true), value: float)
        .onAppear { float = true }
    }
}

/// A ring that fills up with an animation.
struct UsageRing: View {
    var fraction: Double
    var lineWidth: CGFloat = 14
    var label: String
    var sublabel: String
    @State private var shown: Double = 0

    var color: Color {
        if fraction > 0.95 { return Theme.bad }
        if fraction > 0.85 { return Theme.warn }
        return Theme.ok
    }

    var body: some View {
        ZStack {
            Circle().stroke(Color.primary.opacity(0.08), lineWidth: lineWidth)
            Circle()
                .trim(from: 0, to: shown)
                .stroke(AngularGradient(colors: [color.opacity(0.6), color, Theme.accent], center: .center),
                        style: StrokeStyle(lineWidth: lineWidth, lineCap: .round))
                .rotationEffect(.degrees(-90))
                .shadow(color: color.opacity(0.4), radius: 6)
            VStack(spacing: 2) {
                Text(label)
                    .font(.system(size: 22, weight: .bold, design: .rounded))
                    .contentTransition(.numericText())
                Text(L(sublabel)).font(.caption).foregroundStyle(.secondary)
            }
        }
        .onAppear { withAnimation(.spring(response: 1.2, dampingFraction: 0.8).delay(0.15)) { shown = fraction } }
        .onChange(of: fraction) { _, v in withAnimation(.spring(response: 0.8, dampingFraction: 0.85)) { shown = v } }
    }
}

/// Small breathing status dot for lists.
struct PulseDot: View {
    var color: Color
    var pulsing: Bool
    @State private var on = false

    var body: some View {
        ZStack {
            if pulsing {
                Circle().fill(color.opacity(0.35))
                    .frame(width: 16, height: 16)
                    .scaleEffect(on ? 1.2 : 0.6)
                    .opacity(on ? 0 : 1)
                    .animation(.easeOut(duration: 1.6).repeatForever(autoreverses: false), value: on)
            }
            Circle().fill(color).frame(width: 8, height: 8)
        }
        .frame(width: 16, height: 16)
        .onAppear { on = true }
    }
}

struct StatTile: View {
    var icon: String
    var title: String
    var value: String
    var tint: Color = Theme.accent2
    @State private var hover = false

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Image(systemName: icon)
                .font(.title3)
                .foregroundStyle(tint)
                .symbolEffect(.bounce, value: hover)
            Text(L(value))
                .font(.system(.title3, design: .rounded).weight(.semibold))
                .contentTransition(.numericText())
                .lineLimit(1)
                .minimumScaleFactor(0.6)
            Text(L(title)).font(.caption).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(14)
        .background(.background.secondary, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous).strokeBorder(tint.opacity(hover ? 0.5 : 0.12)))
        .scaleEffect(hover ? 1.02 : 1)
        .animation(.spring(response: 0.3, dampingFraction: 0.7), value: hover)
        .onHover { hover = $0 }
    }
}

struct FeatureChip: View {
    var name: String
    var bad: Bool

    var body: some View {
        Text(name)
            .font(.system(.caption, design: .monospaced))
            .padding(.horizontal, 8)
            .padding(.vertical, 3)
            .background((bad ? Theme.warn : Theme.accent2).opacity(0.15), in: Capsule())
            .overlay(Capsule().strokeBorder((bad ? Theme.warn : Theme.accent2).opacity(0.35)))
    }
}

struct InfoRow: View {
    var label: String
    var value: String
    var copyable = false
    @State private var copied = false

    var body: some View {
        HStack(alignment: .firstTextBaseline) {
            Text(L(label)).foregroundStyle(.secondary).frame(width: 190, alignment: .leading)
            Text(L(value)).textSelection(.enabled)
            Spacer()
            if copyable {
                Button {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(value, forType: .string)
                    withAnimation { copied = true }
                    DispatchQueue.main.asyncAfter(deadline: .now() + 1.2) { withAnimation { copied = false } }
                } label: {
                    Image(systemName: copied ? "checkmark" : "doc.on.doc")
                        .contentTransition(.symbolEffect(.replace))
                }
                .buttonStyle(.borderless)
                .help(L("Kopieren"))
            }
        }
        .padding(.vertical, 3)
    }
}

/// Glass card used for every section.
struct Card<Content: View>: View {
    var title: String
    var icon: String
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Label(L(title), systemImage: icon)
                .font(.headline)
            content
        }
        .padding(18)
        // fills the row height when cards stand side by side, so they line up
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 18, style: .continuous).strokeBorder(Color.primary.opacity(0.06)))
    }
}

/// Animated half-circle speedometer.
struct SpeedGauge: View {
    var value: Double
    var maxValue: Double = 1100
    var title: String
    var active: Bool

    var body: some View {
        let f = min(value / maxValue, 1)
        ZStack {
            Circle()
                .trim(from: 0.5, to: 1)
                .stroke(Color.primary.opacity(0.08), style: StrokeStyle(lineWidth: 16, lineCap: .round))
            Circle()
                .trim(from: 0.5, to: 0.5 + f / 2)
                .stroke(Theme.gradient, style: StrokeStyle(lineWidth: 16, lineCap: .round))
                .animation(.spring(response: 0.5, dampingFraction: 0.7), value: f)
            Rectangle()
                .fill(Color.primary)
                .frame(width: 3, height: 58)
                .offset(y: -29)
                .rotationEffect(.degrees(-90 + 180 * f))
                .animation(.spring(response: 0.5, dampingFraction: 0.6), value: f)
            Circle().fill(Color.primary).frame(width: 10, height: 10)
            VStack(spacing: 0) {
                Text(String(format: "%.0f", value))
                    .font(.system(size: 26, weight: .bold, design: .rounded))
                    .contentTransition(.numericText())
                    .animation(.default, value: Int(value))
                Text("MB/s").font(.caption).foregroundStyle(.secondary)
                Text(L(title)).font(.caption.weight(.semibold)).foregroundStyle(active ? Theme.accent : .secondary)
            }
            .offset(y: 48)
        }
        .frame(width: 170, height: 170)
    }
}

// MARK: support / about

enum Links {
    private static func url(_ key: String) -> URL? {
        guard let s = Bundle.main.object(forInfoDictionaryKey: key) as? String, !s.isEmpty else { return nil }
        return URL(string: s)
    }
    /// "Buy me a coffee" by card, Apple Pay & co. (Stripe link, Info.plist L2MSupportURL)
    static var support: URL? { url("L2MSupportURL") }
    /// "Buy me a coffee" by PayPal (paypal.me link, Info.plist L2MPayPalURL)
    static var paypal: URL? { url("L2MPayPalURL") }
    /// project page (Info.plist L2MProjectURL)
    static var project: URL? { url("L2MProjectURL") }
    static var issues: URL? { project.map { $0.appendingPathComponent("issues") } }
    static var version: String { (Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String) ?? "?" }
}

/// A coffee cup with rising steam.
struct SteamingCup: View {
    var size: CGFloat = 16
    @State private var rise = false

    var body: some View {
        ZStack(alignment: .bottom) {
            Image(systemName: "cup.and.saucer.fill")
                .font(.system(size: size))
            HStack(spacing: size * 0.12) {
                ForEach(0..<3, id: \.self) { i in
                    Capsule()
                        .fill(.white.opacity(0.85))
                        .frame(width: size * 0.09, height: size * 0.35)
                        .offset(y: rise ? -size * 0.9 : -size * 0.45)
                        .opacity(rise ? 0 : 0.9)
                        .animation(.easeOut(duration: 1.6).repeatForever(autoreverses: false).delay(Double(i) * 0.35), value: rise)
                }
            }
            .offset(x: -size * 0.08)
        }
        .onAppear { rise = true }
    }
}

/// "Buy me a coffee". With both a PayPal and a card link it asks how to
/// pay; with only one of them it opens that link right away. App Store
/// builds offer tips through Apple instead (TipJar.swift).
struct CoffeeButton: View {
    var compact = false
    @State private var hover = false
    @State private var choosing = false

    var body: some View {
        if Self.available {
            Button {
                #if APPSTORE
                choosing.toggle()
                #else
                if Links.paypal != nil && Links.support != nil {
                    choosing.toggle()
                } else if let only = Links.paypal ?? Links.support {
                    NSWorkspace.shared.open(only)
                }
                #endif
            } label: {
                HStack(spacing: 8) {
                    SteamingCup(size: compact ? 13 : 17)
                    Text(L(compact ? "Unterstützen" : "Einen Kaffee spendieren"))
                        .fontWeight(.semibold)
                }
                .foregroundStyle(.white)
                .padding(.horizontal, compact ? 10 : 16)
                .padding(.vertical, compact ? 5 : 9)
                .background(Capsule().fill(LinearGradient(colors: [Theme.accent, Color(red: 0.93, green: 0.35, blue: 0.45)],
                                                          startPoint: .leading, endPoint: .trailing)))
                .shadow(color: Theme.accent.opacity(hover ? 0.6 : 0.25), radius: hover ? 10 : 4)
                .scaleEffect(hover ? 1.05 : 1)
            }
            .buttonStyle(.plain)
            .onHover { h in withAnimation(.spring(response: 0.3, dampingFraction: 0.6)) { hover = h } }
            .help(L("Gefällt dir ExtBridge? Spendier mir einen Kaffee ☕"))
            .popover(isPresented: $choosing, arrowEdge: .bottom) {
                #if APPSTORE
                TipJarView { choosing = false }
                #else
                PaymentChoice { choosing = false }
                #endif
            }
        }
    }

    /// App Store builds tip through Apple; other builds need a link.
    static var available: Bool {
        #if APPSTORE
        true
        #else
        Links.paypal != nil || Links.support != nil
        #endif
    }
}

/// The ways to pay for the coffee, shown in the coffee button's popover.
struct PaymentChoice: View {
    var done: () -> Void
    @State private var appear = false

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 8) {
                SteamingCup(size: 15).foregroundStyle(Theme.accent)
                Text(L("Wie möchtest du zahlen?")).font(.headline)
            }
            .padding(.bottom, 2)
            if let p = Links.paypal {
                PaymentRow(icon: "p.circle.fill", tint: Color(red: 0.0, green: 0.44, blue: 0.73),
                           title: "PayPal", detail: L("Mit deinem PayPal-Konto"), url: p, done: done)
                    .opacity(appear ? 1 : 0).offset(y: appear ? 0 : 8)
                    .animation(.spring(response: 0.45, dampingFraction: 0.75), value: appear)
            }
            if let s = Links.support {
                PaymentRow(icon: "creditcard.fill", tint: Theme.accent2,
                           title: L("Karte, Apple Pay & mehr"), detail: L("Sicher bezahlen über Stripe"), url: s, done: done)
                    .opacity(appear ? 1 : 0).offset(y: appear ? 0 : 8)
                    .animation(.spring(response: 0.45, dampingFraction: 0.75).delay(0.08), value: appear)
            }
            Text(L("Den Betrag wählst du selbst – jeder Kaffee hilft."))
                .font(.caption).foregroundStyle(.secondary)
                .padding(.top, 2)
        }
        .padding(16)
        .frame(width: 300)
        .onAppear { appear = true }
    }
}

private struct PaymentRow: View {
    let icon: String
    let tint: Color
    let title: String
    let detail: String
    let url: URL
    var done: () -> Void
    @State private var hover = false

    var body: some View {
        Button {
            NSWorkspace.shared.open(url)
            done()
        } label: {
            HStack(spacing: 12) {
                Image(systemName: icon)
                    .font(.system(size: 22))
                    .foregroundStyle(tint)
                    .frame(width: 28)
                VStack(alignment: .leading, spacing: 2) {
                    Text(title).fontWeight(.semibold)
                    Text(detail).font(.caption).foregroundStyle(.secondary)
                }
                Spacer(minLength: 8)
                Image(systemName: "arrow.up.forward")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.secondary)
                    .offset(x: hover ? 2 : 0, y: hover ? -2 : 0)
            }
            .padding(10)
            .background(RoundedRectangle(cornerRadius: 10).fill(tint.opacity(hover ? 0.16 : 0.07)))
            .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(tint.opacity(hover ? 0.5 : 0.2)))
            .contentShape(RoundedRectangle(cornerRadius: 10))
            .scaleEffect(hover ? 1.02 : 1)
        }
        .buttonStyle(.plain)
        .onHover { h in withAnimation(.spring(response: 0.3, dampingFraction: 0.7)) { hover = h } }
    }
}

struct AboutView: View {
    @State private var appear = false

    var body: some View {
        VStack(spacing: 14) {
            Image(nsImage: NSApp.applicationIconImage)
                .resizable()
                .frame(width: 110, height: 110)
                .scaleEffect(appear ? 1 : 0.6)
                .rotationEffect(.degrees(appear ? 0 : -12))
                .animation(.spring(response: 0.6, dampingFraction: 0.55), value: appear)
            Text("ExtBridge").font(.system(size: 26, weight: .bold, design: .rounded))
            Text(L("ext2/3/4-Laufwerke für macOS")).foregroundStyle(.secondary)
            Text(LF("Version %@", Links.version)).font(.caption).foregroundStyle(.secondary)
            Text(L("ExtBridge ist kostenlos und quelloffen. Wenn es dir hilft, freue ich mich über einen Kaffee – das hält die Entwicklung am Laufen."))
                .multilineTextAlignment(.center)
                .frame(maxWidth: 380)
                .padding(.top, 4)
            CoffeeButton()
            HStack(spacing: 18) {
                if let p = Links.project {
                    Link(L("Quellcode auf GitHub"), destination: p)
                }
                if let i = Links.issues {
                    Link(L("Fehler melden"), destination: i)
                }
            }
            .font(.callout)
            Text(L("Lizenz: MIT – frei für alle.")).font(.caption).foregroundStyle(.secondary)
        }
        .padding(28)
        .frame(maxWidth: .infinity)
        .onAppear { appear = true }
    }
}
