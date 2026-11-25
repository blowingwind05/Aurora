// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>

class AuroraHostBootstrap {
public:
    AuroraHostBootstrap() = default;
    ~AuroraHostBootstrap();
    AuroraHostBootstrap(const AuroraHostBootstrap&) = delete;
    AuroraHostBootstrap& operator=(const AuroraHostBootstrap&) = delete;

    bool initialize();
    bool waitForStop() const;
    const std::string& password() const { return password_; }
    void clearPassword();

private:
    std::string password_;
#ifdef _WIN32
    void* stopEvent_ = nullptr;
#endif
};

// The Sunshine adapter keeps the existing profile and service initialization.
bool auroraHostProfileReady();
int runAuroraHost(AuroraHostBootstrap& bootstrap, int argc, char** argv);
