// fastauth-cli — user-facing companion to fastauthd.
//
// M4 scope: just `test` and `auth-test`. Full enroll/list/remove land in M7
// once the daemon-side handlers are real.

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <unistd.h>

#include "common/ipc.hpp"
#include "proto/messages.hpp"

namespace {

std::string gen_request_id() {
    // Cheap pseudo-uuid: random hex string, enough to be greppable per call.
    static thread_local std::mt19937_64 rng{
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016lx",
                  static_cast<unsigned long>(rng()));
    return buf;
}

void cmd_test(const std::string& sock) {
    fastauth::proto::TestRequest req;
    req.request_id = gen_request_id();
    req.timeout_ms = 3000;

    auto fd = fastauth::common::connect_unix(sock);
    fastauth::common::set_timeout(fd.get(),
        std::chrono::milliseconds(req.timeout_ms + 1000),
        std::chrono::milliseconds(1000));
    fastauth::common::write_message(fd.get(), nlohmann::json(req));

    auto resp = fastauth::common::read_message(fd.get());
    std::cout << resp.dump(2) << "\n";
}

void cmd_auth_test(const std::string& sock, uid_t uid) {
    fastauth::proto::AuthRequest req;
    req.request_id = gen_request_id();
    req.timeout_ms = 2000;
    req.uid        = static_cast<uint32_t>(uid);

    auto fd = fastauth::common::connect_unix(sock);
    fastauth::common::set_timeout(fd.get(),
        std::chrono::milliseconds(req.timeout_ms + 1000),
        std::chrono::milliseconds(1000));
    fastauth::common::write_message(fd.get(), nlohmann::json(req));

    auto resp = fastauth::common::read_message(fd.get());
    std::cout << resp.dump(2) << "\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string mgmt_sock = "/run/fastauth/mgmt.sock";
    std::string auth_sock = "/run/fastauth/auth.sock";
    int positional_start = 1;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--mgmt-socket" && i + 1 < argc) { mgmt_sock = argv[++i]; positional_start = i + 1; continue; }
        if (a == "--auth-socket" && i + 1 < argc) { auth_sock = argv[++i]; positional_start = i + 1; continue; }
        if (a == "-h" || a == "--help") {
            std::cout << "usage: " << argv[0] << " [--mgmt-socket P] [--auth-socket P] <cmd>\n"
                      << "commands:\n"
                      << "  test         ping the daemon via mgmt socket\n"
                      << "  auth-test    send auth request for current uid via auth socket\n"
                      << "  enroll       (M7)\n  list (M7)\n  remove (M7)\n";
            return 0;
        }
        positional_start = i;
        break;
    }
    if (positional_start >= argc) { std::cerr << "missing command. try -h\n"; return 2; }
    const std::string cmd = argv[positional_start];

    try {
        if      (cmd == "test")      cmd_test(mgmt_sock);
        else if (cmd == "auth-test") cmd_auth_test(auth_sock, ::getuid());
        else { std::cerr << "unknown command: " << cmd << "\n"; return 2; }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
