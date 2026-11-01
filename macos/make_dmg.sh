#!/bin/bash
# ExtBridge - builds the distributable disk image (DMG).
#   macos/make_dmg.sh path/to/ExtBridge.app [identity]
# With an identity ("Developer ID Application: ...") the DMG is signed too.
# The Finder window layout needs a logged-in GUI session (AppleScript).
set -euo pipefail
APP=${1:?usage: make_dmg.sh ExtBridge.app [identity]}
IDENTITY=${2:-}
cd "$(dirname "$0")/.."
VERSION=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$APP/Contents/Info.plist")
OUT=build/dmg
NAME="ExtBridge-$VERSION"
rm -rf "$OUT" && mkdir -p "$OUT/stage/.background"
ditto "$APP" "$OUT/stage/ExtBridge.app"
ln -s /Applications "$OUT/stage/Applications"
swift macos/Icon/make_dmg_background.swift "$OUT/stage/.background/background.png" >/dev/null

hdiutil create -quiet -volname "ExtBridge" -srcfolder "$OUT/stage" -fs HFS+ -format UDRW -size 80m "$OUT/rw.dmg"
DEV=$(hdiutil attach -readwrite -noverify -noautoopen "$OUT/rw.dmg" | awk '/Apple_HFS/ {print $1}')
MNT="/Volumes/ExtBridge"
osascript <<OSA || echo "note: Finder layout skipped (no GUI session)"
tell application "Finder"
  tell disk "ExtBridge"
    open
    set current view of container window to icon view
    set toolbar visible of container window to false
    set statusbar visible of container window to false
    set the bounds of container window to {200, 120, 800, 520}
    set opts to the icon view options of container window
    set arrangement of opts to not arranged
    set icon size of opts to 128
    set text size of opts to 13
    set background picture of opts to file ".background:background.png"
    set position of item "ExtBridge.app" of container window to {150, 210}
    set position of item "Applications" of container window to {450, 210}
    close
    open
    update without registering applications
    delay 1
    close
  end tell
end tell
OSA
# volume icon last: the Finder layout step above drops it otherwise
cp macos/App/AppIcon.icns "$MNT/.VolumeIcon.icns"
SetFile -a C "$MNT" 2>/dev/null || xcrun SetFile -a C "$MNT" || true
sync
hdiutil detach -quiet "$DEV" || hdiutil detach -force -quiet "$DEV"
hdiutil convert -quiet "$OUT/rw.dmg" -format UDZO -imagekey zlib-level=9 -o "$OUT/$NAME.dmg"
rm -f "$OUT/rw.dmg"
# macOS registers every app copy it sees; stale copies confuse the
# FSKit module lookup, so unregister the staging and DMG copies again
LSR=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
for p in "$PWD/$OUT/stage/ExtBridge.app" "$MNT/ExtBridge.app" "$MNT"; do "$LSR" -u "$p" 2>/dev/null || true; done
rm -rf "$OUT/stage"
if [ -n "$IDENTITY" ]; then
  codesign --force --timestamp --sign "$IDENTITY" "$OUT/$NAME.dmg"
fi
echo "$OUT/$NAME.dmg"
