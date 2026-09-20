#include "cli.hpp"
#include <dlfcn.h>

static constexpr uint32_t device_id = 0;

static int worker(bool importer, int channel, size_t bytes) {
    void* library = nullptr;
    using CloseTsd = uint32_t (*)(uint32_t);
    CloseTsd close_tsd = nullptr;
    bool tsd_open = false, device_open = false;
    int result = 0;
    try {
        printf("ROLE %s pid=%d device=%u bytes=%zu\n", importer ? "importer" : "creator", getpid(), device_id, bytes);
        library = dlopen("libtsdclient.so", RTLD_LAZY);
        if (!library) throw std::runtime_error(dlerror());
        using OpenTsd = uint32_t (*)(uint32_t, uint32_t);
        auto open_tsd = reinterpret_cast<OpenTsd>(dlsym(library, "TsdOpen"));
        close_tsd = reinterpret_cast<CloseTsd>(dlsym(library, "TsdClose"));
        if (!open_tsd || !close_tsd) throw std::runtime_error("missing TsdOpen/TsdClose");
        check(open_tsd(device_id, 0), "TsdOpen"); tsd_open = true;
        halSetRuntimeApiVer(__HAL_API_VERSION);
        halDevOpenIn input{}; halDevOpenOut output{};
        check(halDeviceOpen(device_id, &input, &output), "halDeviceOpen"); device_open = true;
        Allocation mapping;
        if (!importer) {
            drv_mem_prop prop{};
            prop.side = MEM_HOST_SIDE;
            prop.pg_type = MEM_HUGE_PAGE_TYPE;
            prop.mem_type = MEM_DDR_TYPE;
            check(halMemAddressReserve(&mapping.va, bytes, 0, nullptr, 0), "reserve creator VA");
            check(halMemCreate(&mapping.handle, bytes, &prop, 0), "halMemCreate HOST DDR");
            check(halMemMap(mapping.va, bytes, 0, mapping.handle, 0), "map creator HOST"); mapping.mapped = true;
            std::vector<uint64_t> host(bytes / sizeof(uint64_t));
            fill(host.data(), bytes, 101);
            memcpy_info info{};
            info.dir = DRV_MEMCPY_HOST_TO_DEVICE; info.devid = device_id;
            check(halMemcpy(mapping.va, bytes, host.data(), bytes, &info), "halMemcpy HOST_TO_DEVICE into shared mapping");
            uint64_t token;
            check(halMemExportToShareableHandle(mapping.handle, MEM_HANDLE_TYPE_NONE, 0, &token), "export HOST handle");
            ShareHandleAttr attr{}; attr.enableFlag = SHR_HANDLE_NO_WLIST_ENABLE;
            check(halMemShareHandleSetAttribute(token, SHR_HANDLE_ATTR_NO_WLIST_IN_SERVER, attr), "enable same-server sharing");
            send_word(channel, token);
            check(receive_word(channel), "importer verified HAL D2H data");
            send_word(channel, 0);
            check(receive_word(channel), "importer resources released");
        } else {
            uint64_t token = receive_word(channel);
            check(halMemAddressReserve(&mapping.va, bytes, 0, nullptr, 0), "reserve importer VA");
            printf("IMPORT target=DEVICE hal_devid=%u\n", device_id);
            check(halMemImportFromShareableHandle(token, device_id, &mapping.handle), "import shared handle to DEVICE 0");
            check(halMemMap(mapping.va, bytes, 0, mapping.handle, 0), "map imported DEVICE"); mapping.mapped = true;
            std::vector<uint64_t> host(bytes / sizeof(uint64_t));
            memcpy_info info{};
            info.dir = DRV_MEMCPY_DEVICE_TO_HOST; info.devid = device_id;
            check(halMemcpy(host.data(), bytes, mapping.va, bytes, &info), "halMemcpy DEVICE_TO_HOST from shared mapping");
            verify(host.data(), bytes, 101, "HAL_SHARED_DATA");
            send_word(channel, 0);
            check(receive_word(channel), "creator acknowledged verification");
            check(halMemUnmap(mapping.va), "unmap importer DEVICE"); mapping.mapped = false;
            check(halMemRelease(mapping.handle), "release importer handle"); mapping.handle = nullptr;
            check(halMemAddressFree(mapping.va), "free importer VA"); mapping.va = nullptr;
            send_word(channel, 0);
        }
    } catch (const std::exception& error) {
        printf("RESULT FAIL role=%s phase=%s\n", importer ? "importer" : "creator", error.what()); result = 1;
    }
    close(channel);
    if (device_open) {
        halDevCloseIn input{};
        cleanup(halDeviceClose(device_id, &input), "halDeviceClose");
    }
    if (tsd_open) cleanup(close_tsd(device_id), "TsdClose");
    if (library) cleanup(dlclose(library), "dlclose TSD");
    result = result || cleanup_failed;
    printf("RESULT %s role=%s scope=HAL_DEVICE_SHARE_COPY\n", result ? "FAIL" : "PASS", importer ? "importer" : "creator");
    return result;
}

int main(int argc, char** argv) {
    try {
        for (int i = 1; i < argc; ++i) {
            std::string key = argv[i];
            if (key == "--help") {
                puts("device_share [--size-mib EVEN_MIB] [--device 0]\n"
                     "Device and import target are fixed to HAL device 0. Default size: 2 MiB.\n"
                     "TsdOpen -> halDeviceOpen -> Host Create/Export -> Device Import/Map.\n"
                     "Writer uses halMemcpy H2D; reader uses halMemcpy D2H and verifies all data.\n"
                     "No ACL initialization, HBM allocation, D2D or AIO. Unset ASCEND_RT_VISIBLE_DEVICES.");
                return 0;
            }
            if (key != "--device" && key != "--size-mib" && key != "--worker-fd")
                throw std::runtime_error("unsupported option: " + key);
            if (++i == argc) throw std::runtime_error("missing option value");
            if (key == "--device" && number(argv[i]) != device_id)
                throw std::runtime_error("device_share is fixed to HAL device 0");
        }
        if (getenv("ASCEND_RT_VISIBLE_DEVICES"))
            throw std::runtime_error("unset ASCEND_RT_VISIBLE_DEVICES; device_share uses HAL device 0");
    } catch (const std::exception& error) {
        fprintf(stderr, "LAUNCH_ERROR %s\n", error.what()); return 2;
    }
    return launch(argc, argv, worker);
}
