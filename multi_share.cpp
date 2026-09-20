#include "cli.hpp"
#include <dlfcn.h>
#include <sched.h>
#include <sys/prctl.h>
#include <signal.h>
#include <fstream>
#include <set>
#include <sstream>

static constexpr size_t va_alignment = size_t{1} << 30;
static constexpr size_t read_rounds = 2;

struct MultiOptions {
    std::vector<int> devices, nodes;
    size_t bytes = 2 * 1024 * 1024;
    std::string io_dir = ".";
    int rank = -1, fd = -1;
};

static std::vector<int> list(const std::string& value) {
    std::vector<int> out;
    std::istringstream stream(value);
    std::string part;
    while (std::getline(stream, part, ',')) {
        auto n = number(part);
        if (n > INT_MAX) throw std::runtime_error("list integer out of range");
        out.push_back(static_cast<int>(n));
    }
    if (out.empty() || value.back() == ',') throw std::runtime_error("empty list item");
    return out;
}

static void bind_cpu(int node) {
    std::ifstream file("/sys/devices/system/node/node" + std::to_string(node) + "/cpulist");
    std::string cpus;
    if (!(file >> cpus)) throw std::runtime_error("read NUMA node cpulist");
    cpu_set_t allowed, selected;
    CPU_ZERO(&selected);
    check(sched_getaffinity(0, sizeof(allowed), &allowed), "sched_getaffinity");
    std::istringstream stream(cpus);
    std::string part;
    while (std::getline(stream, part, ',')) {
        auto dash = part.find('-');
        int first = std::stoi(part.substr(0, dash));
        int last = dash == std::string::npos ? first : std::stoi(part.substr(dash + 1));
        if (last >= CPU_SETSIZE) throw std::runtime_error("CPU ID exceeds CPU_SETSIZE");
        for (int cpu = first; cpu <= last; ++cpu)
            if (CPU_ISSET(cpu, &allowed)) CPU_SET(cpu, &selected);
    }
    if (!CPU_COUNT(&selected)) throw std::runtime_error("NUMA node has no allowed CPUs");
    check(sched_setaffinity(0, sizeof(selected), &selected), "sched_setaffinity NUMA");
    log_line(0, "CPU_BIND pid=%d numa=%d node_cpus=%s selected_count=%d", getpid(), node, cpus.c_str(), CPU_COUNT(&selected));
}

static void buffered_aio(void* ptr, size_t bytes, uint64_t seed, const std::string& directory) {
    std::vector<uint64_t> control(bytes / 8);
    fill(control.data(), bytes, seed);
    std::string path = directory + "/multi-aio-" + std::to_string(getpid()) + ".bin";
    int fd = open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) throw std::runtime_error("open buffered AIO file");
    if (unlink(path.c_str())) { close(fd); throw std::runtime_error("unlink AIO file"); }
    aio_context_t ctx = 0;
    try {
        check(syscall(SYS_io_setup, 8, &ctx), "io_setup");
        aio_operation(ctx, fd, control.data(), bytes, true);
        memset(ptr, 0, bytes);
        aio_operation(ctx, fd, ptr, bytes, false);
        verify(ptr, bytes, seed, "AIO_READ_OWN_HOST");
        check(ftruncate(fd, 0), "truncate before AIO write");
        aio_operation(ctx, fd, ptr, bytes, true);
        memset(control.data(), 0, bytes);
        aio_operation(ctx, fd, control.data(), bytes, false);
        verify(control.data(), bytes, seed, "AIO_WRITE_OWN_HOST");
    } catch (...) {
        if (ctx) cleanup(syscall(SYS_io_destroy, ctx), "io_destroy failed case");
        cleanup(close(fd), "close failed AIO file");
        throw;
    }
    cleanup(syscall(SYS_io_destroy, ctx), "io_destroy");
    cleanup(close(fd), "close AIO file");
}

static int worker(const MultiOptions& opt) {
    const size_t count = opt.devices.size(), total = opt.bytes * count;
    const size_t reserve_bytes = (total + va_alignment - 1) & ~(va_alignment - 1);
    const int rank = opt.rank, device = opt.devices[rank], node = opt.nodes[rank];
    static std::string role;
    role = "rank=" + std::to_string(rank) + " device=" + std::to_string(device);
    log_role = role.c_str();
    bool acl_init = false, device_set = false;
    void* base = nullptr;
    std::vector<drv_mem_handle_t*> handles(count, nullptr);
    std::vector<bool> mapped(count, false);
    int result = 0;
    auto barrier = [&] { send_word(opt.fd, 0); check(receive_word(opt.fd), "barrier"); };
    try {
        log_line(0, "WORKER rank=%d pid=%d device=%d numa=%d bytes_each=%zu", rank, getpid(), device, node, opt.bytes);
        bind_cpu(node);
        log_line(0, "INIT rank=%d pid=%d aclInit(config=null) -> aclrtSetDevice(device=%d)", rank, getpid(), device);
        check(aclInit(nullptr), "aclInit"); acl_init = true;
        check(aclrtSetDevice(device), "aclrtSetDevice"); device_set = true;
        aclrtContext current = nullptr;
        check(aclrtGetCurrentContext(&current), "aclrtGetCurrentContext");
        if (!current) throw std::runtime_error("aclrtSetDevice did not establish a current context");
        int32_t current_device = -1;
        check(aclrtGetDevice(&current_device), "aclrtGetDevice");
        if (current_device != device) throw std::runtime_error("current ACL device differs from requested device");
        log_line(0, "CONTEXT rank=%d pid=%d device=%d ptr=%p init=aclrtSetDevice", rank, getpid(), current_device, current);

        log_line(1, "CALL halMemAddressReserve(size=%zu, alignment=0, addr=null, flags=0) data_bytes=%zu", reserve_bytes, total);
        check(halMemAddressReserve(&base, reserve_bytes, 0, nullptr, 0), "reserve independent local VA");
        log_line(2, "OUTPUT base=%p required_alignment=%zu", base, va_alignment);
        if (!base || reinterpret_cast<uintptr_t>(base) % va_alignment)
            throw std::runtime_error("local base is null or not 1 GiB aligned");
        log_line(0, "LOCAL_VA rank=%d pid=%d base=%p total_bytes=%zu reserve_bytes=%zu va_alignment=%zu",
               rank, getpid(), base, total, reserve_bytes, va_alignment);
        drv_mem_prop prop{};
        prop.side = MEM_HOST_NUMA_SIDE; prop.devid = node;
        prop.pg_type = MEM_HUGE_PAGE_TYPE; prop.mem_type = MEM_DDR_TYPE;
        log_line(1, "CALL halMemCreate(owner=%d, bytes=%zu, side=MEM_HOST_NUMA_SIDE, numa=%u, pg_type=MEM_HUGE_PAGE_TYPE, mem_type=MEM_DDR_TYPE, flags=0)", rank, opt.bytes, prop.devid);
        check(halMemCreate(&handles[rank], opt.bytes, &prop, 0), "halMemCreate HOST_NUMA");
        log_line(0, "NUMA_ALLOCATION rank=%d requested_node=%d side=%u bytes=%zu",
               rank, node, static_cast<unsigned>(prop.side), opt.bytes);
        using QueryProperties = drvError_t (*)(drv_mem_prop*, drv_mem_handle_t*);
        auto query_properties = reinterpret_cast<QueryProperties>(dlsym(RTLD_DEFAULT, "halMemGetAllocationPropertiesFromHandle"));
        if (query_properties) {
            drv_mem_prop actual{};
            check(query_properties(&actual, handles[rank]), "query allocation properties");
            log_line(0, "NUMA_PROPERTIES rank=%d side=%u reported_node=%u", rank, static_cast<unsigned>(actual.side), actual.devid);
            if (actual.side != MEM_HOST_NUMA_SIDE || actual.devid != static_cast<uint32_t>(node))
                throw std::runtime_error("NUMA allocation properties mismatch");
        } else log_line(1, "NUMA_PROPERTIES query API unavailable; allocation still uses explicit HOST_NUMA, no fallback");
        uint64_t token;
        log_line(0, "SHARE export own Host buffer owner=%d", rank);
        log_line(1, "CALL halMemExportToShareableHandle(handle=%p, type=MEM_HANDLE_TYPE_NONE, flags=0)", static_cast<void*>(handles[rank]));
        check(halMemExportToShareableHandle(handles[rank], MEM_HANDLE_TYPE_NONE, 0, &token), "export NUMA allocation");
        ShareHandleAttr attr{}; attr.enableFlag = SHR_HANDLE_NO_WLIST_ENABLE;
        check(halMemShareHandleSetAttribute(token, SHR_HANDLE_ATTR_NO_WLIST_IN_SERVER, attr), "enable same-server sharing");
        log_line(2, "OUTPUT token=0x%llx", (unsigned long long)token);
        send_word(opt.fd, token);
        for (size_t owner = 0; owner < count; ++owner) {
            token = receive_word(opt.fd);
            if (owner != static_cast<size_t>(rank)) {
                log_line(0, "IMPORT owner=%zu creator_device=%d target=DEVICE target_device=%d", owner, opt.devices[owner], device);
                log_line(1, "CALL halMemImportFromShareableHandle(token=0x%llx, devid=%d, out_handle=%p)",
                         (unsigned long long)token, device, static_cast<void*>(&handles[owner]));
                check(halMemImportFromShareableHandle(token, device, &handles[owner]), "import peer as DEVICE");
                log_line(2, "OUTPUT handle=%p", static_cast<void*>(handles[owner]));
            }
            void* slice = static_cast<char*>(base) + owner * opt.bytes;
            log_line(1, "CALL halMemMap(owner=%zu, view=%s, va=%p, size=%zu, offset=0, handle=%p, flags=0)",
                     owner, owner == static_cast<size_t>(rank) ? "HOST" : "DEVICE", slice, opt.bytes, static_cast<void*>(handles[owner]));
            check(halMemMap(slice, opt.bytes, 0, handles[owner], 0), "map contiguous slice"); mapped[owner] = true;
            log_line(0, "SLICE rank=%d owner=%zu owner_numa=%d va=%p bytes=%zu", rank, owner, opt.nodes[owner], slice, opt.bytes);
        }
        barrier();

        void* own_host = static_cast<char*>(base) + rank * opt.bytes;
        log_line(0, "TEST CPU owner=%d view=HOST bytes=%zu", rank, opt.bytes);
        fill(own_host, opt.bytes, 100 + rank);
        verify(own_host, opt.bytes, 100 + rank, "CPU_OWN_HOST");
        log_line(0, "TEST BUFFERED_AIO owner=%d view=HOST bytes=%zu", rank, opt.bytes);
        buffered_aio(own_host, opt.bytes, 200 + rank, opt.io_dir);
        log_line(0, "CASE_PASS CPU_BUFFERED_AIO owner=%d bytes=%zu", rank, opt.bytes);
        barrier();

        {
            AclResources acl;
            log_line(0, "SETUP HBM and stream copy_bytes=%zu", opt.bytes);
            log_line(1, "CALL aclrtMalloc(size=%zu, policy=ACL_MEM_MALLOC_HUGE_FIRST)", opt.bytes);
            check(aclrtMalloc(&acl.device, opt.bytes, ACL_MEM_MALLOC_HUGE_FIRST), "allocate one-block local NPU HBM");
            DVattribute attr{};
            check(drvMemGetAttribute(reinterpret_cast<DVdeviceptr>(acl.device), &attr), "query local HBM device");
            if (attr.devId != static_cast<uint32_t>(device)) throw std::runtime_error("ACL/HAL device ID mismatch");
            check(aclrtCreateStream(&acl.stream), "aclrtCreateStream");
            std::vector<uint64_t> staging(opt.bytes / sizeof(uint64_t));
            auto copy = [&](void* dst, void* src, aclrtMemcpyKind kind, const char* purpose) {
                const char* direction = kind == ACL_MEMCPY_HOST_TO_DEVICE ? "H2D" :
                                        kind == ACL_MEMCPY_DEVICE_TO_HOST ? "D2H" : "D2D";
                log_line(1, "CALL aclrtMemcpyAsync(dst=%p, dest_max=%zu, src=%p, count=%zu, kind=%s, stream=%p) purpose=%s",
                         dst, opt.bytes, src, opt.bytes, direction, acl.stream, purpose);
                check(aclrtMemcpyAsync(dst, opt.bytes, src, opt.bytes, kind, acl.stream), purpose);
                log_line(1, "CALL aclrtSynchronizeStream(stream=%p)", acl.stream);
                check(aclrtSynchronizeStream(acl.stream), "sync block copy");
            };
            for (size_t round = 0; round < read_rounds; ++round) {
                const uint64_t own_seed = 300 + round * count + rank;
                log_line(0, "TEST OWNER_CPU_WRITE round=%zu owner=%d ptr=%p bytes=%zu seed=%llu",
                         round, rank, own_host, opt.bytes, (unsigned long long)own_seed);
                fill(own_host, opt.bytes, own_seed);
                verify(own_host, opt.bytes, own_seed, "OWNER_CPU_DATA");
                // All owners finish writing before any reader starts; no next-round writes until all readers finish.
                barrier();
                for (size_t owner = 0; owner < count; ++owner) {
                    void* slice = static_cast<char*>(base) + owner * opt.bytes;
                    const uint64_t seed = 300 + round * count + owner;
                    const bool local = owner == static_cast<size_t>(rank);
                    const auto kind = local ? ACL_MEMCPY_HOST_TO_DEVICE : ACL_MEMCPY_DEVICE_TO_DEVICE;
                    log_line(0, "TEST READ_ONLY_BLOCK round=%zu reader=%d owner=%zu view=%s path=%s src=%p bytes=%zu seed=%llu",
                             round, rank, owner, local ? "HOST" : "DEVICE", local ? "H2D" : "D2D", slice, opt.bytes, (unsigned long long)seed);
                    log_line(1, "CALL aclrtMemset(ptr=%p, max_count=%zu, value=0, count=%zu)", acl.device, opt.bytes, opt.bytes);
                    check(aclrtMemset(acl.device, opt.bytes, 0, opt.bytes), "clear HBM before shared read");
                    copy(acl.device, slice, kind, "read owner-written shared block into HBM");
                    memset(staging.data(), 0, opt.bytes);
                    copy(staging.data(), acl.device, ACL_MEMCPY_DEVICE_TO_HOST, "HBM readback for CPU verification");
                    const std::string phase = "OWNER_WRITE_READER_VERIFY round=" + std::to_string(round) + " owner=" + std::to_string(owner);
                    verify(staging.data(), opt.bytes, seed, phase.c_str());
                }
                verify(own_host, opt.bytes, own_seed, "OWNER_DATA_UNCHANGED_AFTER_READS");
                barrier();
            }
            log_line(0, "CASE_PASS OWNER_WRITE_READ_ONLY rounds=%zu own_h2d_blocks=1 imported_d2d_blocks=%zu copy_bytes=%zu", read_rounds, count - 1, opt.bytes);
        }
        barrier();
        // Unmap everywhere before any owner releases its allocation.
        for (size_t i = 0; i < count; ++i) {
            check(halMemUnmap(static_cast<char*>(base) + i * opt.bytes), "unmap slice"); mapped[i] = false;
        }
        barrier();
    } catch (const std::exception& error) {
        log_line(0, "RESULT FAIL rank=%d phase=%s", rank, error.what()); result = 1;
    }
    for (size_t i = 0; i < count; ++i) {
        if (mapped[i]) cleanup(halMemUnmap(static_cast<char*>(base) + i * opt.bytes), "unmap on failure");
        if (handles[i]) cleanup(halMemRelease(handles[i]), "release handle");
    }
    if (base) cleanup(halMemAddressFree(base), "free contiguous VA");
    if (device_set) {
        log_line(0, "CLEANUP rank=%d pid=%d aclrtResetDevice(device=%d)", rank, getpid(), device);
        cleanup(aclrtResetDevice(device), "aclrtResetDevice");
    }
    if (acl_init) cleanup(aclFinalize(), "aclFinalize");
    close(opt.fd);
    result = result || cleanup_failed;
    log_line(0, "WORKER_RESULT rank=%d ret=%d", rank, result);
    return result;
}

int main(int argc, char** argv) {
    log_role = "rank=coordinator";
    setvbuf(stdout, nullptr, _IONBF, 0);
    MultiOptions opt;
    std::vector<int> sockets;
    std::vector<pid_t> children;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string key = argv[i];
            if (key == "--help") {
                puts("multi_share --devices 1,2,4 --numa-nodes 6,4,0 --size-mib 32 --io-dir DIR\n"
                     "One exec worker per device; equal NUMA Host allocations; independent local contiguous VA.\n"
                     "Each worker reserves with addr=null; virtual addresses need not match across processes.\n"
                     "VA base and reserved length are 1 GiB aligned; physical allocation sizes are unchanged.\n"
                     "Only owners write shared Host buffers. Two rounds: own H2D / imported Device D2D reads into HBM.\n"
                     "D2H targets a private verification buffer; no device copy writes into any shared mapping.\n"
                     "Initialize with aclInit + aclrtSetDevice; HAL allocates/shares Host memory. No Direct IO.\n"
                     "Unset ASCEND_RT_VISIBLE_DEVICES; IDs refer to the HAL device namespace.\n"
                     "Use timeout -k 5 180 externally. Size is per process and must be a positive even MiB value.");
                return 0;
            }
            if (++i == argc) throw std::runtime_error("missing option value");
            std::string value = argv[i];
            if (key == "--devices") opt.devices = list(value);
            else if (key == "--numa-nodes") opt.nodes = list(value);
            else if (key == "--io-dir") opt.io_dir = value;
            else if (key == "--size-mib") {
                auto mib = number(value);
                if (!mib || mib % 2 || mib > SIZE_MAX / (1024 * 1024)) throw std::runtime_error("invalid size MiB");
                opt.bytes = mib * 1024 * 1024;
            } else if (key == "--rank" || key == "--worker-fd") {
                auto n = number(value);
                if (n > INT_MAX) throw std::runtime_error("worker integer out of range");
                if (key == "--rank") opt.rank = n; else opt.fd = n;
            } else throw std::runtime_error("unknown option: " + key);
        }
        size_t count = opt.devices.size();
        if (!count || opt.nodes.size() != count || count > 64 || opt.bytes > SIZE_MAX / count)
            throw std::runtime_error("specify 1..64 devices and one NUMA node per device; total size must not overflow");
        if (opt.bytes * count > SIZE_MAX - (va_alignment - 1))
            throw std::runtime_error("1 GiB aligned reservation size overflows");
        if (std::set<int>(opt.devices.begin(), opt.devices.end()).size() != count)
            throw std::runtime_error("duplicate device");
        if (getenv("ASCEND_RT_VISIBLE_DEVICES")) throw std::runtime_error("unset ASCEND_RT_VISIBLE_DEVICES to avoid device renumbering");
        if (opt.rank >= 0 || opt.fd >= 0) {
            if (opt.rank < 0 || static_cast<size_t>(opt.rank) >= count || opt.fd < 0)
                throw std::runtime_error("invalid worker rank/fd");
            return worker(opt);
        }
        for (size_t rank = 0; rank < count; ++rank) {
            int pair[2];
            if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) throw std::runtime_error("socketpair");
            timeval timeout{120, 0};
            for (int fd : pair) {
                if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
                    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) {
                    close(pair[0]); close(pair[1]); throw std::runtime_error("socket timeout");
                }
            }
            std::vector<std::string> args(argv, argv + argc);
            args.insert(args.end(), {"--rank", std::to_string(rank), "--worker-fd", std::to_string(pair[1])});
            std::vector<char*> raw;
            for (auto& arg : args) raw.push_back(arg.data());
            raw.push_back(nullptr);
            pid_t parent = getpid(), pid = fork();
            if (pid < 0) { close(pair[0]); close(pair[1]); throw std::runtime_error("fork"); }
            if (!pid) {
                prctl(PR_SET_PDEATHSIG, SIGTERM);
                if (getppid() != parent) _exit(125);
                for (int fd : sockets) close(fd);
                close(pair[0]);
                execv("/proc/self/exe", raw.data()); _exit(127);
            }
            close(pair[1]); sockets.push_back(pair[0]); children.push_back(pid);
            log_line(0, "WORKER_STARTED rank=%zu pid=%d device=%d numa=%d", rank, pid, opt.devices[rank], opt.nodes[rank]);
        }
        std::vector<uint64_t> tokens;
        for (size_t rank = 0; rank < sockets.size(); ++rank) {
            log_line(0, "WAIT_EXPORT rank=%zu child_pid=%d", rank, children[rank]);
            tokens.push_back(receive_word(sockets[rank]));
        }
        for (int fd : sockets) for (auto token : tokens) send_word(fd, token);
        const size_t barriers = 2 * read_rounds + 4;
        for (size_t phase = 0; phase < barriers; ++phase) {
            for (int fd : sockets) check(receive_word(fd), "worker barrier arrived");
            for (int fd : sockets) send_word(fd, 0);
        }
        for (int fd : sockets) close(fd);
        sockets.clear();
        int failed = 0;
        for (auto& child : children) {
            int status = 0; pid_t ret;
            do { ret = waitpid(child, &status, 0); } while (ret < 0 && errno == EINTR);
            if (ret < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) failed = 1;
            log_line(0, "CHILD_RESULT pid=%d status=%d", child, status); child = -1;
        }
        log_line(0, "MULTI_RESULT %s workers=%zu bytes_each=%zu total=%zu va_mode=independent",
               failed ? "FAIL" : "PASS", count, opt.bytes, count * opt.bytes);
        return failed;
    } catch (const std::exception& error) {
        log_line(0, "MULTI_ERROR %s", error.what());
        for (int fd : sockets) close(fd);
        for (pid_t child : children) if (child > 0) kill(child, SIGTERM);
        for (pid_t child : children) if (child > 0) while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
        return children.empty() ? 2 : 1;
    }
}
