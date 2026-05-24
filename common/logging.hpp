// Structured logging via systemd-journal.
//
// Falls back to stderr when not running under systemd (e.g. ad-hoc test).
// All log entries get a `SERVICE=fastauthd` field for easy filtering with
// `journalctl SERVICE=fastauthd`.

#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace fastauth::common::log {

enum class Level {
    Debug,
    Info,
    Notice,
    Warn,
    Error,
};

// Set the minimum level — messages below this are dropped.
void set_min_level(Level lvl);
Level get_min_level();

// Free-form log line. The `fields` list is optional structured K=V pairs that
// go to the journal as separate entries; on stderr fallback they are appended
// as `key=value` segments.
void log_line(Level lvl, std::string_view msg,
              std::initializer_list<std::pair<std::string_view, std::string>> fields = {});

// Convenience wrappers.
inline void debug(std::string_view m,
                  std::initializer_list<std::pair<std::string_view, std::string>> f = {}) {
    log_line(Level::Debug, m, f);
}
inline void info(std::string_view m,
                 std::initializer_list<std::pair<std::string_view, std::string>> f = {}) {
    log_line(Level::Info, m, f);
}
inline void notice(std::string_view m,
                   std::initializer_list<std::pair<std::string_view, std::string>> f = {}) {
    log_line(Level::Notice, m, f);
}
inline void warn(std::string_view m,
                 std::initializer_list<std::pair<std::string_view, std::string>> f = {}) {
    log_line(Level::Warn, m, f);
}
inline void error(std::string_view m,
                  std::initializer_list<std::pair<std::string_view, std::string>> f = {}) {
    log_line(Level::Error, m, f);
}

} // namespace fastauth::common::log
