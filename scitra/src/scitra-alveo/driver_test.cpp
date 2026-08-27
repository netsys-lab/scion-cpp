#include "scitra/scitra-alveo/dataplane/interface.hpp"
#include "scion/error_codes.hpp"

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <iostream>

std::string sysfile = "/sys/devices/pci0000:b2/0000:b2:00.0/0000:b3:00.0/resource2";
Dataplane dp;

static void parseArgs(int argc, char* argv[])
{
    using LogLevel = std::pair<const char*, spdlog::level::level_enum>;
    static const std::array<LogLevel, 6> logLevelMap{
        LogLevel{"trace", spdlog::level::trace},
        LogLevel{"debug", spdlog::level::debug},
        LogLevel{"info", spdlog::level::info},
        LogLevel{"warning", spdlog::level::warn},
        LogLevel{"error", spdlog::level::err},
        LogLevel{"critical", spdlog::level::critical}
    };
    spdlog::level::level_enum logLevel = spdlog::level::debug;

    CLI::App app{"Scitra-Alveo Driver Test"};
    app.add_option("-s,--sysfile", sysfile, "Path to Alveo OpenNIC device in /sys.");
    app.add_option("-l,--log-level", logLevel,
        "Log level (default: debug)")->transform(CLI::CheckedTransformer(logLevelMap));
    try {
        app.parse(argc, argv);
    }
    catch (const CLI::ParseError& e) {
        std::exit(app.exit(e));
    }

    spdlog::set_default_logger(spdlog::stderr_color_mt("log"));
    spdlog::set_pattern("[%Y-%m-%d %T.%e] [%t] [%^%l%$] %v");
    spdlog::set_level(logLevel);
}

int main(int argc, char* argv[])
{
    parseArgs(argc, argv);
    auto ec = dp.initialize(sysfile);
    if (ec) {
        spdlog::critical("Dataplane initialization: {}", scion::fmtError(ec));
        return 1;
    }

    spdlog::info("Program static tables");
    std::vector<std::uint64_t> keys = {0, 0};
    std::vector<std::uint64_t> params = {};
    ec = dp.tableInsert(
        P4_PROG_IG_TRANSLATOR, "tab_source_translation_46",
        keys, "translateSource46BGP", params);
    if (ec) spdlog::error("{}", scion::fmtError(ec));
    ec = dp.tableInsert(
        P4_PROG_IG_TRANSLATOR, "tab_dest_translation_46",
        keys, "translateDest46BGP", params);
    if (ec) spdlog::error("{}", scion::fmtError(ec));

    keys = {0, 2};
    ec = dp.tableInsert(
        P4_PROG_IG_TRANSLATOR, "tab_source_translation_46",
        keys, "translateSource46SCION", params);
    if (ec) spdlog::error("{}", scion::fmtError(ec));
    ec = dp.tableInsert(
        P4_PROG_IG_TRANSLATOR, "tab_dest_translation_46",
        keys, "translateDest46SCION", params);
    if (ec) spdlog::error("{}", scion::fmtError(ec));

    spdlog::info("Read counters");
    dp.printAllCounters(P4_PROG_IG_CLASSIFIER);
    dp.printAllCounters(P4_PROG_IG_TRANSLATOR);
    dp.printAllCounters(P4_PROG_EG_TRANSLATOR);

    std::cout << "Waiting\n";
    std::cin.get();
    return 0;
}
