#pragma once
#include<iostream>
#include<thread>
#include<string>
#include<list>
#include<mutex>
using namespace std;

class Module_Handle
{
public:
	string name;
	bool run_flag{};
	list<thread> thread_pool;
	mutex mtx;

	Module_Handle(const string& name) : name(name) {};
	Module_Handle(const Module_Handle&) = delete;
	Module_Handle(Module_Handle&& other) = delete;
	Module_Handle& operator=(const Module_Handle&) = delete;
	Module_Handle& operator=(Module_Handle&& other) = delete;
	virtual ~Module_Handle()
	{
		stop_run();
	}
	virtual void start_run()
	{
		if (run_flag)
		{
			cout << "module \"" << name << "\" is already running" << endl;
			return;
		}

		cout << "module \"" << name << "\" start to run" << endl;

		run_flag = true;
	}
	virtual void stop_run()
	{
		cout << "module \"" << name << "\" stop to run" << endl;

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
private:
	
};
