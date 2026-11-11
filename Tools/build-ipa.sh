#!/bin/bash
# Build TraceZero.ipa for a jailbroken iPhone (AppSync Unified installed).
#
#   Tools/build-ipa.sh                              # with the CarPlay entitlement
#   Tools/build-ipa.sh --no-carplay                 # without it
#   Tools/build-ipa.sh --xcode /Applications/Xcode-26.app
#
# The app targets iOS 12, which Xcode 27 and later refuse to build. If the
# selected Xcode is too new, an older one (16 or 26) installed next to it in
# /Applications is used automatically; --xcode picks one explicitly.
#
# The app is built without a certificate and then fake-signed with `ldid`,
# which a jailbroken phone with AppSync Unified accepts. Unlike a free Apple
# ID signature this never expires, and it can carry the CarPlay entitlement
# that Apple only grants on request. Needs Xcode and `brew install ldid`.
# Install the result with Filza, `ideviceinstaller install build/TraceZero.ipa`,
# or any IPA installer. See docs/INSTALL.md.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CARPLAY=1
XCODE_APP=""
while [ $# -gt 0 ]; do
  case "$1" in
    --no-carplay) CARPLAY=0; shift ;;
    --xcode) XCODE_APP="${2:-}"; shift 2 ;;
    -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

xcode_major() {  # major version of the Xcode at developer dir $1 ("" if unusable)
  DEVELOPER_DIR="$1" xcodebuild -version 2>/dev/null | sed -n 's/^Xcode \([0-9]*\).*/\1/p' || true
}

if [ -n "$XCODE_APP" ]; then
  export DEVELOPER_DIR="$XCODE_APP/Contents/Developer"
  [ -d "$DEVELOPER_DIR" ] || { echo "not an Xcode app: $XCODE_APP" >&2; exit 1; }
fi
command -v xcodebuild >/dev/null || { echo "xcodebuild not found: install Xcode" >&2; exit 1; }
CUR_DIR="${DEVELOPER_DIR:-$(xcode-select -p 2>/dev/null || true)}"
MAJOR="$(xcode_major "$CUR_DIR")"
if [ -z "$MAJOR" ]; then
  echo "xcodebuild does not work with the selected developer folder: ${CUR_DIR:-none}" >&2
  echo "Its own message:" >&2
  DEVELOPER_DIR="$CUR_DIR" xcodebuild -version >&2 || true
  cat >&2 <<'MSG'

Usually one of these fixes it:
  sudo xcode-select -s /Applications/Xcode.app/Contents/Developer   (if it points at CommandLineTools)
  sudo xcodebuild -license accept                                    (first run of a new Xcode)
  xcodebuild -runFirstLaunch                                         (installs Xcode's extra components)
MSG
  exit 1
fi
echo "==> Using Xcode $MAJOR ($CUR_DIR)" >&2
if [ -n "$MAJOR" ] && [ "$MAJOR" -ge 27 ]; then
  if [ -n "$XCODE_APP" ]; then
    echo "$XCODE_APP is Xcode $MAJOR, which cannot build for iOS 12. Use Xcode 16 or 26." >&2; exit 1
  fi
  FOUND=""
  for app in /Applications/Xcode*.app; do
    [ -d "$app/Contents/Developer" ] || continue
    m="$(xcode_major "$app/Contents/Developer")"
    if [ -n "$m" ] && [ "$m" -ge 15 ] && [ "$m" -le 26 ]; then FOUND="$app"; fi
  done
  if [ -z "$FOUND" ]; then
    cat >&2 <<MSG
Xcode $MAJOR cannot build apps for iOS 12 (its minimum is iOS 15).
Install Xcode 26 next to it (it does not replace Xcode $MAJOR):
  1. Download Xcode 26.x from https://developer.apple.com/download/all/ (free Apple ID)
  2. Unpack it and rename it to /Applications/Xcode-26.app
  3. Run this script again; it will find it.
See docs/INSTALL.md.
MSG
    exit 1
  fi
  export DEVELOPER_DIR="$FOUND/Contents/Developer"
  echo "==> Xcode $MAJOR is too new for iOS 12; using $FOUND" >&2
fi
command -v ldid >/dev/null || { echo "ldid not found: brew install ldid" >&2; exit 1; }

BUILD="$ROOT/build"
DERIVED="$BUILD/DerivedData"
mkdir -p "$BUILD"

XB="$(DEVELOPER_DIR="${DEVELOPER_DIR:-$CUR_DIR}" xcrun -f xcodebuild 2>/dev/null || echo xcodebuild)"
LOG="$BUILD/xcodebuild.log"
echo "==> Building (Release, unsigned); full log: $LOG"
# Device chatter ("Developer Disk Image", "iPhoneConnect") is filtered out:
# xcodebuild probes plugged-in phones even for a generic build.
xcodebuild -project "$ROOT/TraceZero.xcodeproj" -scheme TraceZero -configuration Release \
  -sdk iphoneos -destination 'generic/platform=iOS' -derivedDataPath "$DERIVED" \
  CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" build > "$LOG" 2>&1 || true
# Real diagnostics only (": error:" from compilers, "xcodebuild: error"); not the
# compiler command lines, which contain flags like -Werror.
grep -E "(: (fatal )?error: |^xcodebuild: error)" "$LOG" | grep -vE "iPhoneConnect|Developer Disk Image|DVTDevice|DDIError" | head -40 || true
WARN=$(grep -cE ": warning: " "$LOG" || true)
[ "${WARN:-0}" -gt 0 ] && echo "    ($WARN compiler warnings, see $LOG)"
grep -E "BUILD (SUCCEEDED|FAILED)" "$LOG" || true

APP="$DERIVED/Build/Products/Release-iphoneos/TraceZero.app"
if [ ! -d "$APP" ]; then
  if grep -q "is not installed. Please download and install the platform" "$LOG"; then
    cat >&2 <<MSG

This Xcode has not downloaded its iOS build components yet. Run:
  sudo $XB -license accept
  $XB -runFirstLaunch
  $XB -downloadPlatform iOS
then run this script again (unplugging the iPhone keeps the output quiet).
MSG
  else
    echo "build failed: see the errors above, or the full log in $LOG" >&2
  fi
  exit 1
fi

echo "==> Fake-signing"
if [ -d "$APP/Frameworks" ]; then
  for fw in "$APP"/Frameworks/*.framework; do
    [ -d "$fw" ] || continue
    bin="$fw/$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$fw/Info.plist")"
    ldid -S "$bin"
  done
  # Swift runtime libraries that some Xcode versions embed for iOS 12.0/12.1
  for lib in "$APP"/Frameworks/*.dylib; do
    [ -f "$lib" ] && ldid -S "$lib"
  done
fi
if [ "$CARPLAY" = 1 ]; then
  ldid -S"$ROOT/App/TraceZero-CarPlay.entitlements" "$APP/TraceZero"
  echo "    with com.apple.developer.carplay-maps"
else
  ldid -S "$APP/TraceZero"
fi

echo "==> Packaging"
rm -rf "$BUILD/Payload" "$BUILD/TraceZero.ipa"
mkdir -p "$BUILD/Payload"
cp -R "$APP" "$BUILD/Payload/"
(cd "$BUILD" && zip -qry TraceZero.ipa Payload)
rm -rf "$BUILD/Payload"
echo "==> $BUILD/TraceZero.ipa"
