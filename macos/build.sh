#!/bin/bash
# ExtBridge - builds ExtBridge.app (app + FSKit extension) without an Xcode
# project. Needs the Xcode command line tools (swiftc, clang); signing for
# real use needs a paid Apple Developer account (see docs/SIGNING.md).
#
#   macos/build.sh                      # compile, ad-hoc signed (for testing the build)
#   macos/build.sh --team ABCDE12345 \
#       --identity "Developer ID Application: Name (ABCDE12345)" \
#       --app-profile app.provisionprofile --ext-profile ext.provisionprofile
#   add --notarize PROFILE to notarize with a stored notarytool profile
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
OUT="$ROOT/build/mac"
VERSION=$(sed -n 's/#define L2M_VERSION "\(.*\)"/\1/p' core/include/l2m/l2m.h)
BUNDLE_PREFIX=${BUNDLE_PREFIX:-de.nexami}
TEAM=""
IDENTITY="-"
APP_PROFILE=""
EXT_PROFILE=""
NOTARIZE=""
ARCHS=${ARCHS:-"arm64 x86_64"}
MIN_OS=15.4
while [ $# -gt 0 ]; do
	case "$1" in
	--team) TEAM=$2; shift 2 ;;
	--identity) IDENTITY=$2; shift 2 ;;
	--app-profile) APP_PROFILE=$2; shift 2 ;;
	--ext-profile) EXT_PROFILE=$2; shift 2 ;;
	--notarize) NOTARIZE=$2; shift 2 ;;
	--prefix) BUNDLE_PREFIX=$2; shift 2 ;;
	*) echo "unknown option $1"; exit 2 ;;
	esac
done
APP_ID="$BUNDLE_PREFIX.ExtBridge"
EXT_ID="$APP_ID.FSModule"
GROUP="${TEAM:+$TEAM.}$BUNDLE_PREFIX.extbridge"

rm -rf "$OUT"
mkdir -p "$OUT/obj"

echo "== core (C) for $ARCHS"
for arch in $ARCHS; do
	mkdir -p "$OUT/obj/$arch"
	for c in core/src/*.c; do
		clang -c -O2 -g -std=c11 -arch "$arch" -mmacosx-version-min=$MIN_OS -Wall -Wextra -Wno-unused-parameter \
			-Icore/include -Icore/src "$c" -o "$OUT/obj/$arch/$(basename "$c" .c).o"
	done
	libtool -static -o "$OUT/libl2m-$arch.a" "$OUT"/obj/$arch/*.o
done
lipo -create "$OUT"/libl2m-*.a -output "$OUT/libl2m.a"

swift_build() { # product sources... (sets entry point/frameworks per product)
	local product=$1; shift
	local bins=()
	for arch in $ARCHS; do
		swiftc -O -g -swift-version 5 -parse-as-library -module-name "$product" \
			-target "$arch-apple-macos$MIN_OS" \
			-I macos/L2MCore -Xcc -Icore/include \
			"$@" "$OUT/libl2m.a" -o "$OUT/$product-$arch"
		bins+=("$OUT/$product-$arch")
	done
	lipo -create "${bins[@]}" -output "$OUT/$product"
}

echo "== FSKit extension"
swift_build ExtBridgeFS macos/Shared/Shared.swift macos/Extension/*.swift \
	-framework FSKit -framework ExtensionFoundation

echo "== app"
swift_build ExtBridge macos/Shared/Shared.swift macos/App/*.swift \
	-framework SwiftUI -framework AppKit -framework DiskArbitration -framework IOKit

echo "== bundle"
APP="$OUT/ExtBridge.app"
EXT="$APP/Contents/Extensions/ExtBridgeFS.appex"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$EXT/Contents/MacOS"
cp "$OUT/ExtBridge" "$APP/Contents/MacOS/ExtBridge"
cp "$OUT/ExtBridgeFS" "$EXT/Contents/MacOS/ExtBridgeFS"
subst() { sed -e "s/@APP_ID@/$APP_ID/g" -e "s/@EXT_ID@/$EXT_ID/g" -e "s/@VERSION@/$VERSION/g" -e "s/@GROUP@/$GROUP/g" \
	-e "s/@TEAM@/$TEAM/g" -e "s/@MIN_OS@/$MIN_OS/g" "$1" > "$2"; }
subst macos/App/Info.plist "$APP/Contents/Info.plist"
subst macos/Extension/Info.plist "$EXT/Contents/Info.plist"
subst macos/App/App.entitlements "$OUT/app.entitlements"
subst macos/Extension/Extension.entitlements "$OUT/ext.entitlements"
[ -f macos/App/AppIcon.icns ] && cp macos/App/AppIcon.icns "$APP/Contents/Resources/"
cp -R macos/App/en.lproj macos/App/de.lproj "$APP/Contents/Resources/"
[ -n "$APP_PROFILE" ] && cp "$APP_PROFILE" "$APP/Contents/embedded.provisionprofile"
[ -n "$EXT_PROFILE" ] && cp "$EXT_PROFILE" "$EXT/Contents/embedded.provisionprofile"

echo "== sign ($IDENTITY)"
if [ "$IDENTITY" = "-" ]; then
	# ad-hoc: proves the bundle is well-formed; macOS will NOT run the
	# extension without a real team signature (restricted entitlement)
	codesign --force --sign - --entitlements "$OUT/ext.entitlements" "$EXT"
	codesign --force --sign - --entitlements "$OUT/app.entitlements" "$APP"
else
	codesign --force --options runtime --timestamp --sign "$IDENTITY" --entitlements "$OUT/ext.entitlements" "$EXT"
	codesign --force --options runtime --timestamp --sign "$IDENTITY" --entitlements "$OUT/app.entitlements" "$APP"
fi
codesign --verify --deep --strict "$APP" && echo "signature ok"

if [ -n "$NOTARIZE" ]; then
	echo "== notarize"
	ditto -c -k --keepParent "$APP" "$OUT/ExtBridge.zip"
	xcrun notarytool submit "$OUT/ExtBridge.zip" --keychain-profile "$NOTARIZE" --wait
	xcrun stapler staple "$APP"
	ditto -c -k --keepParent "$APP" "$OUT/ExtBridge-$VERSION.zip"
fi
echo "built $APP ($VERSION)"
