# DPA Synchronization Notes

## Summary

DOCA DPA 里的“同步”不是单一 API, 而是几类不同原语的组合:

- 激活线程: `completion context` 或 `thread_notify`
- 传播状态: `sync_event`
- 传递 payload: 共享内存, RDMA buffer, Comch
- 消费外部事件: `doca_dpa_dev_get_completion()`
- 保证可见性: `__dpa_thread_fence()`

最重要的区分是:

- `thread_notify` 是唤醒某个 DPA thread
- `sync_event` 是更新一个 64-bit 状态值, 工程上按单 publisher / 单 subscriber 使用
- `get_completion` 是从 completion context 里取事件
- 共享内存和 Comch 才是真正放 payload 的地方

这页和 `wiki/thread.md` 是互补的:

- `thread.md` 讲执行模型
- `sync.md` 讲同步和通信原语

## Quick Comparison

| 原语 | 主要作用 | 是否带 payload | 是否直接唤醒 thread | 是否适合 host 等待 | 常见用途 |
|---|---|---|---|---|---|
| `doca_dpa_dev_thread_notify()` | 点对点唤醒 thread | 否 | 是 | 否 | thread A 唤醒 thread B |
| `doca_dpa_dev_get_completion()` | 取 completion 事件 | 有 metadata, 不是真正消息体 | 如果 attached thread 存在则会触发 | 间接 | RDMA / async ops / Comch 事件消费 |
| `doca_dpa_dev_sync_event_update_set/add()` | 发布状态或计数 | 仅 64-bit value | 否, 除非配 async wait | 是 | 单发布者完成通知, 阶段推进 |
| 共享内存 | 放真实数据 | 是 | 否 | 可由 host 读取 | mailbox, ring, 结果结构体 |
| Comch / Comch MsgQ | 正式消息通道 | 是 | 可通过 completion 驱动 | 是 | Host/DPU/DPA 之间消息传输 |
| `__dpa_thread_fence()` | 保证顺序和可见性 | 否 | 否 | 否 | publish-before-signal |

## 1. Completion Context

`completion context` 是 DPA 上消费外部事件的入口。

host 侧创建接口:

- `doca_dpa_completion_create()`: `/opt/mellanox/doca/include/doca_dpa.h:1182-1203`
- `doca_dpa_completion_set_thread()`: `/opt/mellanox/doca/include/doca_dpa.h:1235-1251`

关键语义:

- completion context 可以附着到一个 DPA thread
- 当有 completion 到达时, attached thread 会被触发
- thread 里再用 `doca_dpa_dev_get_completion()` 取出 completion element

device 侧接口:

- `doca_dpa_dev_get_completion()`: `/opt/mellanox/doca/include/doca_dpa_dev.h:368-381`
- `doca_dpa_dev_completion_ack()`: `/opt/mellanox/doca/include/doca_dpa_dev.h:528-538`
- `doca_dpa_dev_completion_request_notification()`: `/opt/mellanox/doca/include/doca_dpa_dev.h:540-550`

completion element 里常见可读信息:

- type
- user data
- immediate data
- QP number, WQE counter, received bytes, error syndrome

这条路径适合:

- DPA thread 处理 RDMA receive/send completion
- DPA thread 处理 async ops completion
- DPA thread 处理 Comch consumer completion

不适合:

- 仅仅想唤醒另一个 DPA thread
- 仅仅想传一个阶段值给 host

### 1.1 `get_completion` 不是消息通道

`doca_dpa_dev_get_completion()` 只是在 DPA 上取“已经到达 completion context 的事件”。

它本身不是:

- 消息队列
- payload 存储
- 任意线程对线程的 IPC API

换句话说, 必须先有一个真正会产生活动的资源挂到 completion context 上, 比如:

- RDMA context
- DPA async ops
- Comch consumer

然后 DPA thread 才能在 device 侧通过 `get_completion()` 消费这些事件。

## 2. Notification Completion 和 `thread_notify`

`notification completion` 是专门给 DPA thread 做轻量激活的路径。

host 侧创建接口:

- `doca_dpa_notification_completion_create()`: `/opt/mellanox/doca/include/doca_dpa.h:1444-1465`

DOCA 对它的描述很关键:

- 它用来让 device 侧通过 `doca_dpa_dev_thread_notify()` 激活 attached thread
- 激活 thread 时, 不会在 attached completion context 上产生 completion
- 消息应该通过别的方式传, 比如 shared memory

参考:

- `/opt/mellanox/doca/include/doca_dpa.h:1447-1452`

device 侧接口:

- `doca_dpa_dev_thread_notify()`: `/opt/mellanox/doca/include/doca_dpa_dev.h:240-253`

这条路径的特点是:

- 目标是一个具体 DPA thread
- 没有 payload
- 不是 completion queue
- 不能搭配 `doca_dpa_dev_get_completion()` 读取“通知消息”

最典型模式是:

1. thread A 写共享内存
2. thread A `doca_dpa_dev_thread_notify(thread_b_handle)`
3. thread B 被唤醒后读取共享内存

本仓库例子:

- `thread_comm/thread_comm_dev.c`

## 3. `sync_event`

`sync_event` 是 DOCA 里最通用的跨执行单元同步状态。

官方定义:

- 它是一个软件同步机制
- 抽象为一个 64-bit value
- 可以被 CPU, DPU, DPA, GPU 和 remote node 更新、读取、等待

参考:

- `/opt/mellanox/doca/include/doca_sync_event.h:15-23`

### 3.1 配置方式

`sync_event` 需要先配置 publisher 和 subscriber。

典型 host API:

- `doca_sync_event_add_publisher_location_dpa()`: `/opt/mellanox/doca/include/doca_sync_event.h:335-349`
- `doca_sync_event_add_subscriber_location_cpu()`: `/opt/mellanox/doca/include/doca_sync_event.h:397-411`
- `doca_sync_event_add_subscriber_location_dpa()`: `/opt/mellanox/doca/include/doca_sync_event.h:414-428`

导出 DPA handle:

- `doca_sync_event_get_dpa_handle()`: `/opt/mellanox/doca/include/doca_sync_event.h:1210-1234`

### 3.2 DPA 侧操作

device 侧接口:

- `doca_dpa_dev_sync_event_get()`: `/opt/mellanox/doca/include/doca_dpa_dev_sync_event.h:42-49`
- `doca_dpa_dev_sync_event_update_add()`: `/opt/mellanox/doca/include/doca_dpa_dev_sync_event.h:51-58`
- `doca_dpa_dev_sync_event_update_set()`: `/opt/mellanox/doca/include/doca_dpa_dev_sync_event.h:60-67`
- `doca_dpa_dev_sync_event_wait_gt()`: `/opt/mellanox/doca/include/doca_dpa_dev_sync_event.h:69-77`

如果想“等条件满足后再激活 thread”, 可以用 async wait:

- `doca_dpa_dev_sync_event_post_wait_gt()`: `/opt/mellanox/doca/include/doca_dpa_dev_sync_event.h:80-98`
- `doca_dpa_dev_sync_event_post_wait_ne()`: `/opt/mellanox/doca/include/doca_dpa_dev_sync_event.h:99-114`

### 3.3 线程安全边界

`sync_event` 的 publisher 和 subscriber 是按 location/context 声明的, 不是按 thread 一个个注册的。

例如:

- `doca_sync_event_add_publisher_location_dpa()` 是把一个 `doca_dpa` context 声明成 publisher
- `doca_sync_event_add_subscriber_location_dpa()` 是把一个 `doca_dpa` context 声明成 subscriber

参考:

- `/opt/mellanox/doca/include/doca_sync_event.h:335-349`
- `/opt/mellanox/doca/include/doca_sync_event.h:414-428`

这带来一个容易踩坑的点:

- API 权限看起来是给整个 DPA context 开的
- 但这不等价于同一个 `sync_event` 可以安全地被多个 DPA threads 同时当 publisher 或 subscriber 使用
- `sync_event` 应该按单 publisher / 单 subscriber 设计协议

实践规则:

- 不要用一个 `sync_event` broadcast 唤醒多个 DPA threads
- 不要让多个 DPA threads 同时 `wait_gt()` / `post_wait_*()` 同一个 `sync_event`
- 不要让多个 DPA threads 同时 `update_set()` 同一个 `sync_event`; 这本身也是 last-writer-wins
- 即使 `update_add()` 底层可能是原子加, 也不要把它当成跨线程安全协议的默认 building block

推荐替代方案:

- fan-out 到多个 DPA threads: 每个 thread 用自己的 `notification completion`, 由 `doca_dpa_dev_thread_notify()` 点对点唤醒
- fan-in 多个 DPA threads 完成: 在 DPA shared memory 里用原子计数或每线程槽位收敛, 再由一个 owner thread 更新 host-facing `sync_event`
- 如果确实需要多个等待者: 给每个等待者独立的 `sync_event`, 或改成 shared memory + `thread_notify`

本仓库 `qp_post/` 的处理方式就是这个原则:

- 启动 DPA threads 不用共享 `start_sync_event`
- host 通过 RPC 逐个 `thread_notify` 每个 DPA thread 的 notification completion
- DPA threads 之间用 shared memory 的 `start_count` 做 barrier
- 完成时用 shared memory 的 `done_count` fan-in
- 只有 thread 0 更新 `done_sync_event`, host 是唯一 subscriber

另外, `doca_sync_event_set_doca_buf()` 的注释还明确提到:

- 通过共享同一个底层 8-byte buffer, 可以让多个 DOCA Sync Event instances 做 Thread-to-Thread, Process-to-Process, VM-to-VM notifications

参考:

- `/opt/mellanox/doca/include/doca_sync_event.h:482-512`

这说明多参与者场景不应该理解成“一个 event object 让很多 thread 同时操作”, 更稳妥的做法是“每个参与者使用自己的 Sync Event instance, 必要时共享同一个底层值”。

### 3.4 `sync_event` 和 `thread_notify` 的关系

这两个 API 经常出现在同一个协议里, 但它们不是一类东西。

- `thread_notify` 是“唤醒某个 thread”
- `sync_event_update_set` 是“把事件值改成某个值”

所以:

- `thread_notify` 偏 control path
- `sync_event` 偏 state path

本仓库 `thread_comm/` 里用了两者:

- thread A 用 `thread_notify` 唤醒 thread B
- thread B 用 `sync_event_update_set` 告诉 host 自己完成了

### 3.5 适合场景

`sync_event` 最适合:

- host 等待 DPA 完成
- 单 owner 的阶段推进
- 单 publisher / 单 subscriber 的完成通知

它不适合:

- 直接承载大 payload
- 多 DPA thread 共享同一个 event 做 broadcast start gate
- 多 DPA thread 共享同一个 event 做 barrier

需要 barrier 或计数器时, 优先在 shared memory 里做原子计数, 再让一个 owner thread 通过 `sync_event` 对外发布结果。

### 3.6 二进制观察

对 `thread_comm/` 生成的 DPA device ELF 做反汇编后, `sync_event` 这一层的实现可以总结成下面几点:

- `doca_dpa_dev_sync_event_get()` / `update_add()` / `update_set()` 只是很薄的一层 dispatcher
- 这些 public API 会从 handle 里取出 backend 函数指针, 然后 `jr` 跳过去执行
- 所以 `sync_event handle` 不是单纯的整数, 更像“对象句柄 + 操作表”

对 DPA 本地 backend 来说, 底层很直接:

- `dpa_dev_se_dpa_get` 本质是 `ld`
- `dpa_dev_se_dpa_set` 本质是 `sd`
- `dpa_dev_se_dpa_add` 用的是 `amoadd.d.aqrl`

也就是说, 如果 event 是 DPA-local 的, 它非常接近一个共享的 64-bit 槽位:

- `get` 读这个槽位
- `set` 直接写这个槽位
- `add` 做原子加

对 CPU-visible backend 来说, 实现明显更复杂:

- `get/set/add` 不再只是普通的 heap load/store
- 底层会经过 MMIO/window/descriptor 一类路径
- 反汇编里能看到 `fence io, io`, `fence o, o`, `fence w, w` 以及 doorbell/queue 风格的操作

这说明 `sync_event` 的具体代价和语义, 会随 backend 改变:

- DPA-local 更像共享内存上的 64-bit 同步对象
- CPU 路径更像设备同步原语

`wait_gt()` 的底层也不是“硬件阻塞等待”, 而是:

- 反复调用 `get`
- 比较阈值
- 轮询一段时间后 `yield`
- 再继续轮询

而 `post_wait_gt()` / `post_wait_ne()` 则是另一条路:

- 它们会构造一个异步 wait descriptor
- 交给 async ops 路径
- 条件满足后再激活 thread

对协议设计最重要的影响是:

- `doca_dpa_dev_sync_event_update_set()` 负责更新 sync event 自己
- 它不等价于“发布前面所有对共享 heap 的写”

所以如果你的协议是:

```text
write shared payload
signal via sync_event
```

那么仍然应该认真考虑显式 publish fence, 例如:

```c
__dpa_thread_fence(__DPA_HEAP, __DPA_W, __DPA_W);
doca_dpa_dev_sync_event_update_set(...);
```

这也是 `thread_comm/thread_comm_dev.c` 里保留 heap fence 的原因:

- fence 负责发布共享数据
- `sync_event_update_set` 负责发状态信号

## 4. 共享内存

`thread_notify` 和 `sync_event` 都不是真正的数据载体。

如果要传 payload, 常见做法是用共享内存:

- `doca_dpa_mem_alloc()` 分配的 DPA heap
- DPA 可访问的 host buffer
- RDMA receive buffer
- 设备全局状态或 hash table

最典型的协议是:

```text
write shared data
fence
signal
```

其中 `signal` 可以是:

- `doca_dpa_dev_thread_notify()`
- `doca_dpa_dev_sync_event_update_set()`
- 真实 completion 事件

本仓库 `thread_comm/` 就是 mailbox 风格:

- `shared_state` 放在 DPA heap
- thread A 写 `message`
- thread A notify thread B
- thread B 读 `message`, 写 `reply`
- thread B 用 `sync_event` 告诉 host 完成

## 5. Fence 和可见性

同步不只是“谁先执行”, 还包括“谁什么时候能看见前面的写”。

DOCA DPA 的底层 fence intrinsic 在 `dpaintrin.h` 里定义:

- `__DPA_HEAP`, `__DPA_MEMORY`, `__DPA_MMIO`, `__DPA_SYSTEM`: `/opt/mellanox/doca/lib/x86_64-linux-gnu/dpa_llvm/lib/clang/18/include/dpaintrin.h:28-36`
- `__dpa_thread_fence(...)` 语义: `/opt/mellanox/doca/lib/x86_64-linux-gnu/dpa_llvm/lib/clang/18/include/dpaintrin.h:45-55`

核心语义是:

- fence 前的操作先完成并对其他观察者可见
- 再发生 fence 后的操作

### 5.1 为什么 `thread_comm` 里用了 heap fence

`thread_comm/thread_comm_dev.c` 里在两个地方用了:

- thread A 写共享 DPA heap 后, 再 notify thread B
- thread B 写共享 DPA heap 后, 再 update `sync_event` 给 host

对应代码:

- `__dpa_thread_fence(__DPA_HEAP, __DPA_W, __DPA_W);`

含义是:

- 先把对 DPA heap 的写发布出去
- 再执行后续 signal

这样可以避免:

- thread B 先被唤醒, 却还看不到 thread A 写好的 mailbox
- host 先看到 completion, 却还读到旧的 `reply`

### 5.2 什么时候该主动加 fence

如果你的协议依赖下面这种语义, 就应该认真考虑显式 fence:

- 先写共享状态
- 再发信号
- 接收方一旦看到信号, 就默认前面的写应该都可见

这类模式常见于:

- mailbox
- ring buffer
- ready flag
- completion flag

官方 sample 里不一定总写 fence, 但官方 app 也有显式使用 `__dpa_thread_fence()` 的先例:

- `/opt/mellanox/doca/applications/pcc/device/np/switch_telemetry/np_switch_telemetry_dev_main.c:295-296`

## 6. Comch

`Comch` 是 DOCA Communication Channel。

### 6.1 基础 Comch

基础 Comch 是 Host 和 DPU 之间的 direct communication channel:

- 走 RoCE/IB
- 不走 TCP/IP stack

参考:

- `/opt/mellanox/doca/include/doca_comch.h:15-22`

它提供 server/client/connection 这一层控制通道。

### 6.2 Producer / Consumer

`Comch consumer` / `producer` 是建立在 Comch connection 之上的数据通道。

官方定义:

- accelerated data transfer between memory on the host and DPU in a FIFO format
- 运行在 DMA/PCIe 上
- 不占网络带宽

参考:

- `/opt/mellanox/doca/include/doca_comch_consumer.h:20-25`
- `/opt/mellanox/doca/include/doca_comch_producer.h:20-24`

其中一个很关键的能力是:

- `doca_comch_consumer_set_completion()` 可以把 consumer 关联到 DPA completion context
- 这样 DPA thread 就能通过 completion path 处理 Comch 消息/接收完成

参考:

- `/opt/mellanox/doca/include/doca_comch_consumer.h:500-518`

### 6.3 Comch MsgQ

`doca_comch_msgq` 更接近“消息队列”心智模型。

官方定义:

- 它可以在 Host/DPU 和 DPA 之间建立 direct communication channel
- 不走 TCP/IP stack

参考:

- `/opt/mellanox/doca/include/doca_comch_msgq.h:15-22`

MsgQ 还支持把 producer 或 consumer 放到 DPA 上:

- `doca_comch_msgq_set_dpa_consumer()`: `/opt/mellanox/doca/include/doca_comch_msgq.h:79-95`
- `doca_comch_msgq_set_dpa_producer()`: `/opt/mellanox/doca/include/doca_comch_msgq.h:112-127`

## 7. 怎么选

可以按下面的问题选原语:

1. 只是想唤醒另一个 DPA thread
用 `notification completion + doca_dpa_dev_thread_notify()`

2. 想让 DPA thread 响应 RDMA/async ops/Comch 事件
用 `completion context + doca_dpa_dev_get_completion()`

3. 想让 host 等待 DPA 完成
用单 publisher / 单 subscriber 的 `sync_event`

4. 想传真正的数据 payload
用共享内存, RDMA buffer, 或 Comch

5. 想做正式的 Host/DPU/DPA 消息通道
优先看 Comch / Comch MsgQ

6. 想保证“写完数据再发信号”
考虑显式 `__dpa_thread_fence()`

## 8. Practical Takeaway

最安全的心智模型是把同步拆开看:

- 谁被唤醒
- 谁能看到状态变化
- payload 放在哪
- completion 从哪来
- 是否需要 fence 保证发布顺序

在这个仓库里, 最典型的几条路径是:

- `timer/`: host 用 `RPC` 同步调用 DPA
- `p2p_rtt/doca/`: DPA thread 消费 RDMA completion
- `thread_comm/`: shared memory + `thread_notify` + `sync_event`
- `qp_post/`: per-thread `thread_notify` 启动, shared memory 计数 fan-in, 单个 `done_sync_event` 通知 host

如果只记一句话:

- `thread_notify` 唤醒 thread
- `sync_event` 传播状态, 但按单 publisher / 单 subscriber 使用
- `get_completion` 消费外部事件
- payload 另找地方放
