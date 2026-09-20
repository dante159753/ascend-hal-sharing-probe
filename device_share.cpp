#include "cli.hpp"
#include <algorithm>
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
        Allocation hbm;
        auto allocate_hbm = [&] {
            drv_mem_prop prop{};
            prop.side = MEM_DEV_SIDE;
            prop.devid = device_id;
            prop.pg_type = MEM_NORMAL_PAGE_TYPE;
            prop.mem_type = MEM_HBM_TYPE;
            check(halMemAddressReserve(&hbm.va, bytes, 0, nullptr, 0), "reserve HBM VA");
            check(halMemCreate(&hbm.handle, bytes, &prop, 0), "halMemCreate DEVICE HBM normal pages");
            check(halMemMap(hbm.va, bytes, 0, hbm.handle, 0), "map DEVICE HBM"); hbm.mapped = true;
            printf("BUFFERS shared=%p hbm=%p bytes=%zu\n", mapping.va, hbm.va, bytes);
        };
        memcpy_info info{};
        info.devid = device_id;
        std::vector<uint64_t> host(bytes / sizeof(uint64_t));
        if (!importer) {
            drv_mem_prop prop{};
            prop.side = MEM_HOST_SIDE;
            prop.pg_type = MEM_NORMAL_PAGE_TYPE;
            prop.mem_type = MEM_DDR_TYPE;
            check(halMemAddressReserve(&mapping.va, bytes, 0, nullptr, 0), "reserve creator VA");
            check(halMemCreate(&mapping.handle, bytes, &prop, 0), "halMemCreate HOST DDR");
            check(halMemMap(mapping.va, bytes, 0, mapping.handle, 0), "map creator HOST"); mapping.mapped = true;
            fill(mapping.va, bytes, 101);
            allocate_hbm();
            info.dir = DRV_MEMCPY_HOST_TO_DEVICE;
            check(halMemcpy(hbm.va, bytes, mapping.va, bytes, &info), "H2D shared HOST source -> HBM destination");
            info.dir = DRV_MEMCPY_DEVICE_TO_HOST;
            check(halMemcpy(host.data(), bytes, hbm.va, bytes, &info), "D2H creator HBM -> verification buffer");
            verify(host.data(), bytes, 101, "CREATOR_HOST_TO_HBM");
            uint64_t token;
            check(halMemExportToShareableHandle(mapping.handle, MEM_HANDLE_TYPE_NONE, 0, &token), "export HOST handle");
            ShareHandleAttr attr{}; attr.enableFlag = SHR_HANDLE_NO_WLIST_ENABLE;
            check(halMemShareHandleSetAttribute(token, SHR_HANDLE_ATTR_NO_WLIST_IN_SERVER, attr), "enable same-server sharing");
            send_word(channel, token);
            check(receive_word(channel), "importer verified D2D and wrote shared mapping");
            verify(mapping.va, bytes, 202, "CREATOR_SEES_IMPORTER_WRITE");
            send_word(channel, 0);
            check(receive_word(channel), "importer resources released");
        } else {
            uint64_t token = receive_word(channel);
            check(halMemAddressReserve(&mapping.va, bytes, 0, nullptr, 0), "reserve importer VA");
            printf("IMPORT target=DEVICE hal_devid=%u\n", device_id);
            check(halMemImportFromShareableHandle(token, device_id, &mapping.handle), "import shared handle to DEVICE 0");
            check(halMemMap(mapping.va, bytes, 0, mapping.handle, 0), "map imported DEVICE"); mapping.mapped = true;
            allocate_hbm();
            info.dir = DRV_MEMCPY_DEVICE_TO_DEVICE;
            check(halMemcpy(hbm.va, bytes, mapping.va, bytes, &info), "D2D imported DEVICE source -> HBM destination");
            info.dir = DRV_MEMCPY_DEVICE_TO_HOST;
            check(halMemcpy(host.data(), bytes, hbm.va, bytes, &info), "D2H importer HBM -> verification buffer");
            verify(host.data(), bytes, 101, "IMPORTER_SHARED_TO_HBM");
            fill(host.data(), bytes, 202);
            info.dir = DRV_MEMCPY_HOST_TO_DEVICE;
            check(halMemcpy(hbm.va, bytes, host.data(), bytes, &info), "H2D new pattern -> importer HBM");
            info.dir = DRV_MEMCPY_DEVICE_TO_DEVICE;
            check(halMemcpy(mapping.va, bytes, hbm.va, bytes, &info), "D2D HBM source -> imported DEVICE destination");
            std::fill(host.begin(), host.end(), 0);
            info.dir = DRV_MEMCPY_DEVICE_TO_HOST;
            check(halMemcpy(host.data(), bytes, mapping.va, bytes, &info), "D2H imported DEVICE -> verification buffer");
            verify(host.data(), bytes, 202, "IMPORTER_WRITE_READBACK");
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
                puts("device_share [--size-mib POSITIVE_MIB] [--device 0]\n"
                     "Device and import target are fixed to HAL device 0. Default size: 2 MiB.\n"
                     "TsdOpen -> halDeviceOpen -> Host Create/Export -> Device Import/Map.\n"
                     "Normal pages for shared Host DDR and separate HBM allocations.\n"
                     "Creator: shared Host -> H2D -> HBM. Importer: shared Device <-> D2D <-> HBM.\n"
                     "HAL D2H readback and cross-process write verification. No ACL or AIO.\n"
                     "Unset ASCEND_RT_VISIBLE_DEVICES.");
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
    return launch(argc, argv, worker, 1);
}
