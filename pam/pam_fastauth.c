/*
 * pam_fastauth.so — PAM authentication module that delegates face auth
 * to the resident fastauthd daemon over a unix socket.
 *
 * Design rules from DESIGN.md §3, §10, §16:
 *   - Must be `sufficient`, never `required`. We respect that: any
 *     ambiguous outcome (daemon dead, socket missing, timeout, parse
 *     failure) returns PAM_AUTHINFO_UNAVAIL so the next stack entry
 *     (usually pam_unix) gets to ask for a password.
 *   - We never read enrollments, never open the camera, never talk to
 *     anything other than /run/fastauth/auth.sock. Linux file
 *     permissions on that socket (0660 root:fastauth) and the daemon's
 *     SO_PEERCRED check do the access control for us.
 *   - We carry minimal state across the connection: connect, write one
 *     request, read one response, close. No retries.
 *
 * Module options (set on the PAM line, e.g.
 *   auth sufficient pam_fastauth.so timeout=2000 socket=/run/fastauth/auth.sock):
 *     timeout=MS   total deadline including connect, default 2000
 *     socket=PATH  override auth socket path, default /run/fastauth/auth.sock
 *     debug        print extra detail to syslog
 */

#define _GNU_SOURCE 1

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define PAM_SM_AUTH
#include <security/pam_appl.h>
#include <security/pam_modules.h>
#include <security/pam_ext.h>

#define DEFAULT_SOCKET   "/run/fastauth/auth.sock"
#define DEFAULT_TIMEOUT  2000   /* milliseconds */
#define RECV_BUF_SIZE    8192

struct opts {
    const char *socket_path;
    int         timeout_ms;
    int         debug;
};

static void parse_options(int argc, const char **argv, struct opts *o) {
    o->socket_path = DEFAULT_SOCKET;
    o->timeout_ms  = DEFAULT_TIMEOUT;
    o->debug       = 0;
    for (int i = 0; i < argc; ++i) {
        const char *a = argv[i];
        if (strncmp(a, "socket=", 7) == 0) {
            o->socket_path = a + 7;
        } else if (strncmp(a, "timeout=", 8) == 0) {
            int t = atoi(a + 8);
            if (t > 0 && t <= 60000) o->timeout_ms = t;
        } else if (strcmp(a, "debug") == 0) {
            o->debug = 1;
        }
    }
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void make_request_id(char *out, size_t n) {
    /* 16 hex chars from /dev/urandom; falls back to PID^time on failure. */
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        unsigned char buf[8];
        ssize_t r = read(fd, buf, sizeof(buf));
        close(fd);
        if (r == (ssize_t)sizeof(buf) && n >= 17) {
            for (size_t i = 0; i < sizeof(buf); ++i) {
                snprintf(out + i*2, 3, "%02x", buf[i]);
            }
            return;
        }
    }
    snprintf(out, n, "%llx", (unsigned long long)(getpid() ^ time(NULL)));
}

/* === wire helpers ================================================== */

static int set_io_timeout(int fd, int ms) {
    struct timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) return -1;
    return 0;
}

static int connect_socket(const char *path, int timeout_ms) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) { close(fd); errno = ENAMETOOLONG; return -1; }
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    /* Non-blocking connect with a poll deadline so a wedged socket doesn't
     * hang the login prompt past the configured timeout. */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int r = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (r < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    if (r < 0) {
        struct pollfd p; p.fd = fd; p.events = POLLOUT;
        int pr = poll(&p, 1, timeout_ms);
        if (pr <= 0) { close(fd); return -1; }
        int err = 0; socklen_t len = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
            close(fd); return -1;
        }
    }
    if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    set_io_timeout(fd, timeout_ms);
    return fd;
}

static int write_all(int fd, const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    while (n > 0) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += (size_t)w; n -= (size_t)w;
    }
    return 0;
}

static int read_all(int fd, void *data, size_t n) {
    unsigned char *p = (unsigned char *)data;
    while (n > 0) {
        ssize_t r = recv(fd, p, n, 0);
        if (r == 0) return -1; /* peer closed */
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += (size_t)r; n -= (size_t)r;
    }
    return 0;
}

/* Tiny JSON helpers — we only ever inspect a handful of known keys, so
 * the cost of pulling in a full parser isn't worth it for the PAM path. */
static int json_bool(const char *json, const char *key) {
    /* returns 1 for true, 0 for false, -1 if not found */
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) ++p;
    if (strncmp(p, "true",  4) == 0) return 1;
    if (strncmp(p, "false", 5) == 0) return 0;
    return -1;
}

static void json_string(const char *json, const char *key,
                        char *out, size_t out_len) {
    if (out_len == 0) return;
    out[0] = 0;
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return;
    p += strlen(needle);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) ++p;
    if (*p != '"') return;
    ++p;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_len) {
        if (*p == '\\' && p[1]) ++p;
        out[i++] = *p++;
    }
    out[i] = 0;
}

/* === core ========================================================== */

static int do_auth(pam_handle_t *pamh, uid_t uid,
                   const struct opts *o, char *reason_out, size_t reason_len) {
    long deadline = now_ms() + o->timeout_ms;

    int fd = connect_socket(o->socket_path, o->timeout_ms);
    if (fd < 0) {
        if (o->debug)
            pam_syslog(pamh, LOG_DEBUG, "connect %s failed: %s",
                       o->socket_path, strerror(errno));
        return -1;
    }
    long remaining = deadline - now_ms();
    if (remaining < 50) remaining = 50;
    set_io_timeout(fd, (int)remaining);

    char reqid[17] = {0};
    make_request_id(reqid, sizeof(reqid));

    /* Build the JSON payload. uid is the only discretionary input — keep
     * the request small and free of user-supplied strings. */
    char payload[256];
    int n = snprintf(payload, sizeof(payload),
        "{\"type\":\"auth\",\"uid\":%u,\"timeout_ms\":%d,\"request_id\":\"%s\"}",
        (unsigned)uid, o->timeout_ms, reqid);
    if (n <= 0 || n >= (int)sizeof(payload)) { close(fd); return -1; }

    uint32_t len_be = htonl((uint32_t)n);
    if (write_all(fd, &len_be, 4) < 0)          { close(fd); return -1; }
    if (write_all(fd, payload, (size_t)n) < 0)  { close(fd); return -1; }

    uint32_t rlen_be = 0;
    if (read_all(fd, &rlen_be, 4) < 0)          { close(fd); return -1; }
    uint32_t rlen = ntohl(rlen_be);
    if (rlen == 0 || rlen > RECV_BUF_SIZE)      { close(fd); return -1; }

    char resp[RECV_BUF_SIZE + 1];
    if (read_all(fd, resp, rlen) < 0)           { close(fd); return -1; }
    resp[rlen] = 0;
    close(fd);

    if (o->debug) pam_syslog(pamh, LOG_DEBUG, "fastauth resp: %s", resp);

    json_string(resp, "reason", reason_out, reason_len);
    int success = json_bool(resp, "success");
    return success;
}

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags,
                                   int argc, const char **argv) {
    (void)flags;
    struct opts o;
    parse_options(argc, argv, &o);

    const char *username = NULL;
    if (pam_get_user(pamh, &username, NULL) != PAM_SUCCESS || !username) {
        return PAM_USER_UNKNOWN;
    }
    struct passwd *pw = getpwnam(username);
    if (!pw) return PAM_USER_UNKNOWN;

    char reason[64] = {0};
    int success = do_auth(pamh, pw->pw_uid, &o, reason, sizeof(reason));

    if (success == 1) {
        pam_syslog(pamh, LOG_NOTICE,
                   "fastauth granted for %s (reason=%s)",
                   username, reason[0] ? reason : "matched");
        return PAM_SUCCESS;
    }
    if (success == 0) {
        /* Daemon spoke to us and said no — caller should ask for a password
         * via the next stack entry (the recommended config is `sufficient`,
         * so PAM_AUTH_ERR here just means "didn't pass", not "denied"). */
        pam_syslog(pamh, LOG_NOTICE,
                   "fastauth denied for %s (reason=%s)",
                   username, reason[0] ? reason : "no_match");
        return PAM_AUTH_ERR;
    }
    /* success == -1: we couldn't talk to the daemon at all (socket gone,
     * timeout, garbled response). Hand off to the next module without
     * pretending we know. */
    if (o.debug) pam_syslog(pamh, LOG_DEBUG, "fastauth unavailable");
    return PAM_AUTHINFO_UNAVAIL;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t *pamh, int flags,
                              int argc, const char **argv) {
    (void)pamh; (void)flags; (void)argc; (void)argv;
    return PAM_SUCCESS;
}
