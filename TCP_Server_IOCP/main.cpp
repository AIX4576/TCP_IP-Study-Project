#include"main.h"
#include"frame.h"

int main()
{
	WORD version = MAKEWORD(2, 2);
	WSADATA data;
	int result = 0;
	result = WSAStartup(version, &data);
	if (result != NO_ERROR)
	{
		cout << "WSAStartup fail" << endl;

		return -1;
	}

	Server_Handle server_handle;
	if (server_handle.Get_Initialize_Flag() == FALSE)
	{
		cout << "Error: server handle initialize fail" << endl;

		return -1;
	}

	//创建线程池
	bool run_flag = TRUE;
	list<thread> thread_pool;

	for (int i = 0; i < Worker_Threads_Number; i++)
	{
		thread_pool.push_back(thread(work_thread, ref(run_flag), ref(server_handle)));
	}

	while (TRUE)
	{
		char c;
		cin >> c;
		if (c == 'q')
		{
			break;
		}
	}

	run_flag = FALSE;

	for (thread& item : thread_pool)
	{
		if (item.joinable())
		{
			item.join();
		}
	}

	WSACleanup();

	return 0;
}

Server_Handle::Server_Handle() :socket(INVALID_SOCKET), iocp(NULL), initialize_flag(FALSE)
{
	//创建一个支持重叠IO的TCP套接字, WSASocket()函数是Windows系统专用的,socket()函数是跨平台的
	socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (socket == INVALID_SOCKET)
	{
		cout << "Error: create server socket fail" << endl;

		return;
	}

	int result;
	sockaddr_in address_server
	{
		.sin_family = AF_INET,
		.sin_port = htons(8080),
		.sin_addr = INADDR_ANY,
	};

	result = bind(socket, (sockaddr*)&address_server, sizeof(address_server));
	if (result == SOCKET_ERROR)
	{
		cout << "Error: socket bind port fail" << endl;
		closesocket(socket);
		socket = INVALID_SOCKET;

		return;
	}
	else
	{
		cout << "Socket bind port succeed" << endl;
	}

	result = listen(socket, 5);
	if (result == SOCKET_ERROR)
	{
		cout << "Error: socket listen fail" << endl;
		closesocket(socket);
		socket = INVALID_SOCKET;

		return;
	}
	else
	{
		cout << "Socket listen succeed" << endl;
	}

	//创建完成端口
	iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, Worker_Threads_Number);
	if (iocp == NULL)
	{
		cout << "Error: create iocp fail" << endl;
		closesocket(socket);
		socket = INVALID_SOCKET;

		return;
	}

	//将socket和完成端口绑定
	CreateIoCompletionPort((HANDLE)socket, iocp, (ULONG_PTR)this, 0);

	//投递异步accept请求，投递多个
	for (int i = 0; i < Worker_Threads_Number; i++)
	{
		Client_Handle temp;
		Socket_Event* pSocket_event = new Socket_Event;

		if ((temp.socket != INVALID_SOCKET) && temp.buffer.buf && pSocket_event)
		{
			lock_guard<mutex> lock(client_handles_mutex);

			pSocket_event->socket = temp.socket;
			pSocket_event->event = Event_Accept_Connect;

			client_handles.insert({ temp.socket,move(temp) });

			Client_Handle& client_handle = client_handles.at(pSocket_event->socket);

			AcceptEx(
				socket,
				client_handle.socket,
				client_handle.output_buffer,
				Receive_Data_Length,// 不立即收数据
				Local_Address_Length,
				Remote_Address_Length,
				NULL,// 不需要立即返回字节数
				(LPOVERLAPPED)pSocket_event);
		}
	}

	initialize_flag = TRUE;
}

void work_thread(bool& run_flag, Server_Handle& server_handle)
{
	

	DWORD bytes_transferred = 0;
	ULONG_PTR completion_key = 0;
	LPOVERLAPPED pOverlapped = NULL;
	bool result = 0;

	while (run_flag)
	{
		bytes_transferred = 0;
		completion_key = 0;
		pOverlapped = NULL;

		result = GetQueuedCompletionStatus(
			server_handle.Get_IOCP(),
			&bytes_transferred,
			&completion_key,
			&pOverlapped,
			500
		);

		if (result)
		{
			// 成功！我们可以继续处理这个完成的I/O任务
			if (completion_key == (ULONG_PTR)&server_handle)
			{
				Socket_Event* pSocket_event = (Socket_Event*)pOverlapped;
				auto it = server_handle.client_handles.find(pSocket_event->socket);

				if (it != server_handle.client_handles.end())
				{
					Client_Handle& client_handle = it->second;
					switch (pSocket_event->event)
					{
					case Event_Accept_Connect:
					{
						client_handle.Connect_Deal();

						cout << "client [" << (int)client_handle.socket << "] connected, ip is " << client_handle.ip << ", port is " << client_handle.port << endl;

						//投递异步recv请求
						Socket_Event* pEvent1 = new Socket_Event(client_handle.socket, Event_Receive);
						if (pEvent1)
						{
							WSARecv(
								client_handle.socket,
								&client_handle.buffer,							//指向缓冲区数组的指针（一个 WSABUF 数组，至少有一项）
								1,												//上面数组的长度，通常为 1
								NULL,											//实际接收到的字节数（异步操作时通常为 NULL）
								(LPDWORD)client_handle.output_buffer,			//标志位（如 MSG_PARTIAL），通常设为 0，这里用output_buffer填入
								(LPWSAOVERLAPPED)pEvent1,
								NULL											//接收完成后的回调函数（配合事件通知模型），IOCP 不用这个，设为 NULL
							);
						}

						//投递新的异步accept请求
						if (server_handle.client_handles.size() < Max_Clients_Number)
						{
							Client_Handle temp;
							Socket_Event* pEvent2 = new Socket_Event;

							if ((temp.socket != INVALID_SOCKET) && temp.buffer.buf && pEvent2)
							{
								lock_guard<mutex> lock(server_handle.client_handles_mutex);

								pEvent2->socket = temp.socket;
								pEvent2->event = Event_Accept_Connect;

								server_handle.client_handles.insert({ temp.socket,move(temp) });

								Client_Handle& client_handle = server_handle.client_handles.at(pEvent2->socket);

								AcceptEx(
									server_handle.Get_Socket_Server(),
									client_handle.socket,
									client_handle.output_buffer,
									Receive_Data_Length,// 不立即收数据
									Local_Address_Length,
									Remote_Address_Length,
									NULL,// 不需要立即返回字节数
									(LPOVERLAPPED)pEvent2);
							}
						}
					}
					break;
					case Event_Disconnect:
					{
						client_handle.Disonnect_Deal();

						//复用该socket，继续投递异步accept请求(服务器使用DisconnectEx()主动断开socket连接才能复用)
						Socket_Event* pEvent3 = new Socket_Event(client_handle.socket, Event_Accept_Connect);
						if (pEvent3)
						{
							AcceptEx(
								server_handle.Get_Socket_Server(),
								client_handle.socket,
								client_handle.output_buffer,
								Receive_Data_Length,// 不立即收数据
								Local_Address_Length,
								Remote_Address_Length,
								NULL,// 不需要立即返回字节数
								(LPOVERLAPPED)pEvent3);
						}
						else
						{
							lock_guard<mutex> lock{ server_handle.client_handles_mutex };
							server_handle.client_handles.erase(pSocket_event->socket);
						}
					}
					break;
					case Event_Receive:
					{
						
					}
					break;
					default:
					{
						lock_guard<mutex> lock{ server_handle.client_handles_mutex };
						server_handle.client_handles.erase(pSocket_event->socket);
					}
					break;
					}
				}

				delete pSocket_event;
			}
		}
		else
		{
			if (pOverlapped == NULL)
			{
				// 超时了，没有任何任务完成
			}
			else
			{
				// 有任务完成但出错了，要根据错误码来处理，比如连接被断开等
				DWORD error = GetLastError();
			}
		}

		this_thread::sleep_for(chrono::milliseconds(10));
	}
}

