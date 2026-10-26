# CLAUDE.md

本文件为 `Custom_Server` 模块的目录级指导，适用于 `Custom_Server/` 目录下的所有文件；仓库级常驻规则（构建、模块化插件体系约定）见根目录 `CLAUDE.md`。

## 模块职责

Custom_Server 是当前仓库唯一的业务模块，编译为 DLL，实现基于 IOCP（I/O 完成端口）的高性能 TCP 服务器。核心实现位于 `iocp_server.h/cpp`，线程拓扑由 `custom_server_thread.cpp` 的 `main_thread()` 启动。

## 线程拓扑

```
监听 socket + IOCP 完成端口
  ├─ work_thread × Worker_Threads_Number（GetQueuedCompletionStatus 处理完成事件）
  ├─ send_thread × Application_Threads_Number（从专属 send_queue 取数据投递 WSASend，与 application_thread 一对一）
  ├─ guard_thread × 1（按 pending 计数补投 AcceptEx、扫描活动超时连接并投递断开请求）
  └─ application_thread × Application_Threads_Number（从 receive_queues 取数据做业务处理，回投 send_queues）
```

数据流：
`work_thread` 处理完成事件 → 按 socket 哈希入 `receive_queues` 并释放接收信号量 → `application_thread` 获取接收信号量，然后消费、构造发送事件入 `send_queues` 并释放发送信号量 →  `send_thread` 获取发送信号量并投递 `WSASend`。
`work_thread` 对 `application_thread` 为多生产者对单消费者，`application_thread` 对 `send_thread` 为单生产者对单消费者。

- 线程数量由宏控制（默认取 `hardware_concurrency()`）：`Worker_Threads_Number = N/2`、`Application_Threads_Number = N/2`；send_thread 数量与 `Application_Threads_Number` 相同，每个 send_thread 对应一条专属 send_queue（与 application_thread 一对一）
- 队列同步使用 `counting_semaphore<>`（初始 0）：入队方 `release()`（work_thread 批量入队时 `release(count)`），消费方 `try_acquire_for(500ms)` 等待——既避免忙轮询，又能及时响应 `run_flag` 退出
- **线程回收**：`main_thread` 创建业务线程时持有 `thread_pool_mtx`。若 `stop_run_impl()` 持同一把锁 join 这些线程，且 stop 发生在 `main_thread` 完成线程创建之前，会互相等待死锁（join 等线程结束，线程等锁推进）。因此 `stop_run_impl()` 必须遵循收敛式回收：持锁仅换出线程快照、解锁后 join、循环直至 `thread_pool` 清空（详见 `ModuleHandle.h` 基类注释）
- **关闭流程（哨兵驱动的确定性关闭）**：work_thread 生命周期不依赖 `run_flag`，由 `Server_Handle::Close_Server()` 管理——依次执行：置 `io_running=false`（兜底）→ `closesocket` 监听 socket → 投递 `Worker_Threads_Number` 个 `overlapped==nullptr` 哨兵完成包 → 轮询等待 `work_thread_exit_count` 达标 → 持写锁清空 `client_handles` → main_thread 排空 IOCP 队列并销毁全部残留事件。此顺序保证 socket 关闭产生的取消完成事件必有人处理，`Server_Handle` 析构时对象池完全配对、无事件泄漏。其中 `closesocket` 取消 pending AcceptEx 产生的完成包以 `ERROR_OPERATION_ABORTED(995)` 返回，属预期路径：work_thread 仍照常自减 `pending_accept_count`、摘除待 accept 条目、销毁事件，但不按错误记录日志（否则每次退出会刷出约 `Worker_Threads_Number` 条）。application/send 线程由**队列哨兵链**管理退出：main_thread 在 IOCP 排空（事件流断源）后向每条 receive_queue 投递 `nullptr` 哨兵，application_thread 消费完积压收到哨兵、向 send_queue 交接哨兵后退出，send_thread 消费完积压后退出——FIFO 保证退出即排空，send_queue 无残留；两线程均保留 `run_flag` 超时兜底防哨兵丢失死等。guard 线程仍由 `run_flag` 轮询退出，投递 AcceptEx 前检查 `io_running`（须置于其他条件之前），以消除与关闭序列的 socket 数据竞争
- 每个 client 连续投递 `Per_Client_Receive_Event_Number` 个异步 WSARecv；每个接收事件带递增序号，`Client_Handle::Receive_Data_Process()` 用 `ordered_data`（boost::circular_buffer）和 `unordered_data`（folly::F14FastMap）做有序重组，无序数据积压超过阈值（8）时判定数据丢失并断开连接
- **AcceptEx 计数**：待处理 AcceptEx 数量由 `Server_Handle::pending_accept_count` 维护——`Post_AcceptEx()` 在调用 AcceptEx 前自增（三处投递点统一走该函数），work_thread 的两个完成分支（成功分支、AcceptEx 异步失败分支）自减。补投判据一律用它而非 `client_handles.size()`：后者含已建立的活跃连接，会把总连接数误当待处理 AcceptEx 数，导致 pending 耗尽后再无新连接被 accept
- **主动断开统一入口**：guard_thread（活动超时）、send_thread（WSASend 同步失败）与 work_thread（部分发送续发失败）都只做一件事——构造 `Event_Disconnect` 事件并 `PostQueuedCompletionStatus` 投递给 work_thread；work_thread 的 `Event_Disconnect` 分支持写锁统一执行"复位（`Client_Handle::Disonnect_Deal()`）+ 通知业务层 + 从容器摘除"。投递端只携带 socket 值，投递到处理之间若句柄被回收或连接已恢复活动，会被误断（已知并接受的窗口）
- guard_thread 每 `Client_Active_Timeout_Scan_Interval` 秒持共享锁扫描一次 `client_handles`，收集 `Is_Active_Timeout()` 的连接后解锁再逐个投递断开请求；活动时间基准为 `steady_clock`（避免系统时间跳变误判）
- 客户端异常断开或主动断开时，都向业务层投递一个**空数据**的 Event_handle（`buffer.len == 0`）表示断开通知，统一由 `Notify_Client_Disconnected()` 发出
- **发送部分完成**：`WSASend` 可能只发出一部分即完成。work_thread 的 `Event_Send` 完成分支按 `bytes_transferred` 前移 `buffer.buf`、缩减 `buffer.len`，用同一个 OVERLAPPED 续发剩余部分，直到发完；续发同步失败或事件在途期间连接已被摘除/断开时投递 `Event_Disconnect`，走统一断开路径。**不能把事件回投 `send_queue`**——`send_thread` 会无条件把 `buffer.buf` 重置回 `data`，导致已发部分被重复发送

## Event_handle 生命周期（易错点）

`Event_handle`（含 OVERLAPPED + WSABUF + 数据缓冲）全部来自 `ConcurrentObjectPool` 对象池（按线程 ID 哈希选池）。构造/销毁必须经由 `Server_Handle::Construct_Event_handle()` / `Destroy_Event_handle()`，不能直接 new/delete。该类**不可拷贝、不可移动**——它由对象池分配、以指针流转，在途期间被 IOCP 通过 OVERLAPPED 地址引用，移动会让完成包指向旧地址；内嵌的 `data[1460]` 也只能整块拷贝。对象流转位置见 `iocp_server.h` 中 `p_event_handle_pools` 注释：

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
