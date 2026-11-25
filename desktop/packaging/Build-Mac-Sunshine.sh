#!/usr/bin/env bash
set -euo pipefail

architecture="${1:?Pass arm64 or x64}"
contents="${2:?Pass the outer Aurora.app Contents directory}"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
desktop="$(cd "$script_dir/.." && pwd -P)"
source_tool="$desktop/upstream/Prepare-EngineSource.py"
privacy_provenance="$script_dir/Mac-Privacy-SOURCE.txt"
privacy_preview="${AURORA_MACOS_PRIVACY_PREVIEW:-OFF}"

case "$privacy_preview" in
  ON|OFF) ;;
  *) echo "AURORA_MACOS_PRIVACY_PREVIEW must be ON or OFF." >&2; exit 1 ;;
esac

case "$architecture" in
  arm64) native_arch=arm64 ;;
  x64) native_arch=x86_64 ;;
  *) echo "Unsupported macOS architecture: $architecture" >&2; exit 1 ;;
esac
if [[ "$(uname -s)" != Darwin || "$(uname -m)" != "$native_arch" ]]; then
  echo "Build the Sunshine host using a native $native_arch macOS toolchain." >&2
  exit 1
fi

mkdir -p "$desktop/obj"
work="$(mktemp -d "$desktop/obj/sunshine-macos-$architecture.XXXXXXXX")"
source_dir="$work/source"
build="$work/build"
staged="$work/stage"
python3 "$source_tool" checkout sunshine macos "$source_dir"
tag="$(sed -n 's/^Upstream release: //p' "$source_dir/AURORA-SOURCE.txt")"
revision="$(git -C "$source_dir" rev-parse HEAD)"

entry_sources=("$desktop/runtime/host"/AuroraHost*)
cp "${entry_sources[@]}" "$source_dir/src/"
bitrate_source="$desktop/protocol/bitrate/HostControl.h"
cp "$bitrate_source" "$source_dir/src/aurora_video_bitrate.h"
client_proof_source="$desktop/protocol/identity/ClientProof.h"
cp "$client_proof_source" "$source_dir/src/aurora_client_proof.h"

source_index="$desktop/obj/upstream-sources/sunshine/macos-$architecture"
python3 "$source_tool" index "$source_index" "$source_dir"

BRANCH="$tag" BUILD_VERSION="$tag" COMMIT="$revision" \
  cmake -S "$source_dir" -B "$build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$native_arch" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=14.2 \
    -DBUILD_DOCS=OFF -DBUILD_TESTS=OFF \
    -DSUNSHINE_ENABLE_TRAY=OFF -DAURORA_SUNSHINE_HOST=ON \
    -DAURORA_MACOS_PRIVACY_PREVIEW="$privacy_preview" \
    -DICU_ROOT="$(brew --prefix icu4c@78)" \
    -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" \
    -DOpus_ROOT_DIR="$(brew --prefix opus)" \
    -DSUNSHINE_PUBLISHER_NAME=Aurora
cmake --build "$build" --parallel "$(sysctl -n hw.logicalcpu)"
SHOULD_SIGN=false cmake --install "$build" --prefix "$staged"

original="$staged/Sunshine.app"
helper="$contents/Helpers/AuroraHost.app"
mkdir -p "$contents/Helpers"
test ! -e "$helper"
ditto "$original" "$helper"
plist="$helper/Contents/Info.plist"
upstream_executable="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$plist")"
original_binary="$helper/Contents/MacOS/$upstream_executable"
runtime="$helper/Contents/MacOS/AuroraHost"
mv "$original_binary" "$runtime"
/usr/bin/plutil -replace CFBundleExecutable -string 'AuroraHost' "$plist"
/usr/bin/plutil -replace CFBundleIdentifier -string 'org.aurora-remote.desktop.host' "$plist"
/usr/bin/plutil -replace CFBundleName -string 'AuroraHost' "$plist"
/usr/bin/plutil -replace CFBundleDisplayName -string 'Aurora Host' "$plist"
/usr/bin/plutil -replace LSUIElement -bool YES "$plist"
/usr/libexec/PlistBuddy -c 'Set :LSMinimumSystemVersion 14.2' "$plist" 2>/dev/null ||
  /usr/libexec/PlistBuddy -c 'Add :LSMinimumSystemVersion string 14.2' "$plist"
/usr/bin/plutil -replace NSLocalNetworkUsageDescription -string 'Aurora Host advertises desktop streaming to devices on your local network.' "$plist"
/usr/bin/plutil -replace NSBonjourServices -json '["_nvstream._tcp"]' "$plist"
/usr/bin/plutil -replace NSAudioCaptureUsageDescription -string 'Aurora Host sends the Mac’s audio to your connected device.' "$plist"
/usr/bin/plutil -replace NSScreenCaptureUsageDescription -string 'Aurora Host shares your screen with a paired device.' "$plist"
/usr/bin/plutil -replace NSMicrophoneUsageDescription -string 'Aurora Host can stream audio from your selected input.' "$plist"
/usr/bin/plutil -replace NSAppleEventsUsageDescription -string 'Aurora Host asks macOS to show the shutdown or restart confirmation when requested by a paired device.' "$plist"

resources="$helper/Contents/Resources"
test -f "$resources/assets/apps.json"
test -f "$resources/assets/box.png"
test ! -e "$resources/assets/web"
cp "$desktop/Assets/Aurora.icns" "$resources/sunshine.icns"
licenses="$resources/licenses"
mkdir -p "$licenses"
cp "$source_dir/LICENSE" "$source_dir/NOTICE" "${entry_sources[@]}" "$bitrate_source" "$client_proof_source" "$privacy_provenance" "$licenses/"
cp "$source_dir/cmake/dependencies/Boost-Locale-1.89-CMake.patch" "$licenses/"
cp "$source_dir/AURORA-SOURCE.txt" "$licenses/SOURCE.txt"

/usr/bin/codesign --force --deep --sign - "$helper"
/usr/bin/codesign --verify --deep --strict "$helper"
test "$(/usr/bin/lipo -archs "$runtime")" = "$native_arch"
test "$("$runtime" --aurora-runtime-info)" = 'aurora-host/1'
