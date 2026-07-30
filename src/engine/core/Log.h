#pragma once
#include <cstdio>
#include <format>
#include <string_view>

// Minimal logger: std::format to stdout/stderr with a level tag. The one
// permitted piece of global state in the engine.
namespace meat::log {

enum class Level { Info, Warn, Error };

inline void write(Level level, std::string_view msg) {
    const char* tag = level == Level::Info ? "INFO" : level == Level::Warn ? "WARN" : "ERR ";
    std::FILE* out = level == Level::Error ? stderr : stdout;
    std::fprintf(out, "[%s] %.*s\n", tag, static_cast<int>(msg.size()), msg.data());
    std::fflush(out); // survive being killed mid-run; game logs are low-volume
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
