// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#ifdef _WIN32
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace aurora::host {

struct ClosePrivacyHandle {
    void operator()(HANDLE handle) const noexcept { CloseHandle(handle); }
};
using PrivacyImageMapping = std::unique_ptr<void, ClosePrivacyHandle>;

/** @brief A validated, top-down, opaque 32-bit snapshot of the Windows privacy screensaver. */
class PrivacyScreensaverImage {
public:
    /**
     * @brief Read the fixed local PNG from the active host profile.
     * @param profile The trusted Aurora host profile directory.
     * @return An opaque snapshot, or no image for missing, invalid, or unreadable artwork.
     */
    static std::optional<PrivacyScreensaverImage> load(const std::filesystem::path &profile) noexcept
    {
        try {
            if (!profile.is_absolute() || profile.root_name().native().size() != 2 ||
                profile.root_name().native()[1] != L':' ||
                GetDriveTypeW(profile.root_path().c_str()) == DRIVE_REMOTE) {
                return {};
            }

            const auto path = profile / L"privacy-screensaver.png";
            const HANDLE opened = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (opened == INVALID_HANDLE_VALUE) return {};
            const std::unique_ptr<void, ClosePrivacyHandle> file(opened);

            BY_HANDLE_FILE_INFORMATION info{};
            constexpr DWORD encodedLimit = 64 * 1024 * 1024;
            if (GetFileType(file.get()) != FILE_TYPE_DISK || !GetFileInformationByHandle(file.get(), &info) ||
                (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                info.nFileSizeHigh != 0 || info.nFileSizeLow == 0 || info.nFileSizeLow > encodedLimit) {
                return {};
            }

            std::vector<BYTE> png(info.nFileSizeLow);
            DWORD read = 0;
            if (!ReadFile(file.get(), png.data(), info.nFileSizeLow, &read, nullptr) || read != info.nFileSizeLow) {
                return {};
            }
            return decode(png);
        } catch (const std::exception &) {
            return {};
        }
    }

    /**
     * @brief Fill the current curtain paint area with a centered, aspect-preserving crop.
     * @param dc The curtain's paint device context.
     * @param bounds The full client rectangle in device pixels.
     * @return Whether GDI painted the image.
     */
    bool paint(HDC dc, const RECT &bounds) const noexcept
    {
        const auto width = bounds.right - bounds.left;
        const auto height = bounds.bottom - bounds.top;
        if (width <= 0 || height <= 0) return false;

        const auto scaledWidth = static_cast<std::int64_t>(height) * width_;
        const auto scaledHeight = static_cast<std::int64_t>(width) * height_;
        const int cropWidth = scaledWidth > scaledHeight
            ? (std::max)(1, static_cast<int>((scaledHeight + height / 2) / height)) : width_;
        const int cropHeight = scaledHeight > scaledWidth
            ? (std::max)(1, static_cast<int>((scaledWidth + width / 2) / width)) : height_;

        BITMAPINFO dib{};
        auto &header = dib.bmiHeader;
        header.biSize = sizeof(header);
        header.biWidth = width_;
        header.biHeight = -height_;
        header.biPlanes = 1;
        header.biBitCount = 32;
        header.biCompression = BI_RGB;

        const int saved = SaveDC(dc);
        if (!saved) return false;
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        const int lines = StretchDIBits(dc, bounds.left, bounds.top, width, height,
            (width_ - cropWidth) / 2, (height_ - cropHeight) / 2, cropWidth, cropHeight,
            pixels_.data(), &dib, DIB_RGB_COLORS, SRCCOPY);
        RestoreDC(dc, saved);
        return lines != 0 && lines != static_cast<int>(GDI_ERROR);
    }

    /**
     * @brief Publish validated pixels alongside one existing UIAccess overlay lease.
     * @param name The newly generated, session-local lease name with the .image suffix.
     * @return The mapping handle, which the host keeps open for the lease lifetime.
     */
    PrivacyImageMapping share(const std::wstring &name) const noexcept
    {
        // APIM, version, width, height, then tightly packed top-down opaque BGRX.
        const std::array<std::uint32_t, 4> header{0x4D495041, 1,
            static_cast<std::uint32_t>(width_), static_cast<std::uint32_t>(height_)};
        const auto bytes = sizeof header + pixels_.size();
        SetLastError(ERROR_SUCCESS);
        PrivacyImageMapping mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, static_cast<DWORD>(bytes), name.c_str()));
        if (!mapping || GetLastError() == ERROR_ALREADY_EXISTS) return {};

        const auto view = static_cast<BYTE *>(MapViewOfFile(mapping.get(), FILE_MAP_WRITE, 0, 0, bytes));
        if (!view) return {};
        std::memcpy(view, header.data(), sizeof header);
        std::memcpy(view + sizeof header, pixels_.data(), pixels_.size());
        UnmapViewOfFile(view);
        return mapping;
    }

private:
    struct ComApartment {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    };

    PrivacyScreensaverImage(int width, int height, std::vector<BYTE> pixels)
        : width_(width), height_(height), pixels_(std::move(pixels)) {}

    /** @brief Decode only PNG using the Windows codec and verify all alpha bytes. */
    static std::optional<PrivacyScreensaverImage> decode(std::vector<BYTE> &png)
    {
        const ComApartment apartment;
        if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE) return {};

        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        UINT count = 0;
        UINT width = 0;
        UINT height = 0;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_IWICImagingFactory, reinterpret_cast<void **>(factory.GetAddressOf()))) ||
            FAILED(factory->CreateStream(stream.GetAddressOf())) ||
            FAILED(stream->InitializeFromMemory(png.data(), static_cast<DWORD>(png.size()))) ||
            FAILED(CoCreateInstance(CLSID_WICPngDecoder, nullptr, CLSCTX_INPROC_SERVER,
                IID_IWICBitmapDecoder, reinterpret_cast<void **>(decoder.GetAddressOf()))) ||
            FAILED(decoder->Initialize(stream.Get(), WICDecodeMetadataCacheOnDemand)) ||
            FAILED(decoder->GetFrameCount(&count)) || count != 1 ||
            FAILED(decoder->GetFrame(0, frame.GetAddressOf())) ||
            FAILED(frame->GetSize(&width, &height)) ||
            !width || !height || width > 8192 || height > 8192 || width * height > 16 * 1024 * 1024) {
            return {};
        }

        const UINT stride = width * 4;
        std::vector<BYTE> pixels(stride * height);
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) ||
            FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
            FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data()))) {
            return {};
        }
        for (std::size_t alpha = 3; alpha < pixels.size(); alpha += 4) {
            if (pixels[alpha] != 255) return {};
        }
        return PrivacyScreensaverImage(static_cast<int>(width), static_cast<int>(height), std::move(pixels));
    }

    int width_;
    int height_;
    std::vector<BYTE> pixels_;
};

} // namespace aurora::host
#endif
