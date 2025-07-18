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

	//�����̳߳�
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
	//����һ��֧���ص�IO��TCP�׽���, WSASocket()������Windowsϵͳר�õ�,socket()�����ǿ�ƽ̨��
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

	//������ɶ˿�
	iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, Worker_Threads_Number);
	if (iocp == NULL)
	{
		cout << "Error: create iocp fail" << endl;
		closesocket(socket);
		socket = INVALID_SOCKET;

		return;
	}

	//��socket����ɶ˿ڰ�
	CreateIoCompletionPort((HANDLE)socket, iocp, (ULONG_PTR)this, 0);

	//Ͷ���첽accept����Ͷ�ݶ��
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
				Receive_Data_Length,// ������������
				Local_Address_Length,
				Remote_Address_Length,
				NULL,// ����Ҫ���������ֽ���
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
			// �ɹ������ǿ��Լ������������ɵ�I/O����
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

					//�������ӵ� client socket �� iocp �󶨣�Ȼ��Ͷ���첽recv����
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
							&receive_event.buffer,				//ָ�򻺳��������ָ�루һ�� WSABUF ���飬������һ�
							1,									//��������ĳ��ȣ�ͨ��Ϊ 1
							NULL,								//ʵ�ʽ��յ����ֽ���������ͬ�������ɹ�ʱ��Ч���첽����ʱͨ��Ϊ NULL��
							&receive_event.flag,				//��־λ���� MSG_PARTIAL����ͨ����Ϊ 0
							(LPWSAOVERLAPPED)&receive_event,
							NULL								//������ɺ�Ļص�����������¼�֪ͨģ�ͣ���IOCP �����������Ϊ NULL
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

					//Ͷ���µ��첽accept����
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
								Receive_Data_Length,// ������������
								Local_Address_Length,
								Remote_Address_Length,
								NULL,// ����Ҫ���������ֽ���
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

					//���ø�socket������Ͷ���첽accept����(������ʹ��DisconnectEx()�����Ͽ�socket���Ӳ��ܸ���)
					pEvent->event = Event_Accept_Connect;

					AcceptEx(
						server_handle.Get_Socket_Server(),
						client_handle.socket,
						client_handle.output_buffer,
						Receive_Data_Length,// ������������
						Local_Address_Length,
						Remote_Address_Length,
						NULL,// ����Ҫ���������ֽ���
						(LPOVERLAPPED)pEvent);
				}
				break;
				case Event_Receive:
				{
					cout << "receive data from socket [" << (int)client_handle.socket << "], data size is " << bytes_transferred << endl;

					//����Ͷ���첽recv����
					int ret = 0;
					int error = 0;
					ret = WSARecv(
						client_handle.socket,
						&pEvent->buffer,			//ָ�򻺳��������ָ�루һ�� WSABUF ���飬������һ�
						1,							//��������ĳ��ȣ�ͨ��Ϊ 1
						NULL,						//ʵ�ʽ��յ����ֽ���������ͬ�������ɹ�ʱ��Ч���첽����ʱͨ��Ϊ NULL��
						&pEvent->flag,				//��־λ���� MSG_PARTIAL����ͨ����Ϊ 0
						(LPWSAOVERLAPPED)pEvent,
						NULL						//������ɺ�Ļص�����������¼�֪ͨģ�ͣ���IOCP �����������Ϊ NULL
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
				// ��ʱ�ˣ�û���κ��������
			}
			else
			{
				// ��������ɵ������ˣ�Ҫ���ݴ������������������ӱ��Ͽ���
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

