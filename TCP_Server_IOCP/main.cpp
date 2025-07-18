#include"main.h"
#include"frame.h"

atomic<long long> Event_handle::next_id = 0;

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
		Client_Handle temp_client;
		Event_handle temp_event{ FALSE, temp_client.socket ,Event_Accept_Connect };
		SOCKET client_socket = temp_client.socket;
		long long event_id = temp_event.Get_id();

		if (temp_client.socket != INVALID_SOCKET)
		{
			lock_guard<mutex> lock(client_handles_mutex);

			client_handles.insert({ temp_client.socket,move(temp_client) });
			accept_connect_disconnect_events.insert({ temp_event.Get_id(), move(temp_event) });

			Client_Handle& client_handle = client_handles.at(client_socket);
			Event_handle& accept_connect_event = accept_connect_disconnect_events.at(event_id);

			AcceptEx(
				socket,
				client_handle.socket,
				client_handle.output_buffer,
				Receive_Data_Length,// 不立即收数据
				Local_Address_Length,
				Remote_Address_Length,
				NULL,// 不需要立即返回字节数
				(LPOVERLAPPED)&accept_connect_event);
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
				Event_handle* pEvent = (Event_handle*)pOverlapped;
				auto it1 = server_handle.client_handles.find(pEvent->socket);

				if (it1 == server_handle.client_handles.end())
				{
					if ((pEvent->event == Event_Accept_Connect) || (pEvent->event == Event_Disconnect))
					{
						server_handle.accept_connect_disconnect_events.erase(pEvent->Get_id());
					}

					continue;
				}

				Client_Handle& client_handle = it1->second;

				switch (pEvent->event)
				{
				case Event_Accept_Connect:
				{
					client_handle.Connect_Deal();

					cout << "client [" << (int)client_handle.socket << "] connected, ip is " << client_handle.ip << ", port is " << client_handle.port << endl;

					//将新连接的 client socket 与 iocp 绑定，然后投递异步recv请求
					HANDLE iocp = CreateIoCompletionPort((HANDLE)client_handle.socket, server_handle.Get_IOCP(), (ULONG_PTR)&server_handle, 0);
					Event_handle temp_event{ TRUE, client_handle.socket, Event_Receive };
					long long event_id = temp_event.Get_id();

					if (iocp && temp_event.buffer.buf)
					{
						client_handle.receive_events.insert({ temp_event.Get_id() ,move(temp_event) });

						Event_handle& receive_event = client_handle.receive_events.at(event_id);

						int ret = 0;
						int error = 0;
						ret = WSARecv(
							client_handle.socket,
							&receive_event.buffer,				//指向缓冲区数组的指针（一个 WSABUF 数组，至少有一项）
							1,									//上面数组的长度，通常为 1
							NULL,								//实际接收到的字节数（仅在同步操作成功时有效，异步操作时通常为 NULL）
							&receive_event.flag,				//标志位（如 MSG_PARTIAL），通常设为 0
							(LPWSAOVERLAPPED)&receive_event,
							NULL								//接收完成后的回调函数（配合事件通知模型），IOCP 不用这个，设为 NULL
						);

						error = WSAGetLastError();
						if ((ret == SOCKET_ERROR) && (error != WSA_IO_PENDING))
						{
							cout << "Error: WSARecv() error code is " << error << endl;
						}
					}
					else
					{
						lock_guard<mutex> lock{ server_handle.client_handles_mutex };
						server_handle.client_handles.erase(client_handle.socket);
					}

					//投递新的异步accept请求
					if (server_handle.client_handles.size() < Max_Clients_Number)
					{
						Client_Handle temp_client;

						if (temp_client.socket != INVALID_SOCKET)
						{
							lock_guard<mutex> lock(server_handle.client_handles_mutex);

							pEvent->socket = temp_client.socket;
							server_handle.client_handles.insert({ temp_client.socket,move(temp_client) });

							Client_Handle& client_handle = server_handle.client_handles.at(pEvent->socket);

							AcceptEx(
								server_handle.Get_Socket_Server(),
								client_handle.socket,
								client_handle.output_buffer,
								Receive_Data_Length,// 不立即收数据
								Local_Address_Length,
								Remote_Address_Length,
								NULL,// 不需要立即返回字节数
								(LPOVERLAPPED)pEvent);
						}
						else
						{
							server_handle.accept_connect_disconnect_events.erase(pEvent->Get_id());
						}
					}
				}
				break;
				case Event_Disconnect:
				{
					client_handle.Disonnect_Deal();

					//复用该socket，继续投递异步accept请求(服务器使用DisconnectEx()主动断开socket连接才能复用)
					pEvent->event = Event_Accept_Connect;

					AcceptEx(
						server_handle.Get_Socket_Server(),
						client_handle.socket,
						client_handle.output_buffer,
						Receive_Data_Length,// 不立即收数据
						Local_Address_Length,
						Remote_Address_Length,
						NULL,// 不需要立即返回字节数
						(LPOVERLAPPED)pEvent);
				}
				break;
				case Event_Receive:
				{
					cout << "receive data from socket [" << (int)client_handle.socket << "], data size is " << bytes_transferred << endl;

					//继续投递异步recv请求
					int ret = 0;
					int error = 0;
					ret = WSARecv(
						client_handle.socket,
						&pEvent->buffer,			//指向缓冲区数组的指针（一个 WSABUF 数组，至少有一项）
						1,							//上面数组的长度，通常为 1
						NULL,						//实际接收到的字节数（仅在同步操作成功时有效，异步操作时通常为 NULL）
						&pEvent->flag,				//标志位（如 MSG_PARTIAL），通常设为 0
						(LPWSAOVERLAPPED)pEvent,
						NULL						//接收完成后的回调函数（配合事件通知模型），IOCP 不用这个，设为 NULL
					);

					error = WSAGetLastError();
					if ((ret == SOCKET_ERROR) && (error != WSA_IO_PENDING))
					{
						cout << "Error: WSARecv() error code is " << error << endl;
					}
				}
				break;
				default:
				{
					lock_guard<mutex> lock{ server_handle.client_handles_mutex };
					server_handle.client_handles.erase(client_handle.socket);
				}
				break;
				}
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
				DWORD error1 = GetLastError();
				DWORD error2 = WSAGetLastError();
				Event_handle* pEvent = (Event_handle*)pOverlapped;

				if (((error1 == ERROR_NETNAME_DELETED) && (bytes_transferred == 0)) ||
					(error2 == WSAECONNRESET) ||
					(error2 == WSAECONNABORTED))
				{
					cout << "client socket [" << (int)pEvent->socket << "] close" << endl;

					lock_guard<mutex> lock{ server_handle.client_handles_mutex };
					server_handle.client_handles.erase(pEvent->socket);
				}
				else
				{
					server_handle.accept_connect_disconnect_events.erase(pEvent->Get_id());
				}
			}
		}
	}
}

