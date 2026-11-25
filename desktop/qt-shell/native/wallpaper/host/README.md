# Bundled host wallpaper preview

Link `Aurora::HostWallpaper` from `add_subdirectory(native/wallpaper/host)`.
The module uses C++17 and Qt Gui with JPEG image format support.

## Integration

Construct a single `Aurora::Wallpaper::HostWallpaperUpdater` on the GUI
thread with the existing `Aurora::Sunshine::HostLayout::profileDirectory()`.
The legacy path is `Aurora/Sunshine/Bundled/config/aurora-wallpaper.jpg`
under the per-user data directory. `wallpaperPath()` exposes the full
destination.

Call `refreshIfAllowed(foreground && managedHostRunning)` from the desktop's
foreground refresh. The caller owns those two conditions. A true return
schedules one `finished(HostWallpaperResult)` signal. The updater acquires
the source path on the GUI thread, limits requests to one attempt every
45 seconds, and runs conversion and atomic replacement on a private
worker. Keep the updater across refreshes so it can skip a source with
the same resolved path, modification time, and length while its cache
file exists. Destroying the updater waits for its worker to finish.

For a known source path, `HostWallpaperCache::refreshFromFile(path)`
provides the synchronous file-only operation on a serialized worker.
The updater also accepts a source-path callback, which the fixtures use
to provide synthetic files.

## Source and output

Windows picks the wallpaper for the monitor at desktop origin through
`IDesktopWallpaper`, and falls back to the read-only
`SPI_GETDESKWALLPAPER` query. macOS uses the first screen's local file
URL from `NSWorkspace.desktopImageURLForScreen:`. Qt decodes supported
formats, and ImageIO provides the still-image fallback on macOS,
including HEIC.

The legacy caps are 80 MiB of source bytes, 100 million source pixels,
960 pixels on either preview edge, and 512 KiB of JPEG output at quality
78. Conversion keeps aspect ratio and avoids upscaling. A missing,
invalid, or oversized source leaves the existing preview in place.
Replacement uses an atomic same-directory save.

## Offline verification

```sh
cmake -S desktop/qt-shell/native/wallpaper/host -B build/host-wallpaper -DCMAKE_PREFIX_PATH=/path/to/Qt
cmake --build build/host-wallpaper --config Release
ctest --test-dir build/host-wallpaper -C Release --output-on-failure
```

On Windows, put the Qt SDK's `bin` directory on `PATH`. The fixture builds
synthetic images in temporary directories and supplies a fake source
callback. It exercises bounds, conversion, deduplication, atomic cache
replacement, and the caller gate without querying the desktop.
