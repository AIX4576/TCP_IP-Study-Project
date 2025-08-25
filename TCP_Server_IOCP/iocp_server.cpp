#include"iocp_server.h"

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

	client_handles.reserve(Max_Clients_Number + 2);

	//Ͷ���첽accept����Ͷ�ݶ��
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
			Receive_Data_Length,// ������������
			Local_Address_Length,
			Remote_Address_Length,
			NULL,// ����Ҫ���������ֽ���
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
			// �ɹ������ǿ��Լ������������ɵ�I/O����
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
				//�������ӵ� client socket �� iocp �󶨣�Ȼ��Ͷ���첽recv����Ͷ�ݶ��
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
							&pEvent1->buffer,					//ָ�򻺳��������ָ�루һ�� WSABUF ���飬������һ�
							1,									//��������ĳ��ȣ�ͨ��Ϊ 1
							NULL,								//ʵ�ʽ��յ����ֽ���������ͬ�������ɹ�ʱ��Ч���첽����ʱͨ��Ϊ NULL��
							&pEvent1->flag,						//��־λ���� MSG_PARTIAL����ͨ����Ϊ 0
							(LPWSAOVERLAPPED)pEvent1,
							NULL								//������ɺ�Ļص�����������¼�֪ͨģ�ͣ���IOCP �����������Ϊ NULL
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

				//Ͷ���µ��첽accept����
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
							Receive_Data_Length,// ������������
							Local_Address_Length,
							Remote_Address_Length,
							NULL,// ����Ҫ���������ֽ���
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

				//���ø�socket������Ͷ���첽accept����(������ʹ��DisconnectEx()�����Ͽ�socket���Ӳ��ܸ���)
				pEvent->event = Event_Accept_Connect;

				int ret = FALSE;
				int error = 0;
				ret = AcceptEx(
					server_handle.Get_Socket_Server(),
					client_handle.socket,
					client_handle.output_buffer,
					Receive_Data_Length,// ������������
					Local_Address_Length,
					Remote_Address_Length,
					NULL,// ����Ҫ���������ֽ���
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
				//�������ʱ��
				client_handle.Update_Last_Active_Time();

				//�����յ����������򣬴����������ݻ��������������ݻ�����
				client_handle.Sort_Receive_Data(pEvent, bytes_transferred);

				//���������ݻ������������㹻���������Ϣ�����ύ��ҵ���
				if (client_handle.Get_Ordered_Data_Size() >= Completed_Message_Size_Threshold)
				{
					
				}

				//���������ݻ������л�ѹ�Ľ����¼����к�����������ֵ��˵�����ܴ������ݶ�ʧ����ʱӦ�ر�����
				if (client_handle.Get_Unordered_Data_Number() > Per_Client_Unordered_Data_Number_Threshold)
				{
					lock_guard<mutex> lock{ server_handle.client_handles_mutex };
					server_handle.client_handles.erase(client_handle.socket);

					delete pEvent;

					break;
				}

				//���ø�receive�¼��µ�id
				pEvent->Set_id(client_handle.Make_Receive_Event_id());

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
				// ��ʱ�ˣ�û���κ��������
			}
			else
			{
				// ��������ɵ������ˣ�Ҫ���ݴ������������������ӱ��Ͽ���
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
							//��socket�Ͽ�����ʱ������δ��ɵ��첽�������ᱻϵͳǿ����ɣ���ͨ����ɶ˿ڣ�IOCP������֪ͨӦ�ó���
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
		//ÿ Client_Active_Timeout_Scan_Interval �����һ������ client_handles ���ѻ��ʱ�� socket �Ͽ�����
		auto current_time = chrono::system_clock::now();
		if (chrono::duration_cast<chrono::seconds>(current_time - last_scan_time).count() > Client_Active_Timeout_Scan_Interval)
		{
			last_scan_time = current_time;

			lock_guard<mutex> lock{ server_handle.client_handles_mutex };

			// �õ��������������ⷶΧfor����ʽ������ʧЧ����
			auto it = server_handle.client_handles.begin();
			while (it != server_handle.client_handles.end())
			{
				if (it->second.Is_Active_Timeout())
				{
					// erase������һ����Ч������������ʧЧ
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