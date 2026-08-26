#pragma once
#include<iostream>
#include<thread>
#include<string>
#include<list>
#include<mutex>
#include<shared_mutex>
#include<atomic>
#include<semaphore>
#include<functional>
#include<memory>
#include<cstdint>
#include<unordered_map>
using namespace std;

#include"concurrentqueue.h"

constexpr size_t Default_Callback_Thread_Number = 4;
constexpr size_t Default_Task_Queue_Max_Size = 1024;

struct Event_Data
{
	uint64_t event_id;
	string data;
};

using User_Callback = function<void(shared_ptr<Event_Data>, shared_ptr<void>)>;

struct Callback_Entry
{
	uint64_t callback_id;
	User_Callback callback;
	shared_ptr<void> user_data;
};

// 回调任务：entry 为回调条目快照（共享所有权，零拷贝），event_data 为事件数据
struct Callback_Output
{
	shared_ptr<Callback_Entry> entry;
	shared_ptr<Event_Data> event_data;
};

class Module_Handle
{
public:
	string name;
	atomic<bool> run_flag{};
	list<thread> thread_pool;
	mutex thread_pool_mtx;

	Module_Handle(const string& name, size_t callback_thread_number = Default_Callback_Thread_Number,
		size_t task_queue_max_size = Default_Task_Queue_Max_Size)
		: name(name), callback_thread_number(callback_thread_number == 0 ? 1 : callback_thread_number), m_taskQueueMaxSize(task_queue_max_size)
	{
		callback_map.reserve(64);

		for (size_t i = 0; i < callback_thread_number; i++)
		{
			callback_thread_pool.emplace_back([this]()
				{
					while (m_shutdown.load() == false)
					{
						// 信号量精确唤醒：每个入队任务对应一次 release；500ms 超时用于及时响应 m_shutdown
						if (task_semaphore.try_acquire_for(chrono::milliseconds(500)) == false)
						{
							continue;
						}
						Callback_Output task;
						if (task_queue.try_dequeue(task))
						{
							if (task.entry)
							{
								if(task.entry->callback)
								{
									try
									{
										task.entry->callback(task.event_data, task.entry->user_data);
									}
									catch (...)
									{
										cout << "module \"" << this->name << "\" callback exception" << endl;
									}
								}
							}
						}
					}
				});
		}
	}
	Module_Handle(const Module_Handle&) = delete;
	Module_Handle(Module_Handle&& other) = delete;
	Module_Handle& operator=(const Module_Handle&) = delete;
	Module_Handle& operator=(Module_Handle&& other) = delete;
	virtual ~Module_Handle()
	{
		stop_run(); // 停止业务线程并回收回调线程（stop_run 幂等，可重复调用）
	}
	// NVI（模板方法）模式：公共接口非虚，派生类只能覆写受保护的虚钩子，
	// 基类保证 run_flag 置位与回调线程回收必然执行
	/**
	 * @brief 启动模块。
	 * @note 回调函数内禁止调用 start_run/stop_run（会与线程回收互锁）。
	 */
	void start_run()
	{
		if (run_flag.load())
		{
			cout << "module \"" << name << "\" is already running" << endl;
			return;
		}
		if (m_shutdown.load())
		{
			cout << "module \"" << name << "\" callback threads already stopped, cannot restart" << endl;
		}

		cout << "module \"" << name << "\" start to run" << endl;

		run_flag = true;
		start_run_impl();	// 虚钩子：派生类启动业务线程
	}
	/**
	 * @brief 停止模块，回收业务线程与回调线程。
	 * @note 回调线程会先处理完已入队任务（drain）再退出，不会被静默丢弃。
	 * @note 回调函数内禁止调用 start_run/stop_run（会与线程回收互锁）。
	 */
	void stop_run()
	{
		cout << "module \"" << name << "\" stop to run" << endl;

		run_flag = false;
		stop_run_impl();	// 虚钩子：派生类回收业务线程

		// 基类兜底回收回调线程，派生类覆写 stop_run_impl 时无法绕过
		m_shutdown = true;
		for (auto& item : callback_thread_pool)
		{
			if (item.joinable())
			{
				item.join();
			}
		}
		callback_thread_pool.clear();
	}
protected:
	virtual void start_run_impl() // 派生类覆写点：启动业务线程
	{
		cout << "module \"" << name << "\" start_run_impl() base" << endl;
	}
	virtual void stop_run_impl() // 派生类覆写点：回收业务线程
	{
		cout << "module \"" << name << "\" stop_run_impl() base" << endl;

		// 收敛式回收：持锁仅换出线程快照，解锁后 join，循环直至 thread_pool 清空。
		// 禁止持锁 join：被 join 的线程若仍在创建其他线程（创建时持有同一把锁），
		// 持锁等待其结束会形成死锁（join 等线程结束，线程等锁推进）。
		// 派生类重写 stop_run_impl 回收线程时须遵循此规则。
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
public:
	/**
	 * @brief 注册事件回调。
	 * @param event_id 事件 id。
	 * @param callback 回调函数。
	 * @param user_data 用户数据。
	 * @return uint64_t 回调 id，用于 unregister_callback 精确反注册（0 为无效 id）。
	 * @note 写时复制（COW）：注册/反注册重建回调列表快照，不影响已提交的任务。
	 */
	uint64_t register_callback(uint64_t event_id, User_Callback callback, shared_ptr<void> user_data)
	{
		unique_lock<shared_mutex> lock(callback_map_mtx);

		// 创建回调条目并分配唯一 id
		uint64_t callback_id = ++m_callbackIdCounter;
		auto p_entry = make_shared<Callback_Entry>(callback_id, move(callback), move(user_data));

		// COW：复制现有列表后追加，其他读方（submit_task 快照）不受影响
		auto it = callback_map.find(event_id);
		shared_ptr<list<shared_ptr<Callback_Entry>>> new_list;
		if (it == callback_map.end())
		{
			new_list = make_shared<list<shared_ptr<Callback_Entry>>>();
			callback_map.emplace(event_id, new_list);
		}
		else
		{
			new_list = make_shared<list<shared_ptr<Callback_Entry>>>(*it->second);
			it->second = new_list;
		}
		new_list->emplace_back(move(p_entry));

		return callback_id;
	}
	/**
	 * @brief 按回调 id 反注册事件回调。
	 * @param event_id 事件 id。
	 * @param callback_id register_callback 返回的回调 id。
	 * @note 提交时快照语义：不影响已入队的任务，只影响后续 submit_task。
	 */
	void unregister_callback(uint64_t event_id, uint64_t callback_id)
	{
		unique_lock<shared_mutex> lock(callback_map_mtx);

		auto it = callback_map.find(event_id);
		if (it != callback_map.end())
		{
			// COW：复制现有列表后删除目标条目
			auto new_list = make_shared<list<shared_ptr<Callback_Entry>>>(*it->second);
			new_list->remove_if([callback_id](const shared_ptr<Callback_Entry>& entry) {
				return entry->callback_id == callback_id;
				});
			if (new_list->empty())
			{
				callback_map.erase(it);
			}
			else
			{
				it->second = new_list;
			}
		}
	}
	/**
	 * @brief 提交事件任务，触发对应 event_id 的所有回调（异步执行）。
	 * @param event_id 事件 id。
	 * @param event_data 事件数据，所有回调共享同一份数据（拷贝 shared_ptr）。
	 * @return bool 全部回调入队成功返回 true；任务队列满导致拒绝返回 false。
	 * @note 提交时快照回调列表（O(1)），之后的反注册不影响本次提交的任务。
	 * @note 队列上限为近似值（size_approx），并发提交时可能略有超出。
	 */
	bool submit_task(uint64_t event_id, shared_ptr<Event_Data> event_data)
	{
		// 查找对应的回调函数列表（O(1) 快照 shared_ptr，不复制列表）
		shared_ptr<list<shared_ptr<Callback_Entry>>> callback_list;
		{
			shared_lock<shared_mutex> lock(callback_map_mtx);
			auto it = callback_map.find(event_id);
			if (it != callback_map.end())
			{
				callback_list = it->second;
			}
		}
		if (!callback_list)
		{
			return true; // 无回调，视为提交成功
		}
		// 将回调任务添加到无锁队列；entry 与 event_data 均为 shared_ptr 拷贝，零 function 拷贝
		for (auto& p_entry : *callback_list)
		{
			if (task_queue.size_approx() >= m_taskQueueMaxSize)
			{
				cout << "module \"" << name << "\" task queue is full, drop callback" << endl;
				return false;
			}
			Callback_Output task{ p_entry, event_data };
			task_queue.enqueue(move(task));
			task_semaphore.release(); // 精确唤醒一个回调线程
		}
		return true;
	}
private:
	size_t callback_thread_number;
	size_t m_taskQueueMaxSize;
	list<thread> callback_thread_pool;
	atomic<bool> m_shutdown{ false };	// 回调线程停止标志（stop_run 置位，置位后不可恢复）
	uint64_t m_callbackIdCounter{ 0 };	// 回调注册 id 计数器（0 保留为无效 id）

	// 用于存储event_id及其对应的回调函数和用户数据的容器（COW 快照：读路径 O(1) 拷贝指针）
	unordered_map<uint64_t, shared_ptr<list<shared_ptr<Callback_Entry>>>> callback_map;
	shared_mutex callback_map_mtx;		// 专门用于保护callback_map的互斥锁

	// 回调任务无锁队列 + 计数信号量（每个入队任务对应一次 release，精确唤醒）
	moodycamel::ConcurrentQueue<Callback_Output> task_queue;
	counting_semaphore<> task_semaphore{ 0 };
};
