#include "engine/core/Log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace meat::log {
namespace {

constexpr std::size_t kMaxHistory = 2000;
constexpr std::size_t kMaxWatches = 128;

std::mutex g_mu;
std::deque<Entry> g_ring;
// Insertion-order watch names so the Watches panel is stable.
std::vector<std::string> g_watchOrder;
std::unordered_map<std::string, WatchEntry> g_watches;
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

void setWatch(std::string_view name, std::string_view value) {
    if (name.empty()) return;
    const std::string key(name);
    const double t =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    std::lock_guard lock(g_mu);
    auto it = g_watches.find(key);
    if (it == g_watches.end()) {
        if (g_watchOrder.size() >= kMaxWatches) {
            // Evict oldest name.
            const std::string drop = g_watchOrder.front();
            g_watchOrder.erase(g_watchOrder.begin());
            g_watches.erase(drop);
        }
        g_watchOrder.push_back(key);
        WatchEntry w;
        w.name = key;
        w.value.assign(value.data(), value.size());
        w.timeSec = t;
        g_watches.emplace(key, std::move(w));
    } else {
        it->second.value.assign(value.data(), value.size());
        it->second.timeSec = t;
    }
}

void clearWatches() {
    std::lock_guard lock(g_mu);
    g_watches.clear();
    g_watchOrder.clear();
}

std::vector<WatchEntry> snapshotWatches() {
    std::lock_guard lock(g_mu);
    std::vector<WatchEntry> out;
    out.reserve(g_watchOrder.size());
    for (const std::string& name : g_watchOrder) {
        const auto it = g_watches.find(name);
        if (it != g_watches.end()) out.push_back(it->second);
    }
    return out;
}

} // namespace meat::log
