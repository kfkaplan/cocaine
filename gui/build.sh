#!/bin/sh
# Builds Cocaine.app, the launcher for cocaine, into gui/build/.
#
# usage: gui/build.sh [--zip]     --zip also makes build/Cocaine.zip, for a release
#
# Needs Xcode's command line tools (Swift). Apple Silicon only, macOS 14 or
# later; signed for this Mac only (ad hoc), not by a developer ID.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=$here/build
app=$out/Cocaine.app

rm -rf "$app" "$out/AppIcon.iconset"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"

swiftc -O -swift-version 5 -target arm64-apple-macos14.0 -parse-as-library \
  -o "$app/Contents/MacOS/Cocaine" "$here"/Sources/*.swift
cp "$here/Info.plist" "$app/Contents/Info.plist"

swiftc -O -o "$out/make-icon" "$here/make-icon.swift"
"$out/make-icon" "$out/AppIcon.iconset"
iconutil -c icns "$out/AppIcon.iconset" -o "$app/Contents/Resources/AppIcon.icns"

codesign --force --sign - "$app"
echo "built $app"

if [ "$1" = "--zip" ]; then
  rm -f "$out/Cocaine.zip"
  ditto -c -k --keepParent "$app" "$out/Cocaine.zip"
  echo "zipped $out/Cocaine.zip"
fi
