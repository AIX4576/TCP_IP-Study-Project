#include"custom_server_interface.h"
#include"custom_server_thread.h"

Custom_Server_Handle::Custom_Server_Handle(const string& name) : Module_Handle(name)
{
}
Custom_Server_Handle::~Custom_Server_Handle()
{
	stop_run();
}
void Custom_Server_Handle::start_run_impl()
{
	cout << "module \"" << name << "\" start_run_impl() override" << endl;

	thread_pool.emplace_back(main_thread, ref(*this));
}
void Custom_Server_Handle::stop_run_impl()
{
	cout << "module \"" << name << "\" stop_run_impl() override" << endl;

	// 收敛式回收（同基类规则）：持锁仅换出线程快照，解锁后 join，循环直至 thread_pool 清空。
	// main_thread 创建业务线程时持有 thread_pool_mtx，若此处持锁 join 会与之死锁。
	for (;;)
	{
		list<thread> snapshot;
		{
			unique_lock<mutex> lock(thread_pool_mtx);
			snapshot.swap(thread_pool);
		}
		if (snapshot.empty())
		{
			break;
		}
		for (auto& item : snapshot)
		{
			if (item.joinable())
			{
				item.join();
			}
		}
	}
}

Module_Handle* Initialize()
{
	return new Custom_Server_Handle("custom_server");
}

