#pragma once
#include "common.hpp"
#include <sys/socket.h>
#include <sys/wait.h>
#include <climits>
#include <vector>

struct Options {
    int device = 0;
    size_t bytes = 2 * 1024 * 1024;
    std::string io_dir = ".";
    std::string aio = "both";
};
inline Options options;

inline void send_word(int fd, uint64_t value) {
    auto* ptr = reinterpret_cast<char*>(&value);
    size_t sent = 0;
    while (sent < sizeof(value)) {
        ssize_t n = send(fd, ptr + sent, sizeof(value) - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("IPC send / peer closed");
        sent += n;
    }
}
inline uint64_t receive_word(int fd) {
    uint64_t value;
    auto* ptr = reinterpret_cast<char*>(&value);
    size_t received = 0;
    while (received < sizeof(value)) {
        ssize_t n = recv(fd, ptr + received, sizeof(value) - received, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("IPC receive / peer closed or timed out");
        received += n;
    }
    return value;
}
inline unsigned long long number(const std::string& text) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("expected a nonnegative integer");
    return std::stoull(text);
}
inline int launch(int argc, char** argv, int (*worker)(bool, int, size_t)) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    int worker_fd = -1;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string key = argv[i];
            if (key == "--help") {
                puts("Options: --device ACL_LOGICAL_ID --size-mib EVEN_MIB --io-dir DIR\n"
                     "         --aio both|buffered|direct|none (host_share only)\n"
                     "Default: device 0, size 2 MiB, current IO directory, both AIO modes.\n"
                     "Return: 0 all requested cases pass; 1 a case fails; 2 usage/launch error.");
                return 0;
            }
            if (++i == argc) throw std::runtime_error("missing option value");
            std::string value = argv[i];
            if (key == "--device" || key == "--worker-fd") {
                auto n = number(value);
                if (n > INT_MAX) throw std::runtime_error("integer out of range");
                if (key == "--device") options.device = static_cast<int>(n);
                else worker_fd = static_cast<int>(n);
            } else if (key == "--size-mib") {
                auto n = number(value);
                if (!n || n % 2 || n > SIZE_MAX / (1024 * 1024))
                    throw std::runtime_error("size must be a positive multiple of 2 MiB");
                options.bytes = n * 1024 * 1024;
            } else if (key == "--io-dir") options.io_dir = value;
            else if (key == "--aio") {
                if (value != "both" && value != "buffered" && value != "direct" && value != "none")
                    throw std::runtime_error("invalid --aio value");
                options.aio = value;
            } else throw std::runtime_error("unknown option: " + key);
        }
        if (worker_fd >= 0) return worker(true, worker_fd, options.bytes);
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) throw std::runtime_error("socketpair");
        timeval timeout{90, 0};
        for (int fd : sockets) {
            if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
                setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) {
                close(sockets[0]); close(sockets[1]); throw std::runtime_error("socket timeout setup");
            }
        }
        std::string fd_text = std::to_string(sockets[1]);
        std::vector<char*> args(argv, argv + argc);
        args.push_back(const_cast<char*>("--worker-fd"));
        args.push_back(fd_text.data()); args.push_back(nullptr);
        // Fork precedes runtime initialization; exec discards all inherited mappings.
        pid_t child = fork();
        if (child < 0) { close(sockets[0]); close(sockets[1]); throw std::runtime_error("fork"); }
        if (child == 0) {
            close(sockets[0]);
            execv("/proc/self/exe", args.data());
            perror("execv"); _exit(127);
        }
        close(sockets[1]);
        int result = worker(false, sockets[0], options.bytes);
        int status;
        pid_t waited;
        do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
        if (waited < 0) throw std::runtime_error("waitpid");
        printf("PROCESS_RESULT creator=%d importer_wait_status=%d\n", result, status);
        return result || status ? 1 : 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "LAUNCH_ERROR %s\n", error.what());
        return 2;
    }
}
