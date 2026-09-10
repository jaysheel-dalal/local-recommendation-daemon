// lrdd - Local Recommendation Daemon.
//
// Step 0 scaffold: argument handling and a build banner only. The socket
// server arrives in step 1; this exists so the build system has a real target
// to produce and so `lrdd --version` is a working smoke test from day one.

#include "lrd/common/version.hpp"

#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kDefaultSocketPath = "/tmp/lrd.sock";

struct Options {
    std::string_view socket_path = kDefaultSocketPath;
    bool show_help = false;
    bool show_version = false;
};

void print_usage(const char* argv0) {
    std::printf(
        "usage: %s [options]\n"
        "\n"
        "  --socket PATH   unix domain socket to listen on (default: %.*s)\n"
        "  --version       print version and exit\n"
        "  --help          print this message and exit\n",
        argv0, static_cast<int>(kDefaultSocketPath.size()), kDefaultSocketPath.data());
}

// Returns false on a malformed argument list. Deliberately hand-rolled rather
// than pulling in a CLI library: the option set is tiny and a dependency-free
// step 0 keeps the first commit trivially reproducible.
bool parse_args(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            out.show_help = true;
        } else if (arg == "--version" || arg == "-V") {
            out.show_version = true;
        } else if (arg == "--socket") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "lrdd: --socket requires a path\n");
                return false;
            }
            out.socket_path = argv[++i];
        } else {
            std::fprintf(stderr, "lrdd: unknown argument '%.*s'\n",
                         static_cast<int>(arg.size()), arg.data());
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options opts;
    if (!parse_args(argc, argv, opts)) {
        print_usage(argv[0]);
        return 2;
    }

    if (opts.show_help) {
        print_usage(argv[0]);
        return 0;
    }

    const std::string_view build = lrd::build_info();
    if (opts.show_version) {
        std::printf("%.*s\n", static_cast<int>(build.size()), build.data());
        return 0;
    }

    std::printf("%.*s\n", static_cast<int>(build.size()), build.data());
    std::printf("lrdd: socket path %.*s\n", static_cast<int>(opts.socket_path.size()),
                opts.socket_path.data());
    std::fprintf(stderr, "lrdd: server not implemented yet (arrives in step 1)\n");
    return 0;
}
