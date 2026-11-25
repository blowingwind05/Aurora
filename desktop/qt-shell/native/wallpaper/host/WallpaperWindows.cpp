// SPDX-License-Identifier: GPL-3.0-only
#include "HostWallpaper.h"

#include <windows.h>
#include <shobjidl.h>

#include <memory>
#include <string>

namespace Aurora::Wallpaper {
namespace {

struct Apartment
{
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~Apartment()
    {
        if (SUCCEEDED(result))
            CoUninitialize();
    }
};

struct ReleaseWallpaper
{
    void operator()(IDesktopWallpaper *wallpaper) const { wallpaper->Release(); }
};

struct FreeTaskMemory
{
    void operator()(wchar_t *text) const { CoTaskMemFree(text); }
};

QString primaryMonitorImage()
{
    const Apartment apartment;
    if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE)
        return {};

    IDesktopWallpaper *instance = nullptr;
    const auto activated = CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER,
                                            IID_PPV_ARGS(&instance));
    const std::unique_ptr<IDesktopWallpaper, ReleaseWallpaper> wallpaper(instance);
    UINT count = 0;
    if (activated != S_OK || !wallpaper || wallpaper->GetMonitorDevicePathCount(&count) != S_OK)
        return {};

    for (UINT index = 0; index < count; ++index) {
        LPWSTR device = nullptr;
        const auto status = wallpaper->GetMonitorDevicePathAt(index, &device);
        const std::unique_ptr<wchar_t, FreeTaskMemory> monitor(device);
        RECT rect{};
        if (status != S_OK || !monitor || wallpaper->GetMonitorRECT(monitor.get(), &rect) != S_OK
            || rect.left != 0 || rect.top != 0) {
            continue;
        }

        LPWSTR image = nullptr;
        const auto selected = wallpaper->GetWallpaper(monitor.get(), &image);
        const std::unique_ptr<wchar_t, FreeTaskMemory> path(image);
        if (selected == S_OK && path && *path)
            return QString::fromWCharArray(path.get());
    }
    return {};
}

} // namespace

QString currentDesktopWallpaperFile()
{
    const auto primary = primaryMonitorImage();
    if (!primary.isEmpty())
        return primary;

    std::wstring legacy(32768, L'\0');
    if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, static_cast<UINT>(legacy.size()), legacy.data(), 0))
        return {};
    return QString::fromWCharArray(legacy.c_str());
}

} // namespace Aurora::Wallpaper
