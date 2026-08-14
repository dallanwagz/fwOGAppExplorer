#!/bin/sh
# Wrap the built macOS binary in a minimal .app bundle and sign it.
#
# Why a bundle at all, when the project's creed is "one executable": a bare
# Mach-O double-clicked in Finder opens a Terminal window behind the app, and
# Gatekeeper's stapler refuses to attach a notarization ticket to anything
# that is not a bundle, dmg or pkg. The bundle is four files of wrapper around
# the same single executable; nothing moves out of it.
#
# Usage: packaging/make_mac_app.sh [build-dir] [signing-identity]
#   build-dir defaults to build/mac-clang-release
#   identity  defaults to the Developer ID Application cert; pass "-" for
#             ad-hoc (unsigned distribution, local use only)
#
# Output: <build-dir>/fwOGAppExplorer.app
# Notarize + staple afterwards (needs `notarytool store-credentials <profile>`):
#   ditto -c -k --keepParent <build-dir>/fwOGAppExplorer.app /tmp/fwog.zip
#   xcrun notarytool submit /tmp/fwog.zip --keychain-profile <profile> --wait
#   xcrun stapler staple <build-dir>/fwOGAppExplorer.app
set -eu

repo=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-"$repo/build/mac-clang-release"}
identity=${2:-"Developer ID Application"}
app="$build/fwOGAppExplorer.app"

[ -x "$build/fwOGAppExplorer" ] || {
    echo "no built binary at $build/fwOGAppExplorer -- build the preset first" >&2
    exit 1
}

# Icon: the 256px frame of the same .ico Windows embeds, converted. sips reads
# the largest frame of an .ico when asked for png.
iconset=$(mktemp -d)/fwog.iconset
mkdir -p "$iconset"
sips -s format png "$repo/resources/product.ico" --out "$iconset/icon_256x256.png" >/dev/null
for s in 16 32 128; do
    sips -z $s $s "$iconset/icon_256x256.png" --out "$iconset/icon_${s}x${s}.png" >/dev/null
done

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$build/fwOGAppExplorer" "$app/Contents/MacOS/"
iconutil -c icns "$iconset" -o "$app/Contents/Resources/fwOGAppExplorer.icns"

# catalog/ lives beside the executable on every platform, which inside a
# bundle means Contents/MacOS/catalog -- but codesign refuses data files in
# MacOS/ (anything there is presumed nested code). So the real directory sits
# in Resources/, sealed as resources, and a symlink at the path the app
# actually reads keeps catalogDir() working unchanged. Drop sample .uf2s into
# Contents/Resources/catalog BEFORE signing.
mkdir -p "$app/Contents/Resources/catalog"
ln -s ../Resources/catalog "$app/Contents/MacOS/catalog"

cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key>      <string>com.freewili.fwOGAppExplorer</string>
    <key>CFBundleName</key>            <string>FreeWili OG App Explorer</string>
    <key>CFBundleExecutable</key>      <string>fwOGAppExplorer</string>
    <key>CFBundleIconFile</key>        <string>fwOGAppExplorer</string>
    <key>CFBundlePackageType</key>     <string>APPL</string>
    <key>CFBundleShortVersionString</key> <string>1.0.0</string>
    <key>LSMinimumSystemVersion</key>  <string>12.0</string>
    <key>NSHighResolutionCapable</key> <true/>
</dict>
</plist>
PLIST

if [ "$identity" = "-" ]; then
    codesign --force --sign - "$app"
else
    codesign --force --options runtime --timestamp --sign "$identity" "$app"
fi
codesign --verify --strict --verbose=2 "$app"
echo "built $app"
