// pam_fastauth.so — minimal PAM authentication module that delegates to
// fastauthd over a unix socket.
//
// M4 placeholder: just returns PAM_AUTH_ERR so callers fall through to
// the next auth line (`auth sufficient pam_fastauth.so` is no-op). Full
// implementation, including request/response framing and the SO_PEERCRED
// trust contract, lands on M6.
//
// IMPORTANT FOR INSTALLATION (see DESIGN.md §10, §16):
//   * Always configure this module with `auth sufficient`, NEVER `required`.
//   * Test on `sudo`, never directly on a login screen, before wider use.
//   * Keep a second TTY with root logged in while testing.

#define PAM_SM_AUTH

#include <security/pam_modules.h>
#include <security/pam_ext.h>

#include <syslog.h>

PAM_EXTERN int
pam_sm_authenticate(pam_handle_t *pamh,
                    int flags,
                    int argc,
                    const char **argv)
{
    (void)flags;
    (void)argc;
    (void)argv;
    pam_syslog(pamh, LOG_INFO,
               "pam_fastauth M4 stub — returning PAM_AUTH_ERR; real impl on M6");
    return PAM_AUTH_ERR;
}

PAM_EXTERN int
pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    (void)pamh; (void)flags; (void)argc; (void)argv;
    return PAM_SUCCESS;
}
