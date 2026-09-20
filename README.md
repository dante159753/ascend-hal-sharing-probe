# Ascend HAL 跨进程内存测试

用于在 A5/Ascend 950 等环境验证同一台机器上两种共享方式：

1. **Host → Host**：进程 A 用 HAL 分配 Host DDR，进程 B 导入为 Host。双方 CPU 读写同一块内存，双方分别执行 ACL 异步 H2D/D2H，以及 buffered / O_DIRECT Linux AIO。
2. **Host → Device**：进程 B 将 A 的 Host DDR 导入为其 NPU 的 Device 映射，执行 `aclrtMemcpyAsync(..., ACL_MEMCPY_DEVICE_TO_DEVICE)` 到 B 的 NPU HBM，再回读逐项校验；还测试反向 D2D 写回，让 A 的 CPU 验证。

**这是能力探测程序，失败会返回非零退出码和具体阶段。A5 尚未实机验证，不能把“能编译”当成“接口支持”。** 不修改驱动、模型、UCM 或设备配置。完整测试使用 ACL 初始化；另有纯 HAL 初始化/映射诊断，避免混用两种初始化方式。

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

确认 `libascend_hal.so` 来自实际驱动目录，`libascendcl.so` / `libruntime.so` 来自所选 CANN。CMake 不搜索 toolkit 中的 HAL stub 库。自定义安装路径用 `CANN_ROOT` / `DRIVER_ROOT` 指定，也需相应调整环境变量。

在容器中运行时使用 **vLLM-Ascend 0.23.0 或更新且适配 A5 的镜像**，挂载本机驱动、目标设备节点及 ext4/XFS 测试目录；不要挂载其他版本 toolkit 覆盖镜像。具体容器设备授权按机器已有运维配置执行。测试程序不需要访问所有 NPU。

私有仓库克隆需要先配置 GitHub 认证，也可在已登录的开发机下载代码后传到 A5。

## 一键运行两种模式

选一张空闲 NPU。以下示例选本机物理 NPU 0：

```bash
mkdir -p "$HOME/hal-probe-io"
findmnt -T "$HOME/hal-probe-io" -o TARGET,SOURCE,FSTYPE

ASCEND_RT_VISIBLE_DEVICES=0 python3 run_tests.py \
  --device 0 --hal-device 0 \
  --io-dir "$HOME/hal-probe-io" \
  --sizes-mib 2 32 --via-host
```

默认运行 Host、Device 两类完整测试，以及纯 HAL 的 Host/Device 映射诊断。`--via-host` 额外运行“先导入 Host，再 SetAccess Device”的对照路径。每例默认超时 120 秒，超时会终止该例两个进程所在进程组；其他用例继续。日志写入 `logs/<时间>-<PID>/`，`summary.json` 保存命令和退出码。可用 `--log-dir` 指定一个尚不存在的新目录。

两个设备参数不是同一命名空间：

- `--device`：ACL 可见逻辑序号。完整 Device 测试从实际分配的 NPU HBM 查询 HAL `devId`，用它做导入目标。
- `--hal-device`：纯 HAL 诊断的 HAL 设备编号；不经 ACL 可见设备列表重编号。

例如选择物理 NPU 1 时，在普通未重编号的驱动环境中使用 `ASCEND_RT_VISIBLE_DEVICES=1 --device 0 --hal-device 1`。如果容器或驱动另有编号映射，以本机实际 HAL 编号为准。

**O_DIRECT 必须在支持它的文件系统中测试。** 不要把 `--io-dir` 指向 tmpfs（常见的 `/tmp`、`/dev/shm`）。程序只创建本例 PID 命名的临时文件，并在打开后立即 unlink；不删除目录内已有数据。

## 单独运行

```bash
# Host 共享：CPU + ACL async + 两种 AIO
ASCEND_RT_VISIBLE_DEVICES=0 timeout -k 5 120 build/host_share \
  --device 0 --size-mib 2 --io-dir "$HOME/hal-probe-io" --aio both

# 只测试 buffered AIO；也可选 direct 或 none
ASCEND_RT_VISIBLE_DEVICES=0 timeout -k 5 120 build/host_share \
  --device 0 --size-mib 32 --io-dir "$HOME/hal-probe-io" --aio buffered

# Host 导入为 Device，再 ACL async D2D 到该 NPU 的 HBM
ASCEND_RT_VISIBLE_DEVICES=0 timeout -k 5 120 build/device_share \
  --device 0 --size-mib 2

# 替代路径：Host 导入 + SetAccess Device RW
ASCEND_RT_VISIBLE_DEVICES=0 timeout -k 5 120 build/device_share \
  --device 0 --size-mib 2 --via-host

# 跟参考 demo 一致的纯 HAL 初始化/导入顺序，不链接 ACL/runtime
timeout -k 5 120 build/hal_map_probe host 0 2
timeout -k 5 120 build/hal_map_probe device 0 2
```

`--size-mib` 必须为正的 2 MiB 倍数。程序默认设备 0；直接运行二进制时建议使用外部 `timeout`，批量脚本已经管理超时。完整程序不会自动降级到另一种拷贝方向或另一条初始化路径。

## 测试步骤与结果解释

所有程序在初始化运行时前 fork，子进程随后 exec，因此导入进程不继承创建者的 HAL 映射。Unix socket 仅传递共享 handle 和同步消息，不传 buffer 数据。两个映射同时存在，读写按消息协调，避免无同步的数据竞争。测试使用同机共享授权 `SHR_HANDLE_ATTR_NO_WLIST_IN_SERVER`，临时共享 handle 仅发给子进程；生产应用可改为 PID 白名单。

分配属性是 `MEM_HOST_SIDE, devid=0, module_id=0, MEM_HUGE_PAGE_TYPE, MEM_DDR_TYPE, reserve=0`。Host 导入目标通过 `halGetHostID` 查询，不硬编码 65。导入顺序统一为 **Reserve → Import → Map**，所有 VMM offset/flags 为 0。不调用 HostRegister。

`host_share`：A 写入模式 101，B 全量验证并写入 202，A 全量验证。随后 A、B 分别测试各自映射的 ACL H2D/D2H；H2D 完成后清空 Host buffer，D2H 完成后逐项验证。32 次连续提交后查询事件、等待完成并记录耗时；事件 `NOT_READY` 是本次确实未完成的证据，立即完成本身不算功能失败。双方还执行原生 Linux AIO：四个对齐请求分别测试读和写，记录每个完成事件；普通对齐内存作相同文件的对照。AIO 写入使用不同于文件原内容的数据模式并回读验证。后续双方再互相验证共享数据。

`device_share`：A CPU 写入 → B 导入 Device → D2D 到新分配 HBM → D2H 到普通 CPU buffer 校验；A 修改原始 Host 数据后再验证一次，证明导入是共享映射；最后从 B 的 HBM 反向 D2D 写回共享地址，A CPU 验证。Device VA 不直接被 CPU 解引用。物理共享内存仍在 Host DDR，导入不会将它搬到 HBM。

`hal_map_probe`：只调用 **halSetRuntimeApiVer → halDeviceOpen → Reserve → Import/Create → Map** 等 HAL 接口。Host 模式附带 CPU 双向共享校验；Device 模式只确认映射成功，**其 PASS 不代表 D2D 可用**。不会在 HAL 初始化后再混入 ACL 初始化。

- `PASS`：该项完成且数据验证通过（纯 HAL Device 例仅验证映射）。
- `CASE_RESULT ... FAIL`：该独立项失败；例如 Direct AIO 失败不会掩盖 buffered AIO 和 ACL 成功。
- `RESULT FAIL ... phase=...`：初始化、导入、映射等前置步骤失败，后续依赖步骤未运行。不能把 Import 失败写成 D2D 执行失败。
- `AIO submitted=4` 不代表 I/O 成功，还要看四个完成事件的 `res` 和数据校验。
- `res=-14` 是 `EFAULT`；`res=-22` 是 `EINVAL`。HAL 返回码属于另一套枚举，例如 `4=INVALID_HANDLE`、`8=PARA_ERROR`、`65534=NOT_SUPPORT`。
- 退出码：`0` 全部请求项通过，`1` 用例失败，`2` 参数/启动错误；runner 的 `124` 表示超时，`127` 表示无法启动。非法参数不触碰设备。
- Buffered AIO 功能通过不保证 `io_submit` 不阻塞；O_DIRECT 失败不等于 CPU 地址不可读写。

## 已知 A2 对照

此前在 A2 910B3、驱动 25.5.2/HAL 7.35.23、vLLM-Ascend 0.23.0/CANN 9.1.0 上：Host 共享 CPU/ACL/buffered AIO 通过；O_DIRECT AIO 返回 EFAULT；Device 直接导入返回 8；Host 导入后 SetAccess Device 返回 65534；demo 的纯 HAL DeviceOpen 返回 4。这些是 A2 对照，不是 A5 预期结果。

## 参考

- [Host 内存共享 demo](https://github.com/mag1c-h/dev-sandbox/tree/dev-shm)
- [CANN driver 9.0.0 HAL 源码](https://gitcode.com/cann/driver/tree/9.0.0/src/ascend_hal)

本仓库不打包 CANN 头文件、厂商库、机器凭据或部署环境日志；编译使用目标环境已安装的 SDK 和驱动。
