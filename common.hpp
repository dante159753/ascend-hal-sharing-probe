#pragma once
#include <acl/acl.h>
#include <ascend_hal.h>
#include <linux/aio_abi.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

inline const char* log_role = nullptr;
inline void __attribute__((format(printf, 2, 3))) log_line(int indent, const char* format, ...) {
    char line[2048];
    int prefix = snprintf(line, sizeof(line), "[pid=%d %s] %*s", getpid(), log_role ? log_role : "launcher", indent * 2, "");
    va_list args;
    va_start(args, format);
    int count = vsnprintf(line + prefix, sizeof(line) - prefix, format, args);
    va_end(args);
    if (count < 0) return;
    size_t length = static_cast<size_t>(prefix) + static_cast<size_t>(count);
    if (length >= sizeof(line) - 1) length = sizeof(line) - 2;
    line[length++] = '\n';
    // Keep each prefixed line in one write when stdout is a shared pipe.
    size_t sent = 0;
    while (sent < length) {
        ssize_t n = write(STDOUT_FILENO, line + sent, length - sent);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
}
inline bool cleanup_failed = false;
inline void cleanup(long ret, const char* name) {
    if (log_role) log_line(2, "RETURN %s ret=%ld [%s]", name, ret, ret ? "FAIL" : "OK");
    else std::printf("CLEAN %s ret=%ld\n", name, ret);
    if (ret) cleanup_failed = true;
}
inline void check(long ret, const char* name) {
    if (log_role) log_line(2, "RETURN %s ret=%ld [%s]", name, ret, ret ? "FAIL" : "OK");
    else std::printf("CALL %s ret=%ld\n", name, ret);
    if (ret != 0) throw std::runtime_error(name);
}
inline uint64_t pattern(size_t i, uint64_t seed) {
    return (i * 0x9e3779b97f4a7c15ULL) ^ seed;
}
inline void fill(void* ptr, size_t bytes, uint64_t seed) {
    if (log_role) log_line(1, "CPU fill(ptr=%p, bytes=%zu, seed=%llu)", ptr, bytes, (unsigned long long)seed);
    auto p = static_cast<uint64_t*>(ptr);
    for (size_t i = 0; i < bytes / 8; ++i) p[i] = pattern(i, seed);
}
inline void verify(const void* ptr, size_t bytes, uint64_t seed, const char* phase) {
    if (log_role) log_line(1, "VERIFY %s ptr=%p bytes=%zu seed=%llu (all uint64 elements)", phase, ptr, bytes, (unsigned long long)seed);
    auto p = static_cast<const uint64_t*>(ptr);
    for (size_t i = 0; i < bytes / 8; ++i) {
        if (p[i] != pattern(i, seed)) {
            if (log_role) log_line(2, "MISMATCH phase=%s offset=%zu actual=%llx expected=%llx", phase, i*8,
                        (unsigned long long)p[i], (unsigned long long)pattern(i, seed));
            else std::printf("MISMATCH phase=%s offset=%zu actual=%llx expected=%llx\n", phase, i*8,
                        (unsigned long long)p[i], (unsigned long long)pattern(i, seed));
            throw std::runtime_error(phase);
        }
    }
    if (log_role) log_line(0, "PASS %s bytes=%zu", phase, bytes);
    else std::printf("PASS %s bytes=%zu\n", phase, bytes);
}
struct Allocation {
    void* va = nullptr;
    drv_mem_handle_t* handle = nullptr;
    bool mapped = false;
    ~Allocation() {
        if (mapped) {
            if (log_role) log_line(1, "CLEANUP halMemUnmap(va=%p)", va);
            cleanup(halMemUnmap(va), "halMemUnmap");
        }
        if (handle) {
            if (log_role) log_line(1, "CLEANUP halMemRelease(handle=%p)", static_cast<void*>(handle));
            cleanup(halMemRelease(handle), "halMemRelease");
        }
        if (va) {
            if (log_role) log_line(1, "CLEANUP halMemAddressFree(va=%p)", va);
            cleanup(halMemAddressFree(va), "halMemAddressFree");
        }
    }
};
struct AclResources {
    void* device = nullptr;
    aclrtStream stream = nullptr;
    aclrtEvent event = nullptr;
    ~AclResources() {
        if (stream) cleanup(aclrtSynchronizeStream(stream), "aclrtSynchronizeStream");
        if (event) cleanup(aclrtDestroyEvent(event), "aclrtDestroyEvent");
        if (stream) cleanup(aclrtDestroyStream(stream), "aclrtDestroyStream");
        if (device) cleanup(aclrtFree(device), "aclrtFree");
    }
};
inline double ms(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b-a).count();
}
inline void aio_operation(aio_context_t& ctx, int fd, void* ptr, size_t bytes, bool write) {
    constexpr int n = 4;
    iocb blocks[n]{};
    iocb* requests[n];
    size_t chunk = bytes/n;
    for (int i=0; i<n; ++i) {
        blocks[i].aio_data = i;
        blocks[i].aio_lio_opcode = write ? IOCB_CMD_PWRITE : IOCB_CMD_PREAD;
        blocks[i].aio_fildes = fd;
        blocks[i].aio_buf = reinterpret_cast<uint64_t>(static_cast<char*>(ptr)+i*chunk);
        blocks[i].aio_nbytes = chunk;
        blocks[i].aio_offset = i*chunk;
        requests[i] = &blocks[i];
    }
    errno=0;
    long submitted = syscall(SYS_io_submit, ctx, n, requests);
    if (log_role) log_line(1, "AIO %s ptr=%p bytes=%zu submitted=%ld expected=%d errno=%d", write?"write":"read", ptr, bytes, submitted,n,submitted<0?errno:0);
    else std::printf("AIO %s submitted=%ld expected=%d errno=%d\n", write?"write":"read", submitted,n,submitted<0?errno:0);
    if (submitted != n) {
        // Quiesce any accepted requests before the caller reuses its buffer.
        cleanup(syscall(SYS_io_destroy, ctx), "io_destroy incomplete submit"); ctx = 0;
        throw std::runtime_error("io_submit");
    }
    io_event events[n]{};
    timespec timeout{10,0};
    long completed = syscall(SYS_io_getevents,ctx,n,n,events,&timeout);
    if (log_role) log_line(2, "AIO %s completed=%ld", write?"write":"read",completed);
    else std::printf("AIO %s completed=%ld\n", write?"write":"read",completed);
    if (completed != n) {
        cleanup(syscall(SYS_io_destroy, ctx), "io_destroy incomplete completion"); ctx = 0;
        throw std::runtime_error("io_getevents");
    }
    unsigned seen=0;
    bool failed=false;
    for (int i=0;i<n;++i) {
        if (log_role) log_line(2, "AIO event index=%llu res=%lld res2=%lld",
                    (unsigned long long)events[i].data,(long long)events[i].res,(long long)events[i].res2);
        else std::printf("AIO event index=%llu res=%lld res2=%lld\n",
                    (unsigned long long)events[i].data,(long long)events[i].res,(long long)events[i].res2);
        if (events[i].res < 0) {
            if (log_role) log_line(2, "AIO error=%s", strerror(-events[i].res));
            else std::printf("AIO error=%s\n", strerror(-events[i].res));
        }
        if(events[i].data>=n || (seen&(1U<<events[i].data)) || events[i].res!=(long long)chunk || events[i].res2)
            failed=true;
        if(events[i].data<n) seen |= 1U<<events[i].data;
    }
    if(failed) throw std::runtime_error("aio completion");
}
