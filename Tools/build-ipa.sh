#!/bin/bash
# Build TraceZero.ipa for a jailbroken iPhone (AppSync Unified installed).
#
#   Tools/build-ipa.sh              # with the CarPlay entitlement
#   Tools/build-ipa.sh --no-carplay # without it
#
# The app is built without a certificate and then fake-signed with `ldid`,
# which a jailbroken phone with AppSync Unified accepts. Unlike a free Apple
# ID signature this never expires, and it can carry the CarPlay entitlement
# that Apple only grants on request. Needs Xcode and `brew install ldid`.
# Install the result with Filza, `ideviceinstaller -i build/TraceZero.ipa`,
# or any IPA installer. See docs/INSTALL.md.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CARPLAY=1
for arg in "$@"; do
  case "$arg" in
    --no-carplay) CARPLAY=0 ;;
    -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

command -v xcodebuild >/dev/null || { echo "xcodebuild not found: install Xcode" >&2; exit 1; }
command -v ldid >/dev/null || { echo "ldid not found: brew install ldid" >&2; exit 1; }

BUILD="$ROOT/build"
DERIVED="$BUILD/DerivedData"
mkdir -p "$BUILD"

echo "==> Building (Release, unsigned)"
xcodebuild -project "$ROOT/TraceZero.xcodeproj" -scheme TraceZero -configuration Release \
  -sdk iphoneos -destination 'generic/platform=iOS' -derivedDataPath "$DERIVED" \
  CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" build | \
  grep -E "error|warning: unable|BUILD (SUCCEEDED|FAILED)" || true

APP="$DERIVED/Build/Products/Release-iphoneos/TraceZero.app"
[ -d "$APP" ] || { echo "build failed: $APP not found (run the xcodebuild command above without the grep to see why)" >&2; exit 1; }

echo "==> Fake-signing"
if [ -d "$APP/Frameworks" ]; then
  for fw in "$APP"/Frameworks/*.framework; do
    [ -d "$fw" ] || continue
    bin="$fw/$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$fw/Info.plist")"
    ldid -S "$bin"
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
