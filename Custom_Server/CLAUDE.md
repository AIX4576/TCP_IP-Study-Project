# CLAUDE.md

本文件为 `Custom_Server` 模块的目录级指导，适用于 `Custom_Server/` 目录下的所有文件；仓库级常驻规则（构建、模块化插件体系约定）见根目录 `CLAUDE.md`。

## 模块职责

Custom_Server 是当前仓库唯一的业务模块，编译为 DLL，实现基于 IOCP（I/O 完成端口）的高性能 TCP 服务器。核心实现位于 `iocp_server.h/cpp`，线程拓扑由 `custom_server_thread.cpp` 的 `main_thread()` 启动。

## 线程拓扑

```
监听 socket + IOCP 完成端口
  ├─ work_thread × Worker_Threads_Number（GetQueuedCompletionStatus 处理完成事件）
  ├─ send_thread × Application_Threads_Number（从专属 send_queue 取数据投递 WSASend，与 application_thread 一对一）
  ├─ guard_thread × 1（补充 AcceptEx 投递、清理活动超时连接）
  └─ application_thread × Application_Threads_Number（从 receive_queues 取数据做业务处理，回投 send_queues）
```

数据流：
`work_thread` 处理完成事件 → 按 socket 哈希入 `receive_queues` 并释放接收信号量 → `application_thread` 获取接收信号量，然后消费、构造发送事件入 `send_queues` 并释放发送信号量 →  `send_thread` 获取发送信号量并投递 `WSASend`。
`work_thread` 对 `application_thread` 为多生产者对单消费者，`application_thread` 对 `send_thread` 为单生产者对单消费者。

- 线程数量由宏控制（默认取 `hardware_concurrency()`）：`Worker_Threads_Number = N/2`、`Application_Threads_Number = N/2`；send_thread 数量与 `Application_Threads_Number` 相同，每个 send_thread 对应一条专属 send_queue（与 application_thread 一对一）
- 队列同步使用 `counting_semaphore<>`（初始 0）：入队方 `release()`（work_thread 批量入队时 `release(count)`），消费方 `try_acquire_for(500ms)` 等待——既避免忙轮询，又能及时响应 `run_flag` 退出
- **线程回收**：`main_thread` 创建业务线程时持有 `thread_pool_mtx`。若 `stop_run_impl()` 持同一把锁 join 这些线程，且 stop 发生在 `main_thread` 完成线程创建之前，会互相等待死锁（join 等线程结束，线程等锁推进）。因此 `stop_run_impl()` 必须遵循收敛式回收：持锁仅换出线程快照、解锁后 join、循环直至 `thread_pool` 清空（详见 `ModuleHandle.h` 基类注释）
- 每个 client 连续投递 `Per_Client_Receive_Event_Number` 个异步 WSARecv；每个接收事件带递增序号，`Client_Handle::Receive_Data_Process()` 用 `ordered_data`（boost::circular_buffer）和 `unordered_data`（folly::F14FastMap）做有序重组，无序数据积压超过阈值（8）时判定数据丢失并断开连接
- `guard_thread` 保证监听 socket 上始终有足够数量的待处理 AcceptEx
- 客户端异常断开时向业务层投递一个**空数据**的 Event_handle（`buffer.len == 0`）表示断开通知

## Event_handle 生命周期（易错点）

`Event_handle`（含 OVERLAPPED + WSABUF + 数据缓冲）全部来自 `ConcurrentObjectPool` 对象池（按线程 ID 哈希选池）。构造/销毁必须经由 `Server_Handle::Construct_Event_handle()` / `Destroy_Event_handle()`，不能直接 new/delete。对象流转位置见 `iocp_server.h` 中 `p_event_handle_pools` 注释：

1. 存储在 `Client_Handle` 的 ordered/unordered 缓冲区中（等待业务层取出）
2. 投递在 IOCP 中等待异步完成
3. 在无锁队列中等待业务线程/发送线程处理

`Server_Handle` 析构前必须清空各 `Client_Handle` 缓冲区中的 Event_handle 并取消所有异步 IO，否则对象池泄漏。`Client_Handle` 析构函数负责清理自身缓冲区的对象。

## 目录文件说明

- `iocp_server.h/cpp` — 服务器核心（IOCP、Client_Handle、Server_Handle、Event_handle）
- `custom_server_thread.h/cpp` — 线程入口与线程拓扑创建
- `application.h/cpp` — 业务处理层（application_thread 消费 receive_queues）
- `custom_server_interface.cpp` — 导出 `Initialize()`（extern "C"）
- `override_new_delete.cpp` — 全局 operator new/delete 覆盖实现

## 注意

- 本文件随模块开发同步维护；修改本目录代码后检查本文件和根目录 `CLAUDE.md` 是否需要更新
