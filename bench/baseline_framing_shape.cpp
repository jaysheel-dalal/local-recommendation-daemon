// Isolates one variable: does reading the length prefix as a separate syscall
// cost a measurable amount? Same payload both ways; the only difference is
// whether the reader issues one read() or two.
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static bool read_exact(int fd, void* p, size_t n) {
    auto* c = static_cast<char*>(p);
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, c + got, n - got);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

int main(int argc, char** argv) {
    const int n = argc > 1 ? std::atoi(argv[1]) : 5000;
    const bool split = argc > 2 && std::strcmp(argv[2], "split") == 0;
    constexpr size_t kBody = 36;

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 1;

    const pid_t pid = fork();
    if (pid == 0) {
        close(sv[0]);
        char buf[4 + kBody];
        for (;;) {
            if (split) {
                if (!read_exact(sv[1], buf, 4)) break;
                if (!read_exact(sv[1], buf + 4, kBody)) break;
            } else {
                if (!read_exact(sv[1], buf, 4 + kBody)) break;
            }
            if (write(sv[1], buf, sizeof(buf)) < 0) break;
        }
        _exit(0);
    }

    close(sv[1]);
    char buf[4 + kBody] = {};
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
        if (write(sv[0], buf, sizeof(buf)) < 0) break;
        if (split) {
            if (!read_exact(sv[0], buf, 4)) break;
            if (!read_exact(sv[0], buf + 4, kBody)) break;
        } else {
            if (!read_exact(sv[0], buf, 4 + kBody)) break;
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    close(sv[0]);
    waitpid(pid, nullptr, 0);

    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    std::printf("%-6s %d round trips: %.2f us each\n", split ? "split" : "single", n,
                static_cast<double>(us) / n);
    return 0;
}
