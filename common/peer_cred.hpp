// SO_PEERCRED wrapper — the daemon uses this on every connection to
// determine which user owns the calling process, never trusting uid claims
// from the wire (see DESIGN.md §3 threat model).

#pragma once

#include <sys/types.h>

namespace fastauth::common {

struct PeerCred {
    pid_t pid;
    uid_t uid;
    gid_t gid;
};

// Returns the credentials of the peer connected to `fd`. Throws on error.
PeerCred get_peer_cred(int fd);

} // namespace fastauth::common
