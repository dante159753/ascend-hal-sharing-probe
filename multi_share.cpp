#include "cli.hpp"
#include <dlfcn.h>
#include <sched.h>
#include <sys/prctl.h>
#include <signal.h>
#include <fstream>
#include <set>
#include <sstream>

static constexpr size_t va_alignment = size_t{1} << 30;

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
    printf("CPU_BIND pid=%d numa=%d node_cpus=%s selected_count=%d\n", getpid(), node, cpus.c_str(), CPU_COUNT(&selected));
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
        verify(ptr, bytes, seed, "AIO_READ_CONTIGUOUS_REGION");
        check(ftruncate(fd, 0), "truncate before AIO write");
        aio_operation(ctx, fd, ptr, bytes, true);
        memset(control.data(), 0, bytes);
        aio_operation(ctx, fd, control.data(), bytes, false);
        verify(control.data(), bytes, seed, "AIO_WRITE_CONTIGUOUS_REGION");
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
    bool acl_init = false, device_set = false;
    void* base = nullptr;
    std::vector<drv_mem_handle_t*> handles(count, nullptr);
    std::vector<bool> mapped(count, false);
    int result = 0;
    auto barrier = [&] { send_word(opt.fd, 0); check(receive_word(opt.fd), "barrier"); };
    try {
        printf("WORKER rank=%d pid=%d device=%d numa=%d bytes_each=%zu\n", rank, getpid(), device, node, opt.bytes);
        bind_cpu(node);
        printf("INIT rank=%d pid=%d aclInit(config=null) -> aclrtSetDevice(device=%d)\n", rank, getpid(), device);
        check(aclInit(nullptr), "aclInit"); acl_init = true;
        check(aclrtSetDevice(device), "aclrtSetDevice"); device_set = true;
        aclrtContext current = nullptr;
        check(aclrtGetCurrentContext(&current), "aclrtGetCurrentContext");
        if (!current) throw std::runtime_error("aclrtSetDevice did not establish a current context");
        int32_t current_device = -1;
        check(aclrtGetDevice(&current_device), "aclrtGetDevice");
        if (current_device != device) throw std::runtime_error("current ACL device differs from requested device");
        printf("CONTEXT rank=%d pid=%d device=%d ptr=%p init=aclrtSetDevice\n", rank, getpid(), current_device, current);

        printf("[pid=%d rank=%d]   CALL halMemAddressReserve(size=%zu, alignment=0, addr=null, flags=0) data_bytes=%zu\n",
               getpid(), rank, reserve_bytes, total);
        check(halMemAddressReserve(&base, reserve_bytes, 0, nullptr, 0), "reserve independent local VA");
        printf("[pid=%d rank=%d]     OUTPUT base=%p required_alignment=%zu\n", getpid(), rank, base, va_alignment);
        if (!base || reinterpret_cast<uintptr_t>(base) % va_alignment)
            throw std::runtime_error("local base is null or not 1 GiB aligned");
        printf("LOCAL_VA rank=%d pid=%d base=%p total_bytes=%zu reserve_bytes=%zu va_alignment=%zu\n",
               rank, getpid(), base, total, reserve_bytes, va_alignment);
        drv_mem_prop prop{};
        prop.side = MEM_HOST_NUMA_SIDE; prop.devid = node;
        prop.pg_type = MEM_HUGE_PAGE_TYPE; prop.mem_type = MEM_DDR_TYPE;
        check(halMemCreate(&handles[rank], opt.bytes, &prop, 0), "halMemCreate HOST_NUMA");
        printf("NUMA_ALLOCATION rank=%d requested_node=%d side=%u bytes=%zu\n",
               rank, node, static_cast<unsigned>(prop.side), opt.bytes);
        using QueryProperties = drvError_t (*)(drv_mem_prop*, drv_mem_handle_t*);
        auto query_properties = reinterpret_cast<QueryProperties>(dlsym(RTLD_DEFAULT, "halMemGetAllocationPropertiesFromHandle"));
        if (query_properties) {
            drv_mem_prop actual{};
            check(query_properties(&actual, handles[rank]), "query allocation properties");
            printf("NUMA_PROPERTIES rank=%d side=%u reported_node=%u\n", rank, static_cast<unsigned>(actual.side), actual.devid);
            if (actual.side != MEM_HOST_NUMA_SIDE || actual.devid != static_cast<uint32_t>(node))
                throw std::runtime_error("NUMA allocation properties mismatch");
        } else puts("NUMA_PROPERTIES query API unavailable; allocation still uses explicit HOST_NUMA, no fallback");
        uint64_t token;
        check(halMemExportToShareableHandle(handles[rank], MEM_HANDLE_TYPE_NONE, 0, &token), "export NUMA allocation");
        ShareHandleAttr attr{}; attr.enableFlag = SHR_HANDLE_NO_WLIST_ENABLE;
        check(halMemShareHandleSetAttribute(token, SHR_HANDLE_ATTR_NO_WLIST_IN_SERVER, attr), "enable same-server sharing");
        send_word(opt.fd, token);
        uint32_t host;
        check(halGetHostID(&host), "halGetHostID");
        for (size_t owner = 0; owner < count; ++owner) {
            token = receive_word(opt.fd);
            if (owner != static_cast<size_t>(rank))
                check(halMemImportFromShareableHandle(token, host, &handles[owner]), "import peer as HOST");
            void* slice = static_cast<char*>(base) + owner * opt.bytes;
            check(halMemMap(slice, opt.bytes, 0, handles[owner], 0), "map contiguous slice"); mapped[owner] = true;
            printf("SLICE rank=%d owner=%zu owner_numa=%d va=%p bytes=%zu\n", rank, owner, opt.nodes[owner], slice, opt.bytes);
        }
        barrier();

        // One writer at a time; every process verifies every allocation after each write.
        for (size_t writer = 0; writer < count; ++writer) {
            uint64_t seed = 100 + writer;
            if (writer == static_cast<size_t>(rank)) fill(base, total, seed);
            barrier();
            verify(base, total, seed, "CPU_ALL_ALLOCATIONS");
            barrier();
        }
        printf("CASE_PASS rank=%d CPU total_bytes=%zu\n", rank, total);
        for (size_t writer = 0; writer < count; ++writer) {
            uint64_t seed = 200 + writer;
            if (writer == static_cast<size_t>(rank)) buffered_aio(base, total, seed, opt.io_dir);
            barrier();
            verify(base, total, seed, "PEER_SEES_AIO_WRITE");
            barrier();
        }
        printf("CASE_PASS rank=%d BUFFERED_AIO total_bytes=%zu\n", rank, total);

        {
            AclResources acl;
            check(aclrtMalloc(&acl.device, total, ACL_MEM_MALLOC_HUGE_FIRST), "allocate local NPU HBM");
            DVattribute attr{};
            check(drvMemGetAttribute(reinterpret_cast<DVdeviceptr>(acl.device), &attr), "query local HBM device");
            if (attr.devId != static_cast<uint32_t>(device)) throw std::runtime_error("ACL/HAL device ID mismatch");
            check(aclrtCreateStream(&acl.stream), "aclrtCreateStream");
            for (size_t writer = 0; writer < count; ++writer) {
                uint64_t seed = 300 + writer;
                if (writer == static_cast<size_t>(rank)) {
                    fill(base, total, seed);
                    check(aclrtMemcpyAsync(acl.device, total, base, total, ACL_MEMCPY_HOST_TO_DEVICE, acl.stream), "ACL async H2D full region");
                    check(aclrtSynchronizeStream(acl.stream), "sync H2D");
                    memset(base, 0, total);
                    check(aclrtMemcpyAsync(base, total, acl.device, total, ACL_MEMCPY_DEVICE_TO_HOST, acl.stream), "ACL async D2H full region");
                    check(aclrtSynchronizeStream(acl.stream), "sync D2H");
                    verify(base, total, seed, "ACL_ASYNC_ROUNDTRIP_ALL_ALLOCATIONS");
                }
                barrier();
                verify(base, total, seed, "PEER_SEES_D2H_WRITE");
                barrier();
            }
            printf("CASE_PASS rank=%d ACL_ASYNC_H2D_D2H total_bytes=%zu\n", rank, total);
        }
        barrier();
        // Unmap everywhere before any owner releases its allocation.
        for (size_t i = 0; i < count; ++i) {
            check(halMemUnmap(static_cast<char*>(base) + i * opt.bytes), "unmap slice"); mapped[i] = false;
        }
        barrier();
    } catch (const std::exception& error) {
        printf("RESULT FAIL rank=%d phase=%s\n", rank, error.what()); result = 1;
    }
    for (size_t i = 0; i < count; ++i) {
        if (mapped[i]) cleanup(halMemUnmap(static_cast<char*>(base) + i * opt.bytes), "unmap on failure");
        if (handles[i]) cleanup(halMemRelease(handles[i]), "release handle");
    }
    if (base) cleanup(halMemAddressFree(base), "free contiguous VA");
    if (device_set) {
        printf("CLEANUP rank=%d pid=%d aclrtResetDevice(device=%d)\n", rank, getpid(), device);
        cleanup(aclrtResetDevice(device), "aclrtResetDevice");
    }
    if (acl_init) cleanup(aclFinalize(), "aclFinalize");
    close(opt.fd);
    result = result || cleanup_failed;
    printf("WORKER_RESULT rank=%d ret=%d\n", rank, result);
    return result;
}

int main(int argc, char** argv) {
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
                     "CPU, buffered AIO and ACL asynchronous H2D/D2H over the entire shared region.\n"
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
        }
        std::vector<uint64_t> tokens;
        for (int fd : sockets) tokens.push_back(receive_word(fd));
        for (int fd : sockets) for (auto token : tokens) send_word(fd, token);
        const size_t barriers = 6 * count + 3;
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
            printf("CHILD_RESULT pid=%d status=%d\n", child, status); child = -1;
        }
        printf("MULTI_RESULT %s workers=%zu bytes_each=%zu total=%zu va_mode=independent\n",
               failed ? "FAIL" : "PASS", count, opt.bytes, count * opt.bytes);
        return failed;
    } catch (const std::exception& error) {
        fprintf(stderr, "MULTI_ERROR %s\n", error.what());
        for (int fd : sockets) close(fd);
        for (pid_t child : children) if (child > 0) kill(child, SIGTERM);
        for (pid_t child : children) if (child > 0) while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
        return children.empty() ? 2 : 1;
    }
}
