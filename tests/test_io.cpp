#include "lrd/common/fd.hpp"
#include "lrd/net/io.hpp"

#include "test_harness.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using lrd::net::IoStatus;

/// A connected pair of stream sockets, with the same semantics as a real
/// UNIX-domain connection - partial transfers included. socketpair() is the
/// right tool for testing transfer logic: no filesystem entry, no bind, no
/// accept, but identical read/write behaviour.
struct SocketPair {
    lrd::Fd a;
    lrd::Fd b;

    SocketPair() {
        int fds[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) {
            throw std::runtime_error("socketpair failed");
        }
        a.reset(fds[0]);
        b.reset(fds[1]);
    }
};

std::string make_payload(std::size_t size) {
    std::string s(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        // A repeating but non-uniform pattern, so a mis-ordered or duplicated
        // chunk shows up as a mismatch rather than blending in.
        s[i] = static_cast<char>('a' + (i % 26));
    }
    return s;
}

}  // namespace

LRD_TEST("write_all then read_exact round-trips a small payload") {
    SocketPair pair;
    const std::string payload = "the quick brown fox";

    const auto written = lrd::net::write_all(pair.a.get(), payload.data(), payload.size());
    LRD_REQUIRE(written.ok());
    LRD_CHECK_EQ(written.transferred, payload.size());

    std::string received(payload.size(), '\0');
    const auto read = lrd::net::read_exact(pair.b.get(), received.data(), received.size());
    LRD_REQUIRE(read.ok());
    LRD_CHECK_EQ(read.transferred, payload.size());
    LRD_CHECK_EQ(received, payload);
}

LRD_TEST("read_exact reassembles a payload larger than the socket buffer") {
    // 4 MiB is far beyond any default socket buffer, so the kernel is forced to
    // deliver it in many chunks. If read_exact did a single read() this test
    // would fail; if write_all did a single write() it would deadlock or
    // truncate. This is the regression test for the whole partial-transfer
    // premise.
    const std::string payload = make_payload(4u * 1024 * 1024);
    SocketPair pair;

    // The writer has to run concurrently: with both ends in one thread, the
    // socket buffer fills, write_all blocks, and nobody is left to drain it.
    // That deadlock is itself a useful thing to have understood.
    std::thread writer([&] { (void)lrd::net::write_all(pair.a.get(), payload.data(), payload.size()); });

    std::string received(payload.size(), '\0');
    const auto read = lrd::net::read_exact(pair.b.get(), received.data(), received.size());
    writer.join();

    LRD_REQUIRE(read.ok());
    LRD_CHECK_EQ(read.transferred, payload.size());
    LRD_CHECK(received == payload);
}

LRD_TEST("read_some returns whatever has arrived, not the full buffer") {
    SocketPair pair;
    const std::string payload = "short";

    const auto written = lrd::net::write_all(pair.a.get(), payload.data(), payload.size());
    LRD_REQUIRE(written.ok());

    std::vector<char> buffer(4096);
    const auto read = lrd::net::read_some(pair.b.get(), buffer.data(), buffer.size());
    LRD_REQUIRE(read.ok());
    // The distinguishing behaviour: a 4096-byte request satisfied by 5 bytes.
    LRD_CHECK_EQ(read.transferred, payload.size());
}

LRD_TEST("read_exact reports PeerClosed at a clean end of stream") {
    SocketPair pair;
    pair.a.close();  // peer hangs up having sent nothing

    std::vector<char> buffer(16);
    const auto read = lrd::net::read_exact(pair.b.get(), buffer.data(), buffer.size());

    LRD_CHECK(read.status == IoStatus::PeerClosed);
    // Zero bytes in means the connection ended *between* messages, which is a
    // normal disconnect rather than a truncated frame.
    LRD_CHECK_EQ(read.transferred, std::size_t{0});
}

LRD_TEST("read_exact reports how far it got when the peer dies mid-message") {
    SocketPair pair;
    const std::string partial = "1234";

    const auto written = lrd::net::write_all(pair.a.get(), partial.data(), partial.size());
    LRD_REQUIRE(written.ok());
    pair.a.close();

    // Ask for more than was ever sent.
    std::vector<char> buffer(16);
    const auto read = lrd::net::read_exact(pair.b.get(), buffer.data(), buffer.size());

    LRD_CHECK(read.status == IoStatus::PeerClosed);
    // transferred > 0 is what tells the caller this was a truncated message
    // and not a tidy disconnect - the two need different handling once frames
    // exist in step 2.
    LRD_CHECK_EQ(read.transferred, partial.size());
}

LRD_TEST("write_all reports PeerClosed instead of raising SIGPIPE") {
    SocketPair pair;
    pair.b.close();  // reader is gone

    // Without MSG_NOSIGNAL this call delivers SIGPIPE and the default
    // disposition terminates the test binary outright - the test failing to
    // even report a result would be the symptom. Enough data to guarantee the
    // send buffer cannot silently swallow it.
    const std::string payload = make_payload(1024 * 1024);
    const auto written = lrd::net::write_all(pair.a.get(), payload.data(), payload.size());

    LRD_CHECK(written.status == IoStatus::PeerClosed);
}

LRD_TEST("operations on a closed descriptor report an error, not a crash") {
    lrd::Fd fd;  // never opened
    std::vector<char> buffer(8);

    const auto read = lrd::net::read_exact(fd.get(), buffer.data(), buffer.size());
    LRD_CHECK(read.status == IoStatus::Error);
    LRD_CHECK(read.error != 0);

    const auto written = lrd::net::write_all(fd.get(), buffer.data(), buffer.size());
    LRD_CHECK(written.status == IoStatus::Error);
}

LRD_TEST("zero-length transfers succeed trivially") {
    SocketPair pair;
    const auto written = lrd::net::write_all(pair.a.get(), nullptr, 0);
    LRD_CHECK(written.ok());
    LRD_CHECK_EQ(written.transferred, std::size_t{0});

    const auto read = lrd::net::read_exact(pair.b.get(), nullptr, 0);
    LRD_CHECK(read.ok());
    LRD_CHECK_EQ(read.transferred, std::size_t{0});
}
