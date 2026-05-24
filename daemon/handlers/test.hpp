#pragma once

#include "common/peer_cred.hpp"
#include "proto/messages.hpp"

namespace fastauth::daemon {
class Pipeline;
class EnrollmentStore;
class EnrollSessionManager;
}

namespace fastauth::daemon::handlers {

// Shared between every handler. Created fresh per connection in server.cpp.
// Pipeline/Store/EnrollSessionManager are owned by the Server and outlive
// every Context.
struct Context {
    common::PeerCred       peer{};
    int                    conn_fd      = -1;
    bool                   is_auth_sock = false;

    Pipeline*              pipeline         = nullptr;
    EnrollmentStore*       store            = nullptr;
    EnrollSessionManager*  enroll_sessions  = nullptr;
};

proto::AnyResponse handle_test(const Context& ctx, const proto::TestRequest& req);

} // namespace fastauth::daemon::handlers
