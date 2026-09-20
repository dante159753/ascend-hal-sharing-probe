# Ascend HAL 跨进程内存测试

用于在 A5/Ascend 950 等环境验证同一台机器上两种共享方式：

1. **Host → Host**：进程 A 用 HAL 分配 Host DDR，进程 B 导入为 Host。双方 CPU 读写同一块内存，双方分别执行 ACL 异步 H2D/D2H，以及 buffered / O_DIRECT Linux AIO。
2. **Host → Device**：`device_share` 使用 TSD/HAL 初始化设备 0，将 A 的 Host DDR 导入为设备 0 的映射，通过 `halMemcpy` 写入和读回校验，对齐原始 dev-shm demo 的路径。

**这是能力探测程序，失败会返回非零退出码和具体阶段。A5 尚未实机验证，不能把“能编译”当成“接口支持”。** 不修改驱动、模型、UCM 或设备配置。`host_share` 和 `multi_share` 使用 ACL 初始化；`device_share` 使用 TSD/HAL 初始化。

## 编译

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

两个进程都固定使用 **HAL device 0**，导入目标也是 0。可省略 `--device`，若显式传入则只接受 `--device 0`。`--size-mib` 默认为 2，必须为正的 2 MiB 倍数。旧的 `--via-host` 已移除。

流程为：

1. 每个独立 exec 进程执行 `TsdOpen(0, 0) → halSetRuntimeApiVer → halDeviceOpen(0)`。
2. Writer 按 `MEM_HOST_SIDE, devid=0, module_id=0, MEM_HUGE_PAGE_TYPE, MEM_DDR_TYPE` 分配并映射 Host DDR。
3. Writer 只在普通 Host buffer 里生成数据，通过 `halMemcpy` 写入共享映射；`memcpy_info.dir=DRV_MEMCPY_HOST_TO_DEVICE`、`devid=0`。
4. 导出 handle 并启用同服务器共享权限。Reader 执行 Reserve → `halMemImportFromShareableHandle(handle, 0, ...)` → Map。
5. Reader 通过 `halMemcpy` 将映射内容读到普通 Host buffer；`dir=DRV_MEMCPY_DEVICE_TO_HOST`、`devid=0`，随后逐项校验全部数据。
6. Reader 释放映射和导入 handle 后，Writer 才释放原始分配；最后各自关闭 HAL 和 TSD。

不直接解引用共享映射，不分配 NPU HBM，不调用 ACL，不执行 D2D/AIO。HAL memcpy 使用非空 `memcpy_info`，不静默切换拷贝方向或退回 CPU 拷贝。任一 HAL 调用或数据校验失败均返回非零；成功应看到 `PASS HAL_SHARED_DATA` 和双方 `RESULT PASS`。

2026-09-20 在 A2 的 0.23.0/CANN 9.1.0 容器中编译和参数检查通过，确认可执行文件不链接 ACL/runtime。由于 A2 卡 0 有其他进程，本次没有运行固定设备 0 的硬件用例；A5 结果需在目标环境验证。

## 多卡 NUMA Host 共享 demo

`multi_share` 为每张指定的卡启动一个独立 exec 工作进程，另有一个仅交换 handle 和同步消息的协调进程。工作进程只使用 **`aclInit → aclrtCreateContext`** 初始化；Runtime 内部负责 TSD、HAL device open 和当前 context。随后仍用 HAL 分配、导出、导入和映射 NUMA Host 内存。不手动调用 `halDeviceOpen`、`aclrtSetDevice` 或 HostRegister。

```bash
# 首次使用先按上面的“编译”章节配置 CANN 和驱动库路径。
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target multi_share -j

# 按本机拓扑选择空闲卡和对应 NUMA 节点。
npu-smi info -t topo
lscpu
mkdir -p "$HOME/hal-probe-io"
unset ASCEND_RT_VISIBLE_DEVICES

timeout -k 5 180 build/multi_share --devices 1,2,4 --numa-nodes 6,4,0 --size-mib 32 --io-dir "$HOME/hal-probe-io"
```

- `--devices`：不重复的设备 ID，数量为 1～64；容器需暴露全部选中设备。
- `--numa-nodes`：与设备列表一一对应的 Host NUMA 节点号。上例对应测试 A2，其他机器按实际拓扑填写。
- `--size-mib`：**每个进程**的 Host 分配大小，默认 2 MiB，必须是正的 2 MiB 倍数。上例共分配 96 MiB Host 内存，每个进程都映射完整 96 MiB，并在自己的 NPU 上分配等大的测试 HBM。
- `--io-dir`：已有、可写的测试目录。只执行 buffered Linux AIO，文件打开后立即 unlink，不测试 Direct I/O。

取消 `ASCEND_RT_VISIBLE_DEVICES`，避免 HAL 和 ACL 设备编号重排。程序会查询每个工作进程分配的 HBM 的 HAL device ID，确认与所选设备一致。

CPU affinity 绑定到所选 NUMA 节点与当前允许 CPU 集的交集。Host 分配使用 `MEM_HOST_NUMA_SIDE`、`prop.devid=numa_node`、`MEM_HUGE_PAGE_TYPE`、`MEM_DDR_TYPE`，不退回普通 Host 分配。若驱动提供属性查询接口，会核对属性；缺失时明确输出，不据此声称独立验证了物理页位置。

rank 0 预留 `进程数 × 单进程大小` 的 VA 区间并广播基址，其他进程指定同一个地址预留，地址不一致即失败。交换 handle 后，其他进程的内存通过 `halGetHostID` 返回的 Host ID 导入，按 `base + owner_rank × bytes_each` 逐块映射。各进程相同的指针对应相同的共享内容。

CPU、AIO、H2D/D2H 都覆盖整个连续区域及映射边界。每个进程轮流写入，所有进程完整校验，避免数据竞争。异步 H2D 完成后清空 Host 区域，再 D2H 回写并完整校验；其他进程随后验证能看到本次 D2H 写入。结束时先解除所有进程的映射，再释放 handle、VA 和 context。

2026-09-20 在 A2 910B3、vLLM-Ascend 0.23.0、CANN 9.1.0、驱动 25.5.2 上实测：

| 用例 | 每进程 2 MiB | 每进程 32 MiB |
|---|---|---|
| 卡 1/2/4，NUMA 6/4/0，Host 创建/导入/映射 | 通过 | 通过 |
| 三个进程同址连续映射 | 总长 6 MiB | 总长 96 MiB |
| CPU 读写及跨进程可见性 | 通过 | 通过 |
| buffered AIO 读写及完整数据校验 | 通过 | 通过 |
| 各卡 ACL 异步 H2D/D2H，其他进程验证 D2H 结果 | 通过 | 通过 |

本次公共基址均为 `0x12c180000000`；程序动态协商，不硬编码此值。成功时输出 `MULTI_RESULT PASS`，退出码为 0。A5 尚需实机验证。旧 `--copy-api` 诊断选项已移除。

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

# 固定设备 0：HAL H2D 写入、Device 导入、HAL D2H 读回
unset ASCEND_RT_VISIBLE_DEVICES
timeout -k 5 120 build/device_share --size-mib 2

# 跟参考 demo 一致的纯 HAL 初始化/导入顺序，不链接 ACL/runtime
timeout -k 5 120 build/hal_map_probe host 0 2
timeout -k 5 120 build/hal_map_probe device 0 2
```

`--size-mib` 必须为正的 2 MiB 倍数。程序默认设备 0；直接运行二进制时建议使用外部 `timeout`，批量脚本已经管理超时。完整程序不会自动降级到另一种拷贝方向或另一条初始化路径。

## 测试步骤与结果解释

所有程序在初始化运行时前 fork，子进程随后 exec，因此导入进程不继承创建者的 HAL 映射。Unix socket 仅传递共享 handle 和同步消息，不传 buffer 数据。两个映射同时存在，读写按消息协调，避免无同步的数据竞争。测试使用同机共享授权 `SHR_HANDLE_ATTR_NO_WLIST_IN_SERVER`，临时共享 handle 仅发给子进程；生产应用可改为 PID 白名单。

分配属性是 `MEM_HOST_SIDE, devid=0, module_id=0, MEM_HUGE_PAGE_TYPE, MEM_DDR_TYPE, reserve=0`。Host 导入目标通过 `halGetHostID` 查询，不硬编码 65。导入顺序统一为 **Reserve → Import → Map**，所有 VMM offset/flags 为 0。不调用 HostRegister。

`host_share`：A 写入模式 101，B 全量验证并写入 202，A 全量验证。随后 A、B 分别测试各自映射的 ACL H2D/D2H；H2D 完成后清空 Host buffer，D2H 完成后逐项验证。32 次连续提交后查询事件、等待完成并记录耗时；事件 `NOT_READY` 是本次确实未完成的证据，立即完成本身不算功能失败。双方还执行原生 Linux AIO：四个对齐请求分别测试读和写，记录每个完成事件；普通对齐内存作相同文件的对照。AIO 写入使用不同于文件原内容的数据模式并回读验证。后续双方再互相验证共享数据。

`device_share`：普通 Host buffer → HAL H2D → 共享 Host DDR；另一进程按设备 0 导入后，HAL D2H → 普通 Host buffer 校验。物理共享内存始终在 Host DDR。详细步骤见上面的 Device 共享章节。

`hal_map_probe`：只调用 **halSetRuntimeApiVer → halDeviceOpen → Reserve → Import/Create → Map** 等 HAL 接口。Host 模式附带 CPU 双向共享校验；Device 模式只确认映射成功，**其 PASS 不代表 D2D 可用**。不会在 HAL 初始化后再混入 ACL 初始化。

- `PASS`：该项完成且数据验证通过（纯 HAL Device 例仅验证映射）。
- `CASE_RESULT ... FAIL`：该独立项失败；例如 Direct AIO 失败不会掩盖 buffered AIO 和 ACL 成功。
- `RESULT FAIL ... phase=...`：初始化、导入、映射等前置步骤失败，后续依赖步骤未运行。不能把 Import 失败写成 D2D 执行失败。
- `AIO submitted=4` 不代表 I/O 成功，还要看四个完成事件的 `res` 和数据校验。
- `res=-14` 是 `EFAULT`；`res=-22` 是 `EINVAL`。HAL 返回码属于另一套枚举，例如 `4=INVALID_HANDLE`、`8=PARA_ERROR`、`65534=NOT_SUPPORT`。
- 退出码：`0` 全部请求项通过，`1` 用例失败，`2` 参数/启动错误；runner 的 `124` 表示超时，`127` 表示无法启动。非法参数不触碰设备。
- Buffered AIO 功能通过不保证 `io_submit` 不阻塞；O_DIRECT 失败不等于 CPU 地址不可读写。

## 已知 A2 对照

此前在 A2 910B3、驱动 25.5.2/HAL 7.35.23、vLLM-Ascend 0.23.0/CANN 9.1.0 上：ACL 初始化的 Host 共享 CPU/ACL/buffered AIO 通过；O_DIRECT AIO 返回 EFAULT；旧 ACL 初始化版本的 Device 直接导入返回 8；旧 Host 导入后 SetAccess Device 对照返回 65534。这不是当前固定设备 0 的 HAL 拷贝版本的运行结果。旧 `hal_map_probe` 未调用 `TsdOpen`，其 DeviceOpen 返回 4；补齐 TSD 启动后 DeviceOpen 成功；当前 `multi_share` 通过显式 ACL context 统一完成设备初始化。这些是 A2 对照，不是 A5 预期结果。

## 参考

- [Host 内存共享 demo](https://github.com/mag1c-h/dev-sandbox/tree/dev-shm)
- [CANN driver 9.0.0 HAL 源码](https://gitcode.com/cann/driver/tree/9.0.0/src/ascend_hal)

本仓库不打包 CANN 头文件、厂商库、机器凭据或部署环境日志；编译使用目标环境已安装的 SDK 和驱动。
