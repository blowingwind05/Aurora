// SPDX-License-Identifier: GPL-3.0-only
#include "AuroraHostPrivacyImage.h"
#include "AuroraHostPrivacyOverlay.h"

#include <array>
#include <cstdio>
#include <filesystem>

using aurora::host::PrivacyScreensaverImage;

namespace {

bool check(bool result, const char *what)
{
    if (!result) std::fprintf(stderr, "FAIL %s\n", what);
    return result;
}

bool check_crop(const PrivacyScreensaverImage &image, int width, int height, int left, int top)
{
    const HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    BITMAPINFO dib{};
    dib.bmiHeader.biSize = sizeof(dib.bmiHeader);
    dib.bmiHeader.biWidth = width;
    dib.bmiHeader.biHeight = -height;
    dib.bmiHeader.biPlanes = 1;
    dib.bmiHeader.biBitCount = 32;
    dib.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    const auto bitmap = CreateDIBSection(dc, &dib, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool passed = false;
    if (bitmap) {
        const auto original = SelectObject(dc, bitmap);
        const RECT bounds{0, 0, width, height};
        passed = image.paint(dc, bounds);
        GdiFlush();
        const auto pixels = static_cast<const BYTE *>(bits);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const auto pixel = pixels + (y * width + x) * 4;
                passed = passed && pixel[0] == 77 && pixel[1] == (y + top) * 40 && pixel[2] == (x + left) * 30;
            }
        }
        SelectObject(dc, original);
        DeleteObject(bitmap);
    }
    DeleteDC(dc);
    return passed;
}

bool check_mapping(const PrivacyScreensaverImage &image)
{
    GUID id{};
    wchar_t text[39]{};
    if (FAILED(CoCreateGuid(&id)) || StringFromGUID2(id, text, 39) != 39) return false;
    const auto name = std::wstring(L"Local\\Aurora.Privacy.Fixture.") + text + L".image";
    const auto mapping = image.share(name);
    if (!mapping || image.share(name)) return false;
    const aurora::host::PrivacyImageMapping receiver(OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str()));
    if (!receiver) return false;
    const auto memory = static_cast<const BYTE *>(MapViewOfFile(receiver.get(), FILE_MAP_READ, 0, 0, 16 + 8 * 6 * 4));
    if (!memory) return false;
    std::array<std::uint32_t, 4> header{};
    std::memcpy(header.data(), memory, sizeof header);
    const auto *top = memory + sizeof header;
    const auto *bottom = top + (8 * 6 - 1) * 4;
    const bool passed = header == std::array<std::uint32_t, 4>{0x4D495041, 1, 8, 6} &&
        top[0] == 77 && top[1] == 0 && top[2] == 0 && top[3] == 255 &&
        bottom[0] == 77 && bottom[1] == 200 && bottom[2] == 210 && bottom[3] == 255;
    UnmapViewOfFile(memory);
    return passed;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    const auto profiles = std::filesystem::absolute(argv[1]);
    auto image = PrivacyScreensaverImage::load(profiles / "opaque");
    if (!check(image.has_value(), "opaque PNG decoded")) return 1;
    bool passed = check(check_crop(*image, 8, 6, 0, 0), "top-down source without crop");
    passed &= check(check_crop(*image, 6, 6, 1, 0), "center horizontal crop fills the client area");
    passed &= check(check_crop(*image, 8, 4, 0, 1), "center vertical crop fills the client area");
    passed &= check(check_mapping(*image), "opaque lease snapshot and named-object collision");
    passed &= check(PrivacyScreensaverImage::load(profiles / "opaque-rgba").has_value(), "opaque RGBA PNG");
    for (const auto *invalid : {"absent", "fixed-name", "transparent", "transparent-key", "corrupt", "oversized", "not-png"}) {
        passed &= check(!PrivacyScreensaverImage::load(profiles / invalid), invalid);
    }
    passed &= check(!PrivacyScreensaverImage::load({}), "empty profile");
    if (passed) std::puts("PASS WIC decode, fallback inputs, opaque GDI fill and lease snapshot");
    return passed ? 0 : 1;
}
