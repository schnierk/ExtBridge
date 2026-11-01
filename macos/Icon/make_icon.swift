// ExtBridge - renders the app icon (1024 px PNG) with SwiftUI.
//   swift macos/Icon/make_icon.swift out.png
// SPDX-License-Identifier: MIT
import AppKit
import SwiftUI

struct BridgeShape: Shape {
    // a suspension bridge: deck, two towers and the main cable
    func path(in r: CGRect) -> Path {
        var p = Path()
        let deckY = r.minY + r.height * 0.78
        p.addRect(CGRect(x: r.minX, y: deckY, width: r.width, height: r.height * 0.07))
        let towerW = r.width * 0.055
        for x in [0.24, 0.76] {
            p.addRoundedRect(in: CGRect(x: r.minX + r.width * x - towerW / 2, y: r.minY + r.height * 0.12,
                                        width: towerW, height: deckY - r.minY - r.height * 0.12 + r.height * 0.07),
                             cornerSize: CGSize(width: towerW / 3, height: towerW / 3))
        }
        return p
    }
}

struct CableShape: Shape {
    func path(in r: CGRect) -> Path {
        var p = Path()
        let top = r.minY + r.height * 0.14, deck = r.minY + r.height * 0.78
        let lx = r.minX + r.width * 0.24, rx = r.minX + r.width * 0.76
        p.move(to: CGPoint(x: r.minX, y: deck - r.height * 0.05))
        p.addQuadCurve(to: CGPoint(x: lx, y: top), control: CGPoint(x: r.minX + r.width * 0.15, y: deck - r.height * 0.2))
        p.addQuadCurve(to: CGPoint(x: rx, y: top), control: CGPoint(x: r.midX, y: deck + r.height * 0.05))
        p.addQuadCurve(to: CGPoint(x: r.maxX, y: deck - r.height * 0.05), control: CGPoint(x: r.minX + r.width * 0.85, y: deck - r.height * 0.2))
        // hangers
        for i in 1..<10 {
            let t = CGFloat(i) / 10
            let x = lx + (rx - lx) * t
            let y = top + (deck - top) * (1 - 4 * (t - 0.5) * (t - 0.5)) * 0.92
            p.move(to: CGPoint(x: x, y: y))
            p.addLine(to: CGPoint(x: x, y: deck))
        }
        return p
    }
}

struct Icon: View {
    let orange = Color(red: 0.99, green: 0.60, blue: 0.16)
    let pink = Color(red: 0.93, green: 0.30, blue: 0.47)
    let blue = Color(red: 0.30, green: 0.47, blue: 1.0)

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 185, style: .continuous)
                .fill(LinearGradient(colors: [orange, pink, blue], startPoint: .topLeading, endPoint: .bottomTrailing))
                .overlay(
                    RoundedRectangle(cornerRadius: 185, style: .continuous)
                        .fill(RadialGradient(colors: [.white.opacity(0.35), .clear], center: UnitPoint(x: 0.3, y: 0.2),
                                             startRadius: 0, endRadius: 520))
                )
                .overlay(RoundedRectangle(cornerRadius: 185, style: .continuous).strokeBorder(.white.opacity(0.25), lineWidth: 4))
                .frame(width: 824, height: 824)
                .shadow(color: .black.opacity(0.35), radius: 24, y: 14)
            // bridge
            ZStack {
                CableShape().stroke(.white, style: StrokeStyle(lineWidth: 13, lineCap: .round, lineJoin: .round))
                BridgeShape().fill(.white)
            }
            .frame(width: 640, height: 300)
            .offset(y: -150)
            .shadow(color: .black.opacity(0.18), radius: 8, y: 6)
            // drive
            RoundedRectangle(cornerRadius: 70, style: .continuous)
                .fill(LinearGradient(colors: [Color(white: 0.20), Color(white: 0.06)], startPoint: .top, endPoint: .bottom))
                .overlay(RoundedRectangle(cornerRadius: 70, style: .continuous).strokeBorder(.white.opacity(0.22), lineWidth: 5))
                .overlay(alignment: .leading) {
                    Text("ext4")
                        .font(.system(size: 150, weight: .heavy, design: .rounded))
                        .foregroundStyle(LinearGradient(colors: [orange, pink], startPoint: .leading, endPoint: .trailing))
                        .padding(.leading, 58)
                }
                .overlay(alignment: .trailing) {
                    Circle().fill(Color(red: 0.3, green: 0.9, blue: 0.5))
                        .frame(width: 40, height: 40)
                        .shadow(color: Color(red: 0.3, green: 0.9, blue: 0.5), radius: 14)
                        .padding(.trailing, 60)
                }
                .frame(width: 600, height: 260)
                .offset(y: 165)
                .shadow(color: .black.opacity(0.35), radius: 18, y: 10)
        }
        .frame(width: 1024, height: 1024)
    }
}

MainActor.assumeIsolated {
    let renderer = ImageRenderer(content: Icon())
    renderer.scale = 1
    guard let cg = renderer.cgImage else { fatalError("render failed") }
    let rep = NSBitmapImageRep(cgImage: cg)
    let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "icon.png"
    try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: out))
    print("wrote \(out)")
}
