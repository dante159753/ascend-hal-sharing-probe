#include <acl/acl.h>
#include <ascend_hal.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

static constexpr int device_id = 0;

static void check(long result, const char* operation) {
    printf("[pid=%d]     %s ret=%ld [%s]\n", getpid(), operation, result, result ? "FAIL" : "OK");
    if (result) throw std::runtime_error(operation);
}

static bool test_io(void* buffer, size_t buffer_size, const std::string& directory) {
    const char* mode = "O_DIRECT";
    const unsigned char expected = 'A';
    const std::string path = directory + "/hal-io-" + std::to_string(getpid()) + "-direct.bin";
    const int flags = O_RDWR | O_CREAT | O_EXCL | O_SYNC | O_DIRECT;
    printf("[pid=%d] TEST %s write/read bytes=%zu pattern=%c\n", getpid(), mode, buffer_size, expected);
    printf("[pid=%d]   open(path=%s, flags=0x%x, mode=0600)\n", getpid(), path.c_str(), flags);
    int fd = open(path.c_str(), flags, 0600);
    if (fd < 0) {
        const int error = errno;
        printf("[pid=%d] FAIL %s open errno=%d (%s)\n", getpid(), mode, error, strerror(error));
        return false;
    }
    // Keep each syscall below Linux's single-transfer limit for buffers of 2 GiB or more.
    auto transfer = [&](bool write) {
        for (size_t offset = 0; offset < buffer_size;) {
            const size_t bytes = std::min(buffer_size - offset, size_t{1} << 30);
            void* address = static_cast<char*>(buffer) + offset;
            const char* operation = write ? "pwrite" : "pread";
            printf("[pid=%d]   %s(fd=%d, buffer=%p, bytes=%zu, offset=%zu)\n",
                   getpid(), operation, fd, address, bytes, offset);
            ssize_t transferred;
            do {
                transferred = write ? pwrite(fd, address, bytes, static_cast<off_t>(offset))
                                    : pread(fd, address, bytes, static_cast<off_t>(offset));
            } while (transferred < 0 && errno == EINTR);
            const int error = transferred < 0 ? errno : 0;
            printf("[pid=%d]     transferred=%zd expected=%zu errno=%d (%s)\n",
                   getpid(), transferred, bytes, error, error ? strerror(error) : "none");
            if (transferred != static_cast<ssize_t>(bytes)) return false;
            offset += bytes;
        }
        return true;
    };
    memset(buffer, expected, buffer_size);
    bool passed = transfer(true);
    int error = 0;
    if (!passed) {
        printf("[pid=%d]   SKIP read: full write did not complete\n", getpid());
    } else {
        memset(buffer, 0, buffer_size);
        passed = transfer(false);
        if (passed) {
            const auto* data = static_cast<const unsigned char*>(buffer);
            for (size_t i = 0; i < buffer_size; ++i) {
                if (data[i] != expected) {
                    printf("[pid=%d]     MISMATCH offset=%zu actual=%u expected=%u\n",
                           getpid(), i, static_cast<unsigned>(data[i]), static_cast<unsigned>(expected));
                    passed = false;
                    break;
                }
            }
        }
    }
    if (close(fd)) {
        error = errno;
        printf("[pid=%d]   FAIL close errno=%d (%s)\n", getpid(), error, strerror(error));
        passed = false;
    }
    if (unlink(path.c_str())) {
        error = errno;
        printf("[pid=%d]   FAIL unlink(%s) errno=%d (%s)\n", getpid(), path.c_str(), error, strerror(error));
        passed = false;
    }
    printf("[pid=%d] %s %s\n", getpid(), passed ? "PASS" : "FAIL", mode);
    return passed;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    size_t buffer_size = 2 * 1024 * 1024;
    std::string directory = ".";
    bool directory_set = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            puts("Usage: hal_host_io [existing_io_directory] [--size-mib N]\n"
                 "Default directory: current directory. Device: 0. Host DDR: normal pages.\n"
                 "N: positive integer MiB, default 2. Tests synchronous O_DIRECT pwrite/pread.\n"
                 "Transfers use chunks of at most 1 GiB. This is not Linux AIO.\n"
                 "Return 0: test passes; 1: any failure; 2: invalid arguments.");
            return 0;
        }
        if (strcmp(argv[i], "--size-mib") == 0 && i + 1 < argc) {
            const char* value = argv[++i];
            const char* end = value + strlen(value);
            size_t mib = 0;
            const auto parsed = std::from_chars(value, end, mib);
            constexpr size_t max_bytes = std::min(std::numeric_limits<size_t>::max(),
                                                 static_cast<size_t>(std::numeric_limits<off_t>::max()));
            if (parsed.ec != std::errc{} || parsed.ptr != end || mib == 0 || mib > max_bytes / (1024 * 1024)) {
                fprintf(stderr, "Invalid --size-mib: %s (expected a positive integer within the byte/offset range)\n", value);
                return 2;
            }
            buffer_size = mib * 1024 * 1024;
        } else if (argv[i][0] != '-' && !directory_set) {
            directory = argv[i];
            directory_set = true;
        } else {
            fprintf(stderr, "Invalid argument: %s; see --help\n", argv[i]);
            return 2;
        }
    }
    void* host = nullptr;
    drv_mem_handle_t* handle = nullptr;
    bool initialized = false, device_set = false, mapped = false;
    int result = 0;
    try {
        printf("[pid=%d] SETUP aclInit(nullptr) -> aclrtSetDevice(%d)\n", getpid(), device_id);
        check(aclInit(nullptr), "aclInit"); initialized = true;
        check(aclrtSetDevice(device_id), "aclrtSetDevice"); device_set = true;

        printf("[pid=%d]   halMemAddressReserve(size=%zu, alignment=0, addr=null, flags=0)\n", getpid(), buffer_size);
        check(halMemAddressReserve(&host, buffer_size, 0, nullptr, 0), "halMemAddressReserve");
        printf("[pid=%d]     host_va=%p\n", getpid(), host);
        drv_mem_prop prop{};
        prop.side = MEM_HOST_SIDE;
        prop.pg_type = MEM_NORMAL_PAGE_TYPE;
        prop.mem_type = MEM_DDR_TYPE;
        printf("[pid=%d]   halMemCreate(size=%zu, prop={side=MEM_HOST_SIDE, devid=0, module_id=0, "
               "pg_type=MEM_NORMAL_PAGE_TYPE, mem_type=MEM_DDR_TYPE, reserve=0}, flags=0)\n", getpid(), buffer_size);
        check(halMemCreate(&handle, buffer_size, &prop, 0), "halMemCreate");
        printf("[pid=%d]     handle=%p\n", getpid(), static_cast<void*>(handle));
        printf("[pid=%d]   halMemMap(va=%p, size=%zu, offset=0, handle=%p, flags=0)\n",
               getpid(), host, buffer_size, static_cast<void*>(handle));
        check(halMemMap(host, buffer_size, 0, handle, 0), "halMemMap"); mapped = true;
        printf("[pid=%d]     address_mod_4096=%zu\n", getpid(), static_cast<size_t>(reinterpret_cast<uintptr_t>(host) % 4096));

        result = test_io(host, buffer_size, directory) ? 0 : 1;
    } catch (const std::exception& error) {
        printf("[pid=%d] FAIL phase=%s\n", getpid(), error.what());
        result = 1;
    }
    auto cleanup = [&](long ret, const char* operation) {
        printf("[pid=%d]     %s ret=%ld\n", getpid(), operation, ret);
        if (ret) result = 1;
    };
    printf("[pid=%d] CLEANUP\n", getpid());
    if (mapped) {
        printf("[pid=%d]   halMemUnmap(va=%p)\n", getpid(), host);
        cleanup(halMemUnmap(host), "halMemUnmap");
    }
    if (handle) {
        printf("[pid=%d]   halMemRelease(handle=%p)\n", getpid(), static_cast<void*>(handle));
        cleanup(halMemRelease(handle), "halMemRelease");
    }
    if (host) {
        printf("[pid=%d]   halMemAddressFree(va=%p)\n", getpid(), host);
        cleanup(halMemAddressFree(host), "halMemAddressFree");
    }
    if (device_set) cleanup(aclrtResetDevice(device_id), "aclrtResetDevice");
    if (initialized) cleanup(aclFinalize(), "aclFinalize");
    printf("[pid=%d] RESULT %s\n", getpid(), result ? "FAIL" : "PASS");
    return result;
}
