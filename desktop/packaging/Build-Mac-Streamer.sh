#!/bin/bash
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd -P)"
desktop="$(cd "$here/.." && pwd -P)"
repository="$(cd "$desktop/.." && pwd -P)"
architecture="${1:?Expected arm64 or x64}"
destination="${2:?Expected the destination Aurora.app Contents directory}"
qmake="${3:-qmake}"

[[ "$(uname -s)" == Darwin ]] || { echo 'The Aurora streaming runtime requires a macOS build host.' >&2; exit 1; }
case "$architecture" in
  arm64) native_arch="arm64" ;;
  x64) native_arch="x86_64" ;;
  *) echo 'Expected arm64 or x64.' >&2; exit 1 ;;
esac
[[ -d "$destination/MacOS" && -f "$destination/Info.plist" ]] ||
  { echo 'Prepare the staging Contents directory with Info.plist and MacOS before building.' >&2; exit 1; }
xcrun --sdk macosx --find clang++ > /dev/null
command -v cmake > /dev/null || { echo 'CMake is required to build the Aurora Qt interface.' >&2; exit 1; }

qt_version="$("$qmake" -query QT_VERSION)"
[[ "$qt_version" == 6.* ]] || { echo 'Use qmake from Qt 6 for macOS (upstream uses Qt 6.6).' >&2; exit 1; }
qt_bin="$("$qmake" -query QT_HOST_BINS)"
[[ -x "$qt_bin/macdeployqt" ]] || { echo 'The selected Qt must provide macdeployqt.' >&2; exit 1; }

# The upstream qmake project expects a source and build path without spaces.
work="$(mktemp -d /tmp/aurora-streamer.XXXXXXXX)"
# CMake's generated MOC includes must use the physical path behind macOS /tmp.
work="$(cd "$work" && pwd -P)"
source="$work/moonlight"
build="$work/build"
source_tool="$desktop/upstream/Prepare-EngineSource.py"
source_ready=false
source_index="$desktop/obj/upstream-sources/moonlight/macos-$architecture"

finish_build() {
  local build_status=$?
  if "$source_ready"; then
    local archived="$source_index/$(basename "$work")"
    if ! mkdir -p "$archived" || ! mv "$source" "$archived/source" ||
       ! python3 "$source_tool" index "$source_index" "$archived/source"; then
      printf '%s\n' "Could not publish the Moonlight source index. Check $archived and $work." >&2
      exit 1
    fi
  fi
  rm -rf -- "$work"
  exit "$build_status"
}
trap finish_build EXIT

echo '获取 Moonlight 源码和固定版本的依赖…'
python3 "$source_tool" checkout moonlight macos "$source"
entry_sources=("$desktop/runtime/stream"/AuroraStream*)
cp "${entry_sources[@]}" "$source/app/"
connection_views="$desktop/runtime/stream/qt-session"
cp -R "$connection_views" "$source/app/"
connection_stage="$desktop/app/qml/stream/ConnectionStage.qml"
cp "$connection_stage" "$source/app/qt-session/"
bitrate_source="$desktop/protocol/bitrate/ClientProtocol.h"
cp "$bitrate_source" "$source/app/backend/aurora-bitrate.h"
client_proof_source="$desktop/protocol/identity/ClientProof.h"
cp "$client_proof_source" "$source/app/backend/aurora-client-proof.h"
cp "$desktop/Assets/Aurora.icns" "$source/app/moonlight.icns"
source_ready=true

echo '编译极光串流组件…'
mkdir "$build"
(
  cd "$build"
  "$qmake" "$source/moonlight-qt.pro" "QMAKE_APPLE_DEVICE_ARCHS=$native_arch" \
    'DEFINES+=AURORA_STREAM_RUNTIME'
  make -j"$(sysctl -n hw.logicalcpu)" release
)
stage="$build/app/Moonlight.app/Contents"

echo '编译 Aurora…'
qt_prefix="$("$qmake" -query QT_INSTALL_PREFIX)"
min_os="$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$destination/Info.plist")"
aurora_build="$work/aurora"
cmake -S "$desktop" -B "$aurora_build" -DCMAKE_BUILD_TYPE=Release \
  "-DCMAKE_PREFIX_PATH=$qt_prefix" "-DCMAKE_OSX_ARCHITECTURES=$native_arch" \
  "-DCMAKE_OSX_DEPLOYMENT_TARGET=$min_os"
cmake --build "$aurora_build" --config Release --target Aurora --parallel "$(sysctl -n hw.logicalcpu)"
/usr/bin/ditto "$aurora_build/bin/Aurora" "$stage/MacOS/Aurora"

/usr/bin/lipo "$stage/MacOS/Moonlight" -verify_arch "$native_arch"
/usr/bin/lipo "$stage/MacOS/Aurora" -verify_arch "$native_arch"

# Keep QML's relative asset paths identical to the Windows Qt bundle.
ui="$stage/Resources/ui"
mkdir -p "$ui/desktop/app" "$ui/desktop/Assets" "$ui/docs/visual-design/assets/brand"
/usr/bin/ditto "$desktop/app/qml" "$ui/desktop/app/qml"
for asset in Aurora.png Aurora-Windows-32.png tailscale-dot.svg panel-top-tight.svg; do
  cp "$desktop/Assets/$asset" "$ui/desktop/Assets/"
done
/usr/bin/ditto "$desktop/Assets/Wallpapers" "$ui/desktop/Assets/Wallpapers"
/usr/bin/ditto "$repository/docs/visual-design/assets/icons" "$ui/docs/visual-design/assets/icons"
cp "$repository/docs/visual-design/assets/brand/Aurora-transparent-512.png" "$ui/docs/visual-design/assets/brand/"

"$qt_bin/macdeployqt" "$build/app/Moonlight.app" \
  "-executable=$stage/MacOS/Aurora" "-qmldir=$source/app/qt-session" \
  "-qmldir=$ui/desktop/app/qml" -appstore-compliant
expected_arch="$(uname -m)"
if [[ "$expected_arch" == "$native_arch" ]]; then
  [[ "$("$stage/MacOS/Moonlight" --aurora-runtime-info)" == aurora-stream/1 ]] ||
    { echo 'The private Aurora streaming entry point is missing.' >&2; exit 1; }
  cmake -S "$repository/tests/qt-native-instance" -B "$work/instance-test" -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_PREFIX_PATH=$qt_prefix" "-DCMAKE_OSX_ARCHITECTURES=$native_arch"
  cmake --build "$work/instance-test" --config Release
  (cd "$work/instance-test" && ctest -C Release --output-on-failure)
fi

# Keep @executable_path/../Frameworks and the Qt plug-in paths from macdeployqt.
# The upstream build bundle is a staging directory. Aurora gets only the runtime files.
/usr/bin/ditto "$stage/MacOS/Moonlight" "$destination/MacOS/AuroraStream"
/usr/bin/ditto "$stage/MacOS/Aurora" "$destination/MacOS/Aurora"
for directory in Frameworks PlugIns Resources; do
  if [[ -d "$stage/$directory" ]]; then
    /usr/bin/ditto "$stage/$directory" "$destination/$directory"
  fi
done
if [[ "$expected_arch" == "$native_arch" ]]; then
  /usr/bin/env -u QT_PLUGIN_PATH -u QT_QPA_PLATFORM_PLUGIN_PATH -u QML2_IMPORT_PATH -u QML_IMPORT_PATH \
    QT_QPA_PLATFORM=cocoa "$destination/MacOS/Aurora" --check-native \
    "$destination/Resources/ui/desktop/app/qml/Main.qml"
fi

mkdir -p "$destination/Resources"
cp "$desktop/Assets/Aurora.png" "$destination/Resources/AuroraStreamBrand.png"

notices="$destination/Resources/ThirdParty/Moonlight"
mkdir -p "$notices"
cp "$source/LICENSE" "$notices/LICENSE"
cp "${entry_sources[@]}" "$bitrate_source" "$client_proof_source" "$notices/"
cp -R "$connection_views" "$notices/"
cp "$connection_stage" "$notices/qt-session/"
cp "$source/AURORA-SOURCE.txt" "$notices/SOURCE.txt"
(
  cd "$source"
  find app/SDL_GameControllerDB h264bitstream/h264bitstream moonlight-common-c/moonlight-common-c \
    soundio/libsoundio libs -type f \
    \( -iname 'license*' -o -iname 'copying*' -o -iname 'notice*' \) -print0 |
    while IFS= read -r -d '' item; do
      mkdir -p "$notices/$(dirname "$item")"
      cp "$item" "$notices/$item"
    done
)
for license in "$qt_bin/../LICENSES/"* "$qt_bin/../LICENSE."*; do
  if [[ -f "$license" ]]; then
    mkdir -p "$notices/Qt"
    cp "$license" "$notices/Qt/"
  fi
done
