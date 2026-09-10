#include "lrd/common/fd.hpp"

#include <unistd.h>

namespace lrd {

void Fd::close() noexcept {
    if (fd_ == kInvalid) {
        return;
    }

    // Deliberately NOT retried on EINTR.
    //
    // This is the one syscall where the usual "loop until it isn't EINTR"
    // reflex is wrong. On Linux, close() releases the descriptor before it can
    // return EINTR, so retrying closes a descriptor number that is already
    // free - and in a threaded program another thread may have been handed
    // that same number by open()/accept() in between. Retrying would then
    // close an unrelated file. So we close once and drop the error.
    //
    // (POSIX leaves the state unspecified after EINTR, and this is why
    // close_range/posix_close exist. Linux's behaviour is the one that matters
    // here, and it says: do not retry.)
    ::close(fd_);
    fd_ = kInvalid;
}

}  // namespace lrd
