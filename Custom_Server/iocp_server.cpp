#include"iocp_server.h"

// 向业务层投递一个空数据的 Event_handle，表示该 socket 的连接已断开（buffer.len == 0）
// 本文件内的断开路径（正常断开、异常断开、主动断开、无序数据超阈值断开）统一走这里，避免同一段逻辑出现多份副本
static void Notify_Client_Disconnected(
	Server_Handle& server_handle,
	SOCKET socket,
	vector<moodycamel::ConcurrentQueue<Event_handle_Ptr>>& receive_queues,
	vector<unique_ptr<counting_semaphore<>>>& receive_semaphores)
{
	size_t queue_index = server_handle.Socket_Map_In_Range(socket, receive_queues.size());
	auto pEvent_void_data = server_handle.MakeUnique_Event_handle(socket, Event_Receive, 0, 0);
	if (pEvent_void_data)
	{
		receive_queues.at(queue_index).enqueue(move(pEvent_void_data));
		receive_semaphores.at(queue_index)->release();
	}
}

Server_Handle::Server_Handle() :socket(INVALID_SOCKET), iocp(nullptr), initialize_flag(FALSE)
{
	//创建一个支持重叠IO的TCP套接字, WSASocket()函数是Windows系统专用的,socket()函数是跨平台的
	socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
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
	iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, Worker_Threads_Number);
	if (iocp == nullptr)
	{
		cout << "Error: create iocp fail" << endl;
		closesocket(socket);
		socket = INVALID_SOCKET;

		return;
	}

	//将socket和完成端口绑定
	if (CreateIoCompletionPort((HANDLE)socket, iocp, (ULONG_PTR)this, 0) == nullptr)
	{
		//这一步是初始化序列里唯一"失败后什么都不会发生"的环节：监听 socket 不在完成端口上时，
		//AcceptEx 仍会返回 WSA_IO_PENDING、initialize_flag 仍会被置 TRUE，但完成包永远没有去向，
		//服务器会静默地不处理任何连接。因此必须在此拦下。
		//iocp 不在此处关闭：~Server_Handle() 会统一回收
		cout << "Error: bind listen socket to iocp fail, error code is " << GetLastError() << endl;
		closesocket(socket);
		socket = INVALID_SOCKET;

		return;
	}

	//初始化client_handles的桶数量为 Max_Clients_Number * 1.2，保证永远不会触发rehash操作
	client_handles.reserve((size_t)(Max_Clients_Number * 1.2));
	cout << "client_handles buckets count is " << client_handles.bucket_count() << endl;

	//初始化p_event_handle_pools的个数为 Threads_Number，每个无锁对象池预分配 (Max_Clients_Number * 10 / Threads_Number) 个对象
	p_event_handle_pools.reserve(Threads_Number);
	for (size_t i = 0; i < Threads_Number; i++)
	{
		p_event_handle_pools.emplace_back(make_unique<ConcurrentObjectPool<Event_handle>>(Max_Clients_Number * 10 / Threads_Number));
	}
	

	//投递异步accept请求，投递多个（pending_accept_count 的维护统一在 Post_AcceptEx 内）
	for (size_t i = 0; i < Worker_Threads_Number; i++)
	{
		Post_AcceptEx();
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
		closesocket(socket);
		socket = INVALID_SOCKET;
		socket_status = Socket_Invalid;
	}

	//析构前销毁ordered_data和unordered_data中的pEvent，避免内存泄漏
	if (pServer_Handle)
	{
		Destroy_Buffered_Events();
	}
}

//销毁有序/无序缓冲区中存储的全部 Event_handle 并清空容器；调用方必须已持有 Client_Handle::lock
//（不可在临界区内调用 Get_Ordered_Data/Get_Unordered_Data——自旋锁不可重入，故直接操作容器）
void Client_Handle::Destroy_Buffered_Events()
{
	while (ordered_data.empty() == false)
	{
		if (pServer_Handle)
		{
			pServer_Handle->Destroy_Event_handle(ordered_data.front());
		}
		ordered_data.pop_front();
	}

	for (auto& pair : unordered_data)
	{
		if (pServer_Handle)
		{
			pServer_Handle->Destroy_Event_handle(pair.second);
		}
	}
	unordered_data.clear();
	unordered_data_count.store(0, memory_order_relaxed);
}

void work_thread(
	Server_Handle& server_handle,
	vector<moodycamel::ConcurrentQueue<unique_ptr<Event_handle,
	ConcurrentObjectPool<Event_handle>::Deleter>>>& receive_queues,
	vector<unique_ptr<counting_semaphore<>>>& receive_semaphores)
{
	DWORD bytes_transferred = 0;
	ULONG_PTR completion_key = 0;
	LPOVERLAPPED pOverlapped = nullptr;
	bool result = 0;

	/*
	* 退出机制：生命周期由 Server_Handle::Close_Server() 的哨兵包管理，
	* 不依赖模块 run_flag——否则 GQCS 超时退出会先于 socket 关闭发生，取消事件无人处理导致 pending 事件泄漏
	* 1. 取到 overlapped == nullptr 的哨兵包（Close_Server 投递）：处理完手头及排队事件后退出
	* 2. 兜底：GQCS 超时且 io_running 已置 false（哨兵丢失等异常场景）时退出，避免永久阻塞
	*/
	for (;;)
	{
		bytes_transferred = 0;
		completion_key = 0;
		pOverlapped = nullptr;

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

			//退出哨兵包：overlapped 为空，由 Close_Server 投递
			if (pOverlapped == nullptr)
			{
				break;
			}

			Event_handle* pEvent = reinterpret_cast<Event_handle*>(pOverlapped);

			if (completion_key != (ULONG_PTR)&server_handle)
			{
				server_handle.Destroy_Event_handle(pEvent);

				continue;
			}

			switch (pEvent->event)
			{
			case Event_Accept_Connect:
			{
				//本事件对应的 AcceptEx 已完成，先归还 pending 配额（与 Post_AcceptEx 的自增配对）
				server_handle.pending_accept_count.fetch_sub(1, memory_order_release);

				//整个分支在 smutex 写锁下执行：原实现在中途 unlock() 后未恢复共享锁，
				//末尾的 client_handles.size() 成了无锁读，与 guard/send 线程构成数据竞争（问题14a）
				unique_lock<shared_mutex> unique_lock{ server_handle.smutex };

				auto it = server_handle.client_handles.find(pEvent->socket);
				if (it == server_handle.client_handles.end())
				{
					server_handle.Destroy_Event_handle(pEvent);

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
						if (pEvent1 == nullptr)
						{
							break;
						}

						int ret = 0;
						int error = 0;
						ret = WSARecv(
							client_handle.socket,
							&pEvent1->buffer,					//指向缓冲区数组的指针（一个 WSABUF 数组，至少有一项）
							1,									//上面数组的长度，通常为 1
							nullptr,								//实际接收到的字节数（仅在同步操作成功时有效，异步操作时通常为 nullptr）
							&pEvent1->flag,						//标志位（如 MSG_PARTIAL），通常设为 0
							(LPWSAOVERLAPPED)pEvent1,
							nullptr								//接收完成后的回调函数（配合事件通知模型），IOCP 不用这个，设为 nullptr
						);

						error = WSAGetLastError();
						if ((ret == SOCKET_ERROR) && (error != WSA_IO_PENDING))
						{
							cout << "Error: WSARecv() error code is " << error << endl;

							server_handle.Destroy_Event_handle(pEvent1);

							break;
						}
						else
						{
							count++;
						}
					}

					if (count == Per_Client_Receive_Event_Number)
					{
						client_handle.Connect_Deal();
					}
					else
					{
						server_handle.client_handles.erase(pEvent->socket);
					}
				}
				else
				{
					server_handle.client_handles.erase(pEvent->socket);
				}

				//补投新的异步 accept 请求：本次完成消耗了一个 pending AcceptEx，按需补一个
				//判据用 pending_accept_count（待处理 AcceptEx 数）而非 client_handles.size()（总条目数），
				//后者含已建立的活跃连接，会把总连接数误当待处理 AcceptEx 数（问题14b）
				if (server_handle.io_running.load(memory_order_acquire)
					&& (server_handle.pending_accept_count.load(memory_order_acquire) < Worker_Threads_Number)
					&& (server_handle.client_handles.size() < Max_Clients_Number))
				{
					server_handle.Post_AcceptEx();
				}

				server_handle.Destroy_Event_handle(pEvent);
			}
			break;
			case Event_Disconnect:
			{
				//guard_thread（活动超时）或 send_thread（WSASend 同步失败）投递的主动断开请求。
				//投递端只携带 socket 值，断开动作在此统一完成：复位连接状态、通知业务层、从容器摘除。
				//已知限制（两个同类窗口，均为"投递到处理之间的延迟"所致，长度是一次投递往返）：
				//1. 若该客户端已被其它路径摘除、其 socket 句柄值被系统回收给新连接，本事件会误断新连接；
				//2. 若扫描判定超时后、本事件被处理前该连接又有新数据到达（活动时间已被刷新），仍会按已发出的请求断开。
				//此处接受这两个窗口（与既有各断开路径的同类窗口一致），换取 guard/send 线程无需持有连接容器的写锁；
				//误伤的后果是连接被断开，客户端重连即可恢复。
				unique_lock<shared_mutex> unique_lock{ server_handle.smutex };

				auto it = server_handle.client_handles.find(pEvent->socket);
				if (it != server_handle.client_handles.end())
				{
					if (it->second.socket_status.load(memory_order_acquire) == Socket_Connected)
					{
						//只在连接态通知业务层：仍处于 AcceptEx pending 的条目（Disconnected）尚未上线
						Notify_Client_Disconnected(server_handle, pEvent->socket, receive_queues, receive_semaphores);
					}

					//主动断开：复位连接状态并销毁其缓冲区中未被取走的接收事件（Disonnect_Deal 要求持写锁调用），
					//再从容器摘除——析构会关闭 socket，系统随即强制完成该 socket 的全部 pending IO 并投递取消完成包，
					//那些事件在下面查不到对应客户端，走销毁路径，对象池因此保持配对
					it->second.Disonnect_Deal();
					server_handle.client_handles.erase(it);

					cout << "client socket [" << (int)pEvent->socket << "] disconnected: active timeout or send failure" << endl;
				}

				server_handle.Destroy_Event_handle(pEvent);
			}
			break;
			case Event_Send:
			{
				//WSASend 可能只发出一部分即完成（受 TCP 发送窗口/发送缓冲限制）：bytes_transferred 是本段
				//实际发出的字节数，未发完则前移缓冲区、续发剩余部分，直到全部发出。
				//上一次操作已完成，同一 OVERLAPPED 可以复用。
				//不能把事件回投 send_queue——send_thread 会无条件把 buffer.buf 重置回 data，导致已发部分被重复发送
				ULONG sent_bytes = bytes_transferred;

				if (sent_bytes < pEvent->buffer.len)
				{
					SOCKET client_socket = pEvent->socket;
					bool rearmed = false;
					//sent_bytes == 0：本次没有推进任何字节，重投只会原地打转，按发送失败处理
					bool need_disconnect = (sent_bytes == 0);

					if (need_disconnect == false)
					{
						shared_lock<shared_mutex> shared_lock{ server_handle.smutex };

						//事件在途期间连接可能已被其它路径摘除或断开；io_running 为 false 时不再挂新 IO
						auto it = server_handle.client_handles.find(client_socket);
						if ((it != server_handle.client_handles.end())
							&& (it->second.socket_status.load(memory_order_acquire) == Socket_Connected)
							&& (server_handle.io_running.load(memory_order_acquire)))
						{
							pEvent->buffer.buf += sent_bytes;
							pEvent->buffer.len -= sent_bytes;
							pEvent->event = Event_Send;

							int ret = 0;
							int error = 0;
							ret = WSASend(
								it->second.socket,
								&pEvent->buffer,			//指向剩余未发送部分的 WSABUF
								1,							//上面数组的长度，通常为 1
								nullptr,						//实际发送的字节数（仅在同步操作成功时有效，异步操作时通常为 nullptr）
								0,							//发送标志（一般为 0）
								reinterpret_cast<LPWSAOVERLAPPED>(pEvent),
								nullptr						//发送完成后的回调函数（配合事件通知模型），IOCP 不用这个，设为 nullptr
							);

							error = WSAGetLastError();
							if ((ret == SOCKET_ERROR) && (error != WSA_IO_PENDING))
							{
								cout << "Error: WSASend() re-send fail, error code is " << error << ", client socket [" << static_cast<int>(client_socket) << "] will be disconnected" << endl;

								need_disconnect = true;
							}
							else
							{
								rearmed = true;
							}
						}
					}

					//已重新投递：事件所有权仍归 IOCP，此处不能销毁
					if (rearmed)
					{
						break;
					}

					//续发失败：投递主动断开请求，交给 work_thread 的统一断开路径处理（与 send_thread 的同步失败一致）
					if (need_disconnect && server_handle.io_running.load(memory_order_acquire))
					{
						Event_handle* pEvent_disconnect = server_handle.Construct_Event_handle(client_socket, Event_Disconnect);
						if (pEvent_disconnect)
						{
							if (PostQueuedCompletionStatus(server_handle.Get_IOCP(), 0, reinterpret_cast<ULONG_PTR>(&server_handle), reinterpret_cast<LPOVERLAPPED>(pEvent_disconnect)) == FALSE)
							{
								cout << "Error: PostQueuedCompletionStatus() fail, error code is " << GetLastError() << endl;

								server_handle.Destroy_Event_handle(pEvent_disconnect);
							}
						}
					}
				}

				server_handle.Destroy_Event_handle(pEvent);
			}
			break;
			case Event_Receive:
			{
				shared_lock<shared_mutex> shared_lock{ server_handle.smutex };

				auto it = server_handle.client_handles.find(pEvent->socket);
				if (it == server_handle.client_handles.end())
				{
					server_handle.Destroy_Event_handle(pEvent);

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
					if (it != server_handle.client_handles.end())
					{
						Notify_Client_Disconnected(server_handle, pEvent->socket, receive_queues, receive_semaphores);

						server_handle.client_handles.erase(it);
					}

					server_handle.Destroy_Event_handle(pEvent);

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
					receive_queues.at(queue_index).enqueue(server_handle.MakeUnique_Event_handle(pEvent_array[i]));
				}
				receive_semaphores.at(queue_index)->release(count);

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
					if (it != server_handle.client_handles.end())
					{
						Notify_Client_Disconnected(server_handle, socket, receive_queues, receive_semaphores);

						server_handle.client_handles.erase(it);
						cout << "client socket [" << (int)socket << "] unordered data number over threshold, close" << endl;
					}

					if (valid_data == false)
					{
						server_handle.Destroy_Event_handle(pEvent);
					}

					break;
				}

				if (valid_data == false)
				{
					server_handle.Destroy_Event_handle(pEvent);
				}

				//继续投递异步recv请求
				Event_handle* pEvent_new = server_handle.Construct_Event_handle(
					client_handle.socket,
					Event_Receive,
					Event_Buffer_Size,
					client_handle.Make_Receive_Event_id());
				if (pEvent_new == nullptr)
				{
					break;
				}

				int ret = 0;
				int error = 0;
				ret = WSARecv(
					client_handle.socket,
					&pEvent_new->buffer,			//指向缓冲区数组的指针（一个 WSABUF 数组，至少有一项）
					1,							//上面数组的长度，通常为 1
					nullptr,						//实际接收到的字节数（仅在同步操作成功时有效，异步操作时通常为 nullptr）
					&pEvent_new->flag,				//标志位（如 MSG_PARTIAL），通常设为 0
					(LPWSAOVERLAPPED)pEvent_new,
					nullptr						//接收完成后的回调函数（配合事件通知模型），IOCP 不用这个，设为 nullptr
				);

				error = WSAGetLastError();
				if ((ret == SOCKET_ERROR) && (error != WSA_IO_PENDING))
				{
					if (error == 10054) // 对端关闭了连接
					{
						if (shared_lock.owns_lock())
						{
							shared_lock.unlock();
						}
						unique_lock<shared_mutex> unique_lock{ server_handle.smutex };

						it = server_handle.client_handles.find(pEvent_new->socket);
						if (it != server_handle.client_handles.end())
						{
							server_handle.client_handles.erase(it);
							cout << "client socket [" << (int)pEvent_new->socket << "] opposite close" << endl;
						}
					}
					else
					{
						cout << "Error: WSARecv() error code is " << error << endl;
					}

					server_handle.Destroy_Event_handle(pEvent_new);
				}
			}
			break;
			default:
			{
				unique_lock<shared_mutex> unique_lock{ server_handle.smutex };

				server_handle.client_handles.erase(pEvent->socket);
				server_handle.Destroy_Event_handle(pEvent);
			}
			break;
			}
		}
		else
		{
			if (pOverlapped == nullptr)
			{
				// 超时了，没有任何任务完成
				//兜底退出：关闭流程已启动但哨兵未能送达时，避免永久阻塞（正常关闭时哨兵先到）
				if (server_handle.io_running.load(memory_order_acquire) == false)
				{
					break;
				}
			}
			else
			{
				// 有任务完成但出错了，要根据错误码来处理，比如连接被断开等
				Event_handle* pEvent = reinterpret_cast<Event_handle*>(pOverlapped);

				if (completion_key != (ULONG_PTR)&server_handle)
				{
					server_handle.Destroy_Event_handle(pEvent);

					continue;
				}

				//AcceptEx 的异步失败：该条目仍是"待 accept"状态，永远不会变成连接。
				//此处统一自减 pending 配额并摘除条目，不再落入下面的通用错误分支——通用分支的白名单面向已建立连接，
				//且不会自减配额；漏减会令 pending_accept_count 只增不减，补投判定恒假后永久停止补投（与问题14b 同源）
				if (pEvent->event == Event_Accept_Connect)
				{
					DWORD accept_error = GetLastError();//先取错误码：erase 触发的 closesocket 会覆盖 last error

					server_handle.pending_accept_count.fetch_sub(1, memory_order_release);

					{
						unique_lock<shared_mutex> unique_lock{ server_handle.smutex };

						auto it = server_handle.client_handles.find(pEvent->socket);
						if (it != server_handle.client_handles.end())
						{
							server_handle.client_handles.erase(it);
						}
					}

					//关闭序列第 2 步的 closesocket(listen) 会取消全部 pending AcceptEx，完成包以
					//ERROR_OPERATION_ABORTED(995) 返回。这是关闭的预期路径而非错误：每次退出都会产生
					//约 Worker_Threads_Number 条这样的完成包，若按错误记录会刷屏。
					//故仅在"非取消错误且未进入关闭序列"时记录；计数自减、条目摘除、事件销毁照常执行。
					if ((accept_error != ERROR_OPERATION_ABORTED) && (server_handle.io_running.load(memory_order_acquire)))
					{
						cout << "Error: AcceptEx() async fail, error code is " << accept_error << endl;
					}

					server_handle.Destroy_Event_handle(pEvent);

					continue;
				}

				shared_lock<shared_mutex> shared_lock{ server_handle.smutex };

				auto it = server_handle.client_handles.find(pEvent->socket);
				if (it == server_handle.client_handles.end())
				{
					server_handle.Destroy_Event_handle(pEvent);

					continue;
				}

				DWORD error1 = GetLastError();
				DWORD error2 = WSAGetLastError();

				//错误码白名单：覆盖收发两侧"连接已不可用"的情形。发送侧补充 WSAECONNABORTED/WSAESHUTDOWN/WSAENOTCONN，
				//不收进来则这些发送失败的连接只能等 guard_thread 的活动超时兜底（问题12）。
				//WSAENOBUFS 属临时资源耗尽、连接本身仍可用，故意不列入，避免高负载下误断大批正常连接。
				if ((bytes_transferred == 0) &&
					((error1 == ERROR_OPERATION_ABORTED) || (error1 == ERROR_NETNAME_DELETED) || (error1 == ERROR_SUCCESS) || (error1 == ERROR_CONNECTION_ABORTED) || (error2 == WSAECONNRESET) || (error2 == WSAECONNABORTED) || (error2 == WSAESHUTDOWN) || (error2 == WSAENOTCONN)))
				{
					if (shared_lock.owns_lock())
					{
						shared_lock.unlock();
					}
					unique_lock<shared_mutex> unique_lock{ server_handle.smutex };

					it = server_handle.client_handles.find(pEvent->socket);
					if (it != server_handle.client_handles.end())
					{
						Client_Handle& client_handle = it->second;
						if (client_handle.socket_status == Socket_Connected)
						{
							Notify_Client_Disconnected(server_handle, pEvent->socket, receive_queues, receive_semaphores);
						}

						//当socket对端断开连接或本端主动关闭时，所有与该socket相关的未完成的异步操作都会被系统强制完成，并通过完成端口（IOCP）机制通知应用程序
						server_handle.client_handles.erase(it);
					}
				}

				server_handle.Destroy_Event_handle(pEvent);
			}
		}
	}

	//退出计数：Close_Server() 据此确认全部 work_thread 已停止访问 IOCP
	server_handle.work_thread_exit_count.fetch_add(1, memory_order_release);
}

void send_thread(
	atomic<bool>& run_flag,
	Server_Handle& server_handle,
	moodycamel::ConcurrentQueue<Event_handle*>& send_queue,
	counting_semaphore<>& send_semaphore)
{
	/*
	* 退出机制：由 application_thread 交接的 nullptr 哨兵管理，
	* FIFO 保证退出前消费完 send_queue 全部积压（关闭阶段 socket 已关，事件走 Destroy 路径，不产生新 IO）
	* 兜底：等待超时且 run_flag 已置 false（哨兵丢失等异常场景）时退出，避免永久阻塞
	*/
	for (;;)
	{
		if (send_semaphore.try_acquire_for(chrono::milliseconds(500)) == false)
		{
			if (run_flag.load(memory_order_acquire) == false)
			{
				break;
			}
			continue;
		}

		Event_handle* pEvent = nullptr;

		if (send_queue.try_dequeue(pEvent))
		{
			//退出哨兵：send_queue 积压已消费完
			if (pEvent == nullptr)
			{
				break;
			}

			//io_running == false：关闭序列已启动，socket 即将/已经关闭，事件直接销毁不再投递
			//（该检查同时保证此后的 Get_Socket_Server() 读不会与 Close_Server() 的写构成数据竞争）
			if ((pEvent->buffer.len == 0) || (pEvent->buffer.len > Event_Buffer_Size) || (server_handle.io_running.load(memory_order_acquire) == false) || (server_handle.Get_Socket_Server() == INVALID_SOCKET))
			{
				server_handle.Destroy_Event_handle(pEvent);
				continue;
			}

			shared_lock<shared_mutex> shared_lock{ server_handle.smutex };

			auto it = server_handle.client_handles.find(pEvent->socket);
			if (it == server_handle.client_handles.end())
			{
				server_handle.Destroy_Event_handle(pEvent);
				continue;
			}

			Client_Handle& client_handle = it->second;

			if (client_handle.socket_status != Socket_Connected)
			{
				server_handle.Destroy_Event_handle(pEvent);
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
				nullptr,						//实际发送的字节数（仅在同步操作成功时有效，异步操作时通常为 nullptr）
				0,							//发送标志（一般为 0）
				(LPWSAOVERLAPPED)pEvent,
				nullptr						//发送完成后的回调函数（配合事件通知模型），IOCP 不用这个，设为 nullptr
			);

			error = WSAGetLastError();
			if ((ret == SOCKET_ERROR) && (error != WSA_IO_PENDING))
			{
				//同步失败不产生完成包，若不在此处摘除，该连接会一直以 Connected 状态滞留，
				//直到 guard_thread 的活动超时兜底（问题12）
				SOCKET client_socket = client_handle.socket;//先取出句柄值：解锁后 client_handle 引用可能失效

				cout << "Error: WSASend() error code is " << error << ", client socket [" << (int)client_socket << "] will be disconnected" << endl;

				if (shared_lock.owns_lock())
				{
					shared_lock.unlock();
				}

				//投递给 work_thread 统一断开：本线程未持有接收队列，且摘除属连接容器的结构变更，
				//集中在 work_thread 的 Event_Disconnect 分支处理，与其它断开路径语义一致。
				//io_running == false 时不投递——关闭序列正在摘除全部连接，无需重复
				if (server_handle.io_running.load(memory_order_acquire))
				{
					Event_handle* pEvent_disconnect = server_handle.Construct_Event_handle(client_socket, Event_Disconnect);
					if (pEvent_disconnect)
					{
						if (PostQueuedCompletionStatus(server_handle.Get_IOCP(), 0, (ULONG_PTR)&server_handle, (LPOVERLAPPED)pEvent_disconnect) == FALSE)
						{
							cout << "Error: PostQueuedCompletionStatus() fail, error code is " << GetLastError() << endl;

							server_handle.Destroy_Event_handle(pEvent_disconnect);
						}
					}
				}

				server_handle.Destroy_Event_handle(pEvent);
				continue;
			}
		}
	}
}

void guard_thread(atomic<bool>& run_flag, Server_Handle& server_handle)
{
	auto last_scan_time = chrono::steady_clock::now();

	while (run_flag.load())
	{
		//每 Client_Active_Timeout_Scan_Interval 秒扫描一次 client_handles：把活动超时（对端半开/死连接）的
		//连接投递给 work_thread 主动断开，断开动作集中在 work_thread 的 Event_Disconnect 分支（问题8）
		auto current_time = chrono::steady_clock::now();
		if (chrono::duration_cast<chrono::seconds>(current_time - last_scan_time).count() > Client_Active_Timeout_Scan_Interval)
		{
			last_scan_time = current_time;

			//先持共享锁收集快照、解锁后再投递，避免在持锁状态下调用系统 API。
			//Is_Active_Timeout() 只读原子量，持共享锁扫描是安全的
			vector<SOCKET> timeout_sockets;
			{
				shared_lock<shared_mutex> shared_lock{ server_handle.smutex };

				//关闭序列已启动：不再扫描，此时投递已无意义
				if (server_handle.io_running.load(memory_order_acquire) == false)
				{
					break;
				}

				for (auto& pair : server_handle.client_handles)
				{
					if (pair.second.Is_Active_Timeout())
					{
						timeout_sockets.push_back(pair.first);
					}
				}
			}

			if (timeout_sockets.empty() == false)
			{
				cout << "guard thread: " << timeout_sockets.size() << " client(s) active timeout, post disconnect event" << endl;
			}

			for (SOCKET timeout_socket : timeout_sockets)
			{
				//投递前复查 io_running（须置于其它条件之前）：关闭序列已启动时不再投递，
				//避免事件落在 Close_Server 排空 IOCP 队列之后而无人销毁。
				//该复查无法与关闭序列完全串行化：极端时序下事件会滞留在 IOCP 中，
				//随 Server_Handle 析构关闭端口而丢弃，不构成内存泄漏，仅打破"对象池完全配对"的强保证
				if (server_handle.io_running.load(memory_order_acquire) == false)
				{
					break;
				}

				Event_handle* pEvent_disconnect = server_handle.Construct_Event_handle(timeout_socket, Event_Disconnect);
				if (pEvent_disconnect == nullptr)
				{
					continue;
				}

				if (PostQueuedCompletionStatus(server_handle.Get_IOCP(), 0, (ULONG_PTR)&server_handle, (LPOVERLAPPED)pEvent_disconnect) == FALSE)
				{
					cout << "Error: PostQueuedCompletionStatus() fail, error code is " << GetLastError() << endl;

					server_handle.Destroy_Event_handle(pEvent_disconnect);
				}
			}
		}

		{
			shared_lock<shared_mutex> shared_lock{ server_handle.smutex };
			//io_running 检查须置于最前：确认关闭序列未启动后，后续对 socket 的读/AcceptEx 调用
			//才不会与 Close_Server() 的 closesocket 写构成数据竞争（acquire/release 建立先后关系）
			//补投判据必须用 pending_accept_count（待处理 AcceptEx 数）而非 client_handles.size()（总条目数）：
			//后者含已建立的活跃连接，活跃连接一多条件恒假，pending AcceptEx 耗尽后再无新连接被 accept（问题14b）
			if (server_handle.io_running.load(memory_order_acquire) && (server_handle.pending_accept_count.load(memory_order_acquire) < Worker_Threads_Number) && (server_handle.client_handles.size() < Max_Clients_Number) && (server_handle.Get_Socket_Server() != INVALID_SOCKET))
			{
				shared_lock.unlock();

				unique_lock<shared_mutex> unique_lock{ server_handle.smutex };

				//每次成功投递都会自增 pending，失败即 break，故不会无限循环
				while ((server_handle.io_running.load(memory_order_acquire)) && (server_handle.pending_accept_count.load(memory_order_acquire) < Worker_Threads_Number) && (server_handle.client_handles.size() < Max_Clients_Number))
				{
					if (server_handle.Post_AcceptEx() == false)
					{
						break;
					}
				}
			}
		}

		this_thread::sleep_for(chrono::milliseconds(100));
	}
}
