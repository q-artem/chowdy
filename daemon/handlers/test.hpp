#pragma once

#include "common/peer_cred.hpp"
#include "proto/messages.hpp"

namespace fastauth::daemon::handlers {

struct Context {
    common::PeerCred peer;
    int              conn_fd;
    bool             is_auth_sock;
};

proto::AnyResponse handle_test(const Context& ctx, const proto::TestRequest& req);

} // namespace fastauth::daemon::handlers
