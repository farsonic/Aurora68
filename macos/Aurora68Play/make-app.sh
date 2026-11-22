#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
#
# make-app.sh - build Aurora68 Play for macOS and wrap it into "Aurora68 Play.app".
#
#     ./make-app.sh                 build/Aurora68 Play.app (Apple silicon and Intel), ad-hoc signed
#
# Written on 22 November 2026 with the macOS app. Needs Xcode's command line tools (swift, lipo,
# iconutil, codesign) and python3 (the icon, from tools/mac/mkicons.py). GitHub's macOS runner does this
# for every change (.github/workflows/macos-player.yml). The app is signed ad hoc, not by a developer
# account: the first time, macOS asks for it to be allowed (README.md, "Opening it the first time").
# Nothing is deleted: an earlier build is moved aside.
set -eu
cd "$(dirname "$0")"
VERSION="${VERSION:-1.0}"
BUILD="${BUILD:-1}"
OUT=build
APP="$OUT/Aurora68 Play.app"

swift build -c release --arch arm64
swift build -c release --arch x86_64
mkdir -p "$OUT"
if [ -e "$APP" ]; then mv "$APP" "$OUT/Aurora68 Play.$(date +%Y%m%d%H%M%S).old.app"; fi
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
lipo -create -output "$APP/Contents/MacOS/Aurora68Play" \
    .build/arm64-apple-macosx/release/Aurora68Play .build/x86_64-apple-macosx/release/Aurora68Play

python3 make-icon.py "$OUT/AppIcon.iconset"
iconutil -c icns -o "$APP/Contents/Resources/AppIcon.icns" "$OUT/AppIcon.iconset"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleExecutable</key><string>Aurora68Play</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>CFBundleIdentifier</key><string>org.aurora68.play</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleName</key><string>Aurora68 Play</string>
    <key>CFBundleDisplayName</key><string>Aurora68 Play</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$BUILD</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.music</string>
    <key>LSMinimumSystemVersion</key><string>13.0</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSPrincipalClass</key><string>NSApplication</string>
    <key>NSHumanReadableCopyright</key><string>Aurora68, MPL-2.0</string>
    <key>NSLocalNetworkUsageDescription</key>
    <string>Aurora68 Play finds and plays your music server on your home network.</string>
    <key>NSAppTransportSecurity</key>
    <dict>
        <key>NSAllowsArbitraryLoads</key><true/>
        <key>NSAllowsLocalNetworking</key><true/>
    </dict>
</dict>
</plist>
PLIST
plutil -lint "$APP/Contents/Info.plist"
printf 'APPL????' > "$APP/Contents/PkgInfo"
codesign --force --deep --sign - "$APP"
codesign --verify --verbose "$APP"
echo "$APP: $(lipo -archs "$APP/Contents/MacOS/Aurora68Play")"
