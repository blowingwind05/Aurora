# Native account avatar

The shared Tailscale status decoder exposes avatarUrl from the exact local owner's User entry when the status has a verified connected account. The URL must use HTTPS on the default port and have empty user information and no fragment. GitHub permits /u/ followed by ASCII digits and the optional v=4 query. Only lh3 and lh4 of googleusercontent.com are accepted for Google, with a non-root path and no query. Canonical URLs are limited to 2048 bytes.

Add this directory with add_subdirectory and link AuroraTailscaleAvatar. Own an AvatarLoader on the coordinator's event-loop thread. Call setContext(generation, status, visible) on status, generation and visibility changes. Read dataUri() on dataUriChanged. Publish only that local PNG data URI to the view.

A hidden, disconnected or ineligible context clears the displayed image and aborts the active reply. Changing generation, account or URL cancels the old request. The loader also vets URLs passed directly by a caller. A single cache entry is scoped to the account identity and URL. It holds a successful result for ten minutes and a failed result for thirty seconds.

The loader uses a private Qt HTTPS manager with direct connections, manual redirects, disabled cookie load and save, and disabled credential reuse. The reply must be HTTP 200 with a PNG, JPEG or WebP media type and no content encoding. The four-second deadline covers the complete response and image processing. The loader checks the parsed response headers against 16 KiB, bounds the body at 256 KiB, and limits image dimensions to 1024 by 1024 before decoding. Qt parses the headers before the loader receives them. Decoding uses Qt's installed image plugins. The result is centered, square-cropped and antialiased into a 96 by 96 circular PNG with transparent corners.

## Offline tests

~~~sh
cmake -S desktop/app/src/tailscale/avatar -B build/tailscale-avatar -DCMAKE_BUILD_TYPE=Release
cmake --build build/tailscale-avatar --config Release
ctest --test-dir build/tailscale-avatar -C Release --output-on-failure
~~~

AvatarLoaderTests controls the reply entirely in memory. The existing native Tailscale status test target also checks that the URL is attached only to its matching owner.
