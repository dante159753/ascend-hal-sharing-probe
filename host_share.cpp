#include "cli.hpp"
#include <sys/socket.h>
#include <sys/wait.h>
#include <signal.h>
#include <fstream>

static void inspect_mapping(void* ptr) {
    DVattribute attr{};
    int ret = drvMemGetAttribute((DVdeviceptr)ptr, &attr);
    printf("ATTR pid=%d va=%p ret=%d memType=%u devId=%u pageSize=%u\n",
           getpid(), ptr, ret, attr.memType, attr.devId, attr.pageSize);
    std::ifstream file("/proc/self/smaps");
    std::string line;
    bool active = false;
    while (std::getline(file, line)) {
        unsigned long long low, high;
        if (sscanf(line.c_str(), "%llx-%llx", &low, &high) == 2) {
            active = (uintptr_t)ptr >= low && (uintptr_t)ptr < high;
            if (active) printf("SMAPS %s\n", line.c_str());
        } else if (active && line.rfind("VmFlags:", 0) == 0) {
            printf("SMAPS %s\n", line.c_str());
        }
    }
}
static int io_tests(void* ptr, size_t bytes, uint64_t seed, const char* role) {
    int failures = 0;
    AclResources acl;
    try {
        check(aclrtMalloc(&acl.device, bytes, ACL_MEM_MALLOC_HUGE_FIRST), "aclrtMalloc");
        check(aclrtCreateStream(&acl.stream), "aclrtCreateStream");
        check(aclrtCreateEvent(&acl.event), "aclrtCreateEvent");
        fill(ptr, bytes, seed);
        check(aclrtMemcpyAsync(acl.device, bytes, ptr, bytes, ACL_MEMCPY_HOST_TO_DEVICE, acl.stream), "ACL H2D");
        check(aclrtSynchronizeStream(acl.stream), "sync H2D");
        memset(ptr, 0, bytes);
        check(aclrtMemcpyAsync(ptr, bytes, acl.device, bytes, ACL_MEMCPY_DEVICE_TO_HOST, acl.stream), "ACL D2H");
        check(aclrtSynchronizeStream(acl.stream), "sync D2H");
        verify(ptr, bytes, seed, "ACL_ASYNC_ROUNDTRIP");
        for (int d = 0; d < 2; ++d) {
            auto begin = std::chrono::steady_clock::now();
            for (int i = 0; i < 32; ++i) {
                int ret = d == 0 ? aclrtMemcpyAsync(acl.device, bytes, ptr, bytes, ACL_MEMCPY_HOST_TO_DEVICE, acl.stream)
                                 : aclrtMemcpyAsync(ptr, bytes, acl.device, bytes, ACL_MEMCPY_DEVICE_TO_HOST, acl.stream);
                if (ret) check(ret, "ACL backlog");
            }
            auto submitted = std::chrono::steady_clock::now();
            check(aclrtRecordEvent(acl.event, acl.stream), "record event");
            aclrtEventRecordedStatus status;
            check(aclrtQueryEventStatus(acl.event, &status), "query event");
            check(aclrtSynchronizeEvent(acl.event), "sync event");
            auto end = std::chrono::steady_clock::now();
            printf("ASYNC role=%s direction=%s copies=32 submit_ms=%.3f wait_ms=%.3f status=%s\n",
                   role, d ? "D2H" : "H2D", ms(begin, submitted), ms(submitted, end),
                   status == ACL_EVENT_RECORDED_STATUS_NOT_READY ? "NOT_READY" : "COMPLETE");
        }
        verify(ptr, bytes, seed, "ACL_AFTER_BACKLOG");
        printf("CASE_RESULT role=%s ACL PASS\n", role);
    } catch (const std::exception& e) {
        ++failures; printf("CASE_RESULT role=%s ACL FAIL %s\n", role, e.what());
        if (acl.stream) check(aclrtSynchronizeStream(acl.stream), "sync failed ACL before CPU use");
    }
    if (options.aio == "none") { fill(ptr, bytes, seed); return failures; }
    void* control = nullptr;
    check(posix_memalign(&control, 4096, bytes), "allocate IO control");
    fill(control, bytes, seed);
    for (int direct = 0; direct < 2; ++direct) {
        if (options.aio == "buffered" && direct) continue;
        if (options.aio == "direct" && !direct) continue;
        fill(control, bytes, seed);
        std::string path = options.io_dir + "/aio-share-" + std::to_string(getpid()) + "-" + std::to_string(direct) + ".bin";
        int fd = open(path.c_str(), O_CREAT | O_EXCL | O_RDWR | (direct ? O_DIRECT : 0), 0600);
        if (fd < 0) {
            printf("CASE_RESULT role=%s AIO direct=%d FAIL open errno=%d %s\n", role, direct, errno, strerror(errno));
            ++failures; continue;
        }
        if (unlink(path.c_str())) { close(fd); free(control); throw std::runtime_error("unlink temporary AIO file"); }
        aio_context_t ctx = 0;
        try {
            check(syscall(SYS_io_setup, 8, &ctx), "io_setup");
            aio_operation(ctx, fd, control, bytes, true);
            check(fdatasync(fd), "seed file fdatasync");
            memset(control, 0, bytes);
            aio_operation(ctx, fd, control, bytes, false);
            verify(control, bytes, seed, "POSIX_AIO_CONTROL");
            for (int write = 0; write < 2; ++write) {
                uint64_t write_seed = seed ^ 0xfedcba9876543210ULL;
                fill(ptr, bytes, write ? write_seed : 0);
                printf("CASE role=%s AIO direct=%d operation=%s\n", role, direct, write ? "write" : "read");
                try {
                    aio_operation(ctx, fd, ptr, bytes, write);
                    if (write) {
                        check(fdatasync(fd), "HAL AIO fdatasync");
                        memset(control, 0, bytes);
                        aio_operation(ctx, fd, control, bytes, false);
                        verify(control, bytes, write_seed, "HAL_AIO_WRITE_DATA");
                    } else verify(ptr, bytes, seed, "HAL_AIO_READ_DATA");
                    printf("CASE_RESULT role=%s AIO direct=%d operation=%s PASS\n", role, direct, write ? "write" : "read");
                } catch (const std::exception& e) {
                    ++failures;
                    printf("CASE_RESULT role=%s AIO direct=%d operation=%s FAIL %s\n", role, direct, write ? "write" : "read", e.what());
                    if (!ctx) throw;
                }
            }
        } catch (const std::exception& error) {
            ++failures;
            printf("CASE_RESULT role=%s AIO_CONTROL direct=%d FAIL %s\n", role, direct, error.what());
        }
        if (ctx) cleanup(syscall(SYS_io_destroy, ctx), "io_destroy");
        cleanup(close(fd), "close AIO file");
    }
    free(control);
    fill(ptr, bytes, seed);
    return failures;
}
static int worker(bool reader, int channel, size_t bytes) {
    bool initialized = false, device_set = false;
    int failures = 0;
    try {
        check(aclInit(nullptr), "aclInit"); initialized = true;
        check(aclrtSetDevice(options.device), "aclrtSetDevice"); device_set = true;
        uint32_t host_id = 0;
        check(halGetHostID(&host_id), "halGetHostID");
        printf("ROLE %s pid=%d host_id=%u bytes=%zu\n", reader ? "importer" : "creator", getpid(), host_id, bytes);
        Allocation host;
        if (reader) {
            uint64_t token = receive_word(channel);
            printf("IMPORT target=HOST host_id=%u\n", host_id);
            check(halMemAddressReserve(&host.va, bytes, 0, nullptr, 0), "reserve importer VA");
            check(halMemImportFromShareableHandle(token, host_id, &host.handle), "halMemImportFromShareableHandle HOST");
        } else {
            drv_mem_prop prop{};
            prop.side = MEM_HOST_SIDE; prop.pg_type = MEM_HUGE_PAGE_TYPE; prop.mem_type = MEM_DDR_TYPE;
            check(halMemAddressReserve(&host.va, bytes, 0, nullptr, 0), "reserve creator VA");
            check(halMemCreate(&host.handle, bytes, &prop, 0), "halMemCreate HOST DDR");
        }
        check(halMemMap(host.va, bytes, 0, host.handle, 0), "halMemMap HOST"); host.mapped = true;
        inspect_mapping(host.va);
        if (!reader) {
            fill(host.va, bytes, 101);
            verify(host.va, bytes, 101, "CREATOR_CPU_RW");
            uint64_t token;
            check(halMemExportToShareableHandle(host.handle, MEM_HANDLE_TYPE_NONE, 0, &token), "export handle");
            ShareHandleAttr attr{}; attr.enableFlag = SHR_HANDLE_NO_WLIST_ENABLE;
            check(halMemShareHandleSetAttribute(token, SHR_HANDLE_ATTR_NO_WLIST_IN_SERVER, attr), "enable same-server sharing");
            send_word(channel, token);
            check(receive_word(channel), "importer mapped and verified creator data");
            verify(host.va, bytes, 202, "CREATOR_SEES_IMPORTER_WRITE");
            failures += io_tests(host.va, bytes, 303, "creator");
            send_word(channel, 303);
            failures += receive_word(channel);
            verify(host.va, bytes, 404, "CREATOR_SEES_IMPORTER_AFTER_IO");
            send_word(channel, 0);
            check(receive_word(channel), "importer unmapped");
        } else {
            verify(host.va, bytes, 101, "IMPORTER_SEES_CREATOR_WRITE");
            fill(host.va, bytes, 202);
            verify(host.va, bytes, 202, "IMPORTER_CPU_RW");
            send_word(channel, 0);
            check(receive_word(channel) == 303 ? 0 : -1, "creator IO completed");
            verify(host.va, bytes, 303, "IMPORTER_SEES_CREATOR_AFTER_IO");
            failures += io_tests(host.va, bytes, 404, "importer");
            send_word(channel, failures);
            check(receive_word(channel), "creator verified final data");
            check(halMemUnmap(host.va), "importer unmap"); host.mapped = false;
            check(halMemRelease(host.handle), "importer release"); host.handle = nullptr;
            check(halMemAddressFree(host.va), "importer free VA"); host.va = nullptr;
            send_word(channel, 0);
        }
        printf("SHARING_COMPLETE role=%s failed_io_cases=%d\n", reader ? "importer" : "creator", failures);
    } catch (const std::exception& e) {
        printf("RESULT FAIL role=%s phase=%s; dependent cases not completed\n", reader ? "importer" : "creator", e.what());
        failures += 100;
    }
    close(channel);
    if (device_set) cleanup(aclrtResetDevice(options.device), "aclrtResetDevice");
    if (initialized) cleanup(aclFinalize(), "aclFinalize");
    return failures || cleanup_failed ? 1 : 0;
}

int main(int argc, char** argv) { return launch(argc, argv, worker); }
