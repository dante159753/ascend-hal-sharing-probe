# Cache2 HAL / Linux AIO 文件系统诊断

`cache2_hal_aio.cc` 是单文件 Linux C++17 程序，只需要本机 CANN/驱动 SDK、C++ 编译器和 pthread。不依赖 UCM、fmt、gtest、libaio，也不执行任何下载。

## 编译

在能运行 Ascend 的容器或主机内，进入源文件所在目录：

```bash
ASCEND=/usr/local/Ascend/ascend-toolkit/latest
DRIVER=/usr/local/Ascend/driver

g++ -O2 -std=c++17 -Wall -Wextra -pthread cache2_hal_aio.cc \
  -I"$ASCEND/include" -I"$ASCEND/include/driver" -I"$DRIVER/include" \
  -L"$ASCEND/lib64" -L"$DRIVER/lib64/driver" -L"$ASCEND/devlib" \
  -lascendcl -lascend_hal -o cache2_hal_aio

export LD_LIBRARY_PATH="$DRIVER/lib64/driver:$ASCEND/lib64:${LD_LIBRARY_PATH:-}"
```

如果 SDK 使用架构子目录，把对应路径换为 `$ASCEND/aarch64-linux/include`、`include/driver`、`lib64`、`devlib`。头文件必须提供 `halMemCreate` 等虚拟内存 API。
`devlib` 可用于链接；运行时必须使用机器上真实的驱动库，不能把 `devlib` 加进 `LD_LIBRARY_PATH`。

## 执行

`--engine aio`（默认）和 `--engine psync` 共用 HAL buffer、文件提交和校验流程，均可指定 `--direct 1`（默认）或 `--direct 0`。例如：

```bash
./cache2_hal_aio --dir /mnt/custom-fs/test --engine psync --direct 1 --tensor-bytes 1048576
./cache2_hal_aio --dir /mnt/custom-fs/test --engine psync --direct 0 --tensor-bytes 1048576
```

psync 按 UCM `TransQueue::H2S/S2H` 的访问粒度执行同步 `pwrite/pread`：每个 shard 从 `shard_index × slot_bytes` 开始，按 tensor 大小递增偏移。默认每个 shard 一次调用；`--tensor-bytes` 仅用于 psync，必须整除 slot 大小。dump 为每个 shard 打开和关闭临时文件，load 在同一 block 内复用读 fd。短读写直接报错。demo 串行执行 psync，不模拟生产线程池；`--depth`、`--timeout-ms` 只作用于 AIO。`--sync` 是另一个独立选项，表示是否在提交文件前调用 `fdatasync`。

`--dir` 指向已经存在的目标文件系统目录。每次运行会创建独立的 `cache2-hal-aio-XXXXXX` 子目录，不覆盖已有业务文件。默认无论成功或失败都会保留文件，以便检查（`--keep-files 1`）；使用 `--keep-files 0` 可在成功后清理；成功时默认占用约 2GB 磁盘空间。

```bash
set -o pipefail
./cache2_hal_aio --dir /mnt/custom-fs/test --device 0 2>&1 | tee hal-aio-custom.log
./cache2_hal_aio --dir /mnt/local-disk/test --device 0 2>&1 | tee hal-aio-local.log
```

两次运行保持参数相同，对比自研文件系统与已知正常的本地文件系统。最后必须出现 `RESULT PASS` 且退出码为 0；单独的 `PHASE DUMP PASS` 只代表写请求完成。

默认参数：

| 参数 | 默认值 | 含义 |
| --- | --- | --- |
| `--device` | 0 | `aclrtSetDevice` 的设备 ID |
| `--blocks` | 8 | 文件数，一个文件对应一个存储 block |
| `--shards` | 32 | 每个 block 的 shard 数 |
| `--slot-bytes` | 8388608 | 每次 AIO 请求的字节数，一个 shard 使用一块 HAL slot |
| `--depth` | 32 | AIO context 的队列深度及提交窗口 |
| `--iterations` | 2 | dump/load/校验轮数，每轮使用不同的数据 |
| `--timeout-ms` | 60000 | 每个 AIO 阶段的提交、完成等待预算 |
| `--pages` | auto | 大页失败后释放资源并回退普通页；也可指定 `normal` 或 `huge` |
| `--direct` | 1 | 读写文件都使用 `O_DIRECT`；设为 0 可对比 buffered AIO |
| `--sync` | 0 | 设为 1 时，在写完成后、rename 前额外调用 `fdatasync` |
| `--keep-files` | 1 | 保留生成的文件；设为 0 时，仅在所有轮次校验通过后删除本次文件及运行子目录；失败时仍保留 |

有效数据大小为 `blocks × shards × slot-bytes`，默认正好 2GiB。HAL 实际 reserve/create/map 长度按 `lcm(1GiB, allocGranularity)` 向上对齐。即使只测几个小 shard，HAL 也至少申请 1GiB，以保留分支内的分配规则。

缩小请求数量、固定普通页，便于查看日志：

```bash
./cache2_hal_aio --dir /mnt/custom-fs/test --device 0 \
  --blocks 2 --shards 4 --slot-bytes 8388608 --depth 4 --pages normal
```

`--sync 1` 是单独的诊断变量，基线关闭。关闭时的 PASS 表示当前运行中的读写可见性和数据一致性通过，不代表掉电持久性。使用 `--direct 0` 时也不能证明读操作实际穿透了页缓存。

## 与当前分支流程的对应

参考 `feat-a5-hal-per-rank-reserve` 的：

- `ucm/store/cache/v2/data_backend_ascend.cc` 和 `ucm/shared/trans/ascend/hal/hal_memory.cc`：`aclInit → aclrtSetDevice → halMemGetAllocationGranularity → halMemAddressReserve → halMemCreate → halMemMap`。`drv_mem_prop` 清零后设置 `MEM_HOST_SIDE`、`devid=0`、`MEM_DDR_TYPE` 和 page type；reserve/map 从独立 reservation 的起始地址开始，offset/flags 为 0。
- `ucm/store/posix/cc/aio_impl.cc`：直接使用 Linux `io_setup/io_submit/io_getevents/io_destroy` syscall。逐条提交 IOCB，设置 `IOCB_FLAG_RESFD`，独立线程通过 `eventfd + epoll` 收集完成事件；`io_getevents` 使用 `min_nr=1` 和零 timeout。提交遇到 `EAGAIN` 间隔 200µs 重试。
- `ucm/store/posix/cc/io_engine_aio.h`：每个 shard 使用独立 buffer、文件 offset 为 `shard_index × slot_bytes`；检查完成字节数是否等于请求长度。
- `ucm/store/posix/cc/space_layout.cc`：写 `.tmp`，所有 shard 完成并关闭 fd 后 rename 到正式文件，再重新打开执行 load。

demo 只访问本进程拥有的 HAL host buffer，不导出/import peer device 映射，不做 NPU copy。CPU 填充每行 128 字节的可读文本，含 iteration、block、shard、slot_offset 和 buffer_offset，剩余位置填点号、末尾换行。字段记录该行起始位置；末行不足 128 字节时截断。dump 后将 buffer 填成 0xff，再 load 并逐字节校验。报错打印首个错误字节的位置与十六进制值，并并排给出该行 actual/expected 的完整内容；非可见字符转义为 `\xNN`，因此零字节和残留 0xff 也可见。最多打印前 16 条错误记录，仍统计所有错误字节。

生成的文件可以直接用 `head -n 5 block-0` 检查内容。默认每轮使用不同的 iteration，方便识别上一轮的旧数据。

这是存储 I/O 诊断程序，未复现控制块、load fd cache、跨进程调度和后台维护线程。每个阶段使用独立 AIO context、为各 shard 打开 fd，阶段完成后统一关闭；生产代码的 dump fd 在各自回调中关闭。日志较详细，不适合用结果评价生产性能。

**Direct I/O：** demo 默认在读写文件时都传入 `O_DIRECT`，与当前分支默认配置一致。Linux AIO 本身不隐含 Direct I/O；是否开启取决于 open flags。当前分支 `IoEngineAio::Dispatch` 的 dump flags 硬编码 `O_DIRECT`，load 使用 handle cache 时依据 `config.ioDirect`。使用 `--direct 0` 可单独对比 buffered AIO；demo 不会在 Direct I/O 失败后自动回退，也没有修改生产代码。HAL buffer 或文件系统不支持 Direct I/O 时会保留原始错误；自定义 slot 大小时还应满足目标文件系统的地址、长度和文件 offset 对齐要求。

## 如何看故障

- `CALL` 后没有 `RETURN`：对应 HAL、open、rename、sync 或 teardown 调用没有返回。
- `io_submit request=...` 参数行后迟迟没有返回行：提交 syscall 卡住。buffered AIO 的提交本身可能同步阻塞，不能仅凭异步接口名称认定它立即返回。
- `io_submit ret=-1 errno=...`：内核拒绝提交；日志保留原始 errno。
- `complete ... res=-...`：提交成功，完成时失败；`error` 是 `-res`，同时打印 `res2`。
- `res` 非负但小于 `expected`：短读/短写，视为失败。
- 已提交但未完成：等待预算耗尽时报出 submitted/completed；如果 eventfd 没通知，日志也不会出现相应通知行。
- 完成长度正常，但 `MISMATCH`：数据错误；保留正式文件、运行日志用于排查。

超时不会提前释放内核仍可能使用的 HAL 内存。退出前先停止完成线程，再 `io_destroy`，最后关闭 fd、unmap/release/free；文件系统卡住时 `io_destroy` 也可能等待。`--timeout-ms` 不能打断阻塞中的 syscall。定位卡住时可从另一终端抓取：

```bash
strace -ff -ttT -p PID \
  -e trace=io_setup,io_submit,io_getevents,io_destroy,eventfd2,epoll_wait,openat,close,rename,fdatasync
```

或者从启动时通过 `strace -ff -ttT -o /tmp/hal-aio.trace ./cache2_hal_aio ...` 记录调用；HAL 分配失败时重点保留此前打印的 prop、alloc_granularity、rank_stride、addr、handle 及原始返回码。
