//
// ExtBridge - "Buy me a coffee" through Apple (App Store builds only).
//
// The App Store does not allow payment links to outside services, so the
// App Store build sells three consumable tips through StoreKit instead of
// opening PayPal or Stripe. Product IDs: <bundle id>.tip.small/medium/large.
//
// SPDX-License-Identifier: MIT
//

#if APPSTORE
import StoreKit
import SwiftUI

@MainActor
final class TipJar: ObservableObject {
    static let shared = TipJar()

    enum State: Equatable { case loading, ready, buying(String), thanks, failed(String) }

    @Published private(set) var products: [Product] = []
    @Published var state: State = .loading
    private var updates: Task<Void, Never>?

    static var productIDs: [String] {
        let base = Bundle.main.bundleIdentifier ?? "ExtBridge"
        return ["small", "medium", "large"].map { "\(base).tip.\($0)" }
    }

    /// Finishes transactions that arrive outside a purchase in this window
    /// (Ask to Buy, interrupted purchases); Apple requires listening.
    func start() {
        guard updates == nil else { return }
        updates = Task.detached {
            for await result in Transaction.updates {
                if case .verified(let t) = result { await t.finish() }
            }
        }
    }

    func load() async {
        if !products.isEmpty { state = .ready; return }
        state = .loading
        do {
            products = try await Product.products(for: Self.productIDs).sorted { $0.price < $1.price }
            state = products.isEmpty ? .failed(L("Trinkgeld ist gerade nicht verfügbar.")) : .ready
        } catch {
            state = .failed(error.localizedDescription)
        }
    }

    func buy(_ product: Product) async {
        state = .buying(product.id)
        do {
            switch try await product.purchase() {
            case .success(let result):
                if case .verified(let t) = result {
                    await t.finish()
                    state = .thanks
                } else {
                    state = .failed(L("Der Kauf konnte nicht bestätigt werden."))
                }
            case .userCancelled, .pending:
                state = .ready
            @unknown default:
                state = .ready
            }
        } catch {
            state = .failed(error.localizedDescription)
        }
    }
}

/// The coffee button's popover in App Store builds.
struct TipJarView: View {
    @ObservedObject var jar = TipJar.shared
    var done: () -> Void
    @State private var appear = false

    private func cups(_ i: Int) -> String { String(repeating: "☕", count: i + 1) }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 8) {
                SteamingCup(size: 15).foregroundStyle(Theme.accent)
                Text(L("Einen Kaffee spendieren")).font(.headline)
            }
            .padding(.bottom, 2)
            switch jar.state {
            case .loading:
                HStack { ProgressView().controlSize(.small); Text(L("Lädt …")).foregroundStyle(.secondary) }
                    .frame(maxWidth: .infinity, minHeight: 80)
            case .thanks:
                VStack(spacing: 8) {
                    Image(systemName: "heart.fill")
                        .font(.system(size: 34))
                        .foregroundStyle(Theme.gradient)
                        .scaleEffect(appear ? 1 : 0.3)
                        .animation(.spring(response: 0.5, dampingFraction: 0.45), value: appear)
                    Text(L("Vielen Dank! Das hält die Entwicklung am Laufen.")).multilineTextAlignment(.center)
                    Button(L("Schließen")) { done(); jar.state = .ready }
                }
                .frame(maxWidth: .infinity)
                .onAppear { appear = false; withAnimation { appear = true } }
            case .failed(let message):
                Text(message).foregroundStyle(.secondary)
                Button(L("Erneut versuchen")) { Task { await jar.load() } }
            case .ready, .buying:
                ForEach(Array(jar.products.enumerated()), id: \.element.id) { i, p in
                    TipRow(cups: cups(i), product: p, busy: jar.state == .buying(p.id)) {
                        Task { await jar.buy(p) }
                    }
                    .disabled(jar.state != .ready)
                }
                Text(L("Bezahlt wird sicher über Apple. Es ist ein einmaliges Trinkgeld, kein Abo."))
                    .font(.caption).foregroundStyle(.secondary)
                    .padding(.top, 2)
            }
        }
        .padding(16)
        .frame(width: 300)
        .task { await jar.load() }
    }
}

private struct TipRow: View {
    let cups: String
    let product: Product
    let busy: Bool
    var action: () -> Void
    @State private var hover = false

    var body: some View {
        Button(action: action) {
            HStack(spacing: 12) {
                Text(cups).font(.system(size: 18)).frame(width: 64, alignment: .leading)
                Text(product.displayName).fontWeight(.semibold)
                Spacer(minLength: 8)
                if busy { ProgressView().controlSize(.small) } else { Text(product.displayPrice).monospacedDigit() }
            }
            .padding(10)
            .background(RoundedRectangle(cornerRadius: 10).fill(Theme.accent.opacity(hover ? 0.16 : 0.07)))
            .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Theme.accent.opacity(hover ? 0.5 : 0.2)))
            .contentShape(RoundedRectangle(cornerRadius: 10))
            .scaleEffect(hover ? 1.02 : 1)
        }
        .buttonStyle(.plain)
        .onHover { h in withAnimation(.spring(response: 0.3, dampingFraction: 0.7)) { hover = h } }
    }
}
#endif
