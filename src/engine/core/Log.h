#pragma once
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

// Logger: std::format to stdout/stderr + fixed-size ring for the C9 Output Log.
// The one permitted piece of global state in the engine.
namespace meat::log {

enum class Level : std::uint8_t { Info = 0, Warn = 1, Error = 2 };

struct Entry {
    Level level = Level::Info;
    // Seconds since process start (monotonic), for ordering / relative display.
    double timeSec = 0.0;
    // Wall clock "HH:MM:SS" for UE-style Output Log lines.
    char timeWall[16] = {};
    std::string message;
};

// Writes to console and appends to the history ring (max ~2000 lines).
void write(Level level, std::string_view msg);

void clearHistory();
// Thread-safe snapshot for UI (copy).
std::vector<Entry> snapshotHistory();
std::size_t historySize();

inline const char* levelTag(Level level) {
    switch (level) {
    case Level::Info: return "Log";
    case Level::Warn: return "Warning";
    case Level::Error: return "Error";
    }
    return "Log";
}

template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace meat::log
