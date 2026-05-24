// chowdyd entry point.
//
// CLI / env contract:
//   --auth-socket PATH    Override AF_UNIX path for the PAM-facing socket.
//   --mgmt-socket PATH    Override AF_UNIX path for the CLI-facing socket.
//   --log-level LEVEL     debug|info|notice|warn|error (default: info).
//   --foreground          Always log to stderr even if journal is reachable
//                         (otherwise stderr fallback only when TTY).
//
// systemd activation: when LISTEN_FDS is set we adopt those fds instead of
// binding ourselves (see chowdyd.socket — auth first, mgmt second).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <systemd/sd-daemon.h>

#include "common/config.hpp"
#include "common/logging.hpp"
#include "daemon/enroll_session.hpp"
#include "daemon/enrollment_store.hpp"
#include "daemon/pipeline.hpp"
#include "daemon/server.hpp"

namespace {

std::atomic<chowdy::daemon::Server*> g_server{nullptr};

void on_signal(int) {
    auto* s = g_server.load();
    if (s) s->stop();
}

std::optional<chowdy::common::log::Level> parse_level(std::string_view s) {
    using L = chowdy::common::log::Level;
    if (s == "debug")  return L::Debug;
    if (s == "info")   return L::Info;
    if (s == "notice") return L::Notice;
    if (s == "warn")   return L::Warn;
    if (s == "error")  return L::Error;
    return std::nullopt;
}

} // namespace

int main(int argc, char** argv) {
    // 1. Pre-scan CLI for --config so file load happens with the right path.
    std::string config_path = "/etc/chowdy/config.toml";
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        }
    }
    chowdy::common::config::AppConfig acfg;
    try {
        acfg = chowdy::common::config::load(config_path);
    } catch (const std::exception& e) {
        std::cerr << "config: " << e.what() << "\n";
        return 2;
    }
    chowdy::common::log::set_min_level(acfg.log.level);

    // 2. Seed runtime config from the file, then let CLI overrides win.
    chowdy::daemon::ServerConfig srv_cfg;
    srv_cfg.auth_socket_path = "/run/chowdy/auth.sock";
    srv_cfg.mgmt_socket_path = "/run/chowdy/mgmt.sock";

    chowdy::daemon::PipelineConfig pl_cfg;
    pl_cfg.detector_model            = acfg.recognition.detector_model;
    pl_cfg.embedder_model            = acfg.recognition.embedder_model;
    pl_cfg.detector_conf_threshold   = acfg.recognition.detector_conf_threshold;
    pl_cfg.enroll_quality_min        = acfg.recognition.enroll_quality_min;
    pl_cfg.camera.device             = acfg.camera.device;
    pl_cfg.camera.width              = acfg.camera.width;
    pl_cfg.camera.height             = acfg.camera.height;
    pl_cfg.camera.fps                = acfg.camera.fps;
    pl_cfg.dark_threshold            = acfg.camera.dark_threshold;
    pl_cfg.camera_policy             = acfg.camera.policy;
    pl_cfg.idle_keep_ms              = acfg.camera.idle_keep_ms;
    std::string users_dir            = acfg.storage.users_dir;

    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::cerr << "missing value for " << what << "\n"; std::exit(2); }
            return std::string(argv[++i]);
        };
        if      (a == "--config")      { ++i; /* already consumed */ }
        else if (a == "--auth-socket") srv_cfg.auth_socket_path = next("--auth-socket");
        else if (a == "--mgmt-socket") srv_cfg.mgmt_socket_path = next("--mgmt-socket");
        else if (a == "--users-dir")   users_dir = next("--users-dir");
        else if (a == "--detector")    pl_cfg.detector_model = next("--detector");
        else if (a == "--embedder")    pl_cfg.embedder_model = next("--embedder");
        else if (a == "--device")      pl_cfg.camera.device = next("--device");
        else if (a == "--camera-policy") pl_cfg.camera_policy = next("--camera-policy");
        else if (a == "--idle-keep-ms")  pl_cfg.idle_keep_ms = std::atoi(next("--idle-keep-ms").c_str());
        else if (a == "--log-level") {
            auto lvl = parse_level(next("--log-level"));
            if (!lvl) { std::cerr << "bad log level\n"; return 2; }
            chowdy::common::log::set_min_level(*lvl);
        }
        else if (a == "--foreground") { /* no-op for now */ }
        else if (a == "-h" || a == "--help") {
            std::cout << "usage: " << argv[0]
                      << " [--config PATH] [--auth-socket PATH] [--mgmt-socket PATH]\n"
                         "       [--users-dir DIR] [--detector PATH] [--embedder PATH]\n"
                         "       [--device PATH] [--camera-policy lazy|warm|idle_keep]\n"
                         "       [--idle-keep-ms N] [--log-level LVL] [--foreground]\n";
            return 0;
        }
        else { std::cerr << "unknown arg: " << a << "\n"; return 2; }
    }

    // Systemd socket activation: if LISTEN_FDS is set, the listening sockets
    // are already bound. By convention in chowdyd.socket, slot 0 is auth,
    // slot 1 is mgmt.
    int n = ::sd_listen_fds(1);
    if (n == 2) {
        srv_cfg.auth_socket_fd = SD_LISTEN_FDS_START + 0;
        srv_cfg.mgmt_socket_fd = SD_LISTEN_FDS_START + 1;
        chowdy::common::log::info("adopting systemd sockets");
    } else if (n != 0) {
        chowdy::common::log::warn("unexpected LISTEN_FDS",
            {{"n", std::to_string(n)}});
    }

    // Install graceful-shutdown handlers before constructing the server so
    // partial init can still be torn down.
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    ::sigemptyset(&sa.sa_mask);
    ::sigaction(SIGINT,  &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::signal(SIGPIPE, SIG_IGN);

    try {
        chowdy::daemon::Pipeline pipeline(pl_cfg);
        chowdy::daemon::EnrollmentStore store(users_dir);
        chowdy::daemon::EnrollSessionManager sessions;
        chowdy::daemon::Server server(srv_cfg, &pipeline, &store, &sessions);
        g_server.store(&server);

        chowdy::common::log::notice("chowdyd ready",
            {{"auth_sock", srv_cfg.auth_socket_path},
             {"mgmt_sock", srv_cfg.mgmt_socket_path},
             {"users_dir", users_dir},
             {"detector",  pl_cfg.detector_model.string()},
             {"embedder",  pl_cfg.embedder_model.string()}});
        ::sd_notify(0, "READY=1");

        // Watchdog ping. systemd unit declares WatchdogSec=30s; we ping at
        // half-period so a single missed tick doesn't kill the daemon. Pings
        // come from a dedicated thread so a stuck accept loop or stuck
        // pipeline (e.g. wedged V4L2 ioctl) will legitimately fail the
        // watchdog and trigger a Restart=on-failure cycle.
        std::atomic<bool> wd_stop{false};
        std::mutex        wd_mu;
        std::condition_variable wd_cv;
        uint64_t usec = 0;
        std::thread wd_thread;
        if (::sd_watchdog_enabled(0, &usec) > 0 && usec > 0) {
            const auto half = std::chrono::microseconds(usec / 2);
            wd_thread = std::thread([&, half]{
                while (!wd_stop.load()) {
                    ::sd_notify(0, "WATCHDOG=1");
                    std::unique_lock<std::mutex> lk(wd_mu);
                    wd_cv.wait_for(lk, half, [&]{ return wd_stop.load(); });
                }
            });
            chowdy::common::log::info("watchdog ping armed",
                {{"period_us", std::to_string(usec)}});
        }

        server.run();

        ::sd_notify(0, "STOPPING=1");
        wd_stop.store(true);
        wd_cv.notify_all();
        if (wd_thread.joinable()) wd_thread.join();
        g_server.store(nullptr);
    } catch (const std::exception& e) {
        chowdy::common::log::error("fatal", {{"err", e.what()}});
        return 1;
    }
    chowdy::common::log::notice("chowdyd shutting down");
    return 0;
}
