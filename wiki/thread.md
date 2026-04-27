# DPA Thread Notes

## Summary

DOCA DPA 里常见的执行模型有 4 类:

- `RPC`: host 同步调用一个 DPA 函数, 等它返回
- `kernel launch`: host 一次启动一组 DPA 线程执行同一个 kernel
- `DPA thread`: host 创建持久 DPA 线程, 之后通过 completion 或 notify 激活
- `thread group`: host 把多个 DPA thread 组成一组, 作为一组线程运行同一个 kernel

最容易混淆的是:

- `RPC` 不是 thread
- `kernel launch` 不是持久 thread
- `DPA thread` 不能在 device 侧再创建新的 DPA thread
- `notification` 只负责唤醒, 不负责带 payload 传消息
- `affinity` 只有 `DPA thread` 有显式 host API; `RPC`、`kernel launch` 没有, `thread group` 也没有 group 级 API

## Quick Comparison

| 模型 | host API | device 侧函数 | 线程数 | 生命周期 | host 是否阻塞 | 适合场景 |
|---|---|---|---|---|---|---|
| RPC | `doca_dpa_rpc()` | `__dpa_rpc__` | 单次调用 | 一次性 | 是 | 初始化, 小控制操作 |
| Kernel Launch | `doca_dpa_kernel_launch_update_set/add()` | `__dpa_global__` | 一次 `N` 个线程 | 一次性 | 否, 通常靠 event 判断结束 | data-parallel 工作 |
| DPA Thread | `doca_dpa_thread_create()` 等 | `__dpa_global__` | 一个 thread | 持久 | 否 | 常驻 worker, 事件驱动 |
| Thread Group | `doca_dpa_thread_group_create()` 等 | `__dpa_global__` | 一组预创建 thread | 持久 | 否 | 固定大小的一组常驻线程 |

关于 `affinity` 的结论先放这里:

- `RPC`: 不能直接设置
- `kernel launch`: 不能直接设置
- `DPA thread`: 可以, 用 `doca_dpa_thread_set_affinity()`
- `thread group`: 不能直接给 group 设置, 只能对组内每个 `DPA thread` 单独设置

## 1. RPC

`RPC` 可以理解成: host 远程调用 DPA 上的一个函数, 并等待它返回。

关键点:

- host API 是 `doca_dpa_rpc()`
- device 函数必须标记为 `__dpa_rpc__`
- 返回值类型是 `uint64_t`
- 这是 blocking API

DOCA 头文件定义:

- `/opt/mellanox/doca/include/doca_dpa.h:501-524`

本仓库例子:

- `timer/main.c`: host 通过 RPC 调起 DPA 侧等待 1 秒
- `timer/wait_one_second_dev.c`: DPA 侧 `__dpa_rpc__` 实现
- `p2p_rtt/doca/server.c`: host 通过 RPC 让 DPA 先 post 一个 receive
- `p2p_rtt/doca/server_kernels_dev.c`: `latency_post_initial_receive_rpc`

适合:

- 初始化 DPA 侧状态
- 触发一次 one-shot 操作
- 返回一个简单结果给 host

关于 `affinity`:

- `doca_dpa_rpc()` 没有 affinity 参数
- 头文件里也没有 RPC 级 affinity setter

不适合:

- 长期事件循环
- 大量并行线程工作
- DPA thread 间协作

## 2. Kernel Launch

`kernel launch` 更像 GPU 风格的提交: host 一次提交一个 kernel, 指定跑多少个线程。

关键点:

- host API 是 `doca_dpa_kernel_launch_update_set()` 或 `doca_dpa_kernel_launch_update_add()`
- device 函数必须标记为 `__dpa_global__`
- `num_threads` 指定这次提交要并行跑多少个线程
- 所有线程执行同一个函数, 通过 `thread_rank` 分工

DOCA 头文件定义:

- `/opt/mellanox/doca/include/doca_dpa.h:430-499`
- `/opt/mellanox/doca/include/doca_dpa_dev.h:155-182`

device 侧常用辅助 API:

- `doca_dpa_dev_thread_rank()`
- `doca_dpa_dev_num_threads()`

官方例子:

- `/opt/mellanox/doca/samples/doca_dpa/dpa_kernel_launch/host/dpa_kernel_launch_sample.c`

`all_to_all` 的模型也是这个, 不是显式创建很多持久 DPA thread:

- host 提交 kernel: `/opt/mellanox/doca/applications/dpa_all_to_all/host/dpa_all_to_all_core.c:1953-1972`
- device 侧按 `thread_rank` 分工: `/opt/mellanox/doca/applications/dpa_all_to_all/device/dpa_all_to_all_dev.c:79-115`

`all_to_all` 的核心思路是:

```c
unsigned int thread_rank = doca_dpa_dev_thread_rank();
unsigned int num_threads = doca_dpa_dev_num_threads();

for (i = thread_rank; i < num_ranks; i += num_threads) {
    // 每个线程处理自己负责的那部分工作
}
```

适合:

- 并行计算
- 每次调用都是独立的一批工作
- 一次性分摊一组相似任务

关于 `affinity`:

- `doca_dpa_kernel_launch_update_set/add()` 没有 affinity 参数
- 官方 sample 里也没有 kernel-launch 级 affinity 用法

## 3. DPA Thread

`DPA thread` 是持久模型。host 先创建一个 `struct doca_dpa_thread`, 之后再激活它执行。

关键点:

- host API 是 `doca_dpa_thread_create()`
- device 函数必须标记为 `__dpa_global__`
- thread 可以反复被激活
- 更像 event-driven worker, 不是一次性 kernel

DOCA 头文件定义:

- `doca_dpa_thread_create()`: `/opt/mellanox/doca/include/doca_dpa.h:866-888`
- `doca_dpa_thread_set_func_arg()`: `/opt/mellanox/doca/include/doca_dpa.h:907-926`
- `doca_dpa_thread_start()` / `doca_dpa_thread_run()`: `/opt/mellanox/doca/include/doca_dpa.h:1008-1060`

典型流程:

1. host `doca_dpa_thread_create()`
2. host `doca_dpa_thread_set_func_arg()`
3. host `doca_dpa_thread_start()`
4. host 配 completion 或 notification completion
5. host `doca_dpa_thread_run()`
6. 线程之后通过 completion 或 notify 被激活

本仓库例子:

- `p2p_rtt/doca/server.c`: 显式创建并启动 DPA thread
- `p2p_rtt/doca/server_kernels_dev.c`: `__dpa_global__` thread 入口
- `thread_comm/main.c`: 创建 thread A / thread B
- `thread_comm/thread_comm_dev.c`: thread A 写共享内存后 notify thread B

### 3.1 线程创建方式

严格说, `创建 DPA thread 对象` 只有一种 API:

- `doca_dpa_thread_create()`

如果从更广义的“让 DPA 上执行代码”来看, 才可以分成:

- RPC
- kernel launch
- DPA thread

### 3.2 DPA Thread 不能在 device 侧再创建 thread

不能在一个正在运行的 DPA thread 里再去 `create` 一个新的 DPA thread。

原因是:

- `doca_dpa_thread_create()` 是 host API
- device 侧头文件 `doca_dpa_dev.h` 没有 `doca_dpa_dev_thread_create()` 这种接口

device 侧线程相关接口主要是:

- `doca_dpa_dev_thread_get_local_storage()`
- `doca_dpa_dev_thread_reschedule()`
- `doca_dpa_dev_thread_finish()`
- `doca_dpa_dev_thread_notify()`

也就是说, DPA thread 可以:

- 读写共享状态
- 通知另一个已经存在的 DPA thread
- 结束或 reschedule 自己

但不能:

- 在 device 侧动态创建新 thread

### 3.3 DPA Thread Affinity

`DPA thread` 是这里 4 种模型里唯一有显式 affinity API 的。

相关 API:

- `doca_dpa_eu_affinity_create()` / `doca_dpa_eu_affinity_set()`
- `doca_dpa_thread_set_affinity()`

头文件位置:

- `doca_dpa_eu_affinity_*`: `/opt/mellanox/doca/include/doca_dpa.h:789-863`
- `doca_dpa_thread_set_affinity()`: `/opt/mellanox/doca/include/doca_dpa.h:975-1005`

语义:

- 不设 affinity 时, 默认是 `relaxed`, thread reschedule 时可以跑在任意可用 EU
- 设了 affinity 后, thread 变成 `fixed`, 只会跑在指定 EU

限制:

- 必须在 `doca_dpa_thread_start()` 之前设置

官方 sample:

- `/opt/mellanox/doca/samples/doca_dpa/dpa_nvqual/host/dpa_nvqual_sample.c`
- `/opt/mellanox/doca/samples/doca_dpa/dpa_common.c`

## 4. Thread Group

`thread group` 是一组预创建 thread 的集合。

关键点:

- 先用 `doca_dpa_thread_group_create()` 创建 group
- 再把一个个 `doca_dpa_thread` 塞到 group 的各个 rank
- 然后 `doca_dpa_thread_group_start()`
- group 本身没有 `thread_group_set_affinity()` 这类 group 级 affinity API

DOCA 头文件定义:

- `/opt/mellanox/doca/include/doca_dpa.h:1077-1165`

它不是新的线程创建方式, 只是把已经创建好的 thread 组织成一组。

如果想让 `thread group` 里的 thread 绑到特定 EU, 做法是:

- 对每个 `doca_dpa_thread` 分别调用 `doca_dpa_thread_set_affinity()`
- 然后再把这些 thread 组织进 group

和 `kernel launch` 的区别:

- `kernel launch` 是一次性并行执行
- `thread group` 是一组持久 thread

`doca_dpa_dev_thread_rank()` / `doca_dpa_dev_num_threads()` 在这两种模型下都能用:

- `/opt/mellanox/doca/include/doca_dpa_dev.h:159-176`

## 5. Affinity 能设在哪

结论:

1. `RPC`: 不能
2. `kernel launch`: 不能
3. `DPA thread`: 可以
4. `thread group`: 不能直接设, 只能对组内 thread 单独设

为什么:

- 头文件里只有 `doca_dpa_thread_set_affinity()` 这一条 DPA thread affinity API
- `doca_dpa_rpc()` 没有 affinity 参数: `/opt/mellanox/doca/include/doca_dpa.h:501-524`
- `doca_dpa_kernel_launch_update_set/add()` 没有 affinity 参数: `/opt/mellanox/doca/include/doca_dpa.h:430-499`
- `doca_dpa_thread_group_*()` 没有 group 级 affinity API: `/opt/mellanox/doca/include/doca_dpa.h:1077-1165`

从 `/opt/mellanox` 里的官方 sample/application 看, 实际出现的 affinity 用法也都落在:

- `doca_dpa_eu_affinity_create()`
- `doca_dpa_eu_affinity_set()`
- `doca_dpa_thread_set_affinity()`

没有看到 `RPC` 级、`kernel launch` 级或 `thread group` 级 affinity sample。

## 6. DPA Thread 的两种激活方式

头文件里提到的“两种方法”说的是激活方式, 不是线程创建方式。

DOCA 注释位置:

- `/opt/mellanox/doca/include/doca_dpa.h:871-877`

两种激活方式是:

1. 通过 `completion context` 触发 thread
2. 通过 `notification completion` 触发 thread

相关接口:

- `doca_dpa_completion_set_thread()`: `/opt/mellanox/doca/include/doca_dpa.h:1235-1251`
- `doca_dpa_notification_completion_create()`: `/opt/mellanox/doca/include/doca_dpa.h:1445-1465`

区别在于:

- completion 路径常和 RDMA, async ops 的 completion 结合
- notification 路径更轻量, 专门用于唤醒 thread

## 7. DPA Thread 间怎么通信

DOCA 没有一个“thread 直接带 payload 发消息给另一个 thread”的现成消息队列。

实际模型通常拆成两件事:

1. 数据放在哪
2. 怎么通知对方来读

### 7.1 最常见模型: 共享内存 + `thread_notify`

这是最像“thread A 给 thread B 发消息”的做法。

流程:

1. host 预先创建 thread A 和 thread B
2. host 给 thread B 建 notification completion
3. thread A 写共享 DPA heap 内存
4. thread A 调 `doca_dpa_dev_thread_notify()` 唤醒 thread B
5. thread B 被唤醒后读取共享内存

关键接口:

- `doca_dpa_notification_completion_create()`: host 侧创建 notification completion
- `doca_dpa_notification_completion_get_dpa_handle()`: host 取 device handle
- `doca_dpa_dev_thread_notify()`: device 侧唤醒目标 thread

DOCA 头文件特别说明了: `notification` 只负责激活 thread, 消息应该通过别的方式传, 比如 shared memory。

参考:

- `/opt/mellanox/doca/include/doca_dpa.h:1447-1452`
- `/opt/mellanox/doca/include/doca_dpa_dev.h:240-253`

本仓库例子:

- `thread_comm/main.c`
- `thread_comm/thread_comm_dev.c`

这个例子实际验证过, 在本机上使用 `mlx5_4` 可以运行成功。

### 7.2 `sync_event`

如果要传递的是:

- ready flag
- completion count
- 阶段推进
- barrier 风格同步

那 `sync_event` 更适合。

`sync_event` 本质是一个可更新、可读取、可等待的 64-bit 值:

- `/opt/mellanox/doca/include/doca_sync_event.h:20-22`

相关配置:

- `doca_sync_event_add_publisher_location_dpa()`: `/opt/mellanox/doca/include/doca_sync_event.h:335-349`
- `doca_sync_event_add_subscriber_location_dpa()`: `/opt/mellanox/doca/include/doca_sync_event.h:414-428`
- `doca_sync_event_get_dpa_handle()`: `/opt/mellanox/doca/include/doca_sync_event.h:1213-1234`

device 侧操作:

- `doca_dpa_dev_sync_event_get()`
- `doca_dpa_dev_sync_event_update_add()`
- `doca_dpa_dev_sync_event_update_set()`
- `doca_dpa_dev_sync_event_wait_gt()`

参考:

- `/opt/mellanox/doca/include/doca_dpa_dev_sync_event.h:49-77`

如果不想阻塞等待, 还可以 post 一个异步 wait, 等条件满足后再激活 thread:

- `doca_dpa_dev_sync_event_post_wait_gt()`
- `doca_dpa_dev_sync_event_post_wait_ne()`

参考:

- `/opt/mellanox/doca/include/doca_dpa_dev_sync_event.h:80-114`

### 7.3 共享状态容器

数据本身可以放在:

- `doca_dpa_mem_alloc()` 分配的 DPA heap
- device 全局数据
- DPA hash table

hash table 相关:

- host 创建: `/opt/mellanox/doca/include/doca_dpa.h:690-741`
- device 访问: `/opt/mellanox/doca/include/doca_dpa_dev.h:343-365`

## 8. 怎么选

可以用下面这个判断顺序:

1. 只想 host 调 DPA 上一个函数并等返回: 用 `RPC`
2. 想一次起很多线程并行处理一批工作: 用 `kernel launch`
3. 想保留常驻 worker, 之后不断被唤醒: 用 `DPA thread`
4. 想保留固定大小的一组常驻 worker: 用 `thread group`
5. 想让 thread A 通知 thread B: 用 `共享内存 + thread_notify`
6. 想做阶段同步或计数完成: 用 `sync_event`

## 9. Practical Takeaway

在这个仓库里, 最好把 3 个主要概念分开记:

- `RPC` = host 到 DPA 的同步函数调用
- `kernel launch` = host 一次提交一个 `N` 线程的并行 kernel
- `DPA thread` = host 创建一个持久 worker, 以后再激活它

`thread_comm/` 目录演示的是:

- host 创建两个持久 DPA thread
- host 用 RPC 唤醒 thread A
- thread A 通过共享内存和 notify 把工作交给 thread B
- thread B 用 `sync_event` 告诉 host 它已经完成

这也是理解 DPA thread 间通信最直接的一条路径。
