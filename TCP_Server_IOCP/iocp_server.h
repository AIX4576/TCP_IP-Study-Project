#pragma once
#include<iostream>
#include<thread>
#include<chrono>
#include<list>
#include<string>
#include<unordered_map>
#include<mutex>
#include<shared_mutex>
#include<atomic>
using namespace std;

#include"folly/container/F14Map.h"
#include"boost/circular_buffer.hpp"
#include"concurrentqueue.h"

#include"LockFreeObjectPool.h"

#define WIN32_LEAN_AND_MEAN
#include<WS2tcpip.h> //基础套接字API头文件
#include<MSWSock.h>	 //扩展套接字API头文件
#pragma comment(lib, "ws2_32.lib")	// 链接基础套接字库
#pragma comment(lib, "mswsock.lib") // 链接扩展套接字库

#define Server_Port 8080

#define Threads_Number (std::thread::hardware_concurrency())
#define Application_Threads_Number Threads_Number
#define Worker_Threads_Number (Threads_Number / 2)
#define Send_Threads_Number (Threads_Number / 2)
#define Max_Clients_Number 1024

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
	Event_Disconnect,
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

	Event_handle(size_t id = 0, size_t pool_index = 0) : socket(INVALID_SOCKET), event(Event_None), id(id)
	{
		buffer.buf = data;
		buffer.len = Event_Buffer_Size;
	}
	Event_handle(SOCKET socket, Deliver_Event event, uint32_t buffer_size = Event_Buffer_Size, size_t id = 0) : socket(socket), event(event), id(id)
	{
		buffer.buf = data;
		buffer.len = min(buffer_size, Event_Buffer_Size);
	}
	Event_handle(Event_handle&) = delete;
	Event_handle& operator=(const Event_handle&) = delete;
	Event_handle(Event_handle&& other) noexcept
	{
		//移动构造函数，将自身资源初始化为0后交换
		if (this != &other)
		{
			socket = INVALID_SOCKET;
			event = Event_None;

			swap(overlapped, other.overlapped);
			swap(socket, other.socket);
			swap(event, other.event);
			swap(flag, other.flag);
			buffer.len = other.buffer.len;
			buffer.buf = data;
			memcpy(data, other.data, Event_Buffer_Size);
			swap(id, other.id);
		}
	}
	Event_handle& operator=(Event_handle&& other) noexcept
	{
		//移动赋值函数，将自身资源释放后置0，然后交换
		if (this != &other)
		{
			swap(overlapped, other.overlapped);
			swap(socket, other.socket);
			swap(event, other.event);
			swap(flag, other.flag);
			buffer.len = other.buffer.len;
			buffer.buf = data;
			memcpy(data, other.data, Event_Buffer_Size);
			swap(id, other.id);
		}

		return *this;
	}
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

class Client_Handle
{
public:
	Server_Handle* pServer_Handle;
	SOCKET socket;
	Socket_Status socket_status;
	string ip;
	uint16_t port{};
	uint8_t output_buffer[Receive_Data_Length + Local_Address_Length + Remote_Address_Length]{};// 地址 + 数据

	Client_Handle(Server_Handle* pServer_Handle) :socket(INVALID_SOCKET), socket_status(Socket_Invalid), pServer_Handle(pServer_Handle)
	{
		socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
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
		//移动构造函数，将自身资源初始化为0后交换
		if (this != &other)
		{
			socket = INVALID_SOCKET;
			socket_status = Socket_Invalid;

			swap(pServer_Handle, other.pServer_Handle);
			swap(socket, other.socket);
			swap(socket_status, other.socket_status);
			swap(ip, other.ip);
			swap(port, other.port);
			memcpy(output_buffer, other.output_buffer, sizeof(output_buffer));
			swap(next_expected_sequence, other.next_expected_sequence);
			swap(ordered_data, other.ordered_data);
			swap(unordered_data, other.unordered_data);
			swap(last_active_time, other.last_active_time);
		}
	}
	Client_Handle& operator=(Client_Handle&& other) noexcept
	{
		//移动赋值函数，将自身资源释放后置0，然后交换
		if (this != &other)
		{
			if (socket != INVALID_SOCKET)
			{
				CancelIoEx((HANDLE)socket, NULL);
				closesocket(socket);
				socket = INVALID_SOCKET;
				socket_status = Socket_Invalid;
			}

			ip.clear();
			ordered_data.clear();
			unordered_data.clear();

			swap(pServer_Handle, other.pServer_Handle);
			swap(socket, other.socket);
			swap(socket_status, other.socket_status);
			swap(ip, other.ip);
			swap(port, other.port);
			memcpy(output_buffer, other.output_buffer, sizeof(output_buffer));
			swap(next_expected_sequence, other.next_expected_sequence);
			swap(ordered_data, other.ordered_data);
			swap(unordered_data, other.unordered_data);
			swap(last_active_time, other.last_active_time);
		}

		return *this;
	}
	~Client_Handle();
	void Connect_Deal()
	{
		socket_status = Socket_Connected;

		sockaddr_in* pClient_address = reinterpret_cast<sockaddr_in*>(output_buffer);
		//sockaddr_in* pServer_address = reinterpret_cast<sockaddr_in*>(output_buffer + Remote_Address_Length);

		char client_ip[16]{};
		inet_ntop(AF_INET, &pClient_address->sin_addr, client_ip, sizeof(client_ip));

		ip = client_ip;
		port = ntohs(pClient_address->sin_port);

		Update_Last_Active_Time();
	}
	void Disonnect_Deal()
	{
		socket_status = Socket_Disconnected;
		ip.clear();
		port = 0;
		lock.clear();
		receive_event_sequence = 0;
		next_expected_sequence = 0;
		unordered_data.clear();
	}
	void Update_Last_Active_Time()
	{
		last_active_time = chrono::system_clock::now();
	}
	bool Is_Active_Timeout() const
	{
		auto current_time = chrono::system_clock::now();
		long long duration = chrono::duration_cast<chrono::seconds>(current_time - last_active_time).count();

		if ((duration > Client_Active_Timeout_Second) && (socket_status == Socket_Connected))
		{
			return TRUE;
		}
		else
		{
			return FALSE;
		}
	}
	size_t Make_Receive_Event_id()
	{
		return receive_event_sequence.fetch_add(1, memory_order_release);
	}
	/*
	* 处理接收到的数据，如果该数据是过期重复的数据，则为无效数据，没有被ordered_data或unordered_data存储，此时需要外部销毁pEvent
	* 如果是有效数据，则被ordered_data或unordered_data存储，此时外部在数据被取出前不能使用或销毁pEvent
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
			//放进有序数据缓冲区，同时查找无序数据缓冲区中是否有数据和其连续
			ordered_data.push_back(pEvent);
			next_expected_sequence++;

			auto it = unordered_data.find(next_expected_sequence);
			while (it != unordered_data.end())
			{
				ordered_data.push_back(it->second);
				unordered_data.erase(it);
				next_expected_sequence++;
				it = unordered_data.find(next_expected_sequence);
			}

			valid_data = true;
		}
		else if (event_sequence > next_expected_sequence)
		{
			//放进无序数据缓冲区
			unordered_data.emplace(event_sequence, pEvent);
			valid_data = true;
		}
		else
		{
			//重复数据，丢弃
			valid_data = false;
		}

		lock.clear(memory_order_release);

		return valid_data;
	}
	// 获取有序数据，返回有序数据的数量，pEvent_array需确保存在
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
		return unordered_data.size();
	}
	// 获取无序数据，返回无序数据的数量，pEvent_array需确保存在
	size_t Get_Unordered_Data(Event_handle** pEvent_array, size_t max_count)
	{
		size_t count = 0;

		while (lock.test_and_set(memory_order_acquire))
		{
			this_thread::yield();
		}

		for (const auto& pair : unordered_data)
		{
			if (count >= max_count)
			{
				break;
			}

			pEvent_array[count++] = pair.second;
		}
		unordered_data.clear();

		lock.clear(memory_order_release);

		return count;
	}
	
private:
	atomic_flag lock{};// 最简单的原子类型，仅支持 test_and_set 和 clear 操作
	atomic<size_t> receive_event_sequence{};//接收事件的序号;总共可以用2^64大小的事件数量，假设每秒消耗1亿个事件，也可以使用5849年，可以认为不会消耗完
	size_t next_expected_sequence{};//下一个期望接收的事件的序号
	boost::circular_buffer<Event_handle*> ordered_data;//有序数据缓冲区
	folly::F14FastMap<size_t, Event_handle*> unordered_data;//无序数据缓冲区
	chrono::system_clock::time_point last_active_time{};//最后活动时间
};

class Server_Handle
{
public:
	folly::F14FastMap<SOCKET, Client_Handle> client_handles;
	shared_mutex smutex;

	Server_Handle();
	Server_Handle(Server_Handle&) = delete;
	Server_Handle(Server_Handle&& other) = delete;
	Server_Handle& operator=(Server_Handle&) = delete;
	Server_Handle& operator=(Server_Handle&& other) = delete;
	~Server_Handle()
	{
		if (socket != INVALID_SOCKET)
		{
			CancelIoEx((HANDLE)socket, NULL);
			closesocket(socket);
			socket = INVALID_SOCKET;
		}
		if (iocp != NULL)
		{
			CloseHandle(iocp);
			iocp = NULL;
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
	void Close_Socket()
	{
		CancelIoEx((HANDLE)socket, NULL);
		closesocket(socket);
		socket = INVALID_SOCKET;
	}
	size_t Socket_Map_In_Range(SOCKET socket, size_t range)
	{
		if (range == 0)
		{
			return 0;
		}

		return hasher_socket(socket) % range;
	}
	template<typename... Args>
	Event_handle* Construct_Event_handle(Args&&... args)
	{
		size_t pool_index = hasher_thread_id(this_thread::get_id()) % p_event_handle_pools.size();
		LockFreeObjectPool<Event_handle>& event_handle_pool = *(p_event_handle_pools.at(pool_index));

		Event_handle* p = event_handle_pool.Construct(forward<Args>(args)...);
		if (p)
		{
			p->Set_pool_index(pool_index);
		}

		return p;
	}
	void Destory_Event_handle(Event_handle* p)
	{
		if (p == NULL)
		{
			return;
		}

		LockFreeObjectPool<Event_handle>& event_handle_pool = *(p_event_handle_pools.at(p->Get_pool_index()));

		event_handle_pool.Destory(p);
	}

private:
	SOCKET socket;
	HANDLE iocp;
	bool initialize_flag;
	hash<SOCKET> hasher_socket;
	hash<thread::id> hasher_thread_id;
	vector<unique_ptr<LockFreeObjectPool<Event_handle>>> p_event_handle_pools;
};

void work_thread(bool& run_flag, Server_Handle& server_handle, vector<moodycamel::ConcurrentQueue<Event_handle*>>& receive_queues);
void send_thread(bool& run_flag, Server_Handle& server_handle, vector<moodycamel::ConcurrentQueue<Event_handle*>>& send_queues);
void clean_thread(bool& run_flag, Server_Handle& server_handle);
