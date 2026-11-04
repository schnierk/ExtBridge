# App Store listing (draft)

Everything App Store Connect asks for, ready to paste. Product IDs and prices of the tips are in
`Tips.storekit` (also usable for local StoreKit testing in Xcode: Scheme → Run → Options →
StoreKit Configuration).

| Field | Value |
|---|---|
| Name (30) | ExtBridge |
| Subtitle (30) | Use ext4 disks on your Mac |
| Bundle ID | de.nexami.ExtBridge (with the FSKit module de.nexami.ExtBridge.FSModule) |
| SKU | extbridge-macos |
| Primary category | Utilities |
| Secondary category | Developer Tools |
| Price | Free (tips as in-app purchases) |
| Age rating | 4+ (no objectionable content) |
| Privacy policy URL | https://github.com/schnierk/ExtBridge/blob/main/PRIVACY.md |
| Support URL | https://github.com/schnierk/ExtBridge/issues |
| Marketing URL | https://github.com/schnierk/ExtBridge |
| App privacy | Data not collected |
| Encryption | No (ITSAppUsesNonExemptEncryption = NO) |
| Copyright | 2026 Dirk Kollmann |

## Keywords (100)

ext4,ext3,ext2,linux,filesystem,disk,drive,mount,usb,raspberry pi,ubuntu,partition,ssd,sd card

## Promotional text (170)

Plug in a Linux disk and use it like any other: ext2, ext3 and ext4 in Finder – read and write,
with a journal like Linux. Free and open source.

## Description (English)

ExtBridge makes Linux disks work on your Mac. Attach a drive, USB stick or SD card formatted
with ext2, ext3 or ext4 – it appears in Finder like any other external disk, and you can open,
copy, rename and delete files.

• Read and write ext4, including the features modern Linux systems use by default (metadata
  checksums, 64-bit, large directories).
• Crash safe: every change goes through the journal, exactly like on Linux.
• Never locked out: if a disk has errors, ExtBridge mounts it read-only so your files stay
  accessible.
• Built on Apple's FSKit: no kernel extension, no lowered security settings.
• Fast: file data is moved by macOS itself.
• Diagnostics: usage, health, journal state, all file system details and a consistency check.
• Owner mapping between your Linux and Mac user, optional protection from Mac helper files.
• Free and open source (MIT). If it helps you, you can buy the developer a coffee.

After installing, turn on "ExtBridge ext4" once under System Settings → General → Login Items &
Extensions → File System Extensions. The app guides you.

## Beschreibung (Deutsch)

ExtBridge macht Linux-Festplatten auf dem Mac benutzbar. Steck eine Festplatte, einen USB-Stick
oder eine SD-Karte mit ext2, ext3 oder ext4 an – sie erscheint im Finder wie jede andere externe
Platte, und du kannst Dateien öffnen, kopieren, umbenennen und löschen.

• Lesen und Schreiben von ext4, auch mit den Funktionen, die aktuelle Linux-Systeme standardmäßig
  nutzen (Prüfsummen, 64 Bit, große Ordner).
• Absturzsicher: Jede Änderung geht über das Journal, genau wie unter Linux.
• Nie ausgesperrt: Hat eine Platte Fehler, bindet ExtBridge sie nur lesend ein – deine Dateien
  bleiben erreichbar.
• Gebaut auf Apples FSKit: keine Kernel-Erweiterung, keine abgesenkten Sicherheitseinstellungen.
• Schnell: Die Dateidaten bewegt macOS selbst.
• Diagnose: Belegung, Zustand, Journal, alle Dateisystem-Daten und eine Konsistenzprüfung.
• Besitzer-Zuordnung zwischen Linux- und Mac-Benutzer, auf Wunsch keine Mac-Hilfsdateien.
• Kostenlos und quelloffen (MIT). Wenn es dir hilft, kannst du dem Entwickler einen Kaffee
  spendieren.

Nach der Installation einmal unter Systemeinstellungen → Allgemein → Anmeldeobjekte &
Erweiterungen → Dateisystem-Erweiterungen „ExtBridge ext4“ einschalten. Die App führt dich hin.

## In-app purchases (consumable)

| Product ID | Reference name | Price tier (EUR) | Display name EN / DE |
|---|---|---|---|
| de.nexami.ExtBridge.tip.small | Tip small | 1.99 | Small coffee / Kleiner Kaffee |
| de.nexami.ExtBridge.tip.medium | Tip medium | 4.99 | Big coffee / Großer Kaffee |
| de.nexami.ExtBridge.tip.large | Tip large | 9.99 | Coffee and cake / Kaffee und Kuchen |

Review screenshot for each: a screenshot of the App Store build's coffee popover (three Apple tips).
App screenshots (2880×1800, demo data): `screenshots/en`, `screenshots/de`.

## Notes for App Review

ExtBridge is a file system module (FSKit) with a companion app. It lets macOS read and write Linux ext4 disks.

How to test:
1. Open ExtBridge and follow the two-step setup: turn on "ExtBridge ext4" under System Settings → General → Login Items & Extensions → File System Extensions.
2. Unzip the attached ExtBridge-Test.zip and double-click ExtBridge-Test.dmg. It contains a small ext4 disk that was formatted on Linux. It appears in Finder as "ExtBridge-Test" with a README, a photo and two text files.
3. Open, copy, rename, delete or create files on it. The ExtBridge window shows the disk's details, diagnostics and a consistency check.

The tip jar (coffee button at the bottom of the sidebar and in About) offers three consumable tips. They unlock nothing; the app works the same without them.

Attachment for the reviewers: `ExtBridge-Test.zip` (this folder) – a 64 MB read-write DMG with a GPT
disk and one ext4 partition (made with `mkfs.ext4 -d`, converted with `hdiutil convert -format UDRW`).
Review screenshot of the tips: `--snapshot` of an App Store build renders the popover as `tips.png`.
