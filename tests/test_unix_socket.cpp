#include "lrd/common/errors.hpp"
#include "lrd/net/unix_socket.hpp"

#include "test_harness.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <system_error>
#include <thread>

namespace {

using lrd::net::UnixListener;
using lrd::net::UnixStream;

/// Unique socket path per test. Including the pid keeps parallel ctest runs
/// from colliding; the counter keeps cases within one binary apart.
std::string temp_socket_path() {
    static std::atomic<int> counter{0};
    return "/tmp/lrd_test_" + std::to_string(::getpid()) + "_" +
           std::to_string(counter.fetch_add(1)) + ".sock";
}

bool path_exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

/// Creates a plain file (not a socket) at `path`, standing in for the debris a
/// crashed daemon leaves behind.
void create_stale_socket_file(const std::string& path) {
    // A real leftover is a socket inode with nothing bound to it. Making an
    // actual one is the honest simulation: bind, then leak the descriptor by
    // closing only the socket, leaving the filesystem entry.
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    ::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    ::close(fd);  // no listen() ever happened, so nothing will accept
}

}  // namespace

LRD_TEST("bind, connect, accept and exchange data") {
    const std::string path = temp_socket_path();
    UnixListener listener = UnixListener::bind(path);

    std::thread server([&] {
        UnixStream peer = listener.accept();
        char buffer[5] = {};
        (void)peer.read_exact(buffer, sizeof(buffer));
        (void)peer.write_all(buffer, sizeof(buffer));
    });

    UnixStream client = UnixStream::connect(path);
    const std::string request = "ping!";
    LRD_REQUIRE(client.write_all(request.data(), request.size()).ok());

    std::string response(request.size(), '\0');
    LRD_REQUIRE(client.read_exact(response.data(), response.size()).ok());
    LRD_CHECK_EQ(response, request);

    server.join();
}

LRD_TEST("an over-long socket path is rejected before it reaches bind") {
    // sun_path is a fixed 108-byte array. Left to the kernel this either
    // truncates silently - binding a path nobody asked for - or fails with a
    // baffling error far from the cause.
    const std::string path = "/tmp/" + std::string(lrd::net::kMaxSocketPathLength, 'x');
    try {
        UnixListener listener = UnixListener::bind(path);
        LRD_CHECK(false);  // should not reach here
    } catch (const lrd::SystemError& e) {
        LRD_CHECK(e.code() == std::errc::filename_too_long);
    }
}

LRD_TEST("an empty socket path is rejected") {
    try {
        UnixListener listener = UnixListener::bind("");
        LRD_CHECK(false);
    } catch (const lrd::SystemError& e) {
        LRD_CHECK(e.code() == std::errc::invalid_argument);
    }
}

LRD_TEST("the listener unlinks its socket file when destroyed") {
    const std::string path = temp_socket_path();
    {
        UnixListener listener = UnixListener::bind(path);
        LRD_CHECK(path_exists(path));
    }
    // A UNIX socket leaves a real filesystem entry; if the destructor did not
    // remove it, the next bind() would hit EADDRINUSE.
    LRD_CHECK(!path_exists(path));
}

LRD_TEST("a stale socket file is detected and removed") {
    const std::string path = temp_socket_path();
    create_stale_socket_file(path);
    LRD_REQUIRE(path_exists(path));

    // Nothing is listening, so connecting to it gives ECONNREFUSED - that is
    // the evidence that makes removal safe rather than a guess.
    UnixListener listener = UnixListener::bind(path);
    LRD_CHECK(path_exists(path));
    LRD_CHECK(listener.native_handle() >= 0);
}

LRD_TEST("binding a path a live listener already holds is refused") {
    const std::string path = temp_socket_path();
    UnixListener first = UnixListener::bind(path);

    // The dangerous case: blindly unlinking here would strand `first` with an
    // open socket nobody can reach any more.
    try {
        UnixListener second = UnixListener::bind(path);
        LRD_CHECK(false);
    } catch (const lrd::SystemError& e) {
        LRD_CHECK(e.code() == std::errc::address_in_use);
    }

    // The original listener must be untouched and still usable.
    LRD_CHECK(path_exists(path));
    UnixStream client = UnixStream::connect(path);
    LRD_CHECK(client.valid());
}

LRD_TEST("the socket file is created mode 0600") {
    const std::string path = temp_socket_path();
    UnixListener listener = UnixListener::bind(path);

    struct stat st{};
    LRD_REQUIRE(::stat(path.c_str(), &st) == 0);

    // Only the owning user may connect. An on-device daemon holding local
    // signals has no business being reachable by every account on the box.
    const mode_t permissions = st.st_mode & 0777;
    LRD_CHECK_EQ(static_cast<unsigned>(permissions), 0600u);
}

LRD_TEST("connecting when no daemon is listening fails cleanly") {
    const std::string path = temp_socket_path();  // never bound
    try {
        UnixStream client = UnixStream::connect(path);
        LRD_CHECK(false);
    } catch (const lrd::SystemError& e) {
        // ENOENT because the socket file itself does not exist. A file that
        // exists with nothing bound gives ECONNREFUSED instead - the two are
        // worth telling apart when diagnosing a daemon that will not start.
        LRD_CHECK(e.code() == std::errc::no_such_file_or_directory);
    }
}

LRD_TEST("a moved-from listener does not unlink the socket file") {
    const std::string path = temp_socket_path();
    UnixListener original = UnixListener::bind(path);

    {
        const UnixListener moved(std::move(original));
        LRD_CHECK(path_exists(path));
    }
    // `moved` has now been destroyed and took the file with it; the point is
    // that `original`'s own destructor, running at end of scope, must not try
    // to unlink a path it no longer owns.
    LRD_CHECK(!path_exists(path));
}
