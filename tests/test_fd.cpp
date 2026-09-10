#include "lrd/common/fd.hpp"

#include "test_harness.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

namespace {

/// Opens a throwaway descriptor. /dev/null always exists and costs nothing.
int open_scratch_fd() {
    return ::open("/dev/null", O_RDONLY | O_CLOEXEC);
}

/// True if the kernel still knows about this descriptor number.
///
/// This is how we observe that a destructor really closed something: F_GETFD
/// on a closed descriptor fails with EBADF. Note the caveat this test suite
/// lives with - descriptor numbers are recycled, so "is_open" can only be
/// trusted immediately after the close, before anything else opens a file.
bool is_open(int fd) {
    return ::fcntl(fd, F_GETFD) != -1 || errno != EBADF;
}

}  // namespace

LRD_TEST("default-constructed Fd owns nothing") {
    const lrd::Fd fd;
    LRD_CHECK(!fd.valid());
    LRD_CHECK(!static_cast<bool>(fd));
    LRD_CHECK_EQ(fd.get(), lrd::Fd::kInvalid);
}

LRD_TEST("destructor closes the descriptor") {
    const int raw = open_scratch_fd();
    LRD_REQUIRE(raw >= 0);
    LRD_REQUIRE(is_open(raw));

    {
        const lrd::Fd fd(raw);
        LRD_CHECK(fd.valid());
        LRD_CHECK_EQ(fd.get(), raw);
    }

    LRD_CHECK(!is_open(raw));
}

LRD_TEST("move construction transfers ownership and disarms the source") {
    const int raw = open_scratch_fd();
    LRD_REQUIRE(raw >= 0);

    lrd::Fd source(raw);
    const lrd::Fd sink(std::move(source));

    LRD_CHECK_EQ(sink.get(), raw);
    // The moved-from object must not still believe it owns the descriptor,
    // otherwise its destructor double-closes.
    LRD_CHECK(!source.valid());  // NOLINT(bugprone-use-after-move) - the point of the test
    LRD_CHECK(is_open(raw));
}

LRD_TEST("move assignment closes the descriptor it was holding") {
    const int first = open_scratch_fd();
    const int second = open_scratch_fd();
    LRD_REQUIRE(first >= 0);
    LRD_REQUIRE(second >= 0);
    LRD_REQUIRE(first != second);

    lrd::Fd holder(first);
    lrd::Fd other(second);
    holder = std::move(other);

    // `first` must have been released when it was overwritten - a move
    // assignment that forgets this leaks one descriptor per assignment.
    LRD_CHECK(!is_open(first));
    LRD_CHECK_EQ(holder.get(), second);
    LRD_CHECK(is_open(second));
}

LRD_TEST("self-move-assignment is a no-op, not a close") {
    const int raw = open_scratch_fd();
    LRD_REQUIRE(raw >= 0);

    lrd::Fd fd(raw);
    // The guard in operator= exists for exactly this. Without it the object
    // closes its own descriptor and then assigns the invalid value back.
    lrd::Fd& alias = fd;
    fd = std::move(alias);

    LRD_CHECK(fd.valid());
    LRD_CHECK_EQ(fd.get(), raw);
    LRD_CHECK(is_open(raw));
}

LRD_TEST("release hands the descriptor out without closing it") {
    const int raw = open_scratch_fd();
    LRD_REQUIRE(raw >= 0);

    int escaped = lrd::Fd::kInvalid;
    {
        lrd::Fd fd(raw);
        escaped = fd.release();
        LRD_CHECK(!fd.valid());
    }

    LRD_CHECK_EQ(escaped, raw);
    LRD_CHECK(is_open(raw));  // still ours to close
    ::close(escaped);
}

LRD_TEST("reset closes the old descriptor and adopts the new one") {
    const int first = open_scratch_fd();
    const int second = open_scratch_fd();
    LRD_REQUIRE(first >= 0);
    LRD_REQUIRE(second >= 0);

    lrd::Fd fd(first);
    fd.reset(second);
    LRD_CHECK(!is_open(first));
    LRD_CHECK_EQ(fd.get(), second);

    fd.reset();
    LRD_CHECK(!fd.valid());
    LRD_CHECK(!is_open(second));
}

LRD_TEST("close is idempotent") {
    const int raw = open_scratch_fd();
    LRD_REQUIRE(raw >= 0);

    lrd::Fd fd(raw);
    fd.close();
    fd.close();  // must not close the descriptor number a second time
    LRD_CHECK(!fd.valid());
}
