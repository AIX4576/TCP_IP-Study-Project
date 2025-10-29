#pragma once
#include<iostream>
#include<atomic>

#include"boost/pool/object_pool.hpp"
#include"concurrentqueue.h"

constexpr size_t Default_Size = 1024;

template<typename T>
class LockFreeObjectPool
{
public:
	LockFreeObjectPool(size_t size = Default_Size)
	{
		object_pool.set_next_size(size);
		for (int i = 0; i < size; i++)
		{
			T* p = object_pool.malloc();
			if (p)
			{
				bool result = free_object_queue.enqueue(p);
				if (result == false)
				{
					object_pool.free(p);
				}
			}
		}
	}
	LockFreeObjectPool(const LockFreeObjectPool&) = delete;
	LockFreeObjectPool(LockFreeObjectPool&&) = delete;
	LockFreeObjectPool& operator=(const LockFreeObjectPool&) = delete;
	LockFreeObjectPool& operator=(LockFreeObjectPool&&) = delete;
	~LockFreeObjectPool()
	{
		// object_pool 析构时自动释放所有内存
	}

	template<typename... Args>
	T* Construct(Args&&... args)
	{
		T* p = NULL;

		//先从空闲队列中取对象
		bool result = free_object_queue.try_dequeue(p);

		//如果队列中没有空闲对象，则从对象池中分配新对象
		if (result == false)
		{
			lock.test_and_set(std::memory_order_acquire);
			p = object_pool.malloc();//此时可能会触发扩容
			lock.clear(std::memory_order_release);
		}

		//分配对象成功后，调用构造函数
		if (p)
		{
			new(p) T(std::forward<Args>(args)...);
		}

		return p;
	}
	void Destory(T* p)
	{
		if (p == NULL)
		{
			return;
		}

		p->~T();

		bool result = free_object_queue.enqueue(p);
		if (result == false)
		{
			lock.test_and_set(std::memory_order_acquire);
			object_pool.free(p);
			lock.clear(std::memory_order_release);
		}
	}


private:
	std::atomic_flag lock{};
	boost::object_pool<T> object_pool;
	moodycamel::ConcurrentQueue<T*> free_object_queue;
};

