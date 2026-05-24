#include "common/config.hpp"

#include <filesystem>
#include <stdexcept>

#include <toml++/toml.hpp>

#include "common/logging.hpp"

namespace fastauth::common::config {

namespace {

log::Level parse_level(std::string_view s, log::Level fallback) {
    if (s == "debug")  return log::Level::Debug;
    if (s == "info")   return log::Level::Info;
    if (s == "notice") return log::Level::Notice;
    if (s == "warn" || s == "warning") return log::Level::Warn;
    if (s == "error") return log::Level::Error;
    return fallback;
}

template <typename T, typename View>
void assign_if(T& dst, View v) {
    if (auto val = v.template value<T>()) dst = *val;
}

template <typename View>
void assign_if_str(std::string& dst, View v) {
    if (auto val = v.template value<std::string>()) dst = *val;
}

} // namespace

AppConfig load(const std::filesystem::path& path) {
    AppConfig cfg;

    if (!std::filesystem::exists(path)) {
        log::info("config: file missing, using defaults",
            {{"path", path.string()}});
        return cfg;
    }

    toml::table tbl;
    try {
        tbl = toml::parse_file(path.string());
    } catch (const toml::parse_error& e) {
        throw std::runtime_error("config parse error in " + path.string()
                                 + ": " + std::string(e.description()));
    }

    auto cam = tbl["camera"];
    assign_if_str(cfg.camera.device,        cam["device"]);
    assign_if<int>(cfg.camera.width,        cam["width"]);
    assign_if<int>(cfg.camera.height,       cam["height"]);
    assign_if<int>(cfg.camera.fps,          cam["fps"]);
    assign_if<double>(cfg.camera.dark_threshold, cam["dark_threshold"]);
    assign_if_str(cfg.camera.policy,        cam["camera_policy"]);
    assign_if<int>(cfg.camera.idle_keep_ms, cam["idle_keep_ms"]);

    auto rec = tbl["recognition"];
    assign_if_str(cfg.recognition.detector_model, rec["detector_model"]);
    assign_if_str(cfg.recognition.embedder_model, rec["embedder_model"]);
    {
        double d;
        if (auto v = rec["detector_conf_threshold"].value<double>()) cfg.recognition.detector_conf_threshold = static_cast<float>(*v);
        if (auto v = rec["similarity_floor"].value<double>())        cfg.recognition.similarity_floor        = static_cast<float>(*v);
        if (auto v = rec["enroll_quality_min"].value<double>())      cfg.recognition.enroll_quality_min      = static_cast<float>(*v);
        (void)d;
    }

    auto as = tbl["antispoof"];
    assign_if<bool>(cfg.antispoof.enabled, as["enabled"]);
    assign_if_str(cfg.antispoof.model,     as["model"]);
    if (auto v = as["threshold"].value<double>()) cfg.antispoof.threshold = static_cast<float>(*v);

    auto au = tbl["auth"];
    assign_if<int>(cfg.auth.default_timeout_ms, au["default_timeout_ms"]);

    auto lg = tbl["log"];
    if (auto v = lg["level"].value<std::string>()) cfg.log.level = parse_level(*v, cfg.log.level);
    assign_if<bool>(cfg.log.log_failed_attempts,     lg["log_failed_attempts"]);
    assign_if<bool>(cfg.log.log_successful_attempts, lg["log_successful_attempts"]);

    auto st = tbl["storage"];
    assign_if_str(cfg.storage.users_dir, st["users_dir"]);

    log::info("config loaded", {{"path", path.string()}});
    return cfg;
}

} // namespace fastauth::common::config
