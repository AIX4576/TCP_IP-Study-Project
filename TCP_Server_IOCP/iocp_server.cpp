#include"iocp_server.h"

Server_Handle::Server_Handle() :socket(INVALID_SOCKET), iocp(NULL), initialize_flag(FALSE)
{
	//创建一个支持重叠IO的TCP套接字, WSASocket()函数是Windows系统专用的,socket()函数是跨平台的
	socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (socket == INVALID_SOCKET)
	{
		cout << "Error: create server socket fail" << endl;

		return;
	}

	int result = -1;
	sockaddr_in address_server
	{
		.sin_family = AF_INET,
		.sin_port = htons(Server_Port),
		.sin_addr = INADDR_ANY,
	};

	result = ::bind(socket, (sockaddr*)&address_server, sizeof(address_server));
	if (result == SOCKET_ERROR)
	{
		cout << "Error: socket bind port fail" << endl;
		closesocket(socket);
		socket = INVALID_SOCKET;

		return;
	}
	else
	{
		cout << "Socket bind port[" << Server_Port << "] succeed" << endl;
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

	//初始化client_handles的桶数量为 Max_Clients_Number * 1.2，保证永远不会触发rehash操作
	client_handles.reserve((size_t)(Max_Clients_Number * 1.2));
	cout << "client_handles buckets count is " << client_handles.bucket_count() << endl;

	//初始化p_event_handle_pools的个数为 Threads_Number，每个无锁对象池预分配 (Max_Clients_Number * 10 / Threads_Number) 个对象
	p_event_handle_pools.reserve(Threads_Number);
	for (size_t i = 0; i < Threads_Number; i++)
	{
		p_event_handle_pools.emplace_back(make_unique<LockFreeObjectPool<Event_handle>>(Max_Clients_Number * 10 / Threads_Number));
	}
	

	//投递异步accept请求，投递多个
	for (size_t i = 0; i < Worker_Threads_Number; i++)
	{
		Client_Handle temp_client{ this };
		if (temp_client.socket == INVALID_SOCKET)
		{
			continue;
		}

		Event_handle* pEvent = Construct_Event_handle(temp_client.socket, Event_Accept_Connect);
		if (pEvent == NULL)
		{
			continue;
		}

		client_handles.emplace(temp_client.socket, move(temp_client));

		Client_Handle& client_handle = client_handles.at(pEvent->socket);

		int ret = 0;
		int error = 0;
		ret = AcceptEx(
			socket,
			client_handle.socket,
			client_handle.output_buffer,
			Receive_Data_Length,// 不立即收数据
			Local_Address_Length,
			Remote_Address_Length,
			NULL,// 不需要立即返回字节数
			(LPOVERLAPPED)pEvent);

		error = WSAGetLastError();
		if ((ret == SOCKET_ERROR) && (error != WSA_IO_PENDING))
		{
			client_handles.erase(client_handle.socket);
			Destory_Event_handle(pEvent);
		}
	}

	if(client_handles.size())
	{
		initialize_flag = TRUE;
	}
	else
	{
		initialize_flag = FALSE;
	}
}

Client_Handle::~Client_Handle()
{
	if (socket != INVALID_SOCKET)
	{
		CancelIoEx((HANDLE)socket, NULL);
		closesocket(socket);
		socket = INVALID_SOCKET;
		socket_status = Socket_Invalid;
	}

	//析构前销毁ordered_data和unordered_data中的pEvent，避免内存泄漏
	if (pServer_Handle)
	{
		Event_handle* pEvent_array[Per_Client_Unordered_Data_Number_Threshold + 3]{};
		size_t count = 0;

		count = Get_Ordered_Data(pEvent_array, sizeof(pEvent_array) / sizeof(Event_handle*));
		for (size_t i = 0; i < count; i++)
		{
			pServer_Handle->Destory_Event_handle(pEvent_array[i]);
		}

		count = Get_Unordered_Data(pEvent_array, sizeof(pEvent_array) / sizeof(Event_handle*));
		for (size_t i = 0; i < count; i++)
		{
			pServer_Handle->Destory_Event_handle(pEvent_array[i]);
		}
	}
}

void work_thread(bool& run_flag, Server_Handle& server_handle, vector<moodycamel::ConcurrentQueue<Event_handle*>>& receive_queues)
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

			Event_handle* pEvent = (Event_handle*)pOverlapped;

			if (completion_key != (ULONG_PTR)&server_handle)
			{
				server_handle.Destory_Event_handle(pEvent);

				continue;
			}

			switch (pEvent->event)
			{
			case Event_Accept_Connect:
			{
				shared_lock<shared_mutex> shared_lock{ server_handle.smutex };

				auto it = server_handle.client_handles.find(pEvent->socket);
				if (it == server_handle.client_handles.end())
				{
					server_handle.Destory_Event_handle(pEvent);

					break;
				}

				Client_Handle& client_handle = it->second;

				//将新连接的 client socket 与 iocp 绑定，然后投递异步recv请求，投递多个
				HANDLE iocp = CreateIoCompletionPort((HANDLE)client_handle.socket, server_handle.Get_IOCP(), (ULONG_PTR)&server_handle, 0);
				if (iocp)
				{
					size_t count = 0;

					for (int i = 0; i < Per_Client_Receive_Event_Number; i++)
					{
						Event_handle* pEvent1 = server_handle.Construct_Event_handle(
							client_handle.socket,
							Event_Receive,
							Event_Buffer_Size,
							client_handle.Make_Receive_Event_id());
						if (pEvent1 == NULL)
						{
							continue;
						}

						int ret = 0;
						int error = 0;
						ret = WSARecv(
							client_handle.socket,
							&pEvent1->buffer,					//指向缓冲区数组的指针（一个 WSABUF 数组，至少有一项）
							1,									//上面数组的长度，通常为 1
							NULL,								//实际接收到的字节数（仅在同步操作成功时有效，异步操作时通常为 NULL）
							&pEvent1->flag,						//标志位（如 MSG_PARTIAL），通常设为 0
							(LPWSAOVERLAPPED)pEvent1,
							NULL								//接收完成后的回调函数（配合事件通知模型），IOCP 不用这个，设为 NULL
						);

						error = WSAGetLastError();
						if ((ret == SOCKET_ERROR) && (error != WSA_IO_PENDING))
						{
							cout << "Error: WSARecv() error code is " << error << endl;

							server_handle.Destory_Event_handle(pEvent1);
						}
						else
						{
							count++;
						}
					}

					if (count)
					{
						client_handle.Connect_Deal();
						cout << "client [" << (int)client_handle.socket << "] connected, ip is [" << client_handle.ip << "], port is [" << client_handle.port << "]" << endl;
					}
					else
					{
						if (shared_lock.owns_lock())
						{
							shared_lock.unlock();
						}
						unique_lock<shared_mutex> unique_lock{ server_handle.smutex };

						server_handle.client_handles.erase(pEvent->socket);
					}
				}
				else
				{
					if (shared_lock.owns_lock())
					{
						shared_lock.unlock();
					}
					unique_lock<shared_mutex> unique_lock{ server_handle.smutex };

					server_handle.client_handles.erase(pEvent->socket);
				}

				//投递新的异步accept请求
				if (server_handle.client_handles.size() < Max_Clients_Number)
				{
					Client_Handle temp_client{ &server_handle };

					if (temp_client.socket != INVALID_SOCKET)
					{
						if (shared_lock.owns_lock())
						{
							shared_lock.unlock();
						}
						unique_lock<shared_mutex> unique_lock{ server_handle.smutex };

						pEvent->socket = temp_client.socket;
						server_handle.client_handles.emplace(temp_client.socket, move(temp_client));

						Client_Handle& client_handle1 = server_handle.client_handles.at(pEvent->socket);

						int ret = 0;
						int error = 0;
						ret = AcceptEx(
							server_handle.Get_Socket_Server(),
							client_handle1.socket,
							client_handle1.output_buffer,
							Receive_Data_Length,// 不立即收数据
							Local_Address_Length,
							Remote_Address_Length,
							NULL,// 不需要立即返回字节数
							(LPOVERLAPPED)pEvent);

						error = WSAGetLastError();
						if ((ret == SOCKET_ERROR) && (error != WSA_IO_PENDING))
						{
							server_handle.client_handles.erase(pEvent->socket);
							server_handle.Destory_Event_handle(pEvent);
						}
					}
					else
					{
						server_handle.Destory_Event_handle(pEvent);
					}
				}
				else
				{
					server_handle.Destory_Event_handle(pEvent);
				}
			}
			break;
			case Event_Disconnect:
			{
				shared_lock<shared_mutex> shared_lock{ server_handle.smutex };

				auto it = server_handle.client_handles.find(pEvent->socket);
				if (it == server_handle.client_handles.end())
				{
					server_handle.Destory_Event_handle(pEvent);

					break;
				}

				Client_Handle& client_handle = it->second;

				client_handle.Disonnect_Deal();

				//复用该socket，继续投递异步accept请求(服务器使用DisconnectEx()主动断开socket连接才能复用)
				pEvent->event = Event_Accept_Connect;

				int ret = FALSE;
				int error = 0;
				ret = AcceptEx(
					server_handle.Get_Socket_Server(),
					client_handle.socket,
					client_handle.output_buffer,
					Receive_Data_Length,// 不立即收数据
					Local_Address_Length,
					Remote_Address_Length,
					NULL,// 不需要立即返回字节数
					(LPOVERLAPPED)pEvent);

				if ((ret == SOCKET_ERROR) && (error != WSA_IO_PENDING))
				{
					if (shared_lock.owns_lock())
					{
						shared_lock.unlock();
					}
					unique_lock<shared_mutex> unique_lock{ server_handle.smutex };

					server_handle.client_handles.erase(pEvent->socket);
					server_handle.Destory_Event_handle(pEvent);
				}
			}
			break;
			case Event_Send:
			{
				server_handle.Destory_Event_handle(pEvent);
			}
			break;
			case Event_Receive:
			{
				shared_lock<shared_mutex> shared_lock{ server_handle.smutex };

				auto it = server_handle.client_handles.find(pEvent->socket);
				if (it == server_handle.client_handles.end())
				{
					server_handle.Destory_Event_handle(pEvent);

					break;
				}

				Client_Handle& client_handle = it->second;

				//客户端异常关闭
				if (bytes_transferred == 0)
				{
					if (shared_lock.owns_lock())
					{
						shared_lock.unlock();
					}
					unique_lock<shared_mutex> unique_lock{ server_handle.smutex };

					it = server_handle.client_handles.find(pEvent->socket);
					if (it == server_handle.client_handles.end())
					{
						server_handle.Destory_Event_handle(pEvent);

						break;
					}

					Client_Handle& client_handle_new = it->second;

					//发送一个空数据的event_handle给业务层，表示连接已断开
					size_t queue_index = server_handle.Socket_Map_In_Range(client_handle_new.socket, receive_queues.size());
					Event_handle* pEvent_void_data = server_handle.Construct_Event_handle(
						client_handle_new.socket,
						Event_Receive,
						0,
						0);
					if (pEvent_void_data)
					{
						receive_queues.at(queue_index).enqueue(pEvent_void_data);
					}

					server_handle.client_handles.erase(pEvent->socket);
					cout << "client socket [" << (int)pEvent->socket << "] abnormal close" << endl;

					server_handle.Destory_Event_handle(pEvent);

					break;
				}

				//更新最后活动时间
				client_handle.Update_Last_Active_Time();

				//处理接收到的数据
				bool valid_data = client_handle.Receive_Data_Process(pEvent, bytes_transferred);

				//将有序数据缓冲区的数据全部交给业务层
				Event_handle* pEvent_array[Per_Client_Unordered_Data_Number_Threshold + 3]{};
				size_t count = client_handle.Get_Ordered_Data(pEvent_array, sizeof(pEvent_array) / sizeof(Event_handle*));
				size_t queue_index = server_handle.Socket_Map_In_Range(client_handle.socket, receive_queues.size());
				for (size_t i = 0; i < count; i++)
				{
					receive_queues.at(queue_index).enqueue(pEvent_array[i]);
				}

				//若无序数据缓冲区中积压的接收事件序列号数量超过阈值，说明可能存在数据丢失，此时应关闭连接
				if (client_handle.Get_Unordered_Data_Number() > Per_Client_Unordered_Data_Number_Threshold)
				{
					SOCKET socket = client_handle.socket;

					if (shared_lock.owns_lock())
					{
						shared_lock.unlock();
					}
					unique_lock<shared_mutex> unique_lock{ server_handle.smutex };

					it = server_handle.client_handles.find(socket);
					if (it == server_handle.client_handles.end())
					{
						if (valid_data == false)
						{
							server_handle.Destory_Event_handle(pEvent);
						}

						break;
					}

					Client_Handle& client_handle_new = it->second;

					//发送一个空数据的event_handle给业务层，表示连接已断开
					size_t queue_index = server_handle.Socket_Map_In_Range(client_handle_new.socket, receive_queues.size());
					Event_handle* pEvent_void_data = server_handle.Construct_Event_handle(
						client_handle_new.socket,
						Event_Receive,
						0,
						0);
					if (pEvent_void_data)
					{
						receive_queues.at(queue_index).enqueue(pEvent_void_data);
					}

					server_handle.client_handles.erase(socket);
					cout << "client socket [" << (int)socket << "] unordered data number over threshold, close" << endl;

					if (valid_data == false)
					{
						server_handle.Destory_Event_handle(pEvent);
					}

					break;
				}
				if (valid_data == false)
				{
					server_handle.Destory_Event_handle(pEvent);
				}

				//继续投递异步recv请求
				Event_handle* pEvent_new = server_handle.Construct_Event_handle(
					client_handle.socket,
					Event_Receive,
					Event_Buffer_Size,
					client_handle.Make_Receive_Event_id());
				if (pEvent_new == NULL)
				{
					break;
				}

				int ret = 0;
				int error = 0;
				ret = WSARecv(
					client_handle.socket,
					&pEvent_new->buffer,			//指向缓冲区数组的指针（一个 WSABUF 数组，至少有一项）
					1,							//上面数组的长度，通常为 1
					NULL,						//实际接收到的字节数（仅在同步操作成功时有效，异步操作时通常为 NULL）
					&pEvent_new->flag,				//标志位（如 MSG_PARTIAL），通常设为 0
					(LPWSAOVERLAPPED)pEvent_new,
					NULL						//接收完成后的回调函数（配合事件通知模型），IOCP 不用这个，设为 NULL
				);

				error = WSAGetLastError();
				if ((ret == SOCKET_ERROR) && (error != WSA_IO_PENDING))
				{
					cout << "Error: WSARecv() error code is " << error << endl;

					server_handle.Destory_Event_handle(pEvent_new);
				}
			}
			break;
			default:
			{
				shared_lock<shared_mutex> shared_lock{ server_handle.smutex };

				auto it = server_handle.client_handles.find(pEvent->socket);
				if (it != server_handle.client_handles.end())
				{
					if (shared_lock.owns_lock())
					{
						shared_lock.unlock();
					}
					unique_lock<shared_mutex> unique_lock{ server_handle.smutex };

					server_handle.client_handles.erase(pEvent->socket);
				}

				server_handle.Destory_Event_handle(pEvent);
			}
			break;
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
				Event_handle* pEvent = (Event_handle*)pOverlapped;

				if (completion_key != (ULONG_PTR)&server_handle)
				{
					server_handle.Destory_Event_handle(pEvent);

					continue;
				}

				shared_lock<shared_mutex> shared_lock{ server_handle.smutex };

				auto it = server_handle.client_handles.find(pEvent->socket);
				if (it == server_handle.client_handles.end())
				{
					server_handle.Destory_Event_handle(pEvent);

					continue;
				}

				DWORD error1 = GetLastError();
				DWORD error2 = WSAGetLastError();

				if ((bytes_transferred == 0) &&
					((error1 == ERROR_OPERATION_ABORTED) || (error1 == ERROR_NETNAME_DELETED) || (error1 == ERROR_SUCCESS) || (error2 == WSAECONNRESET)))
				{
					if (shared_lock.owns_lock())
					{
						shared_lock.unlock();
					}
					unique_lock<shared_mutex> unique_lock{ server_handle.smutex };

					it = server_handle.client_handles.find(pEvent->socket);
					if (it == server_handle.client_handles.end())
					{
						server_handle.Destory_Event_handle(pEvent);

						continue;
					}

					Client_Handle& client_handle = it->second;

					//发送一个空数据的event_handle给业务层，表示连接已断开
					size_t queue_index = server_handle.Socket_Map_In_Range(client_handle.socket, receive_queues.size());
					Event_handle* pEvent_void_data = server_handle.Construct_Event_handle(
						client_handle.socket,
						Event_Receive,
						0,
						0);
					if (pEvent_void_data)
					{
						receive_queues.at(queue_index).enqueue(pEvent_void_data);
					}

					//当socket断开连接时，所有未完成的异步操作都会被系统强制完成，并通过完成端口（IOCP）机制通知应用程序
					server_handle.client_handles.erase(pEvent->socket);
					cout << "client socket [" << (int)pEvent->socket << "] close" << endl;
				}

				server_handle.Destory_Event_handle(pEvent);
			}
		}
	}
}

void send_thread(bool& run_flag, Server_Handle& server_handle, vector<moodycamel::ConcurrentQueue<Event_handle*>>& send_queues)
{
	while (run_flag)
	{
		auto it = send_queues.begin();
		while (it != send_queues.end())
		{
			Event_handle* pEvent = NULL;
			size_t count = 0;

			while (it->try_dequeue(pEvent))
			{
				if ((pEvent->buffer.len == 0) || (pEvent->buffer.len > Event_Buffer_Size))
				{
					server_handle.Destory_Event_handle(pEvent);
					continue;
				}

				shared_lock<shared_mutex> shared_lock{ server_handle.smutex };

				auto it1 = server_handle.client_handles.find(pEvent->socket);
				if (it1 == server_handle.client_handles.end())
				{
					server_handle.Destory_Event_handle(pEvent);
					continue;
				}

				Client_Handle& client_handle = it1->second;

				if (client_handle.socket_status != Socket_Connected)
				{
					server_handle.Destory_Event_handle(pEvent);
					continue;
				}

				//投递异步send请求
				pEvent->buffer.buf = pEvent->data;
				pEvent->event = Event_Send;

				int ret = 0;
				int error = 0;
				ret = WSASend(
					client_handle.socket,
					&pEvent->buffer,			//指向缓冲区数组的指针（一个 WSABUF 数组，至少有一项）
					1,							//上面数组的长度，通常为 1
					NULL,						//实际发送的字节数（仅在同步操作成功时有效，异步操作时通常为 NULL）
					0,							//发送标志（一般为 0）
					(LPWSAOVERLAPPED)pEvent,
					NULL						//发送完成后的回调函数（配合事件通知模型），IOCP 不用这个，设为 NULL
				);

				error = WSAGetLastError();
				if ((ret == SOCKET_ERROR) && (error != WSA_IO_PENDING))
				{
					server_handle.Destory_Event_handle(pEvent);
				}

				count++;
				if (count > 5)
				{
					break;
				}
			}

			it++;
		}

		this_thread::sleep_for(chrono::milliseconds(1));
	}
}

void clean_thread(bool& run_flag, Server_Handle& server_handle)
{
	auto last_scan_time = chrono::system_clock::now();

	while (run_flag)
	{
		//每 Client_Active_Timeout_Scan_Interval 秒遍历一次所有 client_handles ，把活动超时的socket断开连接，把socket status为invalid的client_handle清除
		auto current_time = chrono::system_clock::now();
		if (chrono::duration_cast<chrono::seconds>(current_time - last_scan_time).count() > Client_Active_Timeout_Scan_Interval)
		{
			last_scan_time = current_time;

			
		}

		this_thread::sleep_for(chrono::seconds(3));
	}
}
