#pragma once

#include <cstdio>
#include <cstring>

inline int prepareAuroraStreamInvocation(int& argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--aurora-runtime-info") == 0) {
        std::puts("aurora-stream/1");
        return 0;
    }
    if (argc < 3 || std::strcmp(argv[1], "--aurora-runtime") != 0 ||
        (std::strcmp(argv[2], "pair") != 0 && std::strcmp(argv[2], "list") != 0 &&
         std::strcmp(argv[2], "stream") != 0)) {
        std::fputs("Aurora Stream is launched by Aurora.\n", stderr);
        return 2;
    }

    // Let Moonlight parse its original command line after checking the private entry point.
    for (int index = 1; index < argc; ++index) {
        argv[index] = argv[index + 1];
    }
    --argc;
    return -1;
}
