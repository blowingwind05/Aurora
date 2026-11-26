// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#ifdef _WIN32
#include "AuroraHostPrivacyImage.h"

#include <windows.h>
#include <shlobj.h>

#include <filesystem>
#include <memory>
#include <string>

namespace aurora::host {

inline bool privacy_uiaccess_requested(const std::filesystem::path &profile)
{
    std::error_code error;
    return !profile.empty() && std::filesystem::is_regular_file(profile / L"privacy-uiaccess-test", error);
}

class PrivacyTestOverlay {
    struct CloseHandleOnExit {
        void operator()(HANDLE handle) const { CloseHandle(handle); }
    };
    using Handle = std::unique_ptr<void, CloseHandleOnExit>;

public:
    ~PrivacyTestOverlay()
    {
        if (stop_) {
            SetEvent(stop_.get());
        }
        if (process_ && WaitForSingleObject(process_.get(), 1500) == WAIT_TIMEOUT) {
            TerminateProcess(process_.get(), 1);
        }
    }

    /**
     * @brief Start the installed UIAccess test overlay and optionally share the host's opaque artwork.
     * @param profile The active Aurora host profile.
     * @return An overlay that passed the existing token and readiness checks.
     */
    static std::unique_ptr<PrivacyTestOverlay> start(const std::filesystem::path &profile)
    {
        PWSTR programFiles = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &programFiles)) || !programFiles) {
            return {};
        }
        const auto freeFolder = std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)>(programFiles, CoTaskMemFree);
        const auto executable = std::filesystem::path(programFiles) / L"Aurora Developer" / L"AuroraPrivacyOverlay.exe";
        std::error_code error;
        if (!std::filesystem::is_regular_file(executable, error)) {
            return {};
        }

        GUID id{};
        wchar_t guidText[39]{};
        if (FAILED(CoCreateGuid(&id)) || StringFromGUID2(id, guidText, 39) != 39) {
            return {};
        }
        const std::wstring prefix = L"Local\\Aurora.Privacy." + std::wstring(guidText + 1, 36);
        auto overlay = std::make_unique<PrivacyTestOverlay>();
        overlay->ready_ = makeEvent(prefix + L".ready");
        overlay->stop_ = makeEvent(prefix + L".stop");
        overlay->lost_ = makeEvent(prefix + L".lost");
        if (!overlay->ready_ || !overlay->stop_ || !overlay->lost_) {
            return {};
        }

        if (const auto image = PrivacyScreensaverImage::load(profile)) {
            overlay->image_ = image->share(prefix + L".image");
        }

        const auto installDirectory = executable.parent_path();
        const auto args = L"--aurora-session " + std::to_wstring(GetCurrentProcessId()) + L" " + prefix;
        SHELLEXECUTEINFOW launch{};
        launch.cbSize = sizeof launch;
        launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        launch.lpVerb = L"open";
        launch.lpFile = executable.c_str();
        launch.lpParameters = args.c_str();
        launch.lpDirectory = installDirectory.c_str();
        launch.nShow = SW_SHOWNOACTIVATE;
        const auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) {
            return {};
        }
        const BOOL started = ShellExecuteExW(&launch);
        if (SUCCEEDED(apartment)) {
            CoUninitialize();
        }
        if (!started || !launch.hProcess) {
            return {};
        }
        overlay->process_.reset(launch.hProcess);
        const HANDLE waiting[]{overlay->process_.get(), overlay->lost_.get(), overlay->ready_.get()};
        if (!hasTestToken(overlay->process_.get())
            || WaitForMultipleObjects(3, waiting, FALSE, 3000) != WAIT_OBJECT_0 + 2
            || !overlay->active()) {
            return {};
        }
        return overlay;
    }

    bool active() const
    {
        return process_ && WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT
            && WaitForSingleObject(lost_.get(), 0) == WAIT_TIMEOUT;
    }

private:
    static Handle makeEvent(const std::wstring &name)
    {
        SetLastError(ERROR_SUCCESS);
        Handle event(CreateEventW(nullptr, TRUE, FALSE, name.c_str()));
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            return {};
        }
        return event;
    }

    static bool hasTestToken(HANDLE process)
    {
        HANDLE rawToken = nullptr;
        if (!OpenProcessToken(process, TOKEN_QUERY, &rawToken)) {
            return false;
        }
        const Handle token(rawToken);
        DWORD uiAccess = 0;
        DWORD size = 0;
        TOKEN_ELEVATION elevation{};
        return GetTokenInformation(token.get(), TokenUIAccess, &uiAccess, sizeof uiAccess, &size)
            && uiAccess != 0
            && GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof elevation, &size)
            && !elevation.TokenIsElevated;
    }

    PrivacyImageMapping image_;
    Handle ready_;
    Handle stop_;
    Handle lost_;
    Handle process_;
};

}  // namespace aurora::host
#endif
