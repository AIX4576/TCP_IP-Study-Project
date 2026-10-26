#pragma once
#include<iostream>
#include<thread>
#include<chrono>
#include<string>
#include<vector>
#include<shared_mutex>
#include<atomic>
#include<memory>
#include<semaphore>
using namespace std;

#include"folly/container/F14Map.h"
#include"folly/hash/Hash.h" // twang_mix64：socket 句柄到接收队列的均匀映射
#include"boost/circular_buffer.hpp"
#include"concurrentqueue.h"

#include"ConcurrentObjectPool.h"

#define WIN32_LEAN_AND_MEAN
#include<WS2tcpip.h> //基础套接字API头文件
#include<MSWSock.h>	 //扩展套接字API头文件
#pragma comment(lib, "ws2_32.lib")	// 链接基础套接字库
#pragma comment(lib, "mswsock.lib") // 链接扩展套接字库

#define Server_Port 8080

#define Threads_Number (std::thread::hardware_concurrency())
#define Application_Threads_Number (Threads_Number / 2)
#define Worker_Threads_Number (Threads_Number / 2)
#define Max_Clients_Number 16384

#define Receive_Data_Length 0
#define Local_Address_Length (sizeof(SOCKADDR_IN) + 16)
#define Remote_Address_Length (sizeof(SOCKADDR_IN) + 16)

#define Per_Client_Receive_Event_Number 3
#define Per_Client_Unordered_Data_Number_Threshold 8
#define Client_Active_Timeout_Second 180
#define Client_Active_Timeout_Scan_Interval 60
#define Event_Buffer_Size 1460

class Server_Handle;
class Client_Handle;
struct Event_handle;

enum Socket_Status
{
	Socket_Invalid,
	Socket_Disconnected,
	Socket_Connected,
};

enum Deliver_Event
{
	Event_None,
	Event_Accept_Connect,
	Event_Disconnect,// 主动断开：由 guard_thread（活动超时）或 send_thread（WSASend 同步失败）投递，work_thread 统一执行复位/通知/摘除
	Event_Send,
	Event_Receive,
};

struct Event_handle
{
public:
	OVERLAPPED overlapped{};//必须清零 OVERLAPPED 结构，否则可能导致未定义行为
	SOCKET socket;
	Deliver_Event event;
	DWORD flag{};//WSARecv()要用到，默认是0
	WSABUF buffer{};//WSARecv()和WSASend()要用到
	char data[Event_Buffer_Size];//数据缓冲区

	Event_handle(size_t id = 0) : socket(INVALID_SOCKET), event(Event_None), id(id)
	{
		buffer.buf = data;
		buffer.len = Event_Buffer_Size;
	}
	Event_handle(SOCKET socket, Deliver_Event event, uint32_t buffer_size = Event_Buffer_Size, size_t id = 0) : socket(socket), event(event), id(id)
	{
		buffer.buf = data;
		buffer.len = min(buffer_size, Event_Buffer_Size);
	}
	/**
	 * @brief 拷贝与移动一律禁用（拷贝构造、拷贝赋值、移动构造、移动赋值均为 delete）。
	 * @details Event_handle 由对象池分配、以指针形式流转；in-flight 期间被 IOCP 通过 OVERLAPPED 的地址引用，
	 *          身份（对象地址 + pool_index + buffer.buf 指向自身内嵌 data）必须保持不变。
	 *          内嵌的 char data[Event_Buffer_Size] 无法"窃取"，移动只能整块拷贝；
	 *          且在途事件被移动会让完成包指向旧地址（悬垂指针）。
	 */
	Event_handle(const Event_handle&) = delete;
	Event_handle& operator=(const Event_handle&) = delete;
	Event_handle(Event_handle&&) = delete;
	Event_handle& operator=(Event_handle&&) = delete;
	~Event_handle() {}
	size_t Get_id() const
	{
		return id;
	}
	void Set_id(size_t id)
	{
		this->id = id;
	}
	size_t Get_pool_index() const
	{
		return pool_index;
	}
	void Set_pool_index(size_t pool_index)
	{
		this->pool_index = pool_index;
	}

private:
	size_t id{};
	size_t pool_index{};
};

// Event_handle 的独占指针类型：删除器绑定到构造它的对象池，保证归还到正确的池
using Event_handle_Ptr = unique_ptr<Event_handle, ConcurrentObjectPool<Event_handle>::Deleter>;

class Client_Handle
{
public:
	Server_Handle* pServer_Handle;
	SOCKET socket;
	atomic<Socket_Status> socket_status;//atomic 化：写发生在 work_thread 的 Connect/Disconnect 处理、读散布在 send_thread 与错误处理分支，读写锁错位无法互斥
	string ip;
	uint16_t port{};
	uint8_t output_buffer[Receive_Data_Length + Local_Address_Length + Remote_Address_Length]{};// 地址 + 数据

	Client_Handle(Server_Handle* pServer_Handle) :socket(INVALID_SOCKET), socket_status(Socket_Invalid), pServer_Handle(pServer_Handle)
	{
		socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
		if (socket != INVALID_SOCKET)
		{
			socket_status = Socket_Disconnected;
			ordered_data.set_capacity(Per_Client_Unordered_Data_Number_Threshold + 3);
			unordered_data.reserve(Per_Client_Unordered_Data_Number_Threshold + 3);
		}
	}
	Client_Handle(Client_Handle&) = delete;
	Client_Handle& operator=(Client_Handle&) = delete;
	Client_Handle(Client_Handle&& other) noexcept
	{
		/**
		 * 移动构造函数，将自身资源初始化为0后交换；atomic 成员不支持 swap，手工交换其载荷并将源对象清零
		 */
		if (this != &other)
		{
			socket = INVALID_SOCKET;
			socket_status = Socket_Invalid;

			swap(pServer_Handle, other.pServer_Handle);
			swap(socket, other.socket);
			Socket_Status status = other.socket_status.load(memory_order_acquire);
			other.socket_status.store(socket_status.load(memory_order_relaxed));
			socket_status.store(status);
			swap(ip, other.ip);
			swap(port, other.port);
			memcpy(output_buffer, other.output_buffer, sizeof(output_buffer));
			swap(next_expected_sequence, other.next_expected_sequence);
			swap(ordered_data, other.ordered_data);
			swap(unordered_data, other.unordered_data);
			unordered_data_count.store(other.unordered_data_count.exchange(0, memory_order_acq_rel), memory_order_release);
			last_active_time_sec.store(other.last_active_time_sec.exchange(0, memory_order_acq_rel), memory_order_release);
		}
	}
	Client_Handle& operator=(Client_Handle&& other) noexcept
	{
		/**
		 * 移动赋值函数，将自身资源释放后置0，然后交换；atomic 成员不支持 swap，手工交换其载荷并将源对象清零
		 */
		if (this != &other)
		{
			if (socket != INVALID_SOCKET)
			{
				CancelIoEx((HANDLE)socket, nullptr);
				closesocket(socket);
				socket = INVALID_SOCKET;
				socket_status = Socket_Invalid;
			}

			ip.clear();
			ordered_data.clear();
			unordered_data.clear();
			unordered_data_count.store(0, memory_order_relaxed);

			swap(pServer_Handle, other.pServer_Handle);
			swap(socket, other.socket);
			Socket_Status status = other.socket_status.load(memory_order_acquire);
			other.socket_status.store(socket_status.load(memory_order_relaxed));
			socket_status.store(status);
			swap(ip, other.ip);
			swap(port, other.port);
			memcpy(output_buffer, other.output_buffer, sizeof(output_buffer));
			swap(next_expected_sequence, other.next_expected_sequence);
			swap(ordered_data, other.ordered_data);
			swap(unordered_data, other.unordered_data);
			unordered_data_count.store(other.unordered_data_count.exchange(0, memory_order_acq_rel), memory_order_release);
			last_active_time_sec.store(other.last_active_time_sec.exchange(0, memory_order_acq_rel), memory_order_release);
		}

		return *this;
	}
	~Client_Handle();
	void Connect_Deal()
	{
		sockaddr_in* pClient_address = reinterpret_cast<sockaddr_in*>(output_buffer);

		char client_ip[16]{};
		inet_ntop(AF_INET, &pClient_address->sin_addr, client_ip, sizeof(client_ip));

		ip = client_ip;
		port = ntohs(pClient_address->sin_port);

		/**
		 * 必须先写活动时间再置 Connected：guard_thread 以（状态 == Connected 且已超时）判定断开，
		 * 若反序，中间窗口会被读到"Connected 但 last_active_time_sec 仍为 0"，
		 * duration 退化为 steady_clock 起点的秒数，活跃连接被判超时
		 */
		Update_Last_Active_Time();
		socket_status = Socket_Connected;
	}
	/**
	 * @brief 断开复位：清空连接状态，并销毁有序/无序缓冲区中尚未被业务层取走的全部 Event_handle。
	 * @details 原实现 unordered_data.clear() 直接丢弃指针泄漏对象，且 ordered_data 未清空——socket 复用后序号归零，
	 *          残留数据会被新连接当作连续数据取走，造成新旧数据混序。
	 * @warning 调用约束：调用方必须持有 Server_Handle::smutex 的写锁。原因有二：
	 *          ① erase 需要写锁且会析构本对象，调用期间必须保证无人摘除，否则这里对 lock 的操作就是 use-after-free；
	 *          ② 本函数的语义是"复位 + 通知业务层 + 摘除"这个原子转换的前半段，写锁保证其它线程只会看到
	 *          "连接完整存活"或"已从容器消失"，看不到缓冲已销毁、序号已归零、socket 上仍挂着在途完成包的中间态，
	 *          也不会在垂死的 socket 上重新挂 IO（Event_Receive 分支末尾会无条件重投 WSARecv）。
	 * @note 附带性质：自旋锁 lock 的持有者都在 work_thread 的 Event_Receive 分支持 smutex 共享锁，
	 *       写锁已排除它们，故此处自旋必然一次成功，锁序（smutex → 客户端自旋锁 → 对象池自旋锁）单向向下。
	 * @note 若将来出现"只复位、不摘除"的用法，至少需持共享锁以阻止并发 erase。
	 */
	void Disonnect_Deal()
	{
		/**
		 * 复位状态必须与 Receive_Data_Process 互斥：next_expected_sequence、ordered_data、unordered_data 均由后者在 lock 临界区内读写
		 */
		while (lock.test_and_set(memory_order_acquire))
		{
			this_thread::yield();
		}

		socket_status = Socket_Disconnected;
		ip.clear();
		port = 0;
		receive_event_sequence = 0;
		next_expected_sequence = 0;
		last_active_time_sec.store(0, memory_order_relaxed);//断开后不再参与超时判定
		Destroy_Buffered_Events();

		lock.clear(memory_order_release);
	}
	/**
	 * @brief 用当前时间刷新最后活动时间（steady_clock 起点的秒数）。
	 * @note 取 steady_clock（单调时钟）而非 system_clock：后者会被 NTP 校时或用户改系统时间影响，
	 *       时间前跳会把活跃连接误判为超时、后跳会让超时永不触发。
	 */
	void Update_Last_Active_Time()
	{
		last_active_time_sec.store(
			chrono::duration_cast<chrono::seconds>(chrono::steady_clock::now().time_since_epoch()).count(),
			memory_order_release);
	}
	bool Is_Active_Timeout() const
	{
		long long last_active = last_active_time_sec.load(memory_order_acquire);
		if (last_active == 0)
		{
			/** 从未记录过活动时间（连接刚建立、Connect_Deal 尚未写完活动时间），不判超时 */
			return false;
		}

		long long current_time = chrono::duration_cast<chrono::seconds>(chrono::steady_clock::now().time_since_epoch()).count();
		long long duration = current_time - last_active;

		return ((duration > Client_Active_Timeout_Second) && (socket_status.load(memory_order_acquire) == Socket_Connected));
	}
	size_t Make_Receive_Event_id()
	{
		return receive_event_sequence.fetch_add(1, memory_order_release);
	}
	/**
	 * @brief 处理接收到的数据：按接收序号判断其为「有效」还是「过期重复」，有效数据进入有序重组流程。
	 * @param pEvent 本次完成的接收事件；有效数据会被本函数存入 ordered_data 或 unordered_data。
	 * @param bytes_transferred 本次实际接收到的字节数，写入 pEvent->buffer.len。
	 * @return bool true 表示数据有效，已被 ordered_data 或 unordered_data 存储；false 表示过期重复数据，
	 *         未被任何缓冲区存储，需要外部销毁 pEvent。
	 * @warning 返回 true 时 pEvent 的所有权已转移给缓冲区，外部在数据被取出前不能使用或销毁 pEvent；
	 *          返回 false 时外部必须销毁 pEvent。
	 */
	bool Receive_Data_Process(Event_handle* pEvent, DWORD bytes_transferred)
	{
		bool valid_data = false;

		pEvent->buffer.len = bytes_transferred;
		size_t event_sequence = pEvent->Get_id();

		while (lock.test_and_set(memory_order_acquire))
		{
			this_thread::yield();
		}

		if (event_sequence == next_expected_sequence)
		{
			/** 放进有序数据缓冲区，同时查找无序数据缓冲区中是否有数据和其连续 */
			ordered_data.push_back(pEvent);
			next_expected_sequence++;

			auto it = unordered_data.find(next_expected_sequence);
			while (it != unordered_data.end())
			{
				ordered_data.push_back(it->second);
				unordered_data.erase(it);
				unordered_data_count.fetch_sub(1, memory_order_relaxed);
				next_expected_sequence++;
				it = unordered_data.find(next_expected_sequence);
			}

			valid_data = true;
		}
		else if (event_sequence > next_expected_sequence)
		{
			/**
			 * 放进无序数据缓冲区；插入成功才计入镜像计数，重复序号插入失败时按无效数据处理（由调用方销毁）
			 */
			if (unordered_data.emplace(event_sequence, pEvent).second)
			{
				unordered_data_count.fetch_add(1, memory_order_relaxed);
				valid_data = true;
			}
		}
		else
		{
			/** 重复数据，丢弃 */
			valid_data = false;
		}

		lock.clear(memory_order_release);

		return valid_data;
	}
	/**
	 * @brief 取走有序缓冲区中的数据，最多 max_count 个。
	 * @param[out] pEvent_array 接收 Event_handle* 的数组，调用方需确保其容量不小于 max_count。
	 * @param max_count 本次最多取出的数据个数。
	 * @return size_t 实际取出的有序数据数量；取出的 Event_handle 所有权随之转交调用方，由其负责销毁。
	 */
	size_t Get_Ordered_Data(Event_handle** pEvent_array, size_t max_count)
	{
		size_t count = 0;

		while (lock.test_and_set(memory_order_acquire))
		{
			this_thread::yield();
		}

		while (ordered_data.empty() == false)
		{
			if (count >= max_count)
			{
				break;
			}

			pEvent_array[count++] = ordered_data.front();
			ordered_data.pop_front();
		}

		lock.clear(memory_order_release);

		return count;
	}
	size_t Get_Unordered_Data_Number() const
	{
		/**
		 * 读原子镜像计数而非容器 size：size() 的锁外读取与其它线程自旋锁临界区内的写构成数据竞争
		 */
		return unordered_data_count.load(memory_order_acquire);
	}
	/**
	 * @brief 取走无序缓冲区中积压的数据，最多 max_count 个。
	 * @param[out] pEvent_array 接收 Event_handle* 的数组，调用方需确保其容量不小于 max_count。
	 * @param max_count 本次最多取出的数据个数。
	 * @return size_t 实际取出的无序数据数量；取出的 Event_handle 所有权随之转交调用方，由其负责销毁。
	 */
	size_t Get_Unordered_Data(Event_handle** pEvent_array, size_t max_count)
	{
		size_t count = 0;

		while (lock.test_and_set(memory_order_acquire))
		{
			this_thread::yield();
		}

		/**
		 * it = erase(it) 模式推进：erase 后旧迭代器失效，范围 for 中对已删元素自增属未定义行为
		 */
		auto it = unordered_data.begin();
		while ((it != unordered_data.end()) && (count < max_count))
		{
			pEvent_array[count++] = it->second;

			it = unordered_data.erase(it);
			unordered_data_count.fetch_sub(1, memory_order_release);
		}

		lock.clear(memory_order_release);

		return count;
	}
	
private:
	/**
	 * @brief 销毁有序/无序缓冲区中存储的全部 Event_handle 并清空容器。
	 * @warning 调用方必须已持有本对象的自旋锁 lock。
	 * @note 实现在 iocp_server.cpp——此处 Server_Handle 尚为前置声明，无法调用其成员函数；
	 *       且不可在临界区内调用 Get_Ordered_Data/Get_Unordered_Data，自旋锁不可重入，故直接操作容器。
	 */
	void Destroy_Buffered_Events();

	atomic_flag lock{};// 最简单的原子类型，仅支持 test_and_set 和 clear 操作
	atomic<size_t> receive_event_sequence{};//接收事件的序号;总共可以用2^64大小的事件数量，假设每秒消耗1亿个事件，也可以使用5849年，可以认为不会消耗完
	size_t next_expected_sequence{};//下一个期望接收的事件的序号（仅在 lock 临界区内读写）
	boost::circular_buffer<Event_handle*> ordered_data;//有序数据缓冲区（存放 Event_handle*，仅在 lock 临界区内读写）
	folly::F14FastMap<size_t, Event_handle*> unordered_data;//无序数据缓冲区（存放 Event_handle*，仅在 lock 临界区内读写）
	atomic<size_t> unordered_data_count{};//unordered_data.size() 的原子镜像，保证锁外读取不构成数据竞争；只能在 lock 临界区内变更
	atomic<long long> last_active_time_sec{};//最后活动时间（epoch 起的秒数），atomic 化供多线程并发更新/读取
};

class Server_Handle
{
public:
	folly::F14FastMap<SOCKET, Client_Handle> client_handles;
	shared_mutex smutex;
	//IOCP 运行标志：Close_Server() 第一步置 false，作为 work_thread 哨兵包丢失时的兜底退出条件
	atomic<bool> io_running{ true };
	//已退出的 work_thread 计数：Close_Server() 据此确认全部 work_thread 已停止访问 IOCP，才继续关闭 socket 与排空队列
	atomic<size_t> work_thread_exit_count{ 0 };
	//待处理 AcceptEx 计数（问题14b）：已成功投递给系统的 AcceptEx 个数（含同步完成的）。
	//补投判据必须用它而非 client_handles.size()——后者含已建立的活跃连接，活跃连接一多条件恒假，
	//pending AcceptEx 耗尽后再无新连接被 accept。
	//自增在 Post_AcceptEx() 内、自减在 work_thread 的两个完成分支（成功分支与 AcceptEx 异步失败分支），必须成对
	atomic<size_t> pending_accept_count{ 0 };

	Server_Handle();
	Server_Handle(Server_Handle&) = delete;
	Server_Handle(Server_Handle&& other) = delete;
	Server_Handle& operator=(Server_Handle&) = delete;
	Server_Handle& operator=(Server_Handle&& other) = delete;
	~Server_Handle()
	{
		if (socket != INVALID_SOCKET)
		{
			closesocket(socket);
			socket = INVALID_SOCKET;
		}
		if (iocp != nullptr)
		{
			CloseHandle(iocp);
			iocp = nullptr;
		}

		initialize_flag = FALSE;
	}

	bool Get_Initialize_Flag() const
	{
		return initialize_flag;
	}
	SOCKET Get_Socket_Server() const
	{
		return socket;
	}
	HANDLE Get_IOCP() const
	{
		return iocp;
	}
	/**
	 * @brief 确定性关闭序列，替代原 sleep 赌时序的写法。
	 * @details 旧实现中 run_flag=false 令 work_thread 在 GQCS 超时后即退出，而 socket 关闭产生的
	 *          取消完成事件发生在其后，无人处理，pending 的 Event_handle 全部滞留 IOCP 队列泄漏。
	 *          执行顺序：
	 *          1. io_running 置 false：兜底标志，防止哨兵包丢失导致 work_thread 永久阻塞
	 *          2. closesocket(listen)：取消全部 pending AcceptEx，取消事件入队（仍存活的 work_thread 会取走销毁）
	 *          3. 投递退出哨兵（overlapped == nullptr 的完成包）× Worker_Threads_Number：
	 *             每个 work_thread 处理完手头及排队事件后，消费一个哨兵退出并计数
	 *          4. 轮询等待 work_thread_exit_count 达标：此刻全部 work_thread 已退出，不再访问 IOCP 与事件
	 *          5. 关闭全部 client socket：取消其 pending WSARecv/WSASend，取消事件入队
	 *          6. main_thread 排空 IOCP 队列：销毁全部残留事件，保证对象池完全配对；残留哨兵包跳过
	 */
	void Close_Server()
	{
		cout << "closing server ..." << endl;
		io_running.store(false, memory_order_release);
		closesocket(socket);
		socket = INVALID_SOCKET;

		for (size_t i = 0; i < Worker_Threads_Number; i++)
		{
			PostQueuedCompletionStatus(iocp, 0, 0, nullptr);
		}

		while (work_thread_exit_count.load(memory_order_acquire) < Worker_Threads_Number)
		{
			this_thread::sleep_for(chrono::milliseconds(10));
		}

		cout << "all work threads exited, closing all client sockets ..." << endl;
		{
			/** work_thread 已全部退出，但仍需持写锁：send_thread 可能正在读取 client_handles */
			unique_lock<shared_mutex> unique_lock{ smutex };
			client_handles.clear();
		}

		cout << "draining iocp queue ..." << endl;
		DWORD bytes_transferred = 0;
		ULONG_PTR completion_key = 0;
		LPOVERLAPPED pOverlapped = nullptr;
		for (;;)
		{
			BOOL result = GetQueuedCompletionStatus(iocp, &bytes_transferred, &completion_key, &pOverlapped, 0);// 超时0：只取已入队事件
			if ((result == FALSE) && (pOverlapped == nullptr))
			{
				break;// 队列已空
			}

			if (pOverlapped != nullptr)
			{
				/** 关闭阶段不再投递业务层，残留事件直接销毁，保证对象池完全配对 */
				Destroy_Event_handle(reinterpret_cast<Event_handle*>(pOverlapped));
			}
			/** pOverlapped == nullptr 且 result == TRUE：残留的退出哨兵包，跳过 */
		}
	}
	/**
	 * @brief 把 socket 路由到对应接收队列：同一 socket 必须恒定映射到同一队列（保序），不同 socket 尽量均匀散开。
	 * @param socket 客户端 socket 句柄。
	 * @param range 接收队列数量，即取模基数。
	 * @return size_t 落点下标，取值范围 [0, range)；range == 0 时返回 0。
	 * @note std::hash<SOCKET> 恒等映射且 Windows 句柄值 4 对齐，直接 % range 会塌缩到极少队列（如 8 队列实际只落 {0,4}），
	 *       故先经 twang_mix64 混淆再取模；range == 0（单核下 Application_Threads_Number 为 0）时保护返回。
	 */
	size_t Socket_Map_In_Range(SOCKET socket, size_t range) const
	{
		if (range == 0)
		{
			return 0;
		}

		return folly::hash::twang_mix64(socket) % range;
	}
	template<typename... Args>
	Event_handle* Construct_Event_handle(Args&&... args)
	{
		size_t pool_index = hasher_thread_id(this_thread::get_id()) % p_event_handle_pools.size();
		ConcurrentObjectPool<Event_handle>& event_handle_pool = *(p_event_handle_pools.at(pool_index));

		Event_handle* p = event_handle_pool.Construct(forward<Args>(args)...);
		if (p)
		{
			p->Set_pool_index(pool_index);
		}

		return p;
	}
	template<typename... Args>
	Event_handle_Ptr MakeUnique_Event_handle(Args&&... args)
	{
		size_t pool_index = hasher_thread_id(this_thread::get_id()) % p_event_handle_pools.size();
		ConcurrentObjectPool<Event_handle>& event_handle_pool = *(p_event_handle_pools.at(pool_index));

		Event_handle_Ptr p(event_handle_pool.MakeUnique(forward<Args>(args)...));
		if (p)
		{
			p->Set_pool_index(pool_index);
		}

		return p;
	}
	Event_handle_Ptr MakeUnique_Event_handle(Event_handle* p)
	{
		if (p == nullptr)
		{
			return Event_handle_Ptr(nullptr, ConcurrentObjectPool<Event_handle>::Deleter{});
		}

		size_t pool_index = p->Get_pool_index();
		ConcurrentObjectPool<Event_handle>& event_handle_pool = *(p_event_handle_pools.at(pool_index));

		return Event_handle_Ptr(p, event_handle_pool.Get_Deleter());
	}
	void Destroy_Event_handle(Event_handle* p)
	{
		if (p == nullptr)
		{
			return;
		}

		ConcurrentObjectPool<Event_handle>& event_handle_pool = *(p_event_handle_pools.at(p->Get_pool_index()));

		event_handle_pool.Destroy(p);
	}
	/**
	 * @brief 投递一个异步 AcceptEx（连同为它准备的 Client_Handle 条目与事件），并维护 pending_accept_count。
	 * @return bool 成功投递（含同步完成）返回 true；socket/事件分配失败或 AcceptEx 直接失败返回 false，
	 *         返回 false 时不留下残留条目（条目、事件、计数均已回收）。
	 * @warning 调用约束：必须已持有 smutex 写锁（构造函数阶段尚未启动其它线程，无需加锁）。
	 */
	bool Post_AcceptEx()
	{
		Client_Handle temp_client{ this };
		if (temp_client.socket == INVALID_SOCKET)
		{
			cout << "Error: create client socket fail" << endl;

			return false;
		}

		Event_handle* pEvent = Construct_Event_handle(temp_client.socket, Event_Accept_Connect);
		if (pEvent == nullptr)
		{
			return false;
		}

		SOCKET client_socket = temp_client.socket;
		client_handles.emplace(client_socket, move(temp_client));

		Client_Handle& client_handle = client_handles.at(client_socket);

		/**
		 * 先自增再调用：AcceptEx 若同步完成，完成包会被某个 work_thread 立即取走并自减，
		 * 若反序会出现"先减后加"，使无符号计数瞬时回绕
		 */
		pending_accept_count.fetch_add(1, memory_order_release);

		int ret = 0;
		int error = 0;
		ret = AcceptEx(
			socket,
			client_handle.socket,
			client_handle.output_buffer,
			Receive_Data_Length,// 不立即收数据
			Local_Address_Length,
			Remote_Address_Length,
			nullptr,// 不需要立即返回字节数
			(LPOVERLAPPED)pEvent);

		error = WSAGetLastError();
		if ((ret == SOCKET_ERROR) && (error != WSA_IO_PENDING))
		{
			cout << "Error: AcceptEx() error code is " << error << endl;
			pending_accept_count.fetch_sub(1, memory_order_release);
			client_handles.erase(client_socket);
			Destroy_Event_handle(pEvent);

			return false;
		}

		return true;
	}

private:
	SOCKET socket;
	HANDLE iocp;
	bool initialize_flag;
	hash<thread::id> hasher_thread_id;

	/*
	* p_event_handle_pools 构造出来的Event_handle对象去处分析：
	* 1. 被Client_Handle::Receive_Data_Process()存储在Client_Handle的ordered_data或unordered_data中
	* 2. 被投递进IOCP系统，等待异步操作完成
	* 3. 在无锁接收队列中的Event_handle对象，会在业务层处理完数据后调用Server_Handle::Destroy_Event_handle()销毁
	* 4. 在无锁发送队列中的Event_handle对象，会在发送线程中被投递进IOCP系统，此时变为第2种情况
	* 5. 在work_thread中从IOCP系统取出的Event_handle对象，会根据不同情况变为第1种、第3种情况或处理完后调用Server_Handle::Destroy_Event_handle()销毁
	* 
	* 在第1和第2种情况下，容易忘记销毁Event_handle对象，导致内存泄漏
	* 第1种情况由 Client_Handle 析构函数兜底销毁（Server_Handle::Close_Server() 清空 client_handles 时触发）
	* 第2种情况由 Server_Handle::Close_Server() 的确定性关闭序列保证：
	  先以哨兵包令全部 work_thread 退出，再关闭所有 socket，最后由 main_thread 排空 IOCP 队列销毁全部残留事件，
	  确保 Server_Handle 析构时 p_event_handle_pools 中所有对象均已归还、完全配对
	*/
	vector<unique_ptr<ConcurrentObjectPool<Event_handle>>> p_event_handle_pools;
};

void work_thread(
	Server_Handle& server_handle,
	vector<moodycamel::ConcurrentQueue<Event_handle_Ptr>>& receive_queues,
	vector<unique_ptr<counting_semaphore<>>>& receive_semaphores);
void send_thread(
	atomic<bool>& run_flag,
	Server_Handle& server_handle,
	moodycamel::ConcurrentQueue<Event_handle*>& send_queue,
	counting_semaphore<>& send_semaphore);
void guard_thread(atomic<bool>& run_flag, Server_Handle& server_handle);
