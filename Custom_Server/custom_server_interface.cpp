#include"custom_server_interface.h"
#include"custom_server_thread.h"
#include"iocp_server.h"
#include"application.h"


Custom_Server_Handle::Custom_Server_Handle(const string& name) : Module_Handle(name)
{
}
Custom_Server_Handle::~Custom_Server_Handle()
{
	stop_run();
}
void Custom_Server_Handle::start_run()
{
	if (run_flag)
	{
		cout << "module \"" << name << "\" is already running" << endl;
		return;
	}

	cout << "module \"" << name << "\" start to run override" << endl;

	run_flag = true;

	thread_pool.emplace_back(main_thread, ref(*this));
}
void Custom_Server_Handle::stop_run()
{
	cout << "module \"" << name << "\" stop to run override" << endl;

	lock_guard<mutex> lock(mtx);

	run_flag = false;
	for (auto& item : thread_pool)
	{
		if (item.joinable())
		{
			item.join();
		}
	}
	thread_pool.clear();
}

Module_Handle* Initialize()
{
	return new Custom_Server_Handle("custom_server");
}

