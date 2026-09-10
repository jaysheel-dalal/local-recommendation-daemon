// Baseline: what does a bare request/response round trip cost between two
// processes on this machine? No framing, no codec, no allocation - just
// write/read of a fixed 20-byte buffer. Anything lrd costs above this is ours.
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    const int n = argc > 1 ? std::atoi(argv[1]) : 5000;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 1;

    const pid_t pid = fork();
    if (pid == 0) {
        close(sv[0]);
        char buf[20];
        for (;;) {
            ssize_t r = read(sv[1], buf, sizeof(buf));
            if (r <= 0) break;
            if (write(sv[1], buf, static_cast<size_t>(r)) < 0) break;
        }
        _exit(0);
    }

    close(sv[1]);
    char buf[20] = {"ping"};
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
        if (write(sv[0], buf, sizeof(buf)) < 0) break;
        if (read(sv[0], buf, sizeof(buf)) <= 0) break;
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    close(sv[0]);
    waitpid(pid, nullptr, 0);

    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    std::printf("%d round trips in %lld us = %.2f us each\n", n,
                static_cast<long long>(us), static_cast<double>(us) / n);
    return 0;
}
