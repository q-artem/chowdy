// /etc/chowdy/config.toml — parsed once at daemon start.
//
// Missing file is fine — defaults below kick in. Bad TOML aborts the daemon
// so a typo never silently uses the wrong device. CLI flags from
// daemon/main.cpp take precedence over the file (they're set after load()
// returns).

#pragma once

#include <filesystem>
#include <string>

#include "common/logging.hpp"

namespace chowdy::common::config {

struct CameraSection {
    // Default matches etc/chowdy/config.toml.example — the stable by-path
    // link for the target IR camera. Override per-machine in config.toml.
    std::string device         = "/dev/v4l/by-path/pci-0000:00:14.0-usb-0:5:1.2-video-index0";
    int         width          = 640;
    int         height         = 360;
    int         fps            = 30;
    double      dark_threshold = 25.0;
    std::string policy         = "lazy";        // lazy | warm | idle_keep
    int         idle_keep_ms   = 10000;
    // lazy-only safety net: force-close the stream this many ms after last
    // use if it's somehow still on. 0 disables.
    int         lazy_safety_close_ms = 5000;
};

struct RecognitionSection {
    std::string detector_model            = "/var/lib/chowdy/models/detector.onnx";
    std::string embedder_model            = "/var/lib/chowdy/models/embedder.onnx";
    float       detector_conf_threshold   = 0.5f;
    float       similarity_floor          = 0.40f;
    float       enroll_quality_min        = 0.10f;
};

struct AntiSpoofSection {
    bool        enabled    = false;
    std::string model;
    float       threshold  = 0.7f;
};

struct AuthSection {
    int default_timeout_ms  = 2000;
};

struct LogSection {
    log::Level level                   = log::Level::Info;
    bool       log_failed_attempts     = true;
    bool       log_successful_attempts = false;
};

struct StorageSection {
    std::string users_dir = "/var/lib/chowdy/users";
};

struct AppConfig {
    CameraSection      camera;
    RecognitionSection recognition;
    AntiSpoofSection   antispoof;
    AuthSection        auth;
    LogSection         log;
    StorageSection     storage;
};

// Try to load /etc/chowdy/config.toml (or the supplied path). If the file
// is missing returns defaults. Throws std::runtime_error on parse errors so
// a misconfigured daemon refuses to start instead of silently using the
// wrong device.
AppConfig load(const std::filesystem::path& path = "/etc/chowdy/config.toml");

} // namespace chowdy::common::config
