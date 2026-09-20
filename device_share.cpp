#include "cli.hpp"
#include <sys/socket.h>
#include <sys/wait.h>
#include <memory>

static DVattribute attributes(void* ptr, const char* label) {
    DVattribute attr{};
    check(drvMemGetAttribute((DVdeviceptr)ptr, &attr), "drvMemGetAttribute");
    printf("ATTR %s ptr=%p memType=%u devId=%u pageSize=%u\n", label, ptr, attr.memType, attr.devId, attr.pageSize);
    return attr;
}
static int worker(bool importer, int channel, size_t bytes) {
    bool initialized = false, device_set = false;
    int result = 0;
    try {
        check(aclInit(nullptr), "aclInit"); initialized = true;
        check(aclrtSetDevice(options.device), "aclrtSetDevice"); device_set = true;
        printf("ROLE %s pid=%d bytes=%zu\n", importer ? "importer" : "creator", getpid(), bytes);
        Allocation mapping;
        if (!importer) {
            drv_mem_prop prop{};
            prop.side = MEM_HOST_SIDE; prop.pg_type = MEM_HUGE_PAGE_TYPE; prop.mem_type = MEM_DDR_TYPE;
            check(halMemAddressReserve(&mapping.va, bytes, 0, nullptr, 0), "reserve creator VA");
            check(halMemCreate(&mapping.handle, bytes, &prop, 0), "halMemCreate HOST DDR");
            check(halMemMap(mapping.va, bytes, 0, mapping.handle, 0), "map creator HOST"); mapping.mapped = true;
            attributes(mapping.va, "creator HOST");
            fill(mapping.va, bytes, 101);
            verify(mapping.va, bytes, 101, "CREATOR_CPU_WRITE");
            uint64_t token;
            check(halMemExportToShareableHandle(mapping.handle, MEM_HANDLE_TYPE_NONE, 0, &token), "export HOST handle");
            ShareHandleAttr attr{}; attr.enableFlag = SHR_HANDLE_NO_WLIST_ENABLE;
            check(halMemShareHandleSetAttribute(token, SHR_HANDLE_ATTR_NO_WLIST_IN_SERVER, attr), "enable same-server sharing");
            send_word(channel, token);
            check(receive_word(channel), "importer D2D read verified");
            fill(mapping.va, bytes, 202);
            send_word(channel, 202);
            check(receive_word(channel), "importer second D2D read and reverse write completed");
            verify(mapping.va, bytes, 303, "CREATOR_SEES_REVERSE_D2D_WRITE");
            send_word(channel, 0);
            check(receive_word(channel), "importer resources released");
        } else {
            AclResources acl;
            check(aclrtMalloc(&acl.device, bytes, ACL_MEM_MALLOC_HUGE_FIRST), "allocate importer NPU HBM");
            auto device_attr = attributes(acl.device, "importer NPU HBM");
            uint32_t host_id;
            check(halGetHostID(&host_id), "halGetHostID");
            if (device_attr.devId == host_id) throw std::runtime_error("unexpected Host destination");
            uint64_t token = receive_word(channel);
            bool via_host = options.via_host;
            printf("IMPORT target=%s hal_devid=%u host_id=%u\n", via_host ? "HOST_THEN_DEVICE_ACCESS" : "DEVICE", device_attr.devId, host_id);
            check(halMemAddressReserve(&mapping.va, bytes, 0, nullptr, 0), "reserve importer VA");
            check(halMemImportFromShareableHandle(token, via_host ? host_id : device_attr.devId, &mapping.handle), "import shared handle");
            check(halMemMap(mapping.va, bytes, 0, mapping.handle, 0), "map imported handle"); mapping.mapped = true;
            if (via_host) {
                drv_mem_access_desc desc{};
                desc.location.side = MEM_DEV_SIDE;
                desc.location.id = device_attr.devId;
                desc.type = MEM_ACCESS_TYPE_READWRITE;
                check(halMemSetAccess(mapping.va, bytes, &desc, 1), "halMemSetAccess imported HOST -> DEVICE RW");
            }
            attributes(mapping.va, "importer shared DEVICE mapping");
            check(aclrtCreateStream(&acl.stream), "create stream");
            check(aclrtCreateEvent(&acl.event), "create event");
            std::unique_ptr<void, decltype(&free)> host(malloc(bytes), &free);
            if (!host) throw std::runtime_error("malloc verification buffer");
            auto copy_and_verify = [&](uint64_t seed) {
                check(aclrtMemcpyAsync(acl.device, bytes, mapping.va, bytes, ACL_MEMCPY_DEVICE_TO_DEVICE, acl.stream), "D2D shared Host-backed DEVICE VA -> NPU HBM");
                check(aclrtSynchronizeStream(acl.stream), "sync D2D");
                memset(host.get(), 0, bytes);
                check(aclrtMemcpy(host.get(), bytes, acl.device, bytes, ACL_MEMCPY_DEVICE_TO_HOST), "readback NPU HBM D2H");
                verify(host.get(), bytes, seed, "IMPORTED_DEVICE_TO_NPU_HBM");
            };
            copy_and_verify(101);
            send_word(channel, 0);
            check(receive_word(channel) == 202 ? 0 : -1, "creator changed shared data");
            copy_and_verify(202);
            auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < 32; ++i) {
                int ret = aclrtMemcpyAsync(acl.device, bytes, mapping.va, bytes, ACL_MEMCPY_DEVICE_TO_DEVICE, acl.stream);
                if (ret) check(ret, "D2D backlog");
            }
            auto submitted = std::chrono::steady_clock::now();
            check(aclrtRecordEvent(acl.event, acl.stream), "record event");
            aclrtEventRecordedStatus status;
            check(aclrtQueryEventStatus(acl.event, &status), "query event");
            check(aclrtSynchronizeEvent(acl.event), "sync event");
            auto finished = std::chrono::steady_clock::now();
            printf("ASYNC D2D copies=32 bytes_each=%zu submit_ms=%.3f wait_ms=%.3f status=%s\n",
                   bytes, ms(start, submitted), ms(submitted, finished),
                   status == ACL_EVENT_RECORDED_STATUS_NOT_READY ? "NOT_READY" : "COMPLETE");
            check(aclrtMemcpy(host.get(), bytes, acl.device, bytes, ACL_MEMCPY_DEVICE_TO_HOST), "readback after backlog");
            verify(host.get(), bytes, 202, "D2D_BACKLOG_DATA");
            puts("CASE_RESULT FORWARD_D2D PASS");
            fill(host.get(), bytes, 303);
            check(aclrtMemcpy(acl.device, bytes, host.get(), bytes, ACL_MEMCPY_HOST_TO_DEVICE), "seed NPU for reverse D2D");
            check(aclrtMemcpyAsync(mapping.va, bytes, acl.device, bytes, ACL_MEMCPY_DEVICE_TO_DEVICE, acl.stream), "reverse D2D NPU HBM -> shared Host-backed DEVICE VA");
            check(aclrtSynchronizeStream(acl.stream), "sync reverse D2D");
            send_word(channel, 0);
            check(receive_word(channel), "creator verified reverse D2D");
            puts("CASE_RESULT REVERSE_D2D PASS");
            check(halMemUnmap(mapping.va), "unmap importer DEVICE"); mapping.mapped = false;
            check(halMemRelease(mapping.handle), "release importer handle"); mapping.handle = nullptr;
            check(halMemAddressFree(mapping.va), "free importer VA"); mapping.va = nullptr;
            send_word(channel, 0);
        }
        printf("RESULT PASS role=%s\n", importer ? "importer" : "creator");
    } catch (const std::exception& e) {
        printf("RESULT FAIL role=%s phase=%s\n", importer ? "importer" : "creator", e.what()); result = 1;
    }
    close(channel);
    if (device_set) cleanup(aclrtResetDevice(options.device), "aclrtResetDevice");
    if (initialized) cleanup(aclFinalize(), "aclFinalize");
    return result || cleanup_failed ? 1 : 0;
}

int main(int argc, char** argv) { return launch(argc, argv, worker); }
