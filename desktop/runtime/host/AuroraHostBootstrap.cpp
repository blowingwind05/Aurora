// SPDX-License-Identifier: GPL-3.0-only
#include "AuroraHostEntry.h"

#include <openssl/crypto.h>

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

AuroraHostBootstrap::~AuroraHostBootstrap()
{
    clearPassword();
#ifdef _WIN32
    if (stopEvent_) {
        CloseHandle(stopEvent_);
    }
#endif
}

bool AuroraHostBootstrap::initialize()
{
    // Populate the engine's profile cache before consuming the launch environment.
    if (!auroraHostProfileReady()) {
        return false;
    }

#ifdef _WIN32
    wchar_t stopName[128];
    const auto length = GetEnvironmentVariableW(L"AURORA_SUNSHINE_STOP_EVENT", stopName, _countof(stopName));
    if (length == 0 || length >= _countof(stopName)) {
        return false;
    }
    stopEvent_ = OpenEventW(SYNCHRONIZE, FALSE, stopName);
    if (!stopEvent_) {
        return false;
    }
    SetEnvironmentVariableW(L"AURORA_SUNSHINE_PROFILE", nullptr);
    SetEnvironmentVariableW(L"AURORA_SUNSHINE_STOP_EVENT", nullptr);

    const auto input = GetStdHandle(STD_INPUT_HANDLE);
    if (GetFileType(input) != FILE_TYPE_PIPE) {
        std::fputs("Aurora requires an administration password pipe.\n", stderr);
        return false;
    }
#else
    struct stat inputInfo {};
    if (fstat(STDIN_FILENO, &inputInfo) != 0 || !S_ISFIFO(inputInfo.st_mode)) {
        return false;
    }
    unsetenv("AURORA_SUNSHINE_PROFILE");
    // The pipe belongs to Aurora and must not be inherited by launched applications.
    if (fcntl(STDIN_FILENO, F_SETFD, FD_CLOEXEC) == -1) {
        return false;
    }
#endif

    password_.resize(64);
    std::size_t offset = 0;
    while (offset < password_.size()) {
        auto* next = password_.data() + offset;
        const auto remaining = password_.size() - offset;
#ifdef _WIN32
        DWORD received = 0;
        if (!ReadFile(input, next, static_cast<DWORD>(remaining), &received, nullptr)) {
            break;
        }
#else
        const auto received = read(STDIN_FILENO, next, remaining);
        if (received < 0 && errno == EINTR) {
            continue;
        }
#endif
        if (received <= 0) {
            break;
        }
        offset += static_cast<std::size_t>(received);
    }
    if (offset != password_.size() || password_.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        std::fputs("Aurora requires a 64-digit hexadecimal administration password.\n", stderr);
        clearPassword();
        return false;
    }
    return true;
}

bool AuroraHostBootstrap::waitForStop() const
{
#ifdef _WIN32
    return WaitForSingleObject(stopEvent_, 200) != WAIT_TIMEOUT;
#else
    pollfd descriptor {STDIN_FILENO, 0, 0};
    const auto ready = poll(&descriptor, 1, 200);
    return (ready > 0 && (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL))) ||
           (ready < 0 && errno != EINTR);
#endif
}

void AuroraHostBootstrap::clearPassword()
{
    if (!password_.empty()) {
        OPENSSL_cleanse(password_.data(), password_.size());
        password_.clear();
    }
}
