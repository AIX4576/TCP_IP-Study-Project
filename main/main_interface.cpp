#include"main_interface.h"

#include<vector>
using namespace std;

Main_Handle::Main_Handle() : Module_Handle("main", 1)
{
	module_handles.reserve(16);
}

Main_Handle::~Main_Handle()
{
	stop_run(); // 与派生模块析构一致：显式回收（stop_run 幂等，可重复调用）
}

Main_Handle& Main_Handle::instance()
{
	static Main_Handle instance;	// 函数内静态对象：程序退出时析构，迟于 main() 返回
	return instance;
}

void Main_Handle::add_module(shared_ptr<Module_Handle> p_module)
{
	if (!p_module)
	{
		cout << "module is null, ignore" << endl;
		return;
	}
	unique_lock<shared_mutex> lock(mtx);
	module_handles[p_module->name] = move(p_module);
}

void Main_Handle::remove_module(const string& name)
{
	unique_lock<shared_mutex> lock(mtx);
	module_handles.erase(name); // 移除后 shared_ptr 释放，模块析构自动 stop_run
}

shared_ptr<Module_Handle> Main_Handle::get_module(const string& name) const
{
	shared_lock<shared_mutex> lock(mtx);
	auto it = module_handles.find(name);
	if (it != module_handles.end())
	{
		return it->second;
	}
	return nullptr;
}

void Main_Handle::start_run_impl()
{
	cout << "module \"" << name << "\" start_run_impl() override" << endl;

	// 快照后解锁再逐个启动：持锁调用子模块 start_run 会与其他线程的 add/remove/get 互斥
	vector<shared_ptr<Module_Handle>> snapshot;
	{
		unique_lock<shared_mutex> lock(mtx);
		snapshot.reserve(module_handles.size());
		for (auto& item : module_handles)
		{
			snapshot.push_back(item.second);
		}
	}
	for (auto& p_module : snapshot)
	{
		p_module->start_run();
	}
}

void Main_Handle::stop_run_impl()
{
	cout << "module \"" << name << "\" stop_run_impl() override" << endl;

	// 快照后解锁再逐个停止：stop_run 会 join 子模块业务线程，
	// 若持锁调用，子模块线程回调 Main_Handle 方法请求本锁时将死锁
	vector<shared_ptr<Module_Handle>> snapshot;
	{
		unique_lock<shared_mutex> lock(mtx);
		snapshot.reserve(module_handles.size());
		for (auto& item : module_handles)
		{
			snapshot.push_back(item.second);
		}
	}
	for (auto& p_module : snapshot)
	{
		p_module->stop_run();
	}
}
