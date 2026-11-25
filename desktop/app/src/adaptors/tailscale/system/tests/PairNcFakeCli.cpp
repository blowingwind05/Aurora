// SPDX-License-Identifier: GPL-3.0-only
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

void output(FILE *stream, const std::string &text)
{
    std::fwrite(text.data(), 1, text.size(), stream);
    std::fflush(stream);
}

bool hasEnvironment(const wchar_t *name)
{
    wchar_t value[2]{};
    return GetEnvironmentVariableW(name, value, 2) != 0;
}

std::string node(const std::string &id, const std::string &owner, const std::string &addresses,
                 const std::string &extra = "")
{
    return "{\"ID\":\"" + id + "\",\"UserID\":" + owner + ",\"TailscaleIPs\":[" + addresses + "]" + extra + '}';
}

std::string status(const std::wstring &mode)
{
    std::string owner = "9223372036854775807";
    if (mode == L"wrong-owner")
        owner = "9223372036854775806";
    const std::string localId = mode == L"wrong-node" ? "other-local" : "system-local";
    const std::string tailnetId = mode == L"wrong-tailnet" ? "other-tailnet" : "system-tailnet";
    std::string peer = node(mode == L"wrong-peer" ? "replacement" : "same-owner",
        mode == L"other-user" ? "42" : owner,
        mode == L"wrong-address" ? "\"100.76.1.9\"" : "\"100.76.1.2\",\"fd7a:115c:a1e0::2\"",
        mode == L"tagged" ? ",\"Tags\":[\"tag:fixture\"]" : mode == L"shared" ? ",\"ShareeNode\":true" : "");
    if (mode == L"ambiguous")
        peer += ",\"key1\":" + node("another", owner, "\"100.76.1.2\"");

    return "{\"BackendState\":\"Running\",\"TailscaleIPs\":[\"100.76.1.1\"],\"Self\":" +
        node(localId, owner, "\"100.76.1.1\"") + ",\"CurrentTailnet\":{\"StableID\":\"" + tailnetId +
        "\"},\"Peer\":{\"key0\":" + peer + "}}";
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3 || std::wstring(argv[1]) != LR"(--socket=\\.\pipe\protectedprefix\administrators\tailscale\tailscaled)")
        return 91;
    if (!hasEnvironment(L"SystemRoot") || !hasEnvironment(L"PATH") || hasEnvironment(L"TS_SOCKET") ||
        hasEnvironment(L"TS_AUTHKEY") || hasEnvironment(L"HTTPS_PROXY") || hasEnvironment(L"APPDATA") ||
        hasEnvironment(L"AURORA_NC_TEST_SECRET"))
        return 92;

    const auto file = std::filesystem::path(argv[0]);
    const auto mode = file.stem().wstring();
    if (std::filesystem::current_path() != file.parent_path())
        return 93;
    const bool readStatus = argc == 4 && std::wstring(argv[2]) == L"status" && std::wstring(argv[3]) == L"--json";
    const bool relay = argc == 5 && std::wstring(argv[2]) == L"nc" && std::wstring(argv[4]) == L"43826";
    if (!readStatus && !relay)
        return 94;
    if (relay && std::wstring(argv[3]) != (mode == L"ipv6" ? L"fd7a:115c:a1e0::2" : L"100.76.1.2"))
        return 95;

    {
        std::ofstream trace(file.parent_path() / L"trace.txt", std::ios::binary | std::ios::app);
        trace << (readStatus ? "status\n" : mode == L"ipv6" ? "nc6\n" : "nc4\n");
    }
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);

    if (readStatus) {
        if (mode == L"status-hold") {
            Sleep(INFINITE);
            return 96;
        }
        if (mode == L"status-denied")
            return 17;
        if (mode == L"status-malformed") {
            output(stdout, "broken status");
            return 0;
        }
        output(stdout, status(mode));
        return 0;
    }

    if (mode == L"relay-hold") {
        Sleep(INFINITE);
        return 97;
    }
    if (mode == L"stdout-flood" || mode == L"stderr-flood") {
        output(mode == L"stdout-flood" ? stdout : stderr, std::string(65537, 'x'));
        return 0;
    }
    output(stderr, "private diagnostic\r\n");
    char request[4]{};
    if (std::fread(request, 1, sizeof request, stdin) != sizeof request || std::string(request, 4) != std::string("a\r\n\0", 4))
        return 98;
    output(stdout, std::string("b\r\n\0", 4));
    return mode == L"exit-error" ? 19 : 0;
}
