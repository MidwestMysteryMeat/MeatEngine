#include "engine/core/Log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>

namespace meat::log {
namespace {

constexpr std::size_t kMaxHistory = 2000;

std::mutex g_mu;
std::deque<Entry> g_ring;
const auto g_start = std::chrono::steady_clock::now();

void formatWallClock(char* out, std::size_t n) {
    using clock = std::chrono::system_clock;
    const auto t = clock::to_time_t(clock::now());
#if defined(_WIN32)
    std::tm tm{};
    localtime_s(&tm, &t);
#else
    std::tm tm{};
    localtime_r(&t, &tm);
#endif
    std::snprintf(out, n, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

} // namespace

void write(Level level, std::string_view msg) {
    const char* tag = level == Level::Info ? "INFO" : level == Level::Warn ? "WARN" : "ERR ";
    std::FILE* out = level == Level::Error ? stderr : stdout;
    std::fprintf(out, "[%s] %.*s\n", tag, static_cast<int>(msg.size()), msg.data());
    std::fflush(out);

    Entry e;
    e.level = level;
    e.timeSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    formatWallClock(e.timeWall, sizeof(e.timeWall));
    e.message.assign(msg.data(), msg.size());

    std::lock_guard lock(g_mu);
    g_ring.push_back(std::move(e));
    while (g_ring.size() > kMaxHistory) g_ring.pop_front();
}

void clearHistory() {
    std::lock_guard lock(g_mu);
    g_ring.clear();
}

std::vector<Entry> snapshotHistory() {
    std::lock_guard lock(g_mu);
    return std::vector<Entry>(g_ring.begin(), g_ring.end());
}

std::size_t historySize() {
    std::lock_guard lock(g_mu);
    return g_ring.size();
}

} // namespace meat::log
