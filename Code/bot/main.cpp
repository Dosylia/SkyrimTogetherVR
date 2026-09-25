// STBot: a headless Skyrim Together player driven by a script. See Bot.h and VR_TODO.md section 6.1.
//
//   STBot <script> [--server host:port] [--password X] [--name X] [--x N --y N] [--hostlog path] [--spacing N]
//
// The script is one command per line, '#' starts a comment:
//   log <text>                      print
//   wait <seconds>
//   walk <x> <y> [speed]            to a world position (units/s, default 150)
//   walk rel <dx> <dy> [speed]      relative to where the bot stands
//   follow <distance> <seconds>     stay that far from the host
//   equip <hex form id> [left|both] [spell]
//   unequip <hex form id> [left|both]
//   health <value>  damage <n>  die (health -4)  respawn
//   disconnect  reconnect  loop  stop
//   start <x> <y>                   where to look for the host (same as --x --y)

#include "Bot.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
// Every bot used to open "logs/bot.log" and truncate it, while other bots had the same file open: a two-sided
// test runs at least two of these at once, and a self-check run cycles four through in seconds. Their lines were
// interleaved in one file, each start threw the previous run away, and on 2026-09-25 one bot hung before it could
// log even its first line, leaving a test that never ran and never finished. One file per bot, named after it.
std::string LogPath(const std::string& acName)
{
    std::string safe;
    for (const char c : acName)
        safe.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '-');
    if (safe.empty())
        safe = "bot";
    return "logs/bot-" + safe + ".log";
}

void SetupLogging(const std::string& acName)
{
    std::error_code error;
    std::filesystem::create_directories("logs", error);

    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file = std::make_shared<spdlog::sinks::basic_file_sink_mt>(LogPath(acName), true);
    auto logger = std::make_shared<spdlog::logger>("bot", spdlog::sinks_init_list{console, file});
    logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    logger->set_level(spdlog::level::info);
    logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(logger);
}

bool ReadScript(const std::string& acPath, std::vector<Command>& aOut, BotOptions& aOptions)
{
    std::ifstream file(acPath);
    if (!file)
    {
        spdlog::error("Cannot open the script '{}'", acPath);
        return false;
    }

    std::string line;
    int number = 0;
    while (std::getline(file, line))
    {
        ++number;
        const auto hash = line.find('#');
        if (hash != std::string::npos)
            line.erase(hash);

        std::istringstream stream(line);
        Command command;
        command.Line = number;
        if (!(stream >> command.Name))
            continue;
        std::string token;
        while (stream >> token)
            command.Args.push_back(token);

        if (command.Name == "start" && command.Args.size() >= 2)
        {
            try
            {
                aOptions.Start = glm::vec2(std::stof(command.Args[0]), std::stof(command.Args[1]));
            }
            catch (...)
            {
                spdlog::warn("Line {}: 'start' needs two numbers", number);
            }
            continue;
        }

        aOut.push_back(std::move(command));
    }

    return true;
}
} // namespace

int main(int argc, char** argv)
{
    // The name decides the log file, so it has to be read before anything can be logged.
    std::string logName = "bot";
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--name")
            logName = argv[i + 1];

    SetupLogging(logName);

    BotOptions options;
    options.HostLog = "E:\\FUS\\tools\\Skyrim Together VR\\logs\\tp_client.log";
    std::string scriptPath;
    std::optional<float> x;
    std::optional<float> y;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto next = [&](std::string& aOut)
        {
            if (i + 1 >= argc)
            {
                spdlog::error("{} needs a value", arg);
                return false;
            }
            aOut = argv[++i];
            return true;
        };

        std::string value;
        if (arg == "--server" && next(value))
            options.Server = value;
        else if (arg == "--password" && next(value))
            options.Password = value;
        else if (arg == "--name" && next(value))
            options.Name = value;
        else if (arg == "--hostlog" && next(value))
            options.HostLog = value;
        else if (arg == "--worldspace" && next(value))
        {
            options.WorldSpaceFormId = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 16));
        }
        else if (arg == "--standalone")
        {
            options.Standalone = true;
        }
        else if (arg == "--max-runtime" && next(value))
        {
            options.MaxRuntime = std::strtof(value.c_str(), nullptr);
        }
        else if (arg == "--host-timeout" && next(value))
        {
            options.HostTimeout = std::strtof(value.c_str(), nullptr);
        }
        else if (arg == "--plugin" && next(value))
        {
            options.WorldSpacePlugin = value;
        }
        else if (arg == "--spacing" && next(value))
            options.Spacing = std::stof(value);
        else if (arg == "--x" && next(value))
            x = std::stof(value);
        else if (arg == "--y" && next(value))
            y = std::stof(value);
        else if (arg.rfind("--", 0) == 0)
        {
            spdlog::error("Unknown option {}", arg);
            return 2;
        }
        else
            scriptPath = arg;
    }

    if (x && y)
        options.Start = glm::vec2(*x, *y);

    if (scriptPath.empty())
    {
        spdlog::error("Usage: STBot <script> [--server host:port] [--password X] [--name X] [--x N --y N] [--hostlog path] [--spacing N]");
        return 2;
    }

    std::vector<Command> script;
    if (!ReadScript(scriptPath, script, options))
        return 2;

    spdlog::info("STBot {} : '{}' with {} commands, server {}, name '{}'", BUILD_COMMIT, scriptPath, script.size(), options.Server, options.Name);

    Bot bot(std::move(options), std::move(script));
    return bot.Run();
}
