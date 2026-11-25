// SPDX-License-Identifier: GPL-3.0-only
#pragma once

enum class AuroraStreamOperation {
    Exit,
    Pair,
    List,
    Stream,
    Wallpaper,
    Power,
    Privacy,
    Bitrate,
    RouteProbe,
    ClientProof,
};

struct AuroraStreamInvocation {
    AuroraStreamOperation operation;
    int exitCode;
};

AuroraStreamInvocation prepareAuroraStreamInvocation(int& argc, char** argv);
int auroraGameStreamRelayBase();

// Implemented by the Moonlight adapter using the existing per-command parsers.
int runAuroraStream(AuroraStreamOperation operation, int argc, char** argv);
