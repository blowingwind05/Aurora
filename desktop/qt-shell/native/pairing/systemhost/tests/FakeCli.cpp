// SPDX-License-Identifier: GPL-3.0-only
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char **argv)
{
    using namespace std::chrono_literals;
    namespace fs = std::filesystem;
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] != R"(--socket=\\.\pipe\protectedprefix\administrators\tailscale\tailscaled)" ||
        std::getenv("AURORA_PAIR_HOST_TEST_SENTINEL"))
        return 1;
    if (args == std::vector<std::string>{args[0], "status", "--json"}) {
        if (fs::exists("delay-status")) {
            std::ofstream("status-started") << "1";
            for (int i = 0; i < 800 && fs::exists("delay-status"); ++i)
                std::this_thread::sleep_for(15ms);
        }
        std::ifstream data("status.json");
        std::cout << data.rdbuf();
        return data.is_open() ? 0 : 2;
    }
    if (args == std::vector<std::string>{args[0], "serve", "status", "--json"}) {
        std::ifstream data("serve.json");
        std::cout << data.rdbuf();
        return data.is_open() ? 0 : 3;
    }
    const std::string target = "tcp://127.0.0.1:";
    if (args.size() != 5 || args[1] != "serve" || args[2] != "--tcp=43826" ||
        args[3] != "--proxy-protocol=2" || args[4].find(target) != 0)
        return 4;
    const auto port = args[4].substr(target.size());
    if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos)
        return 5;

    std::ofstream("relay-port") << port;
    if (fs::exists("bad-banner"))
        std::cout << "Available on the internet:\r\n" << std::flush;
    else if (fs::exists("output-limit"))
        std::cerr << std::string(64 * 1024 + 1, 'x') << std::flush;
    else
        std::cout << "Available within " << std::flush << "your tailnet:\r\n" << std::flush;
    for (int i = 0; i < 1200 && !fs::exists("exit-relay"); ++i)
        std::this_thread::sleep_for(15ms);
    return 0;
}
