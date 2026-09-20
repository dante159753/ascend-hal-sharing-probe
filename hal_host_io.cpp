#include <acl/acl.h>
#include <ascend_hal.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

static constexpr size_t buffer_size = 2 * 1024 * 1024;
static constexpr int device_id = 0;

static void check(long result, const char* operation) {
    printf("[pid=%d]     %s ret=%ld [%s]\n", getpid(), operation, result, result ? "FAIL" : "OK");
    if (result) throw std::runtime_error(operation);
}

static bool test_io(void* buffer, const std::string& directory, bool direct) {
    const char* mode = direct ? "O_DIRECT" : "BUFFERED";
    const unsigned char expected = direct ? 'A' : 'B';
    const std::string path = directory + "/hal-io-" + std::to_string(getpid()) +
                             (direct ? "-direct.bin" : "-buffered.bin");
    const int flags = O_RDWR | O_CREAT | O_EXCL | O_SYNC | (direct ? O_DIRECT : 0);
    printf("[pid=%d] TEST %s write/read bytes=%zu pattern=%c\n", getpid(), mode, buffer_size, expected);
    printf("[pid=%d]   open(path=%s, flags=0x%x, mode=0600)\n", getpid(), path.c_str(), flags);
    int fd = open(path.c_str(), flags, 0600);
    if (fd < 0) {
        const int error = errno;
        printf("[pid=%d] FAIL %s open errno=%d (%s)\n", getpid(), mode, error, strerror(error));
        return false;
    }
    bool passed = true;
    memset(buffer, expected, buffer_size);
    printf("[pid=%d]   pwrite(fd=%d, src=%p, bytes=%zu, offset=0)\n", getpid(), fd, buffer, buffer_size);
    ssize_t written;
    do { written = pwrite(fd, buffer, buffer_size, 0); } while (written < 0 && errno == EINTR);
    int error = written < 0 ? errno : 0;
    printf("[pid=%d]     written=%zd errno=%d (%s)\n", getpid(), written, error, error ? strerror(error) : "none");
    if (written != static_cast<ssize_t>(buffer_size)) {
        passed = false;
        printf("[pid=%d]   SKIP read: full write did not complete\n", getpid());
    } else {
        memset(buffer, 0, buffer_size);
        printf("[pid=%d]   pread(fd=%d, dst=%p, bytes=%zu, offset=0)\n", getpid(), fd, buffer, buffer_size);
        ssize_t received;
        do { received = pread(fd, buffer, buffer_size, 0); } while (received < 0 && errno == EINTR);
        error = received < 0 ? errno : 0;
        printf("[pid=%d]     received=%zd errno=%d (%s)\n", getpid(), received, error, error ? strerror(error) : "none");
        passed = received == static_cast<ssize_t>(buffer_size);
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
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--help") == 0)) {
        puts("Usage: hal_host_io [existing_io_directory]\n"
             "Default directory: current directory. Device: 0. Host DDR: 2 MiB, normal pages.\n"
             "Tests synchronous O_DIRECT and buffered pwrite/pread; this is not Linux AIO.\n"
             "Return 0: both tests pass; 1: any failure; 2: invalid arguments.");
        return argc > 2 ? 2 : 0;
    }
    const std::string directory = argc == 2 ? argv[1] : ".";
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

        const bool direct_ok = test_io(host, directory, true);
        const bool buffered_ok = test_io(host, directory, false);
        result = direct_ok && buffered_ok ? 0 : 1;
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
