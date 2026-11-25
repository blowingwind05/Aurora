// SPDX-License-Identifier: GPL-3.0-only
#include "AuroraStreamEntry.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using Operation = AuroraStreamOperation;

struct Command {
    const char* name;
    Operation operation;
};

constexpr Command Commands[] = {
    {"pair", Operation::Pair},
    {"list", Operation::List},
    {"stream", Operation::Stream},
    {"wallpaper", Operation::Wallpaper},
    {"power", Operation::Power},
    {"privacy", Operation::Privacy},
    {"bitrate", Operation::Bitrate},
    {"route-probe", Operation::RouteProbe},
    {"client-proof", Operation::ClientProof},
};

Operation parseOperation(int argc, char** argv)
{
    if (argc >= 3 && std::strcmp(argv[1], "--aurora-runtime") == 0) {
        for (const auto& command : Commands) {
            if (std::strcmp(argv[2], command.name) == 0) {
                return command.operation;
            }
        }
    }
    return Operation::Exit;
}

} // namespace

int auroraGameStreamRelayBase()
{
    const char* value = std::getenv("AURORA_GAMESTREAM_RELAY_BASE");
    if (value == nullptr) {
        return 0;
    }
    int port = 0;
    for (; *value != '\0'; ++value) {
        if (*value < '0' || *value > '9' || port > 6553) {
            return -1;
        }
        port = port * 10 + (*value - '0');
    }
    return port > 5 && port <= 65535 - 21 ? port : -1;
}

AuroraStreamInvocation prepareAuroraStreamInvocation(int& argc, char** argv)
{
    if (argc == 2) {
#ifdef AURORA_STREAM_HOST_IDENTITY
        if (std::strcmp(argv[1], "--aurora-host-identity-info") == 0) {
            std::puts("aurora-host-identity/1");
            return {Operation::Exit, 0};
        }
#endif
#ifdef _WIN32
        if (std::strcmp(argv[1], "--aurora-embedded-info") == 0) {
            std::puts("aurora-embedded/1");
            return {Operation::Exit, 0};
        }
#endif
        if (std::strcmp(argv[1], "--aurora-runtime-info") == 0) {
            std::puts("aurora-stream/1");
            return {Operation::Exit, 0};
        }
        if (std::strcmp(argv[1], "--aurora-gamestream-relay-info") == 0) {
            std::puts("aurora-gamestream-relay/1");
            return {Operation::Exit, 0};
        }
    }

    const Operation operation = parseOperation(argc, argv);
    if (operation == Operation::Exit) {
        std::fputs("Aurora Stream is launched by Aurora.\n", stderr);
        return {operation, 2};
    }

    const int relayBase = auroraGameStreamRelayBase();
    const char* relayHost = std::getenv("AURORA_GAMESTREAM_HOST_UUID");
    if (relayBase < 0 || (relayBase > 0 && (relayHost == nullptr || *relayHost == '\0'))) {
        std::fputs("Aurora GameStream relay requires a valid base port and host UUID.\n", stderr);
        return {Operation::Exit, 2};
    }

    // Preserve argv[0] and the argument shape used by Moonlight's command parsers.
    for (int index = 1; index < argc; ++index) {
        argv[index] = argv[index + 1];
    }
    --argc;
    return {operation, 0};
}
