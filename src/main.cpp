#include "engine/core/Engine.h"
#include "engine/core/Log.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace {

meat::EngineConfig parseArgs(int argc, char** argv) {
    meat::EngineConfig config;
    using Mode = meat::EngineConfig::Mode;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (arg == "--host") {
            config.mode = Mode::Host;
        } else if (arg == "--join") {
            config.mode = Mode::Join;
            if (const char* addr = next()) {
                const std::string s = addr;
                if (const auto colon = s.find(':'); colon != std::string::npos) {
                    config.address = s.substr(0, colon);
                    config.port = static_cast<std::uint16_t>(std::atoi(s.c_str() + colon + 1));
                } else {
                    config.address = s;
                }
            }
        } else if (arg == "--server") {
            config.mode = Mode::Dedicated;
        } else if (arg == "--port") {
            if (const char* p = next()) config.port = static_cast<std::uint16_t>(std::atoi(p));
        } else if (arg == "--seed") {
            if (const char* s = next())
                config.seed = static_cast<std::uint32_t>(std::strtoul(s, nullptr, 10));
        } else {
            meat::log::warn("unknown argument '{}'", arg);
        }
    }
    return config;
}

} // namespace

int main(int argc, char** argv) {
    meat::Engine engine;
    return engine.run(parseArgs(argc, argv));
}
