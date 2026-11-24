#!/bin/bash
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
desktop="$(cd "$here/../.." && pwd)"
repository="$(cd "$desktop/.." && pwd)"
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
  { echo 'Publish Aurora.app before building the streaming runtime.' >&2; exit 1; }
xcrun --sdk macosx --find clang++ > /dev/null
command -v cmake > /dev/null || { echo 'CMake is required to build the Aurora Qt interface.' >&2; exit 1; }

qt_version="$("$qmake" -query QT_VERSION)"
[[ "$qt_version" == 6.* ]] || { echo 'Use qmake from Qt 6 for macOS (upstream uses Qt 6.6).' >&2; exit 1; }
qt_bin="$("$qmake" -query QT_HOST_BINS)"
[[ -x "$qt_bin/macdeployqt" ]] || { echo 'The selected Qt must provide macdeployqt.' >&2; exit 1; }

# The upstream qmake project expects a source and build path without spaces.
work="$(mktemp -d /tmp/aurora-streamer.XXXXXXXX)"
trap 'rm -rf -- "$work"' EXIT
source="$work/moonlight"
build="$work/build"
revision='e7a2d9f44d7b43fe7f6d2e0f08bf55b7cca48818'

echo '获取 Moonlight 源码和固定版本的依赖…'
git clone --depth 1 --branch v6.0.0 --recurse-submodules https://github.com/moonlight-stream/moonlight-qt.git "$source"
[[ "$(git -C "$source" rev-parse HEAD)" == "$revision" ]] || { echo 'Unexpected Moonlight revision.' >&2; exit 1; }
patches=()
for patch in "$here"/*.patch; do
  [[ "$(basename "$patch")" != *-windows-* ]] || continue
  tr -d '\r' < "$patch" | git -C "$source" apply -
  patches+=("$patch")
done
cp "$here/AuroraStreamEntry.h" "$source/app/"
cp -R "$here/qt-session" "$source/app/"
cp "$here/../../Assets/Aurora.icns" "$source/app/moonlight.icns"

echo '编译极光串流组件…'
mkdir "$build"
(
  cd "$build"
  "$qmake" "$source/moonlight-qt.pro" "QMAKE_APPLE_DEVICE_ARCHS=$native_arch" \
    'DEFINES+=AURORA_STREAM_RUNTIME'
  make -j"$(sysctl -n hw.logicalcpu)" release
)
stage="$build/app/Moonlight.app/Contents"

echo '编译 Aurora Qt 界面…'
qt_prefix="$("$qmake" -query QT_INSTALL_PREFIX)"
min_os="$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$destination/Info.plist")"
cmake -S "$desktop/qt-shell" -B "$work/qt-shell" -DCMAKE_BUILD_TYPE=Release \
  "-DCMAKE_PREFIX_PATH=$qt_prefix" "-DCMAKE_OSX_ARCHITECTURES=$native_arch" \
  "-DCMAKE_OSX_DEPLOYMENT_TARGET=$min_os" "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=$work/qt-shell/bin"
cmake --build "$work/qt-shell" --config Release --parallel "$(sysctl -n hw.logicalcpu)"
/usr/bin/ditto "$work/qt-shell/bin/AuroraQtShell" "$stage/MacOS/AuroraQtShell"

/usr/bin/lipo "$stage/MacOS/Moonlight" -verify_arch "$native_arch"
/usr/bin/lipo "$stage/MacOS/AuroraQtShell" -verify_arch "$native_arch"

# Keep QML's relative asset paths identical to the Windows Qt bundle.
preview="$stage/Resources/QtPreview"
mkdir -p "$preview/desktop/qt-shell/bridge" "$preview/desktop/Assets" "$preview/docs/visual-design/assets"
/usr/bin/ditto "$desktop/qt-shell/qml" "$preview/desktop/qt-shell/qml"
cp "$desktop/qt-shell/bridge/Live.qml" "$preview/desktop/qt-shell/bridge/"
for asset in Aurora.png Aurora-Windows-32.png tailscale-dot.svg panel-top-tight.svg; do
  cp "$desktop/Assets/$asset" "$preview/desktop/Assets/"
done
/usr/bin/ditto "$desktop/Assets/Wallpapers" "$preview/desktop/Assets/Wallpapers"
/usr/bin/ditto "$repository/docs/visual-design/assets/icons" "$preview/docs/visual-design/assets/icons"

"$qt_bin/macdeployqt" "$build/app/Moonlight.app" \
  "-executable=$stage/MacOS/AuroraQtShell" "-qmldir=$source/app" \
  "-qmldir=$preview/desktop/qt-shell" -appstore-compliant
expected_arch="$(uname -m)"
if [[ "$expected_arch" == "$native_arch" ]]; then
  [[ "$("$stage/MacOS/Moonlight" --aurora-runtime-info)" == aurora-stream/1 ]] ||
    { echo 'The private Aurora streaming entry point is missing.' >&2; exit 1; }
fi

# Keep @executable_path/../Frameworks and the Qt plug-in paths from macdeployqt.
# The upstream build bundle is a staging directory. Aurora gets only the runtime files.
/usr/bin/ditto "$stage/MacOS/Moonlight" "$destination/MacOS/AuroraStream"
/usr/bin/ditto "$stage/MacOS/AuroraQtShell" "$destination/MacOS/AuroraQtShell"
for directory in Frameworks PlugIns Resources; do
  if [[ -d "$stage/$directory" ]]; then
    /usr/bin/ditto "$stage/$directory" "$destination/$directory"
  fi
done

mkdir -p "$destination/Resources"
cp "$here/../../Assets/Aurora.png" "$destination/Resources/AuroraStreamBrand.png"

notices="$destination/Resources/ThirdParty/Moonlight"
mkdir -p "$notices"
cp "$source/LICENSE" "$notices/LICENSE"
cp "${patches[@]}" "$here/AuroraStreamEntry.h" "$notices/"
cp -R "$here/qt-session" "$notices/"
printf '%s\n' 'https://github.com/moonlight-stream/moonlight-qt' "$revision" > "$notices/SOURCE.txt"
git -C "$source" submodule status --recursive >> "$notices/SOURCE.txt"
(
  cd "$source"
  find app/SDL_GameControllerDB h264bitstream/h264bitstream moonlight-common-c/moonlight-common-c \
    qmdnsengine/qmdnsengine soundio/libsoundio libs -type f \
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
