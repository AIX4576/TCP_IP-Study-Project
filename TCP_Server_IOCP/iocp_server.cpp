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

	int result;
	sockaddr_in address_server
	{
		.sin_family = AF_INET,
		.sin_port = htons(Server_Port),
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

	client_handles.reserve(Max_Clients_Number + 2);

	//投递异步accept请求，投递多个
	for (int i = 0; i < Worker_Threads_Number; i++)
	{
		Client_Handle temp_client;
		if (temp_client.socket == INVALID_SOCKET)
		{
			continue;
		}

		Event_handle* pEvent = new Event_handle{ temp_client.socket ,Event_Accept_Connect ,FALSE };
		if (pEvent == NULL)
		{
			continue;
		}

		lock_guard<mutex> lock(client_handles_mutex);
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
			delete pEvent;
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
			if (completion_key != (ULONG_PTR)&server_handle)
			{
				continue;
			}

			Event_handle* pEvent = (Event_handle*)pOverlapped;
			auto it = server_handle.client_handles.find(pEvent->socket);

			if (it == server_handle.client_handles.end())
			{
				delete pEvent;

				continue;
			}

			Client_Handle& client_handle = it->second;

			switch (pEvent->event)
			{
			case Event_Accept_Connect:
			{
				//将新连接的 client socket 与 iocp 绑定，然后投递异步recv请求，投递多个
				HANDLE iocp = CreateIoCompletionPort((HANDLE)client_handle.socket, server_handle.Get_IOCP(), (ULONG_PTR)&server_handle, 0);
				if (iocp)
				{
					uint32_t count = 0;

					for (int i = 0; i < Per_Client_Receive_Event_Number; i++)
					{
						Event_handle* pEvent1 = new Event_handle
						{ client_handle.socket,
							Event_Receive,
							TRUE,
							Event_Buffer_Size,
							client_handle.Make_Receive_Event_id()
						};
						if (pEvent1 == NULL)
						{
							continue;
						}
						if (pEvent1->buffer.buf == NULL)
						{
							delete pEvent1;
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

							delete pEvent1;
						}
						else
						{
							count++;
						}
					}

					if (count)
					{
						client_handle.Connect_Deal();
						cout << "client [" << (int)client_handle.socket << "] connected, ip is " << client_handle.ip << ", port is " << client_handle.port << endl;
					}
					else
					{
						lock_guard<mutex> lock{ server_handle.client_handles_mutex };
						server_handle.client_handles.erase(client_handle.socket);
					}
				}
				else
				{
					lock_guard<mutex> lock{ server_handle.client_handles_mutex };
					server_handle.client_handles.erase(client_handle.socket);
				}

				//投递新的异步accept请求
				if (server_handle.client_handles.size() <= Max_Clients_Number)
				{
					Client_Handle temp_client;

					if (temp_client.socket != INVALID_SOCKET)
					{
						lock_guard<mutex> lock(server_handle.client_handles_mutex);

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
							server_handle.client_handles.erase(client_handle1.socket);
							delete pEvent;
						}
					}
					else
					{
						delete pEvent;
					}
				}
				else
				{
					delete pEvent;
				}
			}
			break;
			case Event_Disconnect:
			{
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
					lock_guard<mutex> lock(server_handle.client_handles_mutex);
					server_handle.client_handles.erase(client_handle.socket);
					delete pEvent;
				}
			}
			break;
			case Event_Send:
			{
				delete pEvent;
			}
			break;
			case Event_Receive:
			{
				//更新最后活动时间
				client_handle.Update_Last_Active_Time();

				//将接收到的数据排序，存入有序数据缓冲区和乱序数据缓冲区
				client_handle.Sort_Receive_Data(pEvent, bytes_transferred);

				//若有序数据缓冲区的数据足够组成完整消息，则提交给业务层
				if (client_handle.Get_Ordered_Data_Size() >= Completed_Message_Size_Threshold)
				{
					
				}

				//若乱序数据缓冲区中积压的接收事件序列号数量超过阈值，说明可能存在数据丢失，此时应关闭连接
				if (client_handle.Get_Unordered_Data_Number() > Per_Client_Unordered_Data_Number_Threshold)
				{
					lock_guard<mutex> lock{ server_handle.client_handles_mutex };
					server_handle.client_handles.erase(client_handle.socket);

					delete pEvent;

					break;
				}

				//设置该receive事件新的id
				pEvent->Set_id(client_handle.Make_Receive_Event_id());

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

					delete pEvent;
				}
			}
			break;
			default:
			{
				lock_guard<mutex> lock{ server_handle.client_handles_mutex };
				server_handle.client_handles.erase(client_handle.socket);

				delete pEvent;
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

				if (completion_key == (ULONG_PTR)&server_handle)
				{
					DWORD error1 = GetLastError();
					DWORD error2 = WSAGetLastError();

					if ((bytes_transferred == 0) &&
						((error1 == ERROR_OPERATION_ABORTED) || (error1 == ERROR_NETNAME_DELETED) || (error1 == ERROR_SUCCESS) || (error2 == WSAECONNRESET)))
					{
						auto it1 = server_handle.client_handles.find(pEvent->socket);
						if (it1 != server_handle.client_handles.end())
						{
							//当socket断开连接时，所有未完成的异步操作都会被系统强制完成，并通过完成端口（IOCP）机制通知应用程序
							cout << "client socket [" << (int)pEvent->socket << "] close" << endl;

							lock_guard<mutex> lock{ server_handle.client_handles_mutex };
							server_handle.client_handles.erase(pEvent->socket);
						}
					}
				}

				delete pEvent;
			}
		}
	}
}

void clean_thread(bool& run_flag, Server_Handle& server_handle)
{
	auto last_scan_time = chrono::system_clock::now();

	while (run_flag)
	{
		//每 Client_Active_Timeout_Scan_Interval 秒遍历一次所有 client_handles ，把活动超时的 socket 断开连接
		auto current_time = chrono::system_clock::now();
		if (chrono::duration_cast<chrono::seconds>(current_time - last_scan_time).count() > Client_Active_Timeout_Scan_Interval)
		{
			last_scan_time = current_time;

			lock_guard<mutex> lock{ server_handle.client_handles_mutex };

			// 用迭代器遍历，避免范围for的隐式迭代器失效问题
			auto it = server_handle.client_handles.begin();
			while (it != server_handle.client_handles.end())
			{
				if (it->second.Is_Active_Timeout())
				{
					// erase返回下一个有效迭代器，避免失效
					it = server_handle.client_handles.erase(it);
				}
				else
				{
					it++;
				}
			}
		}

		this_thread::sleep_for(chrono::seconds(3));
	}
}