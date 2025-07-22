#include"iocp.h"
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

	server_handle.Close_Socket();
	this_thread::sleep_for(chrono::milliseconds(500));
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
