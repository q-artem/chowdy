#include "common/logging.hpp"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include <sys/uio.h>

#include <unistd.h>

#include <systemd/sd-journal.h>

namespace chowdy::common::log {

namespace {

std::atomic<Level> g_min_level{Level::Info};
std::mutex g_stderr_mu;

bool stderr_is_terminal() {
    static const bool tty = ::isatty(STDERR_FILENO) != 0;
    return tty;
}

int journal_priority(Level lvl) {
    // Maps to /usr/include/sys/syslog.h LOG_* values.
    switch (lvl) {
        case Level::Debug:  return 7; // LOG_DEBUG
        case Level::Info:   return 6; // LOG_INFO
        case Level::Notice: return 5; // LOG_NOTICE
        case Level::Warn:   return 4; // LOG_WARNING
        case Level::Error:  return 3; // LOG_ERR
    }
    return 6;
}

const char* level_name(Level lvl) {
    switch (lvl) {
        case Level::Debug:  return "DEBUG";
        case Level::Info:   return "INFO";
        case Level::Notice: return "NOTICE";
        case Level::Warn:   return "WARN";
        case Level::Error:  return "ERROR";
    }
    return "INFO";
}

} // namespace

void set_min_level(Level lvl) { g_min_level.store(lvl, std::memory_order_relaxed); }
Level get_min_level()         { return g_min_level.load(std::memory_order_relaxed); }

void log_line(Level lvl, std::string_view msg,
              std::initializer_list<std::pair<std::string_view, std::string>> fields) {
    if (lvl < get_min_level()) return;

    // sd_journal_send copies its varargs; just hand the fields as MESSAGE plus
    // arbitrary K=V tail entries.
    // Build the variadic dynamically via a small heap of std::strings.
    std::vector<std::string> kvs;
    kvs.reserve(fields.size() + 3);
    kvs.emplace_back("MESSAGE=" + std::string(msg));
    kvs.emplace_back("PRIORITY=" + std::to_string(journal_priority(lvl)));
    kvs.emplace_back("SERVICE=chowdyd");
    for (const auto& [k, v] : fields) {
        std::string field;
        field.reserve(k.size() + 1 + v.size());
        // journal field names must be uppercase. Be permissive and let callers
        // pass whatever; uppercase here.
        for (char c : k) field.push_back(static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c));
        field.push_back('=');
        field.append(v);
        kvs.push_back(std::move(field));
    }

    // sd_journal_sendv wants iovecs.
    std::vector<iovec> iovs;
    iovs.reserve(kvs.size());
    for (auto& s : kvs) iovs.push_back({s.data(), s.size()});
    int rc = ::sd_journal_sendv(iovs.data(), static_cast<int>(iovs.size()));

    // Fallback to stderr if journal isn't available (e.g. running ad-hoc).
    if (rc < 0 || stderr_is_terminal()) {
        std::lock_guard<std::mutex> lock(g_stderr_mu);
        timespec ts{};
        ::clock_gettime(CLOCK_REALTIME, &ts);
        std::fprintf(stderr, "[%ld.%03ld] %s %.*s",
                     static_cast<long>(ts.tv_sec),
                     static_cast<long>(ts.tv_nsec / 1'000'000),
                     level_name(lvl),
                     static_cast<int>(msg.size()), msg.data());
        for (const auto& [k, v] : fields) {
            std::fprintf(stderr, "  %.*s=%s",
                         static_cast<int>(k.size()), k.data(), v.c_str());
        }
        std::fputc('\n', stderr);
    }
}

} // namespace chowdy::common::log
