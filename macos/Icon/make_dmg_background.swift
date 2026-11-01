// ExtBridge - renders the DMG window background (1200x800 = 600x400 @2x).
//   swift macos/Icon/make_dmg_background.swift out.png
// SPDX-License-Identifier: MIT
import AppKit
import SwiftUI

struct Background: View {
    let orange = Color(red: 0.99, green: 0.60, blue: 0.16)
    let pink = Color(red: 0.93, green: 0.30, blue: 0.47)
    let blue = Color(red: 0.30, green: 0.47, blue: 1.0)

    var body: some View {
        ZStack {
            LinearGradient(colors: [Color(red: 0.10, green: 0.10, blue: 0.14), Color(red: 0.16, green: 0.12, blue: 0.20)],
                           startPoint: .top, endPoint: .bottom)
            Circle().fill(RadialGradient(colors: [orange.opacity(0.35), .clear], center: .center, startRadius: 0, endRadius: 420))
                .frame(width: 840, height: 840).offset(x: -420, y: 240)
            Circle().fill(RadialGradient(colors: [blue.opacity(0.35), .clear], center: .center, startRadius: 0, endRadius: 420))
                .frame(width: 840, height: 840).offset(x: 440, y: -260)
            VStack(spacing: 18) {
                Text("ExtBridge")
                    .font(.system(size: 64, weight: .bold, design: .rounded))
                    .foregroundStyle(LinearGradient(colors: [orange, pink, blue], startPoint: .leading, endPoint: .trailing))
                Text("ext2 · ext3 · ext4 for macOS")
                    .font(.system(size: 30, weight: .medium, design: .rounded))
                    .foregroundStyle(.white.opacity(0.7))
                Spacer()
                Text("Drag ExtBridge into Applications  ·  ExtBridge in „Programme“ ziehen")
                    .font(.system(size: 26, weight: .medium, design: .rounded))
                    .foregroundStyle(.white.opacity(0.75))
            }
            .padding(.vertical, 56)
            // arrow between the two icons (icons sit at x=300 and x=900, y=420)
            Image(systemName: "arrow.right")
                .font(.system(size: 110, weight: .heavy))
                .foregroundStyle(LinearGradient(colors: [orange, pink], startPoint: .leading, endPoint: .trailing))
                .offset(y: 20)
        }
        .frame(width: 1200, height: 800)
    }
}

MainActor.assumeIsolated {
    let r = ImageRenderer(content: Background())
    r.scale = 1
    guard let cg = r.cgImage else { fatalError("render failed") }
    let rep = NSBitmapImageRep(cgImage: cg)
    rep.size = NSSize(width: 600, height: 400) // 144 dpi -> sharp on Retina
    let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "background.png"
    try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: out))
    print("wrote \(out)")
}
