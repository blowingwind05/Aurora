#!/usr/bin/env bash
set -euo pipefail

architecture="${1:?Pass arm64 or x64}"
contents="${2:?Pass the outer Aurora.app Contents directory}"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
desktop="$(cd "$script_dir/../.." && pwd)"
tag='v2026.914.233613'
revision='63d35f702ee9e362e43263742981836ec0710384'
patch="$script_dir/0002-aurora-managed-macos-host.patch"
wallpaper_patch="$script_dir/0003-aurora-wallpaper.patch"
private_admin_patch="$script_dir/0005-aurora-private-admin.patch"
pair_source_port_patch="$script_dir/0006-aurora-pair-source-port.patch"
encoder_probe_patch="$script_dir/0008-aurora-background-encoder-probe.patch"
power_patch="$script_dir/0009-aurora-device-power.patch"
privacy_patch="$script_dir/0010-aurora-macos-privacy-screensaver.patch"
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
git clone --depth 1 --branch "$tag" --recurse-submodules --shallow-submodules \
  https://github.com/SunshineStream/Sunshine.git "$source_dir"
[[ "$(git -C "$source_dir" rev-parse HEAD)" == "$revision" ]]
for host_patch in "$patch" "$wallpaper_patch" "$private_admin_patch" "$pair_source_port_patch" "$encoder_probe_patch" "$power_patch" "$privacy_patch"; do
  git -C "$source_dir" apply --check "$host_patch"
  git -C "$source_dir" apply "$host_patch"
done

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
test -f "$resources/assets/web/index.html"
cp "$desktop/Assets/Aurora.icns" "$resources/sunshine.icns"
licenses="$resources/licenses"
mkdir -p "$licenses"
cp "$source_dir/LICENSE" "$source_dir/NOTICE" "$patch" "$wallpaper_patch" "$private_admin_patch" "$pair_source_port_patch" "$encoder_probe_patch" "$power_patch" "$privacy_patch" "$privacy_provenance" "$licenses/"
{
  printf '%s\n' 'https://github.com/SunshineStream/Sunshine' "$tag" "$revision"
  git -C "$source_dir" submodule status --recursive
} > "$licenses/SOURCE.txt"

/usr/bin/codesign --force --deep --sign - "$helper"
/usr/bin/codesign --verify --deep --strict "$helper"
test "$(/usr/bin/lipo -archs "$runtime")" = "$native_arch"
test "$("$runtime" --aurora-runtime-info)" = 'aurora-host/1'
