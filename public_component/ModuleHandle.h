#pragma once
#include<iostream>
#include<thread>
#include<string>
#include<list>
#include<mutex>
#include<semaphore>
#include<functional>
#include<unordered_map>
using namespace std;

struct Event_Data
{
	uint64_t event_id;
	string data;
};

using User_Callback = function<void(shared_ptr<Event_Data>, shared_ptr<void>)>;

struct Callback_Entry
{
	User_Callback callback;
	shared_ptr<void> user_data;
	shared_ptr<Event_Data> event_data;
};

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

		unique_lock<mutex> lock(mtx);

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
	void register_callback(uint64_t event_id, User_Callback callback, shared_ptr<void> user_data)
	{
		unique_lock<mutex> lock(callback_mtx);

		// 创建回调条目
		Callback_Entry entry{ move(callback), move(user_data) };

		// 将条目添加到对应event_id的列表中
		callback_map[event_id].push_back(entry);
	}
private:
    // 用于存储event_id及其对应的回调函数和用户数据的容器
    unordered_map<uint64_t, list<Callback_Entry>> callback_map;
    mutex callback_mtx;  // 专门用于保护callback_map的互斥锁

};