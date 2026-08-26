#include"custom_server_thread.h"
#include"iocp_server.h"
#include"application.h"

void main_thread(Custom_Server_Handle& handle)
{
	WORD version = MAKEWORD(2, 2);
	WSADATA data;
	int result = 0;
	result = WSAStartup(version, &data);
	if (result != NO_ERROR)
	{
		cout << "WSAStartup fail" << endl;

		return;
	}

	Server_Handle server_handle;
	if (server_handle.Get_Initialize_Flag() == FALSE)
	{
		cout << "Error: server handle initialize fail" << endl;

		return;
	}

	//创建无锁并发队列以及对应的计数信号量
	vector<moodycamel::ConcurrentQueue<unique_ptr<Event_handle, ConcurrentObjectPool<Event_handle>::Deleter>>> receive_queues;
	vector<moodycamel::ConcurrentQueue<Event_handle*>> send_queues;
	receive_queues.reserve(Application_Threads_Number);
	send_queues.reserve(Application_Threads_Number);

	vector<unique_ptr<counting_semaphore<>>> receive_semaphores;
	vector<unique_ptr<counting_semaphore<>>> send_semaphores;
	receive_semaphores.reserve(Application_Threads_Number);
	send_semaphores.reserve(Application_Threads_Number);

	for (size_t i = 0; i < Application_Threads_Number; i++)
	{
		receive_queues.emplace_back(2048);
		send_queues.emplace_back(2048);
		receive_semaphores.emplace_back(make_unique<counting_semaphore<>>(0));
		send_semaphores.emplace_back(make_unique<counting_semaphore<>>(0));
	}

	cout << "work threads number is " << Worker_Threads_Number << endl;
	cout << "send threads number is " << Application_Threads_Number << endl;
	cout << "application threads number is " << Application_Threads_Number << endl;

	{
		unique_lock<mutex> lock(handle.thread_pool_mtx);

		//添加线程
		//iocp工作线程
		for (size_t i = 0; i < Worker_Threads_Number; i++)
		{
			handle.thread_pool.emplace_back(work_thread, ref(handle.run_flag), ref(server_handle), ref(receive_queues), ref(receive_semaphores));
		}

		//iocp发送线程
		for (size_t i = 0; i < Application_Threads_Number; i++)
		{
			handle.thread_pool.emplace_back(send_thread, ref(handle.run_flag), ref(server_handle), ref(send_queues.at(i)), ref(*send_semaphores.at(i)));
		}

		//iocp守护线程
		handle.thread_pool.emplace_back(guard_thread, ref(handle.run_flag), ref(server_handle));

		//应用线程
		for (size_t i = 0; i < Application_Threads_Number; i++)
		{
			handle.thread_pool.emplace_back(application_thread, ref(handle.run_flag), ref(server_handle), ref(receive_queues.at(i)), ref(*receive_semaphores.at(i)), ref(send_queues.at(i)), ref(*send_semaphores.at(i)));
		}
	}

	while (handle.run_flag.load())
	{
		this_thread::sleep_for(chrono::milliseconds(200));
	}

	server_handle.Close_Server();

	WSACleanup();
}
