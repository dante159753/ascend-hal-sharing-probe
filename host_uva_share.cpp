#include "common.h"
#include <cstdlib>
 
inline constexpr size_t kShmSize = 2ULL << 30;
inline constexpr uint32_t kMagic = 0x48464D53U; /* "HFMS" */
 
static void PrintUsage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s writer <device>             Allocate host memory and export shareable handle\n", prog);
    printf("  %s reader <device> <handle>    Import shared memory and verify data\n", prog);
}
 
/* 向映射的内存写入测试数据: 魔数 + 递增字节序列 + 校验和 */
static void WriteTestData(void *va, size_t size)
{
    uint32_t *p = (uint32_t *)va;
    size_t count = size / sizeof(uint32_t);
    p[0] = kMagic;
    for (size_t i = 1; i < count - 1; i++)
    {
        p[i] = (uint32_t)(i & 0xFF);
    }
    /* 末尾 4 字节存校验和: 前面所有 uint32 的累加和 */
    uint32_t checksum = 0;
    for (size_t i = 0; i < count - 1; i++)
    {
        checksum += p[i];
    }
    p[count - 1] = checksum;
}
 
/* 校验 reader 端读取的数据与 writer 写入的是否一致 */
static int32_t VerifyTestData(const void *va, size_t size)
{
    const uint32_t *p = (const uint32_t *)va;
    size_t count = size / sizeof(uint32_t);
    if (p[0] != kMagic)
    {
        LOG_ERROR("magic mismatch: 0x%x vs 0x%x", p[0], kMagic);
        return -1;
    }
    uint32_t checksum = 0;
    for (size_t i = 0; i < count - 1; i++)
    {
        checksum += p[i];
    }
    if (p[count - 1] != checksum)
    {
        LOG_ERROR("checksum mismatch: 0x%x vs 0x%x", p[count - 1], checksum);
        return -1;
    }
    return 0;
}
 
int32_t RunWriter(const char *prog, int32_t device)
{
    LOG_INFO("Run on device(%d).", device);
    halSetRuntimeApiVer(__HAL_API_VERSION);
    CHECK(aclInit(nullptr));
    CHECK(aclrtSetDevice(device));
 
    void *va = nullptr;
    struct drv_mem_prop prop = {0};
    prop.side = MEM_HOST_SIDE;
    prop.devid = 0;
    prop.module_id = 0;
    prop.pg_type = MEM_HUGE_PAGE_TYPE;
    prop.mem_type = MEM_DDR_TYPE;
    prop.reserve = 0;
    drv_mem_handle_t *handle = nullptr;
    uint64_t shareableHandle = 0;
    struct ShareHandleAttr wlistAttr = {.enableFlag = SHR_HANDLE_NO_WLIST_ENABLE, .rsv = {0}};
 
    CHECK_GOTO(halMemAddressReserve(&va, kShmSize, 0, nullptr, MEM_NORMAL_PAGE_TYPE), finished);
    LOG_INFO("Reserve address at %p", va);
    CHECK_GOTO(halMemCreate(&handle, kShmSize, &prop, 0), free_va);
    LOG_INFO("Created Host DRAM physical memory (huge page, %lu bytes).", kShmSize);
    CHECK_GOTO(halMemMap(va, kShmSize, 0, handle, 0), release_handle);
    LOG_INFO("Mapped physical memory to VA: %p", va);
    WriteTestData(va, kShmSize);
    LOG_INFO("Test data written (%lu bytes, magic 0x%x).", kShmSize, kMagic);
    CHECK_GOTO(halMemExportToShareableHandle(handle, MEM_HANDLE_TYPE_NONE, 0, &shareableHandle),
               unmap_va);
    LOG_INFO("Exported shareable handle: %lu", shareableHandle);
    CHECK_GOTO(
        halMemShareHandleSetAttribute(shareableHandle, SHR_HANDLE_ATTR_NO_WLIST_IN_SERVER, wlistAttr),
        unmap_va);
    LOG_INFO("Whitelist disabled, any process can import.");
 
    LOG_INFO("========================================");
    LOG_INFO("Shareable handle: %lu", shareableHandle);
    LOG_INFO("========================================");
    LOG_INFO("In another terminal run:");
    LOG_INFO("  %s reader <device> %lu", prog, shareableHandle);
    LOG_INFO("Press Enter after reader finishes to cleanup...");
    (void)getchar();
 
unmap_va:
    halMemUnmap(va);
release_handle:
    halMemRelease(handle);
free_va:
    halMemAddressFree(va);
finished:
    aclrtResetDevice(device);
    aclFinalize();
    return 0;
}
 
int32_t RunReader(int32_t device, uint64_t shareableHandle)
{
    LOG_INFO("Run on device(%d).", device);
    LOG_INFO("Run with handle(%lu).", shareableHandle);
    halSetRuntimeApiVer(__HAL_API_VERSION);
    CHECK(aclInit(nullptr));
    CHECK(aclrtSetDevice(device));
 
    void *va = nullptr;
    drv_mem_handle_t *handle = nullptr;
    void *verify = nullptr;
 
    CHECK_GOTO(halMemAddressReserve(&va, kShmSize, 0, nullptr, MEM_NORMAL_PAGE_TYPE), finished);
    LOG_INFO("Reserve address at %p", va);
    CHECK_GOTO(halMemImportFromShareableHandle(shareableHandle, device, &handle), free_va);
    LOG_INFO("Imported shareable handle: %lu.", shareableHandle);
    CHECK_GOTO(halMemMap(va, kShmSize, 0, handle, 0), release_handle);
    LOG_INFO("Mapped physical memory to VA: %p", va);
    CHECK_GOTO(aclrtMallocHost(&verify, kShmSize), release_handle);
    CHECK_GOTO(aclrtMemcpy(verify, kShmSize, va, kShmSize, ACL_MEMCPY_DEVICE_TO_HOST), free_buffer);
    LOG_INFO("Data copied from shared VA to host buffer.");
    if (VerifyTestData(verify, kShmSize) == 0)
    {
        LOG_INFO("========================================");
        LOG_INFO("Data verified OK! (%lu bytes matched)", kShmSize);
        LOG_INFO("========================================");
    }
 
free_buffer:
    aclrtFreeHost(verify);
release_handle:
    halMemRelease(handle);
free_va:
    halMemAddressFree(va);
finished:
    aclrtResetDevice(device);
    aclFinalize();
    return 0;
}
 
int32_t main(int32_t argc, char const *argv[])
{
    if (argc < 3)
    {
        PrintUsage(argv[0]);
        return -1;
    }
    if (argc == 3 && strcmp(argv[1], "writer") == 0)
    {
        auto device = std::atol(argv[2]);
        return RunWriter(argv[0], device);
    }
    if (argc == 4 && strcmp(argv[1], "reader") == 0)
    {
        auto device = std::atol(argv[2]);
        char *endptr = NULL;
        uint64_t handle = strtoull(argv[3], &endptr, 10);
        return RunReader(device, handle);
    }
    PrintUsage(argv[0]);
    return -1;
}
 