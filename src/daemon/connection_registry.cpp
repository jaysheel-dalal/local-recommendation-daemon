#include "lrd/daemon/connection_registry.hpp"

#include <sys/socket.h>

namespace lrd::daemon {

std::uint64_t ConnectionRegistry::add(int fd) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
        return 0;
    }
    const std::uint64_t token = next_token_++;
    connections_.emplace(token, fd);
    return token;
}

void ConnectionRegistry::remove(std::uint64_t token) noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    connections_.erase(token);
}

void ConnectionRegistry::stop_all() noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;

    for (const auto& [token, fd] : connections_) {
        // SHUT_RDWR, not close(): the descriptor still belongs to a UnixStream
        // on another thread, which will close it in the normal way. This just
        // makes its blocked read() return 0, which the connection loop already
        // treats as a peer disconnect.
        //
        // Errors are ignored deliberately. The only interesting one is ENOTCONN,
        // which means the peer has already gone - exactly the state we were
        // trying to reach.
        ::shutdown(fd, SHUT_RDWR);
    }
}

bool ConnectionRegistry::stopping() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return stopping_;
}

std::size_t ConnectionRegistry::active() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return connections_.size();
}

}  // namespace lrd::daemon
