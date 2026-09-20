#include "cli.hpp"
#include <algorithm>
#include <dlfcn.h>

static constexpr uint32_t device_id = 0;

static int worker(bool importer, int channel, size_t bytes) {
    log_role = importer ? "importer" : "creator";
    void* library = nullptr;
    using CloseTsd = uint32_t (*)(uint32_t);
    CloseTsd close_tsd = nullptr;
    bool tsd_open = false, device_open = false;
    int result = 0;
    try {
        log_line(0, "SETUP initialize TSD/HAL device=%u bytes=%zu page_type=MEM_NORMAL_PAGE_TYPE", device_id, bytes);
        log_line(1, "CALL dlopen(file=libtsdclient.so, flags=RTLD_LAZY)");
        library = dlopen("libtsdclient.so", RTLD_LAZY);
        log_line(2, "OUTPUT library=%p", library);
        if (!library) throw std::runtime_error(dlerror());
        using OpenTsd = uint32_t (*)(uint32_t, uint32_t);
        log_line(1, "CALL dlsym(handle=%p, symbol=TsdOpen)", library);
        auto open_tsd = reinterpret_cast<OpenTsd>(dlsym(library, "TsdOpen"));
        log_line(2, "OUTPUT TsdOpen=%s", open_tsd ? "resolved" : "missing");
        log_line(1, "CALL dlsym(handle=%p, symbol=TsdClose)", library);
        close_tsd = reinterpret_cast<CloseTsd>(dlsym(library, "TsdClose"));
        log_line(2, "OUTPUT TsdClose=%s", close_tsd ? "resolved" : "missing");
        if (!open_tsd || !close_tsd) throw std::runtime_error("missing TsdOpen/TsdClose");
        log_line(1, "CALL TsdOpen(device_id=%u, rank_size=0)", device_id);
        check(open_tsd(device_id, 0), "TsdOpen"); tsd_open = true;
        log_line(1, "CALL halSetRuntimeApiVer(version=%u)", static_cast<unsigned>(__HAL_API_VERSION));
        halSetRuntimeApiVer(__HAL_API_VERSION);
        log_line(2, "RETURN halSetRuntimeApiVer (void)");
        halDevOpenIn input{}; halDevOpenOut output{};
        log_line(1, "CALL halDeviceOpen(device_id=%u, input=%p {zero-initialized}, output=%p)", device_id, static_cast<void*>(&input), static_cast<void*>(&output));
        check(halDeviceOpen(device_id, &input, &output), "halDeviceOpen"); device_open = true;
        Allocation mapping;
        Allocation hbm;
        auto allocate_hbm = [&] {
            drv_mem_prop prop{};
            prop.side = MEM_DEV_SIDE;
            prop.devid = device_id;
            prop.pg_type = MEM_NORMAL_PAGE_TYPE;
            prop.mem_type = MEM_HBM_TYPE;
            log_line(1, "CALL halMemAddressReserve(out_va=%p, size=%zu, alignment=0, requested_va=null, flags=0)", static_cast<void*>(&hbm.va), bytes);
            check(halMemAddressReserve(&hbm.va, bytes, 0, nullptr, 0), "reserve HBM VA");
            log_line(2, "OUTPUT va=%p", hbm.va);
            log_line(1, "CALL halMemCreate(out_handle=%p, size=%zu, prop={side=MEM_DEV_SIDE(%u), devid=%u, module_id=%u, pg_type=MEM_NORMAL_PAGE_TYPE(%u), mem_type=MEM_HBM_TYPE(%u), reserve=0}, flags=0)", static_cast<void*>(&hbm.handle), bytes, static_cast<unsigned>(prop.side), prop.devid, prop.module_id, static_cast<unsigned>(prop.pg_type), static_cast<unsigned>(prop.mem_type));
            check(halMemCreate(&hbm.handle, bytes, &prop, 0), "halMemCreate DEVICE HBM normal pages");
            log_line(2, "OUTPUT handle=%p", static_cast<void*>(hbm.handle));
            log_line(1, "CALL halMemMap(va=%p, size=%zu, offset=0, handle=%p, flags=0)", hbm.va, bytes, static_cast<void*>(hbm.handle));
            check(halMemMap(hbm.va, bytes, 0, hbm.handle, 0), "map DEVICE HBM"); hbm.mapped = true;
            log_line(1, "BUFFERS shared=%p shared_view=%s hbm=%p hbm_device=%u bytes=%zu", mapping.va, importer ? "DEVICE" : "HOST", hbm.va, device_id, bytes);
        };
        memcpy_info info{};
        info.devid = device_id;
        std::vector<uint64_t> host(bytes / sizeof(uint64_t));
        if (!importer) {
            log_line(0, "SETUP allocate shared Host DDR (normal pages)");
            drv_mem_prop prop{};
            prop.side = MEM_HOST_SIDE;
            prop.pg_type = MEM_NORMAL_PAGE_TYPE;
            prop.mem_type = MEM_DDR_TYPE;
            log_line(1, "CALL halMemAddressReserve(out_va=%p, size=%zu, alignment=0, requested_va=null, flags=0)", static_cast<void*>(&mapping.va), bytes);
            check(halMemAddressReserve(&mapping.va, bytes, 0, nullptr, 0), "reserve creator VA");
            log_line(2, "OUTPUT va=%p", mapping.va);
            log_line(1, "CALL halMemCreate(out_handle=%p, size=%zu, prop={side=MEM_HOST_SIDE(%u), devid=%u, module_id=%u, pg_type=MEM_NORMAL_PAGE_TYPE(%u), mem_type=MEM_DDR_TYPE(%u), reserve=0}, flags=0)", static_cast<void*>(&mapping.handle), bytes, static_cast<unsigned>(prop.side), prop.devid, prop.module_id, static_cast<unsigned>(prop.pg_type), static_cast<unsigned>(prop.mem_type));
            check(halMemCreate(&mapping.handle, bytes, &prop, 0), "halMemCreate HOST DDR");
            log_line(2, "OUTPUT handle=%p", static_cast<void*>(mapping.handle));
            log_line(1, "CALL halMemMap(va=%p, size=%zu, offset=0, handle=%p, flags=0)", mapping.va, bytes, static_cast<void*>(mapping.handle));
            check(halMemMap(mapping.va, bytes, 0, mapping.handle, 0), "map creator HOST"); mapping.mapped = true;
            log_line(0, "TEST CREATOR_HOST_TO_HBM: shared Host source -> H2D -> separate HBM -> D2H -> verify seed 101");
            fill(mapping.va, bytes, 101);
            allocate_hbm();
            info.dir = DRV_MEMCPY_HOST_TO_DEVICE;
            log_line(1, "CALL halMemcpy(dst=%p, dest_max=%zu, src=%p, count=%zu, info={dir=DRV_MEMCPY_HOST_TO_DEVICE(%u), devid=%u})", static_cast<void*>(hbm.va), bytes, static_cast<void*>(mapping.va), bytes, static_cast<unsigned>(info.dir), info.devid);
            check(halMemcpy(hbm.va, bytes, mapping.va, bytes, &info), "H2D shared HOST source -> HBM destination");
            info.dir = DRV_MEMCPY_DEVICE_TO_HOST;
            log_line(1, "CALL halMemcpy(dst=%p, dest_max=%zu, src=%p, count=%zu, info={dir=DRV_MEMCPY_DEVICE_TO_HOST(%u), devid=%u})", static_cast<void*>(host.data()), bytes, static_cast<void*>(hbm.va), bytes, static_cast<unsigned>(info.dir), info.devid);
            check(halMemcpy(host.data(), bytes, hbm.va, bytes, &info), "D2H creator HBM -> verification buffer");
            verify(host.data(), bytes, 101, "CREATOR_HOST_TO_HBM");
            log_line(0, "SHARE export Host allocation and send handle to importer");
            uint64_t token;
            log_line(1, "CALL halMemExportToShareableHandle(handle=%p, type=MEM_HANDLE_TYPE_NONE, flags=0, out_token=%p)", static_cast<void*>(mapping.handle), static_cast<void*>(&token));
            check(halMemExportToShareableHandle(mapping.handle, MEM_HANDLE_TYPE_NONE, 0, &token), "export HOST handle");
            log_line(2, "OUTPUT shared_token=0x%llx", (unsigned long long)token);
            ShareHandleAttr attr{}; attr.enableFlag = SHR_HANDLE_NO_WLIST_ENABLE;
            log_line(1, "CALL halMemShareHandleSetAttribute(token=0x%llx, type=SHR_HANDLE_ATTR_NO_WLIST_IN_SERVER, attr={enableFlag=SHR_HANDLE_NO_WLIST_ENABLE(%u), remaining_fields=0})", (unsigned long long)token, static_cast<unsigned>(attr.enableFlag));
            check(halMemShareHandleSetAttribute(token, SHR_HANDLE_ATTR_NO_WLIST_IN_SERVER, attr), "enable same-server sharing");
            send_word(channel, token);
            log_line(0, "TEST CREATOR_SEES_IMPORTER_WRITE: wait for importer writeback, then verify shared Host seed 202");
            check(receive_word(channel), "importer verified D2D and wrote shared mapping");
            verify(mapping.va, bytes, 202, "CREATOR_SEES_IMPORTER_WRITE");
            send_word(channel, 0);
            log_line(0, "SYNC wait until importer has released shared mapping");
            check(receive_word(channel), "importer resources released");
        } else {
            log_line(0, "SHARE wait for creator handle, then import as Device mapping on device 0");
            uint64_t token = receive_word(channel);
            log_line(1, "CALL halMemAddressReserve(out_va=%p, size=%zu, alignment=0, requested_va=null, flags=0)", static_cast<void*>(&mapping.va), bytes);
            check(halMemAddressReserve(&mapping.va, bytes, 0, nullptr, 0), "reserve importer VA");
            log_line(2, "OUTPUT va=%p", mapping.va);
            log_line(1, "IMPORT target=DEVICE hal_devid=%u physical_backing=HOST_DDR", device_id);
            log_line(1, "CALL halMemImportFromShareableHandle(token=0x%llx, device_id=%u, out_handle=%p)", (unsigned long long)token, device_id, static_cast<void*>(&mapping.handle));
            check(halMemImportFromShareableHandle(token, device_id, &mapping.handle), "import shared handle to DEVICE 0");
            log_line(2, "OUTPUT imported_handle=%p", static_cast<void*>(mapping.handle));
            log_line(1, "CALL halMemMap(va=%p, size=%zu, offset=0, handle=%p, flags=0)", mapping.va, bytes, static_cast<void*>(mapping.handle));
            check(halMemMap(mapping.va, bytes, 0, mapping.handle, 0), "map imported DEVICE"); mapping.mapped = true;
            allocate_hbm();
            log_line(0, "TEST IMPORTER_SHARED_TO_HBM: shared Device source -> D2D -> separate HBM -> D2H -> verify seed 101");
            info.dir = DRV_MEMCPY_DEVICE_TO_DEVICE;
            log_line(1, "CALL halMemcpy(dst=%p, dest_max=%zu, src=%p, count=%zu, info={dir=DRV_MEMCPY_DEVICE_TO_DEVICE(%u), devid=%u})", static_cast<void*>(hbm.va), bytes, static_cast<void*>(mapping.va), bytes, static_cast<unsigned>(info.dir), info.devid);
            check(halMemcpy(hbm.va, bytes, mapping.va, bytes, &info), "D2D imported DEVICE source -> HBM destination");
            info.dir = DRV_MEMCPY_DEVICE_TO_HOST;
            log_line(1, "CALL halMemcpy(dst=%p, dest_max=%zu, src=%p, count=%zu, info={dir=DRV_MEMCPY_DEVICE_TO_HOST(%u), devid=%u})", static_cast<void*>(host.data()), bytes, static_cast<void*>(hbm.va), bytes, static_cast<unsigned>(info.dir), info.devid);
            check(halMemcpy(host.data(), bytes, hbm.va, bytes, &info), "D2H importer HBM -> verification buffer");
            verify(host.data(), bytes, 101, "IMPORTER_SHARED_TO_HBM");
            log_line(0, "TEST IMPORTER_WRITE_READBACK: seed 202 -> H2D -> HBM -> D2D -> shared Device destination -> D2H -> verify");
            fill(host.data(), bytes, 202);
            info.dir = DRV_MEMCPY_HOST_TO_DEVICE;
            log_line(1, "CALL halMemcpy(dst=%p, dest_max=%zu, src=%p, count=%zu, info={dir=DRV_MEMCPY_HOST_TO_DEVICE(%u), devid=%u})", static_cast<void*>(hbm.va), bytes, static_cast<void*>(host.data()), bytes, static_cast<unsigned>(info.dir), info.devid);
            check(halMemcpy(hbm.va, bytes, host.data(), bytes, &info), "H2D new pattern -> importer HBM");
            info.dir = DRV_MEMCPY_DEVICE_TO_DEVICE;
            log_line(1, "CALL halMemcpy(dst=%p, dest_max=%zu, src=%p, count=%zu, info={dir=DRV_MEMCPY_DEVICE_TO_DEVICE(%u), devid=%u})", static_cast<void*>(mapping.va), bytes, static_cast<void*>(hbm.va), bytes, static_cast<unsigned>(info.dir), info.devid);
            check(halMemcpy(mapping.va, bytes, hbm.va, bytes, &info), "D2D HBM source -> imported DEVICE destination");
            log_line(1, "CPU clear verification buffer ptr=%p bytes=%zu", static_cast<void*>(host.data()), bytes);
            std::fill(host.begin(), host.end(), 0);
            info.dir = DRV_MEMCPY_DEVICE_TO_HOST;
            log_line(1, "CALL halMemcpy(dst=%p, dest_max=%zu, src=%p, count=%zu, info={dir=DRV_MEMCPY_DEVICE_TO_HOST(%u), devid=%u})", static_cast<void*>(host.data()), bytes, static_cast<void*>(mapping.va), bytes, static_cast<unsigned>(info.dir), info.devid);
            check(halMemcpy(host.data(), bytes, mapping.va, bytes, &info), "D2H imported DEVICE -> verification buffer");
            verify(host.data(), bytes, 202, "IMPORTER_WRITE_READBACK");
            send_word(channel, 0);
            log_line(0, "SYNC wait for creator to verify shared writeback before unmapping");
            check(receive_word(channel), "creator acknowledged verification");
            log_line(0, "CLEANUP release imported shared mapping");
            log_line(1, "CALL halMemUnmap(va=%p)", static_cast<void*>(mapping.va));
            check(halMemUnmap(mapping.va), "unmap importer DEVICE"); mapping.mapped = false;
            log_line(1, "CALL halMemRelease(handle=%p)", static_cast<void*>(mapping.handle));
            check(halMemRelease(mapping.handle), "release importer handle"); mapping.handle = nullptr;
            log_line(1, "CALL halMemAddressFree(va=%p)", static_cast<void*>(mapping.va));
            check(halMemAddressFree(mapping.va), "free importer VA"); mapping.va = nullptr;
            send_word(channel, 0);
        }
        log_line(0, "CLEANUP release remaining HBM and shared allocation resources");
    } catch (const std::exception& error) {
        log_line(0, "RESULT FAIL phase=%s", error.what()); result = 1;
    }
    log_line(0, "CLEANUP close IPC, HAL device and TSD");
    log_line(1, "CALL close(fd=%d)", channel);
    close(channel);
    if (device_open) {
        halDevCloseIn input{};
        log_line(1, "CALL halDeviceClose(device_id=%u, input=%p {zero-initialized})", device_id, static_cast<void*>(&input));
        cleanup(halDeviceClose(device_id, &input), "halDeviceClose");
    }
    if (tsd_open) {
        log_line(1, "CALL TsdClose(device_id=%u)", device_id);
        cleanup(close_tsd(device_id), "TsdClose");
    }
    if (library) {
        log_line(1, "CALL dlclose(handle=%p)", library);
        cleanup(dlclose(library), "dlclose TSD");
    }
    result = result || cleanup_failed;
    log_line(0, "RESULT %s scope=HAL_DEVICE_SHARE_COPY", result ? "FAIL" : "PASS");
    return result;
}

int main(int argc, char** argv) {
    log_role = "launcher";
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
        log_line(0, "LAUNCH_ERROR %s", error.what()); return 2;
    }
    return launch(argc, argv, worker, 1);
}
