# Native account avatar

The desktop supplies an opaque account key and an allowlisted HTTPS URL for the signed-in account. The [Tailscale adaptor](../../adaptors/tailscale/README.md) verifies the account and canonicalizes the URL.

Add this directory with add_subdirectory and link AuroraAvatarLoader. Own an AvatarLoader on the coordinator's event-loop thread. Call setContext(generation, source, visible) when the account, URL, generation or visibility changes. Read dataUri() on dataUriChanged and publish the local PNG data URI to the view.

A hidden or missing source clears the displayed image and aborts the active reply. Changing generation, account or URL cancels the old request. The loader checks for HTTPS on the default port with an empty user information field and no fragment. A single cache entry is scoped to the account key and URL. It holds a successful result for ten minutes and a failed result for thirty seconds.

The loader uses a private Qt HTTPS manager with direct connections, manual redirects, disabled cookie load and save, and disabled credential reuse. The reply must be HTTP 200 with a PNG, JPEG or WebP media type and no content encoding. The four-second deadline covers the complete response and image processing. The loader checks the parsed response headers against 16 KiB, bounds the body at 256 KiB, and limits image dimensions to 1024 by 1024 before decoding. Qt parses the headers before the loader receives them. Decoding uses Qt's installed image plugins. The result is centered, square-cropped and antialiased into a 96 by 96 circular PNG with transparent corners.

## Offline tests

~~~sh
cmake -S desktop/app/src/ui/avatar -B build/avatar -DCMAKE_BUILD_TYPE=Release
cmake --build build/avatar --config Release
ctest --test-dir build/avatar -C Release --output-on-failure
~~~

AvatarLoaderTests controls the reply entirely in memory. The existing native Tailscale status test target also checks that the URL is attached only to its matching owner.
