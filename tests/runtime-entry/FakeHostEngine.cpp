// SPDX-License-Identifier: GPL-3.0-only
#include "AuroraHostEntry.h"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

bool auroraHostProfileReady()
{
#ifdef _WIN32
    return GetEnvironmentVariableW(L"AURORA_SUNSHINE_PROFILE", nullptr, 0) > 0;
#else
    return std::getenv("AURORA_SUNSHINE_PROFILE") != nullptr;
#endif
}

int runAuroraHost(AuroraHostBootstrap& bootstrap, int, char**)
{
    if (auroraHostProfileReady() || bootstrap.password() != std::string(64, 'a')) {
        return 3;
    }
#ifdef _WIN32
    if (GetEnvironmentVariableW(L"AURORA_SUNSHINE_STOP_EVENT", nullptr, 0) != 0) {
        return 4;
    }
#endif

    bootstrap.clearPassword();
    if (!bootstrap.password().empty()) {
        return 5;
    }
    while (!bootstrap.waitForStop()) {
    }
    std::puts("host stopped");
    return 0;
}
