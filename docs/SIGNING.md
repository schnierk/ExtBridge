# Signing and installing

macOS only loads an FSKit module whose signature carries the entitlement
`com.apple.developer.fskit.fsmodule`. That entitlement is *restricted*: it must be granted by a
provisioning profile of a **paid Apple Developer Program** team. Ad-hoc signatures, free personal
teams and "Sign to Run Locally" do not work (the mount hangs, `fskitd` logs `entitlement no`).
SIP stays enabled – no security settings have to be lowered.

## One-time setup (developer portal)

1. Join the Apple Developer Program and install Xcode.
2. In Xcode → Settings → Accounts add your Apple ID, then *Manage Certificates* → **+** →
   *Developer ID Application*. (Alternatively create it on developer.apple.com with a CSR.)
3. On <https://developer.apple.com/account/resources/identifiers> register two App IDs
   (use your own prefix instead of `de.nexami` and pass it to the build with `--prefix`):
   * `de.nexami.ExtBridge` – capability **App Groups**
   * `de.nexami.ExtBridge.FSModule` – capabilities **FSKit Module** and **App Groups**
4. Under *Profiles* create two **Developer ID** profiles (macOS), one per App ID, and download them.

## Build, sign, notarize

```bash
macos/build.sh --team ABCDE12345 \
  --identity "Developer ID Application: Your Name (ABCDE12345)" \
  --app-profile ~/Downloads/ExtBridge.provisionprofile \
  --ext-profile ~/Downloads/ExtBridgeFS.provisionprofile \
  --notarize NOTARY_PROFILE      # optional, see `xcrun notarytool store-credentials`
```

The app group is `TEAMID.de.nexami.extbridge` and is written into both bundles automatically.

## Install

1. Move `build/mac/ExtBridge.app` to `/Applications` and open it once.
2. System Settings → General → Login Items & Extensions → *By Category* →
   **File System Extensions** (ⓘ) → enable **ExtBridge ext4**. The app guides you there.
3. Attach a Linux disk – it mounts automatically. Manual mounting:
   `diskutil mount diskXsY`, `diskutil mount readOnly diskXsY` or
   `mount -F -t extbridge diskXsY /Volumes/x`.

After every rebuild with a new signature macOS disables the extension again; re-enable it in
System Settings or with `pluginkit -e use -i de.nexami.ExtBridge.FSModule`.

## Logs

```bash
log stream --predicate 'subsystem == "de.nexami.extbridge" OR process == "fskitd"' --level info
```
