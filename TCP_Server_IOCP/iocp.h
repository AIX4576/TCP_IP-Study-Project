#pragma once
#include<iostream>
#include<thread>
#include<chrono>
#include<list>
#include<string>
#include<map>
#include<unordered_map>
#include<mutex>
#include<atomic>

using namespace std;

#define WIN32_LEAN_AND_MEAN
#include<WS2tcpip.h> //基础套接字API头文件
#include<MSWSock.h>	 //扩展套接字API头文件
#pragma comment(lib, "ws2_32.lib")	// 链接基础套接字库
#pragma comment(lib, "mswsock.lib") // 链接扩展套接字库

#define Worker_Threads_Number 4
#define Max_Clients_Number 8

#define Receive_Data_Length 0
#define Local_Address_Length (sizeof(SOCKADDR_IN) + 16)
#define Remote_Address_Length (sizeof(SOCKADDR_IN) + 16)
#define Client_Buffer_Size 1024

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
	OVERLAPPED overlapped{};//必须清零 OVERLAPPED 结构，否则可能导致未定义行为
	SOCKET socket;
	Deliver_Event event;
	DWORD flag{};//WSARecv()要用到，默认是0
	WSABUF buffer{};//WSARecv()和WSASend()要用到

	Event_handle(bool allocate_buffer, size_t buffer_size = Client_Buffer_Size, size_t event_id = 0) : socket(INVALID_SOCKET), event(Event_None)
	{
		if(allocate_buffer)
		{
			buffer.buf = new char[(ULONG)buffer_size] {};
			buffer.len = buffer.buf ? (ULONG)buffer_size : 0;
		}

		id = event_id;
	}
	Event_handle(SOCKET socket, Deliver_Event event, bool allocate_buffer, size_t buffer_size = Client_Buffer_Size, size_t event_id = 0) : socket(socket), event(event)
	{
		if (allocate_buffer)
		{
			buffer.buf = new char[(ULONG)buffer_size] {};
			buffer.len = buffer.buf ? (ULONG)buffer_size : 0;
		}

		id = event_id;
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
			swap(buffer, other.buffer);
			swap(id, other.id);
		}
	}
	Event_handle& operator=(Event_handle&& other) noexcept
	{
		//移动赋值函数，将自身资源释放后置0，然后交换
		if (this != &other)
		{
			if (buffer.buf)
			{
				delete[] buffer.buf;
				buffer.buf = NULL;
				buffer.len = 0;
			}

			swap(overlapped, other.overlapped);
			swap(socket, other.socket);
			swap(event, other.event);
			swap(flag, other.flag);
			swap(buffer, other.buffer);
			swap(id, other.id);
		}

		return *this;
	}
	~Event_handle()
	{
		if (buffer.buf)
		{
			delete[] buffer.buf;
			buffer.buf = NULL;
			buffer.len = 0;
		}
	}
	size_t Get_id() const
	{
		return id;
	}
	void Set_id(size_t event_id)
	{
		id = event_id;
	}

private:
	size_t id{};
};

struct Client_Handle
{
	SOCKET socket;
	Socket_Status socket_status;
	string ip;
	uint16_t port{};
	uint8_t output_buffer[Receive_Data_Length + Local_Address_Length + Remote_Address_Length]{};// 地址 + 数据

	Client_Handle() :socket(INVALID_SOCKET), socket_status(Socket_Invalid)
	{
		socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
		if (socket != INVALID_SOCKET)
		{
			socket_status = Socket_Disconnected;
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

			swap(socket, other.socket);
			swap(socket_status, other.socket_status);
			swap(ip, other.ip);
			swap(port, other.port);
			memcpy(output_buffer, other.output_buffer, sizeof(output_buffer));
			swap(receive_data, other.receive_data);
			swap(total_receive_data_size, other.total_receive_data_size);
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
			receive_data.clear();

			swap(socket, other.socket);
			swap(socket_status, other.socket_status);
			swap(ip, other.ip);
			swap(port, other.port);
			memcpy(output_buffer, other.output_buffer, sizeof(output_buffer));
			swap(receive_data, other.receive_data);
			swap(total_receive_data_size, other.total_receive_data_size);
		}

		return *this;
	}
	~Client_Handle()
	{
		if (socket != INVALID_SOCKET)
		{
			CancelIoEx((HANDLE)socket, NULL);
			closesocket(socket);
			socket = INVALID_SOCKET;
			socket_status = Socket_Invalid;
		}
	}
	void Connect_Deal()
	{
		socket_status = Socket_Connected;

		sockaddr_in* pClient_address = reinterpret_cast<sockaddr_in*>(output_buffer);
		//sockaddr_in* pServer_address = reinterpret_cast<sockaddr_in*>(output_buffer + Remote_Address_Length);

		char client_ip[16]{};
		inet_ntop(AF_INET, &pClient_address->sin_addr, client_ip, sizeof(client_ip));

		ip = client_ip;
		port = ntohs(pClient_address->sin_port);
	}
	void Disonnect_Deal()
	{
		socket_status = Socket_Disconnected;
	}
	size_t Make_Receive_Event_id()
	{
		return receive_event_count.fetch_add(1, memory_order_release);
	}
	size_t Get_Total_Receive_Data_Size() const
	{
		return total_receive_data_size;
	}
	bool Insert_To_Receive_Data(Event_handle* event, uint32_t data_size)
	{
		if (event == NULL)
		{
			return FALSE;
		}
		if ((event->event != Event_Receive) || (event->buffer.buf == NULL))
		{
			return FALSE;
		}

		// 循环尝试获取锁，test_and_set会原子地将flag设为true，并返回操作前的值
		while (flag.test_and_set(memory_order_acquire))
		{
			this_thread::yield();
		}

		string data(event->buffer.buf, min(event->buffer.len, data_size));
		total_receive_data_size += data.size();
		receive_data.emplace(event->Get_id(), move(data));

		flag.clear(memory_order_release);

		return TRUE;
	}
	string Peek_All_Receive_Data()
	{
		string data;

		while (flag.test_and_set(memory_order_acquire))
		{
			this_thread::yield();
		}

		for (auto& it : receive_data)
		{
			data += it.second;
		}

		flag.clear(memory_order_release);

		return data;
	}
	string Get_All_Receive_Data()
	{
		string data;

		while (flag.test_and_set(memory_order_acquire))
		{
			this_thread::yield();
		}

		for (auto& it : receive_data)
		{
			data += it.second;
		}
		receive_data.clear();
		total_receive_data_size = 0;

		flag.clear(memory_order_release);

		return data;
	}

private:
	atomic_flag flag{};// 最简单的原子类型，仅支持 test_and_set 和 clear 操作
	atomic<size_t> receive_event_count{};
	map<size_t, string> receive_data;
	size_t total_receive_data_size{};
};

class Server_Handle
{
public:
	unordered_map<SOCKET, Client_Handle> client_handles;
	mutex client_handles_mutex;

	Server_Handle();
	Server_Handle(SOCKET socket, HANDLE iocp) :socket(socket), iocp(iocp), initialize_flag(TRUE) {}
	Server_Handle(Server_Handle&) = delete;
	Server_Handle& operator=(Server_Handle&) = delete;
	Server_Handle(Server_Handle&& other) noexcept
	{
		//移动构造函数，将自身资源初始化为0后交换
		if (this != &other)
		{
			socket = INVALID_SOCKET;
			iocp = NULL;
			initialize_flag = FALSE;

			swap(initialize_flag, other.initialize_flag);
			swap(socket, other.socket);
			swap(iocp, other.iocp);
			swap(client_handles, other.client_handles);
		}
	}
	Server_Handle& operator=(Server_Handle&& other) noexcept
	{
		//移动赋值函数，将自身资源释放后置0，然后交换
		if (this != &other)
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

			client_handles.clear();
			initialize_flag = FALSE;

			swap(initialize_flag, other.initialize_flag);
			swap(socket, other.socket);
			swap(iocp, other.iocp);
			swap(client_handles, other.client_handles);
		}

		return *this;
	}
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

private:
	SOCKET socket;
	HANDLE iocp;
	bool initialize_flag;
};

void work_thread(bool& run_flag, Server_Handle& server_handle);
