#pragma once
#include <atomic>
#include <thread>
#include <memory>
#include <type_traits>
#include <immintrin.h>
#include <cstdint>

#include "boost/pool/object_pool.hpp"
#include "concurrentqueue.h"

constexpr size_t Default_Size = 1024;

template <typename T>
class ConcurrentObjectPool
{
public:
	explicit ConcurrentObjectPool(size_t size = Default_Size)
	{
		object_pool.set_next_size(size);
		for (size_t i = 0; i < size; i++)
		{
			T* p = object_pool.malloc();
			if (p)
			{
				bool result = free_object_queue.enqueue(p);
				if (!result)
				{
					object_pool.free(p);
				}
			}
		}
	}

	ConcurrentObjectPool(const ConcurrentObjectPool&) = delete;
	ConcurrentObjectPool(ConcurrentObjectPool&&) = delete;
	ConcurrentObjectPool& operator=(const ConcurrentObjectPool&) = delete;
	ConcurrentObjectPool& operator=(ConcurrentObjectPool&&) = delete;

	~ConcurrentObjectPool() = default;
	// boost::object_pool 析构时自动释放所有内存

	// ---- 异常安全版本 ----
	template <typename... Args>
	T* Construct(Args &&...args)
	{
		T* p = acquire_object();
		if (!p)
		{
			return nullptr;
		}

		// 用 scope guard 保证异常时归还对象
		try
		{
			::new (p) T(std::forward<Args>(args)...);
		}
		catch (...)
		{
			release_object(p);
			throw;
		}

		return p;
	}

	void Destroy(T* p) noexcept
	{
		if (!p)
		{
			return;
		}

		p->~T();

		release_object(p);
	}

	struct Deleter
	{
		ConcurrentObjectPool* pool{};
		void operator()(T* p) const noexcept
		{
			if (pool)
			{
				pool->Destroy(p);
			}
		}
	};

	Deleter Get_Deleter()
	{
		return Deleter{ this };
	}

	template <typename... Args>
	std::unique_ptr<T, Deleter> MakeUnique(Args &&...args)
	{
		return std::unique_ptr<T, Deleter>(Construct(std::forward<Args>(args)...), Deleter{ this });
	}

	// ---- 查询接口 ----
	size_t capacity() const noexcept { return capacity_; }
	// ConcurrentQueue 不直接提供 size()，可考虑用计数器追踪

private:
	// 从空闲队列获取对象，失败时加锁扩容
	T* acquire_object()
	{
		T* p = nullptr;
		if (free_object_queue.try_dequeue(p))
		{
			++capacity_;

			return p;
		}

		// 加锁从 boost_pool 分配新对象
		lock_test_and_set();

		p = object_pool.malloc();
		if (p)
		{
			++capacity_;
		}

		lock_clear();

		return p;
	}

	// 归还对象到空闲队列，队列满时回收到 boost_pool
	void release_object(T* p) noexcept
	{
		if (!p)
		{
			return;
		}
		if (free_object_queue.enqueue(p))
		{
			--capacity_;

			return;
		}

		lock_test_and_set();

		object_pool.free(p);
		--capacity_;

		lock_clear();
	}

	// 自旋锁 + 指数退避
	void lock_test_and_set()
	{
		int backoff = 1;
		while (lock.test_and_set(std::memory_order_acquire))
		{
#if defined(__x86_64__) || defined(__i386__) || defined(_M_IX86) || defined(_M_X64)
			// x86/x64: PAUSE 指令 —— 提示处理器这是自旋等待，降低功耗并提升性能
			for (int i = 0; i < backoff; ++i)
			{
				_mm_pause();
			}
#elif defined(__arm__) || defined(__aarch64__) || defined(_M_ARM64)
			// ARM: YIELD 指令 —— 相当于 x86 PAUSE 的语义
			for (int i = 0; i < backoff; ++i)
			{
				__asm__ __volatile__("yield" ::: "memory");
			}
#else
			// 兜底：退化为系统调用级别的让权
			for (int i = 0; i < backoff; ++i)
			{
				std::this_thread::yield();
			}
#endif
			backoff = std::min(backoff * 2, 256);
		}
	}

	void lock_clear() noexcept
	{
		lock.clear(std::memory_order_release);
	}

	std::atomic_flag lock{};
	boost::object_pool<T> object_pool;
	moodycamel::ConcurrentQueue<T*> free_object_queue;
	std::atomic<size_t> capacity_{}; // 追踪当前总分配数
};
