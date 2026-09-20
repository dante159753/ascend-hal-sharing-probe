# Ascend HAL 跨进程内存测试

用于在 A5/Ascend 950 等环境验证同一台机器上两种共享方式：

1. **Host → Host**：进程 A 用 HAL 分配 Host DDR，进程 B 导入为 Host。双方 CPU 读写同一块内存，双方分别执行 ACL 异步 H2D/D2H，以及 buffered / O_DIRECT Linux AIO。
2. **Host → Device**：`device_share` 使用 TSD/HAL 初始化设备 0，将 A 的 Host DDR 导入为设备 0 的映射，独立分配 HBM，验证 Host 源 H2D、导入映射与 HBM 的双向 D2D 和跨进程写回。Host DDR 和 HBM 均使用普通页。

**这是能力探测程序，失败会返回非零退出码和具体阶段。A5 尚未实机验证，不能把“能编译”当成“接口支持”。** 不修改驱动、模型、UCM 或设备配置。`host_share` 和 `multi_share` 使用 ACL 初始化；`device_share` 使用 TSD/HAL 初始化。

## 编译

### 原样保留的 Host UVA 共享文件

`host_uva_share.cpp` 原样保存用户提供的文件，含换行和空白；SHA256 为 `1fcb4c0ea9d7674880601475a87d1831102a039438069a31e30f6e2aec37ff81`。另行提供 `common.h` 中的 SDK includes、日志和错误检查宏。该文件使用 C++20 指定成员初始化，CMake 为这个目标单独启用 C++20。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target host_uva_share -j
```

该目标需要环境提供 `MEM_RSV_TYPE_HOST_UVA`。2026-09-20 在 A2 0.23.0/CANN 9.1.0 中编译失败，原因是已安装头文件没有这个定义；原参考 demo 的 `common.h` 也未定义该常量，需要实际构建环境提供其定义。程序没有将这个 flag 替换为其他值。为保持其他测试可编译，该目标不加入默认构建，需按上面命令单独构建。

源码固定分配 2 GiB，writer 和 reader 分别手动运行。原程序的 goto 清理路径和校验失败后仍返回 0 的行为也原样保留；验证数据是否通过应检查 `Data verified OK!` 及错误日志，不能只看退出码。本次没有运行该程序。

### 单文件 Host 内存文件 I/O demo

`hal_host_io.cpp` 可单独复制到目标机器编译，不依赖本仓库的其他文件或 CMake。仍需已安装的 CANN/HAL 开发头文件及库。

程序执行 `aclInit → aclrtSetDevice(0)`，以 `Reserve → halMemCreate → Map` 申请 2 MiB、`MEM_HOST_SIDE / MEM_NORMAL_PAGE_TYPE / MEM_DDR_TYPE` 的 Host 内存，分别执行 O_DIRECT 和普通文件的同步 `pwrite/pread` 并全量校验。文件按 PID 命名，结束后删除；一个模式失败仍继续另一个模式。任一测试或清理失败返回 1。它不是 Linux AIO 测试。

```bash
source /usr/local/Ascend/ascend-toolkit/set_env.sh
CANN_ROOT=/usr/local/Ascend/ascend-toolkit/latest
DRIVER_ROOT=/usr/local/Ascend/driver
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic hal_host_io.cpp -o hal_host_io \
  -I "$CANN_ROOT/include" -I "$CANN_ROOT/include/driver" -I "$DRIVER_ROOT/include" \
  -L "$CANN_ROOT/lib64" -L "$DRIVER_ROOT/lib64/driver" \
  -Wl,-rpath,"$CANN_ROOT/lib64" -Wl,-rpath,"$DRIVER_ROOT/lib64/driver" \
  -lascendcl -lascend_hal

./hal_host_io /mnt/txh/yz
```

目录参数可省略，默认当前目录；目录须已存在。本例自动选址，没有跨进程指定 VA，因此按实际 2 MiB 预留即可。2026-09-20 在 A2 0.23.0/CANN 9.1.0 容器中直接编译和参数检查通过，本次未执行硬件和文件 I/O 测试。

### 编译其他共享测试

需要 Linux、C++17 编译器、CMake ≥ 3.18、Python 3，以及本机 CANN toolkit 和 Ascend 驱动开发库。Linux AIO 使用内核 syscall，不依赖 libaio 开发包。不需要编译或安装 vLLM/UCM。

```bash
git clone https://github.com/dante159753/ascend-hal-sharing-probe.git
cd ascend-hal-sharing-probe

# 按实际 CANN 安装路径选择 set_env.sh；不要覆盖为其他机器的 toolkit。
source /usr/local/Ascend/ascend-toolkit/set_env.sh

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCANN_ROOT=/usr/local/Ascend/ascend-toolkit/latest \
  -DDRIVER_ROOT=/usr/local/Ascend/driver
cmake --build build -j

export LD_LIBRARY_PATH=/usr/local/Ascend/driver/lib64/driver:/usr/local/Ascend/driver/lib64:${LD_LIBRARY_PATH:-}
ldd build/device_share
```

确认 `libascend_hal.so` 来自实际驱动目录。`device_share` 不链接 `libascendcl.so` / `libruntime.so`；运行时需能找到 CANN 的 `libtsdclient.so`。其他 ACL 测试的库应来自所选 CANN。CMake 不搜索 toolkit 中的 HAL stub 库。自定义安装路径用 `CANN_ROOT` / `DRIVER_ROOT` 指定，也需相应调整环境变量。

在容器中运行时使用 **vLLM-Ascend 0.23.0 或更新且适配 A5 的镜像**，挂载本机驱动、目标设备节点及 ext4/XFS 测试目录；不要挂载其他版本 toolkit 覆盖镜像。具体容器设备授权按机器已有运维配置执行。测试程序不需要访问所有 NPU。

## Device 共享：TSD/HAL 初始化和 HAL 拷贝

```bash
git pull --ff-only origin main
source /usr/local/Ascend/ascend-toolkit/set_env.sh
export LD_LIBRARY_PATH=/usr/local/Ascend/driver/lib64/driver:${LD_LIBRARY_PATH:-}
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target device_share -j
unset ASCEND_RT_VISIBLE_DEVICES
timeout -k 5 120 build/device_share --size-mib 2
```

两个进程都固定使用 **HAL device 0**，导入目标也是 0。可省略 `--device`，若显式传入则只接受 `--device 0`。`--size-mib` 默认为 2，接受任意正整数 MiB（包括 1、3）。旧的 `--via-host` 已移除。

流程为：

1. 每个独立 exec 进程执行 `TsdOpen(0, 0) → halSetRuntimeApiVer → halDeviceOpen(0)`。
2. Creator 按 `MEM_HOST_SIDE, devid=0, module_id=0, MEM_NORMAL_PAGE_TYPE, MEM_DDR_TYPE` 分配并映射 Host DDR，由 CPU 写入模式 101。两个进程还各自独立分配 HBM，属性为 `MEM_DEV_SIDE, devid=0, MEM_NORMAL_PAGE_TYPE, MEM_HBM_TYPE`；均使用 Reserve → Create → Map。
3. Creator 执行 `halMemcpy(HBM目的, bytes, 共享Host源, bytes, H2D信息)`，再 D2H 到普通 Host 校验 buffer，全量校验模式 101。
4. 导出 handle 并启用同服务器共享权限。Importer 执行 Reserve → `halMemImportFromShareableHandle(handle, 0, ...)` → Map。
5. Importer 将导入的 Device 映射 D2D 拷贝到自己的 HBM，再 D2H 到普通 Host buffer，校验模式 101。
6. Importer 在普通 Host buffer 中生成模式 202，H2D 到 HBM，再 D2D 到导入的共享映射（此时共享映射是目的地址）。D2H 读回该映射校验，通知 Creator 通过 CPU 校验共享 Host DDR 已变为模式 202。
7. Importer 释放映射和导入 handle 后，Creator 才释放原始分配；各自释放 HBM，最后关闭 HAL 和 TSD。

Creator 可由 CPU 读写原始 Host 映射；Importer 不直接解引用 Device 映射。所有内存拷贝使用 HAL，不调用 ACL，不执行 AIO。`memcpy_info` 非空，`devid=0`，每次显式设置方向，不自动降级或回退大页。成功应看到 `PASS CREATOR_HOST_TO_HBM`、`PASS IMPORTER_SHARED_TO_HBM`、`PASS IMPORTER_WRITE_READBACK`、`PASS CREATOR_SEES_IMPORTER_WRITE` 和双方 `RESULT PASS`。

2026-09-20 在 A2 的 0.23.0/CANN 9.1.0 容器中编译和参数检查通过，确认可执行文件不链接 ACL/runtime。由于 A2 卡 0 有其他进程，本次没有运行固定设备 0 的硬件用例；A5 结果需在目标环境验证。

### Device 日志格式

每行以 `[pid=进程号 creator|importer|launcher]` 开头。顶层 `SETUP / SHARE / TEST / SYNC / CLEANUP / PASS / RESULT` 表示阶段和测试结果；缩进两格的 `CALL` 打印调用前的真实参数，缩进四格的 `RETURN / OUTPUT` 打印返回值、VA、handle 或共享 token。`halMemcpy` 明确列出 `dst`、`src`、字节数、方向名称/数值和设备号。IPC 日志区分发送、等待和收到消息；最后 `PROCESS_RESULT` 汇总两个进程的退出状态。

格式示意（地址、PID 为示例，不是实测结果）：

```text
[pid=123 creator] TEST CREATOR_HOST_TO_HBM: shared Host source -> H2D -> separate HBM -> D2H -> verify seed 101
[pid=123 creator]   CALL halMemcpy(dst=0x200000, dest_max=1048576, src=0x100000, count=1048576, info={dir=DRV_MEMCPY_HOST_TO_DEVICE(1), devid=0})
[pid=123 creator]     RETURN H2D shared HOST source -> HBM destination ret=0 [OK]
[pid=124 importer]   IPC WAIT fd=4
```

调用信息在执行前打印，即使接口失败或卡住也能定位。单行先组装再写出，减少两个进程输出在同一行内交错。可保存日志后按 PID 或角色筛选：

```bash
set -o pipefail
timeout -k 5 120 build/device_share --size-mib 1 2>&1 | tee device-share.log
grep ' creator]' device-share.log
grep ' importer]' device-share.log
```

## 多卡 NUMA Host 共享 demo

`multi_share` 为每张指定的卡启动一个独立 exec 工作进程，另有一个仅交换 handle 和同步消息的协调进程。工作进程使用 **`aclInit(nullptr) → aclrtSetDevice(device_id)`** 初始化；Runtime 内部负责 TSD、HAL device open 和当前 context。程序查询并验证当前 context 非空、当前设备与指定设备一致。随后仍用 HAL 分配、导出、导入和映射 NUMA Host 内存。不手动调用 `halDeviceOpen`、`aclrtCreateContext` 或 HostRegister。

```bash
# 首次使用先按上面的“编译”章节配置 CANN 和驱动库路径。
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target multi_share -j

# 按本机拓扑选择测试卡和对应 NUMA 节点。
npu-smi info -t topo
lscpu
mkdir -p "$HOME/hal-probe-io"
unset ASCEND_RT_VISIBLE_DEVICES

timeout -k 5 180 build/multi_share --devices 1,2,4 --numa-nodes 6,4,0 --size-mib 32 --io-dir "$HOME/hal-probe-io"
```

- `--devices`：不重复的设备 ID，数量为 1～64；容器需暴露全部选中设备。
- `--numa-nodes`：与设备列表一一对应的 Host NUMA 节点号。上例对应测试 A2，其他机器按实际拓扑填写。
- `--size-mib`：**每个进程**的 Host 分配大小，默认 2 MiB，必须是正的 2 MiB 倍数。上例共分配 96 MiB Host 内存，每个进程都映射完整 96 MiB，并在自己的 NPU 上分配单块大小（32 MiB）的测试 HBM。
- `--io-dir`：已有、可写的测试目录。只执行 buffered Linux AIO，文件打开后立即 unlink，不测试 Direct I/O。

取消 `ASCEND_RT_VISIBLE_DEVICES`，避免 HAL 和 ACL 设备编号重排。程序会查询每个工作进程分配的 HBM 的 HAL device ID，确认与所选设备一致。

CPU affinity 绑定到所选 NUMA 节点与当前允许 CPU 集的交集。Host 分配使用 `MEM_HOST_NUMA_SIDE`、`prop.devid=numa_node`、`MEM_HUGE_PAGE_TYPE`、`MEM_DDR_TYPE`，不退回普通 Host 分配。若驱动提供属性查询接口，会核对属性；缺失时明确输出，不据此声称独立验证了物理页位置。

数据长度为 `进程数 × 单进程大小`，VA 预留长度向上取整到 **1 GiB**。每个进程都通过 `halMemAddressReserve(..., addr=nullptr, ...)` 独立自动选址，检查本地基址按 **1 GiB** 对齐。进程间不交换基址、不要求地址相同，只交换共享 handle 和同步消息。HAL 的 `alignment` 参数是保留参数，仍传 0。

日志 `LOCAL_VA` 分别打印各进程的 `base`、`total_bytes`（实际数据长度）、`reserve_bytes`（对齐后的 VA 长度）和 `va_alignment`。例如三进程各 32 MiB，实际共享数据为 96 MiB，每个进程预留 1 GiB VA；额外 VA 不做物理内存分配或映射。交换 handle 后，自己的 handle 映射为 Host；其他进程的 handle 使用当前 rank 的设备 ID 调用 `halMemImportFromShareableHandle(token, device_id, ...)`，导入为 Device 映射。各块按 `local_base + owner_rank × bytes_each` 排列。每个进程内部仍连续；不同进程通过相同块序号和块内偏移访问同一份共享数据，不能直接交换本地指针。驱动可能恰好返回相同地址，程序不依赖这一点。

CPU 和 buffered AIO 只访问每个 rank 自己创建的 Host 块，不能直接解引用导入的 Device 地址。每次 ACL copy 只覆盖一个 owner 的分配块：

| 执行进程与 buffer 的关系 | 共享块的数据来源 | 共享块读入本卡 HBM |
|---|---|---|
| 当前 rank 是 owner，原始 Host 映射 | owner CPU 写入 | H2D |
| 当前 rank 是 importer，Device 映射 | 只读取 owner 写入的数据 | D2D |

共享传输测试执行两轮，每轮所有 owner 用 CPU 写各自的 Host 块，数据 seed 为 `300 + round × rank数量 + owner`。通过 barrier 确保写入完成后，各 rank 逐块读取自己的 Host 映射（H2D）或其他 owner 的 Device 映射（D2D）到本卡 HBM，再 D2H 到私有普通 Host buffer 全量校验。每块读取前清空 HBM 和私有校验 buffer。共享映射从不作为 device copy 的目的地址，导入者不写回；D2H 只写私有校验 buffer。全部读者完成后才进入下一轮，避免读取与 owner 更新数据并发。

程序日志统一带 `[pid=... rank=... device=...]`；协调进程使用 `rank=coordinator` 并打印 rank/PID/device/NUMA 对应关系。`OWNER_CPU_WRITE` 列出写入者和 seed；`READ_ONLY_BLOCK` 列出 round、reader、owner、HOST/DEVICE 视图及传输路径；缩进的调用行列出实际地址、长度、方向和 stream。校验阶段名也包含 round 和 owner，因此 `MISMATCH` 与 `RESULT FAIL` 可直接定位到具体数据块。

释放 stream/HBM、全部共享映射、handle 和 VA 后，各进程执行 `aclrtResetDevice(device_id) → aclFinalize()`。初始化保持 `aclInit + aclrtSetDevice`，不自动退回 Host 导入。

2026-09-20 在 A2 910B3、vLLM-Ascend 0.23.0、CANN 9.1.0、驱动 25.5.2 上实测当前 Device 导入版本：

| 用例 | 结果 |
|---|---|
| 单 rank，设备 1、NUMA 6，2 MiB | CPU、buffered AIO、两轮 owner CPU 写入 → H2D → 私有 buffer D2H 校验通过，退出 0；没有导入块，不代表 D2D 通过 |
| 三 rank，设备 1/2/4、NUMA 6/4/0，每 rank 2 MiB | 三个 rank 的 Device 导入均返回 `8`，退出 1；尚未执行 D2D |

此前 Host 导入、单块 H2D/D2H 版本的三卡 2/32 MiB 用例通过，是另一条路径的结果。当前 owner 写、importer 只读版本的 A5 数据可见性仍需目标环境验证。成功时输出 `MULTI_RESULT PASS ... va_mode=independent`；`CASE_PASS OWNER_WRITE_READ_ONLY` 列出轮数、owner H2D 块数和 importer D2D 块数，单 rank 的 D2D 块数为 0。

## 一键运行两种模式

选一张空闲 NPU。以下示例选本机物理 NPU 0：

```bash
mkdir -p "$HOME/hal-probe-io"
findmnt -T "$HOME/hal-probe-io" -o TARGET,SOURCE,FSTYPE

unset ASCEND_RT_VISIBLE_DEVICES
python3 run_tests.py \
  --device 0 --hal-device 0 \
  --io-dir "$HOME/hal-probe-io" \
  --sizes-mib 2 32
```

默认运行 Host、Device 两类完整测试，以及纯 HAL 的 Host/Device 映射诊断。`device` suite 固定使用 HAL 设备 0，不受 runner 的 `--device` 参数影响。每例默认超时 120 秒，超时会终止该例两个进程所在进程组；其他用例继续。日志写入 `logs/<时间>-<PID>/`，`summary.json` 保存命令和退出码。可用 `--log-dir` 指定一个尚不存在的新目录。

两个设备参数不是同一命名空间：

- `--device`：仅用于 Host suite 的 ACL 可见逻辑序号。Device suite 固定使用 HAL device 0。
- `--hal-device`：纯 HAL 诊断的 HAL 设备编号；不经 ACL 可见设备列表重编号。

选择其他卡测试时单独运行 `--suite host` 或 `--suite hal`；`--suite device` 和 `--suite all` 包含固定设备 0 的用例，并要求取消 `ASCEND_RT_VISIBLE_DEVICES`。

**O_DIRECT 必须在支持它的文件系统中测试。** 不要把 `--io-dir` 指向 tmpfs（常见的 `/tmp`、`/dev/shm`）。程序只创建本例 PID 命名的临时文件，并在打开后立即 unlink；不删除目录内已有数据。

## 单独运行

```bash
# Host 共享：CPU + ACL async + 两种 AIO
ASCEND_RT_VISIBLE_DEVICES=0 timeout -k 5 120 build/host_share \
  --device 0 --size-mib 2 --io-dir "$HOME/hal-probe-io" --aio both

# 只测试 buffered AIO；也可选 direct 或 none
ASCEND_RT_VISIBLE_DEVICES=0 timeout -k 5 120 build/host_share \
  --device 0 --size-mib 32 --io-dir "$HOME/hal-probe-io" --aio buffered

# 固定设备 0：普通页 Host 源 H2D 到独立 HBM、Device 导入后双向 D2D
unset ASCEND_RT_VISIBLE_DEVICES
timeout -k 5 120 build/device_share --size-mib 2

# 跟参考 demo 一致的纯 HAL 初始化/导入顺序，不链接 ACL/runtime
timeout -k 5 120 build/hal_map_probe host 0 2
timeout -k 5 120 build/hal_map_probe device 0 2
```

`device_share` 的 `--size-mib` 接受任意正整数；其他程序仍要求正的 2 MiB 倍数。批量脚本单独选择 `--suite device` 时也接受奇数 MiB。程序默认设备 0；直接运行二进制时建议使用外部 `timeout`，批量脚本已经管理超时。完整程序不会自动降级到另一种拷贝方向或另一条初始化路径。

## 测试步骤与结果解释

所有程序在初始化运行时前 fork，子进程随后 exec，因此导入进程不继承创建者的 HAL 映射。Unix socket 仅传递共享 handle 和同步消息，不传 buffer 数据。两个映射同时存在，读写按消息协调，避免无同步的数据竞争。测试使用同机共享授权 `SHR_HANDLE_ATTR_NO_WLIST_IN_SERVER`，临时共享 handle 仅发给子进程；生产应用可改为 PID 白名单。

`host_share` 的分配属性是 `MEM_HOST_SIDE, devid=0, module_id=0, MEM_HUGE_PAGE_TYPE, MEM_DDR_TYPE, reserve=0`。Host 导入目标通过 `halGetHostID` 查询，不硬编码 65。导入顺序统一为 **Reserve → Import → Map**，所有 VMM offset/flags 为 0。不调用 HostRegister。

`host_share`：A 写入模式 101，B 全量验证并写入 202，A 全量验证。随后 A、B 分别测试各自映射的 ACL H2D/D2H；H2D 完成后清空 Host buffer，D2H 完成后逐项验证。32 次连续提交后查询事件、等待完成并记录耗时；事件 `NOT_READY` 是本次确实未完成的证据，立即完成本身不算功能失败。双方还执行原生 Linux AIO：四个对齐请求分别测试读和写，记录每个完成事件；普通对齐内存作相同文件的对照。AIO 写入使用不同于文件原内容的数据模式并回读验证。后续双方再互相验证共享数据。

`device_share`：共享 Host DDR → HAL H2D → 独立 HBM；另一进程按设备 0 导入后，验证共享映射与独立 HBM 的双向 D2D，并 D2H 读回校验。物理共享内存始终在 Host DDR，另行申请的 HBM 是不同的物理内存。详细步骤见上面的 Device 共享章节。

`hal_map_probe`：只调用 **halSetRuntimeApiVer → halDeviceOpen → Reserve → Import/Create → Map** 等 HAL 接口。Host 模式附带 CPU 双向共享校验；Device 模式只确认映射成功，**其 PASS 不代表 D2D 可用**。不会在 HAL 初始化后再混入 ACL 初始化。

- `PASS`：该项完成且数据验证通过（纯 HAL Device 例仅验证映射）。
- `CASE_RESULT ... FAIL`：该独立项失败；例如 Direct AIO 失败不会掩盖 buffered AIO 和 ACL 成功。
- `RESULT FAIL ... phase=...`：初始化、导入、映射等前置步骤失败，后续依赖步骤未运行。不能把 Import 失败写成 D2D 执行失败。
- `AIO submitted=4` 不代表 I/O 成功，还要看四个完成事件的 `res` 和数据校验。
- `res=-14` 是 `EFAULT`；`res=-22` 是 `EINVAL`。HAL 返回码属于另一套枚举，例如 `4=INVALID_HANDLE`、`8=PARA_ERROR`、`65534=NOT_SUPPORT`。
- 退出码：`0` 全部请求项通过，`1` 用例失败，`2` 参数/启动错误；runner 的 `124` 表示超时，`127` 表示无法启动。非法参数不触碰设备。
- Buffered AIO 功能通过不保证 `io_submit` 不阻塞；O_DIRECT 失败不等于 CPU 地址不可读写。

## 已知 A2 对照

此前在 A2 910B3、驱动 25.5.2/HAL 7.35.23、vLLM-Ascend 0.23.0/CANN 9.1.0 上：ACL 初始化的 Host 共享 CPU/ACL/buffered AIO 通过；O_DIRECT AIO 返回 EFAULT；旧 ACL 初始化版本的 Device 直接导入返回 8；旧 Host 导入后 SetAccess Device 对照返回 65534。这不是当前固定设备 0 的 HAL 拷贝版本的运行结果。旧 `hal_map_probe` 未调用 `TsdOpen`，其 DeviceOpen 返回 4；补齐 TSD 启动后 DeviceOpen 成功；当前 `multi_share` 通过 `aclInit + aclrtSetDevice` 统一完成设备初始化。这些是 A2 对照，不是 A5 预期结果。

## 参考

- [Host 内存共享 demo](https://github.com/mag1c-h/dev-sandbox/tree/dev-shm)
- [CANN driver 9.0.0 HAL 源码](https://gitcode.com/cann/driver/tree/9.0.0/src/ascend_hal)

本仓库不打包 CANN 头文件、厂商库、机器凭据或部署环境日志；编译使用目标环境已安装的 SDK 和驱动。
