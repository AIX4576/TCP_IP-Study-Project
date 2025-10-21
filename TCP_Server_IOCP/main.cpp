#include"iocp_server.h"
#include"application_layer.h"

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

	//创建无锁并发队列
	vector<moodycamel::ConcurrentQueue<Message>> receive_queues;
	vector<moodycamel::ConcurrentQueue<Message>> send_queues;
	receive_queues.reserve(Application_Threads_Number);
	send_queues.reserve(Application_Threads_Number);
	for (uint32_t i = 0; i < Application_Threads_Number; i++)
	{
		receive_queues.emplace_back();
		send_queues.emplace_back();
	}
	cout << "application threads number is " << Application_Threads_Number << endl;

	//创建线程池
	bool run_flag = TRUE;
	list<thread> thread_pool;

	//iocp工作线程
	for (uint32_t i = 0; i < Worker_Threads_Number; i++)
	{
		thread_pool.emplace_back(work_thread, ref(run_flag), ref(server_handle), ref(receive_queues));
	}

	//iocp发送线程
	for (uint32_t i = 0; i < Send_Threads_Number; i++)
	{
		thread_pool.emplace_back(send_thread, ref(run_flag), ref(server_handle), ref(send_queues));
	}

	//iocp清理线程
	thread_pool.emplace_back(clean_thread, ref(run_flag), ref(server_handle));

	//应用线程
	for (uint32_t i = 0; i < Application_Threads_Number; i++)
	{
		thread_pool.emplace_back(application_thread, ref(run_flag), ref(server_handle), ref(receive_queues.at(i)), ref(send_queues.at(i)));
	}

	while (TRUE)
	{
		char c;
		cin >> c;
		if (c == 'q')
		{
			break;
		}
		else if (c == 'i')
		{
			cout << "=====================================================================" << endl;
			cout << "total clients number is " << server_handle.client_handles.size() << endl;
			cout << "connected clients number is " << server_handle.client_handles.size() - Worker_Threads_Number << endl;
			cout << "=====================================================================" << endl;
		}
	}

	server_handle.Close_Socket();
	this_thread::sleep_for(chrono::seconds(1));
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
