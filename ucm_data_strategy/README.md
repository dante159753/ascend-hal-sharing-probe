# UCM DataStrategy 单进程复现程序

来源：UCM `feat-a5-memory-allocation`，提交 `9da9100`。

以下文件从该提交逐字节复制，不修改接口、HAL 参数、调用顺序、失败回退或释放逻辑：

- `data_strategy.h`、`data_strategy.cc`
- `trans/ascend/hal/hal_memory.h`、`trans/ascend/hal/hal_memory.cc`
- `status/status.h`

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

需要 Linux、C++17、CMake >= 3.18、CANN ACL/HAL 开发头文件和库。fmt 与 UCM 一致使用 11.2.0：优先使用已安装的兼容版本，否则 CMake 从 GitHub 下载。HAL 链接实际驱动目录，不搜索 toolkit devlib。运行时需挂载实际驱动和设备。

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

每个 rank 分配 2 GiB，slot 为 8 MiB。执行顺序保持 UCM 原样：

1. 入口 `Device::Init` 调用 `aclInit`；`DataStrategy::Setup` 内 `Device::Setup` 调用 `aclrtSetDevice`。
2. 查询推荐粒度，计算 rankStride 和整个预留区长度，Reserve → Create → Map 本 rank。先尝试 Huge，任何本地初始化失败时按原逻辑释放并回退 Normal。
3. 导出本 rank 的句柄并禁用白名单，`SetRankDesc` 打印 DESC 行。
4. 按 rank 顺序获取其他 rank 的 desc，导入到当前 device，并映射到预留区对应偏移。
5. 成功后打印每个 rank 第一个 slot 的 `HostAccessibleOf`、`DataAt`、`DeviceDataAt`，然后等待 Enter，保持本地分配及导入映射存活。
6. **所有 rank 都打印 `SETUP PASS` 后**，再在各终端按 Enter。析构按原逻辑 Unmap、Release、AddressFree，随后入口调用 `aclFinalize`。

看到 `GetRankDesc: waiting for rank=1` 时，将终端 1 打印的 `DESC 1 ...` 整行粘贴到终端 0；反向操作相同。不要粘贴日志前缀。更多 rank 时，按每次提示粘贴相应进程的 DESC 行。

`--timeout-ms` 默认 600000，原封不动传入 Setup。**GetRankDesc 按需求阻塞终端输入，原轮询超时不能中断这个阻塞调用**；输入格式错误时继续等待。输入 EOF 会使 Setup 返回失败并执行原清理路径；成功后的驻留阶段遇到 EOF 返回非零，避免将管道提前关闭误报为完整交互成功。

这个入口只复现初始化和地址查询，不增加 CPU 写入、ACL copy 或 AIO 测试。`SETUP PASS` 只代表 Setup 成功，不代表已验证数据传输。

## 复现当前 8 GiB / 64 GiB 参数

```bash
./build/ucm_data_strategy/data_strategy_demo \
  --rank 1 --device 1 --ranks 8 \
  --slot-size 8588328960 --slots-per-rank 1
```

这里用单个大 slot 精确构造 `data_bytes=8588328960`。若驱动返回 `alloc_granularity=2097152`，则 `rank_stride=8589934592`、`reserve_bytes=68719476736`，与当前报错参数一致。VA 仍由 HAL 自动选择，实际地址不保证相同。

若只定位本地 Map 失败，启动这一个进程即可；若本地 Map 成功进入句柄交换，需要再启动其余 rank 并交换句柄。若要保持业务中的 slot 划分，也可直接传入业务的真实 slot-size 和 slots-per-rank。

## 已完成的验证

2026-09-24：使用 Linux GCC 13、真实 ACL/HAL 9.0.0 源码头文件、fmt 11.2.0，以 `-Wall -Wextra -Wpedantic -Werror` 编译通过。独立模拟 SDK 验证了双进程 DESC 交互、Huge 失败回退 Normal、8 GiB / 64 GiB 参数计算、Map 返回 8 和输入 EOF 时的清理。核心五个文件逐字节一致，四个 Device 方法的函数体与来源一致。模拟检查文件在仓库外；尚未在 A5 实机验证，也未把模拟库加入 demo 构建。
