# UCM DataStrategy 单进程复现程序

来源：UCM `feat-a5-memory-allocation`，提交 `9da9100`。

以下文件从该提交逐字节复制：

- `trans/ascend/hal/hal_memory.h`、`trans/ascend/hal/hal_memory.cc`
- `status/status.h`

`data_strategy.h`、`data_strategy.cc` 基于同一提交，改为每个 rank 的映射独立预留 VA，并调整地址查询和释放逻辑。对外接口、逻辑 slot 编号、HAL 分配属性、句柄交换和 Huge → Normal 回退保持不变。

每个进程为本地 Host 内存和每个 peer 的 Device 导入分别调用一次 `halMemAddressReserve`，Map 始终使用该次 Reserve 返回的起点。`mappings_[rank].addr` 保存该逻辑 rank 的实际 VA；不同 rank 的 VA 不要求连续。

Reserve、Create、Map 三者长度统一使用 `rankStride`：将每 rank 的有效数据长度按 1 GiB 与 HAL `allocGranularity` 的最小公倍数向上取整。例如有效数据为 3 MiB 时，三者均为 1 GiB；原来三者已经都是 2 GiB 的用例不变。slot 大小及数量保持用户传入值，额外空间只用于分配对齐。

例如 rank 1 进程有三个独立预留区：

| 逻辑 rank | 地址 | 映射类型 |
|---|---|---|
| 0 | `mappings_[0].addr`，独立 Reserve 返回值 | Device 导入 rank 0 |
| 1 | `mappings_[1].addr`，独立 Reserve 返回值 | 本地 Host rank 1 |
| 2 | `mappings_[2].addr`，独立 Reserve 返回值 | Device 导入 rank 2 |

`DataAt` / `DeviceDataAt` 仍按全局逻辑 slot 编号找到对应 rank，再加上 rank 内的 slot 偏移。释放时先 Unmap 所有成功映射，释放导入及本地句柄，再逐个 AddressFree；中途失败也会释放已经预留的 VA。此布局用于验证 A5 上共享预留区内部地址 Map 失败的问题，尚未确认独立 Reserve 能在 A5 上规避该问题。

`trans/device.cc` 保留 UCM 的 `Device::Init/Setup/Reset/Finalize` 函数体原样；仅去掉本程序不需要的 stream/buffer 工厂依赖。`logger/logger.h` 提供带 PID/rank 的终端日志。

`ctrl_layout.h` 是终端版控制块，仅提供 DataStrategy 使用的接口：`SlotCount`、`SetRankDesc`、`GetRankDesc`，以及原样的 `RankDataDesc` 定义。Set 打印 `DESC <rank> <十进制handle>`；Get 等待人工粘贴指定 rank 的这一整行。它不创建共享控制内存。

## 编译

在 demo 仓库根目录执行，独立构建，不依赖 UCM 源码或 Python 包：

```bash
source /usr/local/Ascend/ascend-toolkit/set_env.sh
cmake -S ucm_data_strategy -B build/ucm_data_strategy \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCANN_ROOT=/usr/local/Ascend/ascend-toolkit/latest \
  -DDRIVER_ROOT=/usr/local/Ascend/driver
cmake --build build/ucm_data_strategy --target data_strategy_demo -j
```

需要 Linux、C++17、CMake >= 3.18、CANN ACL/HAL 开发头文件和库。构建不访问网络，也不需要安装 fmt：仓库自带 fmt 11.2.0 的三个必要头文件，以 `FMT_HEADER_ONLY` 编译，不下载或链接 fmt 库。HAL 链接实际驱动目录，不搜索 toolkit devlib。运行时需挂载实际驱动和设备。

旧版在 `fmt-populate` 下载阶段失败时，更新源码后直接重新执行上面的配置和编译命令即可；无需完成旧下载。

## 两个进程手动交换句柄

同一台机器开两个终端，每个命令只启动一个进程，不 fork、不自动启动 peer。各 rank 的 `--ranks`、`--slot-size`、`--slots-per-rank` 必须相同；rank 必须唯一并覆盖 `0..ranks-1`。device 为该进程可见的 ACL 逻辑设备 ID。

终端 0：

```bash
./build/ucm_data_strategy/data_strategy_demo \
  --rank 0 --device 0 --ranks 2 --slot-size 8388608 --slots-per-rank 256
```

终端 1：

```bash
./build/ucm_data_strategy/data_strategy_demo \
  --rank 1 --device 1 --ranks 2 --slot-size 8388608 --slots-per-rank 256
```

每个 rank 分配 2 GiB，slot 为 8 MiB。执行顺序：

1. 入口 `Device::Init` 调用 `aclInit`；`DataStrategy::Setup` 内 `Device::Setup` 调用 `aclrtSetDevice`。
2. 查询推荐粒度，计算 rankStride 和每个 rank 的预留长度。Reserve → Create → Map 本 rank 到自己的 Reserve 起点。成功时打印 `HAL local mapping` 和 `va_offset=0`。先尝试 Huge，任何本地初始化失败时按原逻辑释放并回退 Normal。
3. 导出本 rank 的句柄并禁用白名单，`SetRankDesc` 打印 DESC 行。
4. 按 rank 顺序获取其他 rank 的 desc，Import 到当前 device → 为该 rank 独立 Reserve → Map 到本次 Reserve 返回的起点。每个 peer 成功时同样打印 `va_offset=0`。
5. 成功后打印每个 rank 第一个 slot 的 `HostAccessibleOf`、`DataAt`、`DeviceDataAt`，然后等待 Enter，保持本地分配及导入映射存活。
6. **所有 rank 都打印 `SETUP PASS` 后**，再在各终端按 Enter。析构按原逻辑 Unmap、Release、AddressFree，随后入口调用 `aclFinalize`。

看到 `GetRankDesc: waiting for rank=1` 时，将终端 1 打印的 `DESC 1 ...` 整行粘贴到终端 0；反向操作相同。不要粘贴日志前缀。更多 rank 时，按每次提示粘贴相应进程的 DESC 行。

`--timeout-ms` 默认 600000，原封不动传入 Setup。**GetRankDesc 按需求阻塞终端输入，原轮询超时不能中断这个阻塞调用**；输入格式错误时继续等待。输入 EOF 会使 Setup 返回失败并执行原清理路径；成功后的驻留阶段遇到 EOF 返回非零，避免将管道提前关闭误报为完整交互成功。

这个入口只复现初始化和地址查询，不增加 CPU 写入、ACL copy 或 AIO 测试。`SETUP PASS` 只代表 Setup 成功，不代表已验证数据传输。

## 使用每 rank 8 GiB、共 8 个 rank 的参数

```bash
./build/ucm_data_strategy/data_strategy_demo \
  --rank 1 --device 1 --ranks 8 \
  --slot-size 8588328960 --slots-per-rank 1
```

这里用单个大 slot 精确构造 `data_bytes=8588328960`。若驱动返回 `alloc_granularity=2097152`，则 `rank_stride=8589934592`、`reserve_bytes_per_rank=8589934592`。完整初始化后，每个进程有 8 次独立的 8 GiB VA 预留，总计 64 GiB，而不再是一次预留连续的 64 GiB。Reserve、Create 和 Map 长度均为 rankStride；日志打印每段 VA 和预留长度。

若只定位本地 Map 失败，启动这一个进程即可；若本地 Map 成功进入句柄交换，需要再启动其余 rank 并交换句柄。若要保持业务中的 slot 划分，也可直接传入业务的真实 slot-size 和 slots-per-rank。

## 已完成的验证

2026-09-24 初版：使用 Linux GCC 13、真实 ACL/HAL 9.0.0 源码头文件、fmt 11.2.0，以 `-Wall -Wextra -Wpedantic -Werror` 编译通过。独立模拟 SDK 验证了双进程 DESC 交互、Huge 失败回退 Normal、8 GiB / 64 GiB 参数计算、Map 返回 8 和输入 EOF 时的清理。模拟检查文件在仓库外，模拟库未加入 demo 构建。

同日首次调整布局后：重新编译通过，模拟检查覆盖 rank 0/1/2 的本地起点映射、peer 连续排列、逻辑 slot 地址查询以及部分 peer 映射失败时的清理；原有交互和失败回退检查也通过。此次验证未在 A5 实机运行。

同日改为独立 Reserve 后：重新编译通过，模拟检查覆盖三个 owner 的独立 VA 起点映射、peer Reserve/Map 中途失败的清理，以及不连续 VA 下各 slot 的地址查询。另以每 rank 3 MiB 数据、4 MiB Map 长度、1 GiB Reserve 长度检查不同对齐长度的处理。原有双进程交互和失败回退检查也通过；待 A5 实机确认。

同日统一长度后：编译和模拟检查通过；3 MiB 有效数据对应的 Reserve、Create、Map 长度均为 1 GiB，所有 Map 的长度严格等于对应预留区长度。slot 查询、双进程交互及失败清理检查通过；未在 A5 实机运行。
