#!/bin/sh
# Archive the iOS app and upload it to App Store Connect / TestFlight.
#
#   packaging/make_ios_upload.sh <build-number> [team-id]
#
# The build number is required and must be one App Store Connect has not seen
# for this version string -- it becomes FWOG_IOS_BUILD_NUMBER at configure
# time (see CMakeLists.txt: release state, not source). The team id defaults
# to the one the ios-release preset carries.
#
# Authentication is Xcode's: the Apple ID signed into Xcode > Settings >
# Accounts does the signing (-allowProvisioningUpdates) and the upload.
# Nothing here touches the App Store Connect web session.
#
# THE dSYM SEAT, and why this script exists at all: CMake's Xcode generator
# pins CONFIGURATION_BUILD_DIR per target, so during `xcodebuild archive` the
# dSYM is written to the CMake build tree while Xcode's archive-collection
# step looks in its own intermediates -- the archive gets an EMPTY dSYMs
# folder, the upload reports "Upload Symbols Failed", and a TestFlight crash
# cannot be symbolicated. MEASURED here: builds 2 and 3 both shipped without
# symbols before this script; symbols cannot be attached to a non-bitcode
# build after the fact, so the seat has to happen between archive and export.
# The UUID check below is what keeps the seat honest.

set -eu
cd "$(dirname "$0")/.."

BUILD_NUMBER="${1:?usage: packaging/make_ios_upload.sh <build-number> [team-id]}"
TEAM_ID="${2:-TLYZS5M3LM}"
BUILD_DIR=build/ios-release
ARCHIVE="$BUILD_DIR/fwog.xcarchive"

cmake --preset ios-release -DFWOG_IOS_BUILD_NUMBER="$BUILD_NUMBER" \
      -DFWOG_IOS_DEV_TEAM="$TEAM_ID"

# Release-iphoneos also goes: a prior archive leaves dangling symlinks there
# (into DerivedData ArchiveIntermediates) that make the next incremental
# device build fail on a bare MkDir. Removing it costs a rebuild this script
# was going to pay anyway.
rm -rf "$ARCHIVE" "$BUILD_DIR/export-upload" "$BUILD_DIR/Release-iphoneos"
xcodebuild -project "$BUILD_DIR/fwOGAppExplorer.xcodeproj" \
           -scheme fwOGAppExplorer -configuration Release \
           -destination 'generic/platform=iOS' \
           archive -archivePath "$ARCHIVE" -allowProvisioningUpdates

# Seat the dSYM the collection step missed, and refuse to continue unless its
# UUID matches the archived binary's -- a mismatched dSYM symbolicates
# nothing and is worse than none, because it looks like coverage.
DSYM="$BUILD_DIR/Release-iphoneos/fwOGAppExplorer.app.dSYM"
APP_BIN="$ARCHIVE/Products/Applications/fwOGAppExplorer.app/fwOGAppExplorer"
uuid_of() { xcrun dwarfdump --uuid "$1" | awk '{print $2}' | sort; }
[ -d "$DSYM" ] || { echo "ERROR: no dSYM at $DSYM" >&2; exit 1; }
if [ "$(uuid_of "$DSYM")" != "$(uuid_of "$APP_BIN")" ]; then
    echo "ERROR: dSYM UUID does not match the archived binary" >&2
    exit 1
fi
mkdir -p "$ARCHIVE/dSYMs"
cp -R "$DSYM" "$ARCHIVE/dSYMs/"

EXPORT_TMP=$(mktemp -t fwog-export)
EXPORT_PLIST="$EXPORT_TMP.plist"
cat > "$EXPORT_PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>method</key>      <string>app-store-connect</string>
    <key>destination</key> <string>upload</string>
    <key>teamID</key>      <string>$TEAM_ID</string>
    <key>signingStyle</key><string>automatic</string>
    <key>uploadSymbols</key><true/>
</dict>
</plist>
EOF

xcodebuild -exportArchive -archivePath "$ARCHIVE" \
           -exportOptionsPlist "$EXPORT_PLIST" \
           -exportPath "$BUILD_DIR/export-upload" -allowProvisioningUpdates
rm -f "$EXPORT_PLIST" "$EXPORT_TMP"

echo "Uploaded build $BUILD_NUMBER. It appears in App Store Connect after processing."
