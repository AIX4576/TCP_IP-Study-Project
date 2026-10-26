#include"application.h"

void application_thread(
	atomic<bool>& run_flag,
	Server_Handle& server_handle,
	moodycamel::ConcurrentQueue<unique_ptr<Event_handle, ConcurrentObjectPool<Event_handle>::Deleter>>& receive_queue,
	counting_semaphore<>& receive_semaphore,
	moodycamel::ConcurrentQueue<Event_handle*>& send_queue,
	counting_semaphore<>& send_semaphore)
{
	/*
	* 退出机制：由 main_thread 在 IOCP 排空（事件流断源）后投递的 nullptr 哨兵管理，
	* 不依赖 run_flag 超时退出——FIFO 保证退出前消费完 receive_queue 全部积压，
	* 退出前向 send_queue 交接哨兵，令 send_thread 同样排空后退出，事件所有权链条闭合、无残留泄漏
	* 兜底：等待超时且 run_flag 已置 false（哨兵丢失等异常场景）时同样交接哨兵后退出，避免永久阻塞
	*/
	for (;;)
	{
		if (receive_semaphore.try_acquire_for(chrono::milliseconds(500)) == false)
		{
			if (run_flag.load(memory_order_acquire) == false)
			{
				send_queue.enqueue(nullptr);// 兜底退出同样交接哨兵，send_thread 不会死等
				send_semaphore.release();
				break;
			}
			continue;
		}

		unique_ptr<Event_handle, ConcurrentObjectPool<Event_handle>::Deleter> pEvent = nullptr;

		if (receive_queue.try_dequeue(pEvent))
		{
			//退出哨兵：receive_queue 积压已消费完，向 send_queue 交接哨兵后退出
			if (pEvent == nullptr)
			{
				send_queue.enqueue(nullptr);
				send_semaphore.release();
				break;
			}

			// 占位业务逻辑：仅演示"收到数据 -> 构造发送事件"的数据流回环。
			// 注意：当前并没有把收到的数据（pEvent->data，长度 pEvent->buffer.len）拷贝到 pEvent_send->data，
			// 发送出去的是对象池中残留的旧内存。正式业务接入时必须在此补充数据拷贝/加工步骤。
			auto pEvent_send = server_handle.Construct_Event_handle(
				pEvent->socket,
				Event_Send,
				pEvent->buffer.len);
			send_queue.enqueue(pEvent_send);
			send_semaphore.release();
		}
	}
}
