// fastauth-cli — пользовательский интерфейс к fastauthd.
//
// Команды:
//   test            быстрый ping mgmt-сокета: захватывает один кадр и
//                   сообщает что увидел; флаг would_auth говорит, прошёл бы
//                   полноценный auth.
//   auth-test       шлёт настоящий auth-запрос для своего uid через
//                   auth-сокет. Возвращает success/fail.
//   enroll --label L [-n N]   интерактивный энроллмент: жмёшь Enter,
//                   утилита крутит цикл frame-запросов до N успешных кадров.
//   list            показать энроллменты текущего uid.
//   remove [--label L | --all]  удалить один лейбл или все.
//
// Все команды кроме auth-test ходят на mgmt-сокет.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <thread>

#include <unistd.h>

#include "common/ipc.hpp"
#include "proto/messages.hpp"

namespace {

namespace proto = fastauth::proto;
namespace common = fastauth::common;

std::string gen_request_id() {
    static thread_local std::mt19937_64 rng{
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016lx",
                  static_cast<unsigned long>(rng()));
    return buf;
}

nlohmann::json round_trip(const std::string& sock, const nlohmann::json& req,
                          std::chrono::milliseconds total_budget) {
    auto fd = common::connect_unix(sock);
    common::set_timeout(fd.get(),
                        total_budget + std::chrono::milliseconds{1000},
                        std::chrono::milliseconds{1000});
    common::write_message(fd.get(), req);
    return common::read_message(fd.get());
}

void cmd_test(const std::string& mgmt) {
    proto::TestRequest req;
    req.request_id = gen_request_id();
    req.timeout_ms = 3000;
    auto resp = round_trip(mgmt, nlohmann::json(req), std::chrono::milliseconds{3000});
    std::cout << resp.dump(2) << "\n";
}

void cmd_auth_test(const std::string& auth_sock, uid_t uid) {
    proto::AuthRequest req;
    req.request_id = gen_request_id();
    req.timeout_ms = 2000;
    req.uid        = static_cast<uint32_t>(uid);
    auto resp = round_trip(auth_sock, nlohmann::json(req), std::chrono::milliseconds{2000});
    std::cout << resp.dump(2) << "\n";
}

void cmd_list(const std::string& mgmt) {
    proto::ListRequest req;
    req.request_id = gen_request_id();
    auto resp = round_trip(mgmt, nlohmann::json(req), std::chrono::milliseconds{1000});
    if (resp.value("type", "") != "list_result") {
        std::cout << resp.dump(2) << "\n";
        return;
    }
    auto lr = resp.get<proto::ListResponse>();
    if (lr.enrollments.empty()) {
        std::cout << "энроллментов нет\n";
        return;
    }
    std::cout << "Метка        Кадры  Порог   Создан\n";
    std::cout << "-----------  -----  ------  -------------------\n";
    for (const auto& it : lr.enrollments) {
        char tbuf[32]; tbuf[0] = 0;
        time_t t = static_cast<time_t>(it.created);
        struct tm tm{};
        localtime_r(&t, &tm);
        std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);
        std::printf("%-11s  %5d  %5.3f  %s\n",
                    it.label.c_str(), it.embeddings, it.threshold, tbuf);
    }
}

void cmd_remove(const std::string& mgmt, const std::string& label, bool all) {
    proto::RemoveRequest req;
    req.request_id = gen_request_id();
    req.label      = all ? "" : label;
    auto resp = round_trip(mgmt, nlohmann::json(req), std::chrono::milliseconds{1000});
    std::cout << resp.dump(2) << "\n";
}

int cmd_enroll(const std::string& mgmt, const std::string& label, int n) {
    // 1. start session.
    proto::EnrollStartRequest start;
    start.request_id = gen_request_id();
    start.label      = label;
    start.min_frames = std::max(3, n / 2);
    start.max_frames = n;

    std::cout << "Подключение к fastauthd...\n";
    auto resp = round_trip(mgmt, nlohmann::json(start), std::chrono::milliseconds{2000});
    if (resp.value("type", "") != "enroll_progress") {
        std::cerr << "ошибка enroll_start: " << resp.dump(2) << "\n";
        return 1;
    }
    auto progress = resp.get<proto::EnrollProgressResponse>();
    const std::string session = progress.session;
    std::cout << "Готов? Смотри прямо в камеру.\n";
    std::cout << "Нажми Enter и держи позу пока не наберём " << n << " хороших кадров...\n";
    std::cin.get();

    // 2. frame loop. Daemon's process_one_frame is blocking with its own
    // ~800ms timeout, so a tight client-side loop is fine.
    int last_collected = 0;
    while (!progress.done) {
        proto::EnrollFrameRequest fr;
        fr.request_id = gen_request_id();
        fr.session    = session;
        auto rr = round_trip(mgmt, nlohmann::json(fr), std::chrono::milliseconds{3000});
        if (rr.value("type", "") != "enroll_progress") {
            std::cerr << "ошибка enroll_frame: " << rr.dump(2) << "\n";
            return 1;
        }
        progress = rr.get<proto::EnrollProgressResponse>();
        if (progress.frames_collected > last_collected) {
            std::printf("  кадр %d/%d: ok (quality %.2f)\n",
                        progress.frames_collected, progress.frames_needed, progress.quality);
            last_collected = progress.frames_collected;
        } else {
            std::printf("  %s\n", progress.hint.c_str());
        }
        // Маленькая пауза между запросами — даёт пользователю шанс поправить позу,
        // и снижает шанс что подряд два одинаковых кадра попадут в энроллмент.
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }

    // 3. finish.
    proto::EnrollFinishRequest fin;
    fin.request_id = gen_request_id();
    fin.session    = session;
    auto fresp = round_trip(mgmt, nlohmann::json(fin), std::chrono::milliseconds{2000});
    if (fresp.value("type", "") != "enroll_done") {
        std::cerr << "ошибка enroll_finish: " << fresp.dump(2) << "\n";
        return 1;
    }
    auto done = fresp.get<proto::EnrollDoneResponse>();
    std::printf("Сохранено: %d эмбеддингов под лейблом '%s'. Порог: %.3f\n",
                done.embeddings_saved, done.label.c_str(), done.threshold);
    return 0;
}

void usage(const char* p) {
    std::cout << "использование: " << p << " [--mgmt-socket P] [--auth-socket P] <команда>\n"
              << "команды:\n"
              << "  test                            quick ping daemon, что видит камера\n"
              << "  auth-test                       полноценный auth для текущего uid\n"
              << "  enroll --label LABEL [-n N=8]   интерактивный энроллмент\n"
              << "  list                            показать энроллменты\n"
              << "  remove (--label LABEL | --all)  удалить лейбл или все\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string mgmt_sock = "/run/fastauth/mgmt.sock";
    std::string auth_sock = "/run/fastauth/auth.sock";
    int i = 1;
    for (; i < argc; ++i) {
        std::string_view a = argv[i];
        if      (a == "--mgmt-socket" && i + 1 < argc) { mgmt_sock = argv[++i]; }
        else if (a == "--auth-socket" && i + 1 < argc) { auth_sock = argv[++i]; }
        else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else break;
    }
    if (i >= argc) { std::cerr << "пропущена команда. -h для помощи\n"; return 2; }
    const std::string cmd = argv[i++];

    try {
        if (cmd == "test")           { cmd_test(mgmt_sock); return 0; }
        if (cmd == "auth-test")      { cmd_auth_test(auth_sock, ::getuid()); return 0; }
        if (cmd == "list")           { cmd_list(mgmt_sock); return 0; }

        if (cmd == "enroll") {
            std::string label;
            int n = 8;
            for (; i < argc; ++i) {
                std::string_view a = argv[i];
                if      (a == "--label" && i + 1 < argc) label = argv[++i];
                else if (a == "-n"      && i + 1 < argc) n = std::max(3, std::atoi(argv[++i]));
                else { std::cerr << "неизвестный аргумент enroll: " << a << "\n"; return 2; }
            }
            if (label.empty()) { std::cerr << "enroll: нужен --label\n"; return 2; }
            return cmd_enroll(mgmt_sock, label, n);
        }
        if (cmd == "remove") {
            std::string label;
            bool all = false;
            for (; i < argc; ++i) {
                std::string_view a = argv[i];
                if      (a == "--label" && i + 1 < argc) label = argv[++i];
                else if (a == "--all")                   all = true;
                else { std::cerr << "неизвестный аргумент remove: " << a << "\n"; return 2; }
            }
            if (!all && label.empty()) { std::cerr << "remove: нужен --label или --all\n"; return 2; }
            cmd_remove(mgmt_sock, label, all);
            return 0;
        }
        std::cerr << "неизвестная команда: " << cmd << "\n";
        usage(argv[0]);
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "ошибка: " << e.what() << "\n";
        return 1;
    }
}
