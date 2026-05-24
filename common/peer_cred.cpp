#include "common/peer_cred.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include <sys/socket.h>
#include <sys/un.h>

namespace chowdy::common {

PeerCred get_peer_cred(int fd) {
    ucred c{};
    socklen_t len = sizeof(c);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &c, &len) < 0)
        throw std::runtime_error(std::string("SO_PEERCRED: ") + std::strerror(errno));
    return PeerCred{c.pid, c.uid, c.gid};
}

} // namespace chowdy::common
