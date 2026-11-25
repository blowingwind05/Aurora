# Native controller wallpaper previews

Link `aurora_native_wallpaper_client` and give `PreviewClient` the Aurora application-data directory and the absolute path to bundled `AuroraStream`. The data directory is the private per-user Aurora profile used by desktop preferences. The platform supplies the JPEG image plugin.

The cache stays under `Aurora/Wallpapers`. The filename is the uppercase SHA-256 of the UTF-8 device ID plus the `.jpg` extension. For a merged tailnet device the hash input is `tailnet:{owner}:{localId}:{device.id}`, even when using its LAN route. Use the ID from the merged discovery catalog and its authenticated network snapshot. The GameStream host UUID selects the Moonlight certificate pin. Use `existingPreviewPath` to look up an existing bounded cache entry without fetching.

Call `setContext(epoch, foreground)` synchronously on the coordinator's Qt thread whenever the Devices page gains or loses foreground eligibility. Advance the epoch when the account, provider, catalog or trust changes. Capture the returned authorization before preparing a route and supply it to `start` along with the device and snapshot used to prepare that route. The caller revalidates the exact endpoint and host identity and supplies a route with a saved Moonlight certificate pin. For an embedded endpoint, retain the relay lease with a same-thread lifetime token through completion. Revocation cancels an accepted fetch and invalidates its authorization. Before launch the cancellation prevents the child from starting, and during a fetch it kills the child and drops any pending result.

A fetch uses one supplied route. To preserve LAN preference, prepare that route first and consider a fresh authorized tailnet attempt after `Unavailable` or `TimedOut`. The client accepts one request at a time. A zero return from `start` means the request was rejected before launch. Only `Updated` calls for reloading the preview. `Unchanged` preserves the cache entry and its modification time.

The bundled Moonlight process receives the uppercase SHA-256 of existing bounded cache bytes or `-` on a miss. The fetch has a five-second deadline and caps captured stdout at the base64 size of 512 KiB plus a line ending. A successful reply is `=` or a base64 JPEG with both dimensions at most 960 pixels. The client checks dimensions before decoding and publishes validated bytes via a same-directory atomic replacement. The cache rejects links and non-file destinations. A failed replacement preserves the prior entry.

The current upstream Moonlight wallpaper parser requires port 48989. An embedded relay commonly leases a different loopback base port, so a real fetch on that route can return `Unavailable`. The fake CLI covers the relay arguments and cancellation.

## Offline tests

```sh
cmake -S desktop/qt-shell/native/wallpaper/client -B build/wallpaper-client -DCMAKE_BUILD_TYPE=Release
cmake --build build/wallpaper-client --config Release
ctest --test-dir build/wallpaper-client -C Release --output-on-failure
```

The fixtures use a fake `AuroraStream` and temporary profiles on Windows and macOS. They cover legacy filenames and revisions, conditional responses, protocol bounds, atomic write failures, foreground and lease revocation, and deadlines.
