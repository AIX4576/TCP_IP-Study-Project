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

#include"concurrentqueue.h"

#define WIN32_LEAN_AND_MEAN
#include<WS2tcpip.h> //�����׽���APIͷ�ļ�
#include<MSWSock.h>	 //��չ�׽���APIͷ�ļ�
#pragma comment(lib, "ws2_32.lib")	// ���ӻ����׽��ֿ�
#pragma comment(lib, "mswsock.lib") // ������չ�׽��ֿ�

#define Server_Port 8080

#define Application_Threads_Number (std::thread::hardware_concurrency())
#define Worker_Threads_Number (std::thread::hardware_concurrency() / 4 * 3)
#define Send_Threads_Number (std::thread::hardware_concurrency() / 4)
#define Max_Clients_Number 1024

#define Receive_Data_Length 0
#define Local_Address_Length (sizeof(SOCKADDR_IN) + 16)
#define Remote_Address_Length (sizeof(SOCKADDR_IN) + 16)

#define Per_Client_Receive_Event_Number 3
#define Per_Client_Ordered_Data_Default_Capacity (1024 * 16)
#define Per_Client_Unordered_Data_Number_Threshold 8
#define Completed_Message_Size_Threshold 32
#define Client_Active_Timeout_Second 180
#define Client_Active_Timeout_Scan_Interval 60
#define Event_Buffer_Size 1500
#define Each_Send_Queue_Limit_Send_Count 5

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
	OVERLAPPED overlapped{};//�������� OVERLAPPED �ṹ��������ܵ���δ������Ϊ
	SOCKET socket;
	Deliver_Event event;
	DWORD flag{};//WSARecv()Ҫ�õ���Ĭ����0
	WSABUF buffer{};//WSARecv()��WSASend()Ҫ�õ�

	Event_handle(bool allocate_buffer, uint32_t buffer_size = Event_Buffer_Size, size_t event_id = 0) : socket(INVALID_SOCKET), event(Event_None)
	{
		if(allocate_buffer)
		{
			buffer.buf = new char[buffer_size] {};
			buffer.len = buffer.buf ? buffer_size : 0;
		}

		id = event_id;
	}
	Event_handle(SOCKET socket, Deliver_Event event, bool allocate_buffer, uint32_t buffer_size = Event_Buffer_Size, size_t event_id = 0) : socket(socket), event(event)
	{
		if (allocate_buffer)
		{
			buffer.buf = new char[buffer_size] {};
			buffer.len = buffer.buf ? buffer_size : 0;
		}

		id = event_id;
	}
	Event_handle(Event_handle&) = delete;
	Event_handle& operator=(const Event_handle&) = delete;
	Event_handle(Event_handle&& other) noexcept
	{
		//�ƶ����캯������������Դ��ʼ��Ϊ0�󽻻�
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
		//�ƶ���ֵ��������������Դ�ͷź���0��Ȼ�󽻻�
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

class Client_Handle
{
public:
	SOCKET socket;
	Socket_Status socket_status;
	string ip;
	uint16_t port{};
	uint8_t output_buffer[Receive_Data_Length + Local_Address_Length + Remote_Address_Length]{};// ��ַ + ����

	Client_Handle() :socket(INVALID_SOCKET), socket_status(Socket_Invalid)
	{
		socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
		if (socket != INVALID_SOCKET)
		{
			socket_status = Socket_Disconnected;
			ordered_data.reserve(Per_Client_Ordered_Data_Default_Capacity);
			unordered_data.reserve(Per_Client_Unordered_Data_Number_Threshold + 2);
		}
	}
	Client_Handle(Client_Handle&) = delete;
	Client_Handle& operator=(Client_Handle&) = delete;
	Client_Handle(Client_Handle&& other) noexcept
	{
		//�ƶ����캯������������Դ��ʼ��Ϊ0�󽻻�
		if (this != &other)
		{
			socket = INVALID_SOCKET;
			socket_status = Socket_Invalid;

			swap(socket, other.socket);
			swap(socket_status, other.socket_status);
			swap(ip, other.ip);
			swap(port, other.port);
			memcpy(output_buffer, other.output_buffer, sizeof(output_buffer));
			swap(next_expected_sequence, other.next_expected_sequence);
			swap(ordered_data, other.ordered_data);
			swap(unordered_data, other.unordered_data);
			swap(last_active_time, other.last_active_time);
			swap(completed_message_size_threshold, other.completed_message_size_threshold);
		}
	}
	Client_Handle& operator=(Client_Handle&& other) noexcept
	{
		//�ƶ���ֵ��������������Դ�ͷź���0��Ȼ�󽻻�
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

			swap(socket, other.socket);
			swap(socket_status, other.socket_status);
			swap(ip, other.ip);
			swap(port, other.port);
			memcpy(output_buffer, other.output_buffer, sizeof(output_buffer));
			swap(next_expected_sequence, other.next_expected_sequence);
			swap(ordered_data, other.ordered_data);
			swap(unordered_data, other.unordered_data);
			swap(last_active_time, other.last_active_time);
			swap(completed_message_size_threshold, other.completed_message_size_threshold);
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
		ordered_data.clear();
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
	void Set_Completed_Message_Size_Threshold(size_t value)
	{
		completed_message_size_threshold = value;
	}
	size_t Get_Completed_Message_Size_Threshold()
	{
		return completed_message_size_threshold;
	}
	size_t Get_Unordered_Data_Number()
	{
		return unordered_data.size();
	}
	size_t Get_Ordered_Data_Size()
	{
		return ordered_data.size();
	}
	string Get_Ordered_Data()
	{
		// ѭ�����Ի�ȡ����test_and_set��ԭ�ӵؽ�lock��Ϊtrue�������ز���ǰ��ֵ
		while (lock.test_and_set(memory_order_acquire))
		{
			this_thread::yield();

			if (Get_Ordered_Data_Size() == 0)
			{
				return {};
			}
		}

		string data{ ordered_data };
		ordered_data.clear();

		lock.clear(memory_order_release);

		return data;
	}
	bool Sort_Receive_Data(Event_handle* event, uint32_t data_size)
	{
		if (event == NULL)
		{
			return FALSE;
		}
		if ((event->event != Event_Receive) || (event->buffer.buf == NULL))
		{
			return FALSE;
		}

		// ѭ�����Ի�ȡ����test_and_set��ԭ�ӵؽ�lock��Ϊtrue�������ز���ǰ��ֵ
		while (lock.test_and_set(memory_order_acquire))
		{
			this_thread::yield();
		}

		size_t event_id = event->Get_id();
		if (event_id == next_expected_sequence)
		{
			// 1. ����ǰreceive�¼���id�������������кţ�ֱ��ƴ��
			ordered_data.append(event->buffer.buf, min(event->buffer.len, data_size));
			next_expected_sequence++;// �������к�+1

			// 2. ������򻺳���������������������к�
			auto it = unordered_data.find(next_expected_sequence);
			while (it != unordered_data.end())
			{
				// ƴ����һ����������
				ordered_data += it->second;
				next_expected_sequence++;// �������к�+1

				//�Ƴ�������
				unordered_data.erase(it);

				// ���������һ�����к�
				it = unordered_data.find(next_expected_sequence);
			}
		}
		else if (event_id > next_expected_sequence)
		{
			// 3. ��id���������������򻺳�����������δ�������кţ������ظ�/�������ݣ�
			unordered_data.emplace(event_id, string(event->buffer.buf, min(event->buffer.len, data_size)));
		}
		else
		{
			// 4. idС�����������кţ��Ѵ�������ظ����ݣ�
			lock.clear(memory_order_release);

			return FALSE;
		}

		lock.clear(memory_order_release);

		return TRUE;
	}
	bool Send_Data_Ex(const char* data, size_t size)
	{
		if (socket_status != Socket_Connected)
		{
			return FALSE;
		}
		if (size == 0)
		{
			return FALSE;
		}

		//Ͷ���첽send����
		Event_handle* pEvent = new Event_handle{ socket,Event_Send ,TRUE,(uint32_t)size };
		if (pEvent)
		{
			if (pEvent->buffer.buf)
			{
				memcpy(pEvent->buffer.buf, data, size);

				int ret = 0;
				int error = 0;
				ret = WSASend(
					socket,
					&pEvent->buffer,			//ָ�򻺳��������ָ�루һ�� WSABUF ���飬������һ�
					1,							//��������ĳ��ȣ�ͨ��Ϊ 1
					NULL,						//ʵ�ʷ��͵��ֽ���������ͬ�������ɹ�ʱ��Ч���첽����ʱͨ��Ϊ NULL��
					0,							//���ͱ�־��һ��Ϊ 0��
					(LPWSAOVERLAPPED)pEvent,
					NULL						//������ɺ�Ļص�����������¼�֪ͨģ�ͣ���IOCP �����������Ϊ NULL
				);

				error = WSAGetLastError();
				if ((ret == SOCKET_ERROR) && (error != WSA_IO_PENDING))
				{
					delete pEvent;

					return FALSE;
				}
			}
			else
			{
				delete pEvent;

				return FALSE;
			}
		}
		else
		{
			return FALSE;
		}

		return TRUE;
	}

private:
	atomic_flag lock{};// ��򵥵�ԭ�����ͣ���֧�� test_and_set �� clear ����
	atomic<size_t> receive_event_sequence{};//�����¼������;�ܹ�������2^64��С���¼�����������ÿ������1�ڸ��¼���Ҳ����ʹ��5849�꣬������Ϊ����������
	size_t next_expected_sequence{};//��һ���������յ��¼������
	string ordered_data;//�������ݻ�����
	unordered_map<size_t, string> unordered_data;//�������ݻ�����
	chrono::system_clock::time_point last_active_time{};//���ʱ��
	size_t completed_message_size_threshold = Completed_Message_Size_Threshold;
};

class Server_Handle
{
public:
	unordered_map<SOCKET, Client_Handle> client_handles;
	vector<unique_ptr<shared_mutex>> buckets_shared_mutexes;
	mutex global_mutex;

	Server_Handle();
	Server_Handle(Server_Handle&) = delete;
	Server_Handle& operator=(Server_Handle&) = delete;
	Server_Handle(Server_Handle&& other) noexcept
	{
		//�ƶ����캯������������Դ��ʼ��Ϊ0�󽻻�
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
		//�ƶ���ֵ��������������Դ�ͷź���0��Ȼ�󽻻�
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
	size_t Socket_Map_In_Range(SOCKET socket, size_t range)
	{
		if (range == 0)
		{
			return 0;
		}

		return hasher_socket(socket) % range;
	}
	bool Set_Completed_Message_Size_Threshold(SOCKET socket, size_t value)
	{
		size_t bucket_index = client_handles.bucket(socket) % buckets_shared_mutexes.size();
		shared_lock<shared_mutex> lock{ *(buckets_shared_mutexes.at(bucket_index)) };

		auto it = client_handles.find(socket);
		if (it == client_handles.end())
		{
			return false;
		}

		Client_Handle& client_handle = it->second;
		client_handle.Set_Completed_Message_Size_Threshold(value);

		return true;
	}

private:
	SOCKET socket;
	HANDLE iocp;
	bool initialize_flag;
	hash<SOCKET> hasher_socket;
};

struct Message
{
	SOCKET socket;
	string data;

	Message() :socket(INVALID_SOCKET) {}
	Message(SOCKET socket, string& data) :socket(socket), data(data) {}
	Message(SOCKET socket, string&& data) :socket(socket), data(data) {}
	Message(Message&) = delete;
	Message& operator=(Message&) = delete;
	Message(Message&& other) noexcept
	{
		if (this != &other)
		{
			socket = INVALID_SOCKET;

			swap(socket, other.socket);
			swap(data, other.data);
		}
	}
	Message& operator=(Message&& other) noexcept
	{
		if (this != &other)
		{
			socket = INVALID_SOCKET;

			swap(socket, other.socket);
			swap(data, other.data);
		}

		return *this;
	}
};

void work_thread(bool& run_flag, Server_Handle& server_handle, vector<moodycamel::ConcurrentQueue<Message>>& receive_queues);
void send_thread(bool& run_flag, Server_Handle& server_handle, vector<moodycamel::ConcurrentQueue<Message>>& send_queues);
void clean_thread(bool& run_flag, Server_Handle& server_handle);
