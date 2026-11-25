// SPDX-License-Identifier: GPL-3.0-only
#include "AuroraHostEntry.h"

#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--aurora-runtime-info") == 0) {
        std::puts("aurora-host/1");
        return 0;
    }

    AuroraHostBootstrap bootstrap;
    if (!bootstrap.initialize()) {
        return 1;
    }
    return runAuroraHost(bootstrap, argc, argv);
}
