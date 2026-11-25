// SPDX-License-Identifier: GPL-3.0-only
#include "AuroraStreamEntry.h"

int main(int argc, char** argv)
{
    const auto invocation = prepareAuroraStreamInvocation(argc, argv);
    if (invocation.operation == AuroraStreamOperation::Exit) {
        return invocation.exitCode;
    }
    return runAuroraStream(invocation.operation, argc, argv);
}
