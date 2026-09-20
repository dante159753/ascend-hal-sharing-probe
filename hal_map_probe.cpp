#include <ascend_hal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

static void check(int ret, const char* name) {
    printf("CALL %s ret=%d\n", name, ret);
    if (ret) throw std::runtime_error(name);
}
static void send_word(int fd, uint64_t value) {
    ssize_t n;
    do { n = send(fd, &value, sizeof(value), MSG_NOSIGNAL); } while (n < 0 && errno == EINTR);
    if (n != sizeof(value)) throw std::runtime_error("IPC send");
}
static uint64_t receive_word(int fd) {
    uint64_t value;
    ssize_t n;
    do { n = recv(fd, &value, sizeof(value), MSG_WAITALL); } while (n < 0 && errno == EINTR);
    if (n != sizeof(value)) throw std::runtime_error("IPC receive / peer closed");
    return value;
}
static void write_pattern(void* ptr, size_t bytes, uint64_t seed) {
    auto* p = static_cast<uint64_t*>(ptr);
    for (size_t i = 0; i < bytes / 8; ++i) p[i] = seed ^ i;
}
static void verify(void* ptr, size_t bytes, uint64_t seed) {
    auto* p = static_cast<uint64_t*>(ptr);
    for (size_t i = 0; i < bytes / 8; ++i)
        if (p[i] != (seed ^ i)) throw std::runtime_error("CPU shared data mismatch");
    printf("PASS CPU_SHARED_PATTERN seed=%llu bytes=%zu\n", (unsigned long long)seed, bytes);
}
static int worker(bool importer, bool host_mode, uint32_t device, size_t bytes, int socket) {
    bool opened = false, mapped = false;
    void* va = nullptr;
    drv_mem_handle_t* handle = nullptr;
    int result = 0;
    try {
        halSetRuntimeApiVer(__HAL_API_VERSION);
        printf("CALL halSetRuntimeApiVer version=%d\n", __HAL_API_VERSION);
        halDevOpenIn input{};
        halDevOpenOut output{};
        check(halDeviceOpen(device, &input, &output), "halDeviceOpen"); opened = true;
        printf("ROLE %s hal_device=%u bytes=%zu\n", importer ? "importer" : "creator", device, bytes);
        uint64_t token = importer ? receive_word(socket) : 0;
        check(halMemAddressReserve(&va, bytes, 0, nullptr, 0), "halMemAddressReserve");
        if (importer) {
            uint32_t target = device;
            if (host_mode) check(halGetHostID(&target), "halGetHostID");
            printf("IMPORT side=%s target=%u\n", host_mode ? "HOST" : "DEVICE", target);
            check(halMemImportFromShareableHandle(token, target, &handle), "halMemImportFromShareableHandle");
        } else {
            drv_mem_prop prop{};
            prop.side = MEM_HOST_SIDE; prop.pg_type = MEM_HUGE_PAGE_TYPE; prop.mem_type = MEM_DDR_TYPE;
            check(halMemCreate(&handle, bytes, &prop, 0), "halMemCreate HOST DDR");
        }
        check(halMemMap(va, bytes, 0, handle, 0), "halMemMap"); mapped = true;
        printf("PASS HAL_MAPPING role=%s va=%p\n", importer ? "importer" : "creator", va);
        if (!importer) {
            write_pattern(va, bytes, 101);
            check(halMemExportToShareableHandle(handle, MEM_HANDLE_TYPE_NONE, 0, &token), "export handle");
            ShareHandleAttr attr{}; attr.enableFlag = SHR_HANDLE_NO_WLIST_ENABLE;
            check(halMemShareHandleSetAttribute(token, SHR_HANDLE_ATTR_NO_WLIST_IN_SERVER, attr), "same-server sharing");
            send_word(socket, token);
            check(static_cast<int>(receive_word(socket)), "importer mapped");
            if (host_mode) verify(va, bytes, 202);
            send_word(socket, 0);
            check(static_cast<int>(receive_word(socket)), "importer released mapping");
        } else {
            if (host_mode) { verify(va, bytes, 101); write_pattern(va, bytes, 202); }
            send_word(socket, 0);
            check(static_cast<int>(receive_word(socket)), "creator acknowledged");
            check(halMemUnmap(va), "importer unmap"); mapped = false;
            check(halMemRelease(handle), "importer release"); handle = nullptr;
            check(halMemAddressFree(va), "importer free VA"); va = nullptr;
            send_word(socket, 0);
        }
    } catch (const std::exception& error) {
        printf("RESULT FAIL role=%s phase=%s\n", importer ? "importer" : "creator", error.what()); result = 1;
    }
    close(socket);
    auto cleanup = [&](int ret, const char* name) {
        printf("CLEAN %s ret=%d\n", name, ret); if (ret) result = 1;
    };
    if (mapped) cleanup(halMemUnmap(va), "halMemUnmap");
    if (handle) cleanup(halMemRelease(handle), "halMemRelease");
    if (va) cleanup(halMemAddressFree(va), "halMemAddressFree");
    if (opened) { halDevCloseIn input{}; cleanup(halDeviceClose(device, &input), "halDeviceClose"); }
    printf("RESULT %s role=%s scope=HAL_MAPPING_ONLY\n", result ? "FAIL" : "PASS", importer ? "importer" : "creator");
    return result;
}
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            puts("Usage: hal_map_probe host|device HAL_DEVICE_ID SIZE_MIB\nPure HAL initialization and mapping diagnostic; no ACL or D2D execution."); return 0;
        }
        if (argc != 4 && argc != 5) throw std::runtime_error("use --help for arguments");
        std::string mode = argv[1];
        if (mode != "host" && mode != "device") throw std::runtime_error("mode must be host or device");
        auto numeric = [](const std::string& s) {
            if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) throw std::runtime_error("invalid integer");
            return std::stoull(s);
        };
        auto dev = numeric(argv[2]), mib = numeric(argv[3]);
        if (dev > UINT32_MAX || !mib || mib % 2 || mib > SIZE_MAX / (1024 * 1024)) throw std::runtime_error("invalid device or size");
        size_t bytes = mib * 1024 * 1024;
        if (argc == 5) return worker(true, mode == "host", dev, bytes, std::stoi(argv[4]));
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) throw std::runtime_error("socketpair");
        timeval timeout{90, 0};
        for (int fd : sockets) {
            if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
                setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) {
                close(sockets[0]); close(sockets[1]); throw std::runtime_error("socket timeout setup");
            }
        }
        std::string fd = std::to_string(sockets[1]);
        pid_t child = fork();
        if (child < 0) { close(sockets[0]); close(sockets[1]); throw std::runtime_error("fork"); }
        if (child == 0) {
            close(sockets[0]);
            execl("/proc/self/exe", argv[0], argv[1], argv[2], argv[3], fd.c_str(), nullptr);
            _exit(127);
        }
        close(sockets[1]);
        int result = worker(false, mode == "host", dev, bytes, sockets[0]), status;
        pid_t waited;
        do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
        if (waited < 0) throw std::runtime_error("waitpid");
        printf("PROCESS_RESULT creator=%d importer_wait_status=%d\n", result, status);
        return result || status ? 1 : 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "LAUNCH_ERROR %s\n", error.what()); return 2;
    }
}
