// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
// Standalone build/run instructions: cache2_hal_aio.md. No UCM or libaio dependency.
#include <acl/acl.h>
#include <algorithm>
#include <ascend_hal.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <linux/aio_abi.h>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <thread>
#include <unistd.h>
#include <vector>

using Clock = std::chrono::steady_clock;
static const Clock::time_point startTime = Clock::now();
static std::mutex logMutex;
static bool cleanupFailed = false;

static double Milliseconds(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

static void Log(const char* format, ...)
{
    std::lock_guard<std::mutex> lock(logMutex);
    std::printf("[pid=%d t=%.3fms] ", getpid(), Milliseconds(startTime));
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::putchar('\n');
    std::fflush(stdout);
}

template <typename F>
static long Call(const char* expression, F fn, bool osCall)
{
    Log("  CALL %s", expression);
    const Clock::time_point begin = Clock::now();
    errno = 0;
    const long ret = fn();
    const int error = osCall && ret < 0 ? errno : 0;
    Log("  RETURN %s ret=%ld errno=%d(%s) elapsed_ms=%.3f", expression, ret, error,
        error ? strerror(error) : "none", Milliseconds(begin));
    if ((osCall && ret < 0) || (!osCall && ret != 0)) {
        throw std::runtime_error(std::string(expression) + " ret=" + std::to_string(ret) +
                                 " errno=" + std::to_string(error));
    }
    return ret;
}

#define SDK(call) Call(#call, [&] { return (call); }, false)
#define OS(call) Call(#call, [&] { return (call); }, true)
#define CLEANUP(call)                             \
    do {                                          \
        try {                                     \
            call;                                 \
        } catch (const std::exception& error) {   \
            Log("CLEANUP FAIL %s", error.what()); \
            cleanupFailed = true;                 \
        }                                         \
    } while (false)

struct Options {
    std::string dir;
    int device = 0;
    size_t blocks = 8;
    size_t shards = 32;
    size_t slotBytes = 8ULL << 20;
    size_t depth = 32;
    size_t iterations = 2;
    size_t timeoutMs = 60000;
    std::string pages = "auto";
    bool direct = true;
    bool sync = false;
};

struct HostMemory {
    bool initialized = false;
    bool deviceSet = false;
    bool mapped = false;
    int device = 0;
    void* addr = nullptr;
    drv_mem_handle_t* handle = nullptr;

    void Release()
    {
        if (mapped) {
            SDK(halMemUnmap(addr));
            mapped = false;
        }
        if (handle) {
            SDK(halMemRelease(handle));
            handle = nullptr;
        }
        if (addr) {
            SDK(halMemAddressFree(addr));
            addr = nullptr;
        }
    }

    ~HostMemory()
    {
        CLEANUP(Release());
        if (deviceSet) { CLEANUP(SDK(aclrtResetDevice(device))); }
        if (initialized) { CLEANUP(SDK(aclFinalize())); }
    }

    void Allocate(size_t bytes, bool huge)
    {
        drv_mem_prop prop{};
        prop.side = MEM_HOST_SIDE;
        prop.devid = 0;
        prop.pg_type = huge ? MEM_HUGE_PAGE_TYPE : MEM_NORMAL_PAGE_TYPE;
        prop.mem_type = MEM_DDR_TYPE;
        size_t allocGranularity = 0;
        Log("HAL prop side=%u devid=%u pg_type=%u mem_type=%u flags=0 data_bytes=%zu",
            unsigned(prop.side), unsigned(prop.devid), unsigned(prop.pg_type),
            unsigned(prop.mem_type), bytes);
        SDK(halMemGetAllocationGranularity(&prop, MEM_ALLOC_GRANULARITY_RECOMMENDED,
                                           &allocGranularity));
        constexpr size_t vaAlignment = 1ULL << 30;
        if (!allocGranularity || allocGranularity > (1ULL << 40)) {
            throw std::runtime_error("invalid allocation granularity");
        }
        const size_t alignment = std::lcm(vaAlignment, allocGranularity);
        const size_t rankStride = (bytes + alignment - 1) / alignment * alignment;
        Log("HAL alloc_granularity=%zu rank_stride=%zu reserve_alignment=0 requested_addr=null",
            allocGranularity, rankStride);
        SDK(halMemAddressReserve(&addr, rankStride, 0, nullptr, 0));
        if (!addr) { throw std::runtime_error("null HAL reservation"); }
        Log("  reserved_addr=%p", addr);
        SDK(halMemCreate(&handle, rankStride, &prop, 0));
        Log("  handle=%p map_addr=%p size=%zu offset=0 flags=0", static_cast<void*>(handle), addr,
            rankStride);
        SDK(halMemMap(addr, rankStride, 0, handle, 0));
        mapped = true;
    }
};

struct Request {
    iocb cb{};
    int fd = -1;
    bool done = false;
};

// Requests, descriptors and HAL memory must outlive io_destroy, including on failure.
class AioPhase {
    aio_context_t ctx_ = 0;
    int eventFd_ = -1;
    int epollFd_ = -1;
    std::vector<Request> requests_;
    std::thread completion_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    std::atomic<size_t> completed_{0};

public:
    ~AioPhase()
    {
        stop_ = true;
        if (completion_.joinable()) { completion_.join(); }
        if (ctx_) { CLEANUP(OS(syscall(SYS_io_destroy, ctx_))); }
        for (Request& req : requests_) {
            if (req.fd >= 0) { CLEANUP(OS(close(req.fd))); }
        }
        if (epollFd_ >= 0) { CLEANUP(OS(close(epollFd_))); }
        if (eventFd_ >= 0) { CLEANUP(OS(close(eventFd_))); }
    }

    void Run(const Options& opt, const std::vector<std::string>& paths, void* buffer, bool dump)
    {
        const size_t count = opt.blocks * opt.shards;
        Log("PHASE %s requests=%zu slot_bytes=%zu depth=%zu timeout_ms=%zu", dump ? "DUMP" : "LOAD",
            count, opt.slotBytes, opt.depth, opt.timeoutMs);
        OS(syscall(SYS_io_setup, opt.depth, &ctx_));
        eventFd_ = OS(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
        epollFd_ = OS(epoll_create1(EPOLL_CLOEXEC));
        epoll_event notification{};
        notification.events = EPOLLIN;
        notification.data.fd = eventFd_;
        OS(epoll_ctl(epollFd_, EPOLL_CTL_ADD, eventFd_, &notification));
        requests_.resize(count);
        const int flags = (dump ? O_CREAT | O_WRONLY : O_RDONLY) | (opt.direct ? O_DIRECT : 0);
        for (size_t i = 0; i < count; ++i) {
            Request& req = requests_[i];
            Log("  open request=%zu block=%zu shard=%zu path=%s flags=0x%x mode=0644", i,
                i / opt.shards, i % opt.shards, paths[i / opt.shards].c_str(), flags);
            req.fd = OS(open(paths[i / opt.shards].c_str(), flags, 0644));
            req.cb.aio_fildes = req.fd;
            req.cb.aio_lio_opcode = dump ? IOCB_CMD_PWRITE : IOCB_CMD_PREAD;
            req.cb.aio_buf = reinterpret_cast<uintptr_t>(buffer) + i * opt.slotBytes;
            req.cb.aio_nbytes = opt.slotBytes;
            req.cb.aio_offset = (i % opt.shards) * opt.slotBytes;
            req.cb.aio_data = i;
            req.cb.aio_flags = IOCB_FLAG_RESFD;
            req.cb.aio_resfd = eventFd_;
        }
        completion_ = std::thread([this, &opt] {
            while (!stop_) {
                epoll_event notification{};
                const int ready = epoll_wait(epollFd_, &notification, 1, 10);
                if (ready < 0) {
                    if (errno == EINTR) { continue; }
                    Log("FAIL epoll_wait errno=%d(%s)", errno, strerror(errno));
                    failed_ = true;
                    return;
                }
                if (ready == 0) { continue; }
                uint64_t notifications = 0;
                const ssize_t bytes = read(eventFd_, &notifications, sizeof(notifications));
                if (bytes < 0 && (errno == EAGAIN || errno == EINTR)) { continue; }
                if (bytes != sizeof(notifications)) {
                    Log("FAIL eventfd read bytes=%zd errno=%d", bytes, errno);
                    failed_ = true;
                    return;
                }
                Log("  eventfd notifications=%llu", static_cast<unsigned long long>(notifications));
                for (;;) {
                    io_event events[128]{};
                    timespec zero{};
                    const long n = syscall(SYS_io_getevents, ctx_, 1, 128, events, &zero);
                    if (n < 0 && errno == EINTR) { continue; }
                    if (n < 0) {
                        Log("FAIL io_getevents ret=%ld errno=%d(%s)", n, errno, strerror(errno));
                        failed_ = true;
                        return;
                    }
                    for (long j = 0; j < n; ++j) {
                        const io_event& ev = events[j];
                        const size_t id = ev.data;
                        if (id >= requests_.size() || requests_[id].done ||
                            ev.obj != reinterpret_cast<uintptr_t>(&requests_[id].cb)) {
                            Log("FAIL invalid completion data=%llu obj=0x%llx",
                                static_cast<unsigned long long>(ev.data),
                                static_cast<unsigned long long>(ev.obj));
                            failed_ = true;
                            return;
                        }
                        Request& req = requests_[id];
                        const int error = ev.res < 0 ? static_cast<int>(-ev.res) : 0;
                        Log("  complete request=%zu block=%zu shard=%zu fd=%d offset=%lld "
                            "res=%lld expected=%zu res2=%lld error=%d(%s)",
                            id, id / opt.shards, id % opt.shards, req.fd,
                            static_cast<long long>(req.cb.aio_offset),
                            static_cast<long long>(ev.res), opt.slotBytes,
                            static_cast<long long>(ev.res2), error,
                            error ? strerror(error) : "none");
                        if (ev.res != static_cast<int64_t>(opt.slotBytes) || ev.res2 != 0) {
                            failed_ = true;
                        }
                        req.done = true;
                        completed_.fetch_add(1);
                    }
                    if (n < 128) { break; }
                }
            }
        });

        const Clock::time_point begin = Clock::now();
        const Clock::time_point deadline = begin + std::chrono::milliseconds(opt.timeoutMs);
        size_t submitted = 0;
        const auto check = [&] {
            if (failed_) {
                throw std::runtime_error("AIO completion failed (see res/errno above)");
            }
            if (Clock::now() >= deadline) {
                Log("FAIL AIO timeout submitted=%zu completed=%zu; io_destroy may wait for the FS",
                    submitted, completed_.load());
                throw std::runtime_error("AIO phase timed out");
            }
        };
        while (submitted < count) {
            check();
            if (submitted - completed_.load() >= opt.depth) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                continue;
            }
            iocb* cb = &requests_[submitted].cb;
            Log("  io_submit request=%zu ctx=%llu nr=1 fd=%u opcode=%u buf=0x%llx bytes=%llu "
                "offset=%lld flags=%u resfd=%u",
                submitted, static_cast<unsigned long long>(ctx_), cb->aio_fildes,
                cb->aio_lio_opcode, static_cast<unsigned long long>(cb->aio_buf),
                static_cast<unsigned long long>(cb->aio_nbytes),
                static_cast<long long>(cb->aio_offset), cb->aio_flags, cb->aio_resfd);
            const Clock::time_point submitStart = Clock::now();
            const long ret = syscall(SYS_io_submit, ctx_, 1, &cb);
            const int error = ret < 0 ? errno : 0;
            Log("  io_submit request=%zu ret=%ld errno=%d(%s) elapsed_ms=%.3f", submitted, ret,
                error, error ? strerror(error) : "none", Milliseconds(submitStart));
            if (ret == 1) {
                ++submitted;
            } else if (error == EAGAIN || error == EINTR || ret == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            } else {
                throw std::runtime_error("io_submit failed");
            }
        }
        while (completed_ < count) {
            check();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check();
        stop_ = true;
        completion_.join();
        Log("PHASE %s PASS bytes=%zu elapsed_ms=%.3f", dump ? "DUMP" : "LOAD",
            count * opt.slotBytes, Milliseconds(begin));
        for (Request& req : requests_) {
            const int fd = req.fd;
            req.fd = -1;
            OS(close(fd));
        }
    }
};

static uint8_t Pattern(size_t offset, size_t iteration)
{
    uint64_t value = (offset >> 3) + iteration * 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return static_cast<uint8_t>(value >> ((offset & 7) * 8));
}

int main(int argc, char** argv)
{
    try {
        Options opt;
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (key == "--help") {
                std::puts(
                    "Usage: cache2_hal_aio --dir EXISTING_DIR [--device 0] [--blocks 8] "
                    "[--shards 32] [--slot-bytes 8388608] [--depth 32] [--iterations 2] "
                    "[--timeout-ms 60000] [--pages auto|normal|huge] [--direct 0|1] [--sync 0|1]\n"
                    "Default: 2 GiB HAL buffer, Linux AIO with O_DIRECT, files retained. "
                    "See cache2_hal_aio.md for build instructions.");
                return 0;
            }
            if (++i >= argc) { throw std::runtime_error("missing value for " + key); }
            const std::string value = argv[i];
            if (key == "--dir") {
                opt.dir = value;
                continue;
            }
            if (key == "--pages") {
                opt.pages = value;
                continue;
            }
            if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) {
                throw std::runtime_error("expected unsigned decimal for " + key);
            }
            const unsigned long long number = std::stoull(value);
            if (number > (1ULL << 40)) { throw std::runtime_error("argument too large"); }
            if (key == "--device" && number <= INT32_MAX) {
                opt.device = number;
            } else if (key == "--blocks") {
                opt.blocks = number;
            } else if (key == "--shards") {
                opt.shards = number;
            } else if (key == "--slot-bytes") {
                opt.slotBytes = number;
            } else if (key == "--depth") {
                opt.depth = number;
            } else if (key == "--iterations") {
                opt.iterations = number;
            } else if (key == "--timeout-ms") {
                opt.timeoutMs = number;
            } else if (key == "--direct" && number <= 1) {
                opt.direct = number;
            } else if (key == "--sync" && number <= 1) {
                opt.sync = number;
            } else {
                throw std::runtime_error("unknown option or invalid value: " + key);
            }
        }
        constexpr size_t maxBytes = 1ULL << 40;
        if (opt.dir.empty() || !opt.blocks || !opt.shards || !opt.slotBytes || !opt.depth ||
            !opt.iterations || !opt.timeoutMs || opt.depth > INT32_MAX ||
            opt.blocks > maxBytes / opt.shards ||
            opt.blocks * opt.shards > maxBytes / opt.slotBytes ||
            (opt.pages != "auto" && opt.pages != "normal" && opt.pages != "huge")) {
            throw std::runtime_error("invalid options; use --help");
        }
        const size_t bytes = opt.blocks * opt.shards * opt.slotBytes;
        std::string runTemplate = opt.dir + "/cache2-hal-aio-XXXXXX";
        if (!mkdtemp(runTemplate.data())) {
            throw std::runtime_error("mkdtemp: " + std::string(strerror(errno)));
        }
        Log("RUN dir=%s device=%d bytes=%zu blocks=%zu shards=%zu slot_bytes=%zu direct=%d sync=%d",
            runTemplate.c_str(), opt.device, bytes, opt.blocks, opt.shards, opt.slotBytes,
            opt.direct, opt.sync);
        struct statfs fs{};
        OS(statfs(runTemplate.c_str(), &fs));
        Log("FILESYSTEM type=0x%lx block_size=%ld", static_cast<unsigned long>(fs.f_type),
            static_cast<long>(fs.f_bsize));
        {
            HostMemory memory;
            memory.device = opt.device;
            SDK(aclInit(nullptr));
            memory.initialized = true;
            SDK(aclrtSetDevice(opt.device));
            memory.deviceSet = true;
            try {
                memory.Allocate(bytes, opt.pages != "normal");
            } catch (const std::exception& error) {
                if (opt.pages != "auto") { throw; }
                Log("HAL huge allocation failed: %s; retry normal pages", error.what());
                memory.Release();
                memory.Allocate(bytes, false);
            }
            uint8_t* buffer = static_cast<uint8_t*>(memory.addr);
            for (size_t iteration = 0; iteration < opt.iterations; ++iteration) {
                Log("ITERATION %zu CPU fill bytes=%zu addr=%p", iteration, bytes, memory.addr);
                for (size_t i = 0; i < bytes; ++i) { buffer[i] = Pattern(i, iteration); }
                std::vector<std::string> temporary;
                std::vector<std::string> archived;
                for (size_t block = 0; block < opt.blocks; ++block) {
                    archived.push_back(runTemplate + "/block-" + std::to_string(block));
                    temporary.push_back(archived.back() + ".tmp");
                }
                {
                    AioPhase dump;
                    dump.Run(opt, temporary, buffer, true);
                }
                for (size_t block = 0; block < opt.blocks; ++block) {
                    if (opt.sync) {
                        Log("SYNC path=%s", temporary[block].c_str());
                        const int fd = OS(open(temporary[block].c_str(), O_WRONLY));
                        try {
                            OS(fdatasync(fd));
                        } catch (...) {
                            CLEANUP(OS(close(fd)));
                            throw;
                        }
                        OS(close(fd));
                    }
                    Log("COMMIT rename from=%s to=%s", temporary[block].c_str(),
                        archived[block].c_str());
                    OS(rename(temporary[block].c_str(), archived[block].c_str()));
                    struct stat st{};
                    OS(stat(archived[block].c_str(), &st));
                    Log("  file_bytes=%lld expected=%zu", static_cast<long long>(st.st_size),
                        opt.shards * opt.slotBytes);
                    if (st.st_size != static_cast<int64_t>(opt.shards * opt.slotBytes)) {
                        throw std::runtime_error("unexpected file size");
                    }
                }
                Log("CPU poison buffer before LOAD bytes=%zu", bytes);
                for (size_t i = 0; i < bytes; ++i) { buffer[i] = Pattern(i, iteration) ^ 0xff; }
                {
                    AioPhase load;
                    load.Run(opt, archived, buffer, false);
                }
                size_t mismatches = 0;
                for (size_t i = 0; i < bytes; ++i) {
                    const uint8_t expected = Pattern(i, iteration);
                    if (buffer[i] == expected) { continue; }
                    if (mismatches++ < 16) {
                        Log("MISMATCH iteration=%zu block=%zu shard=%zu slot_offset=%zu "
                            "buffer_offset=%zu actual=0x%02x expected=0x%02x",
                            iteration, i / opt.slotBytes / opt.shards,
                            (i / opt.slotBytes) % opt.shards, i % opt.slotBytes, i,
                            unsigned(buffer[i]), unsigned(expected));
                    }
                }
                if (mismatches) {
                    throw std::runtime_error("data mismatches=" + std::to_string(mismatches));
                }
                Log("VERIFY PASS iteration=%zu bytes=%zu", iteration, bytes);
            }
        }
        if (cleanupFailed) { throw std::runtime_error("resource cleanup failed"); }
        Log("RESULT PASS; retained files: %s", runTemplate.c_str());
        return 0;
    } catch (const std::exception& error) {
        Log("RESULT FAIL: %s", error.what());
        return 1;
    }
}
