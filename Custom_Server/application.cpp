#include"application.h"

void application_thread(
	atomic<bool>& run_flag,
	Server_Handle& server_handle,
	moodycamel::ConcurrentQueue<unique_ptr<Event_handle, ConcurrentObjectPool<Event_handle>::Deleter>>& receive_queue,
	counting_semaphore<>& receive_semaphore,
	moodycamel::ConcurrentQueue<Event_handle*>& send_queue,
	counting_semaphore<>& send_semaphore)
{
	while (run_flag.load())
	{
		if (receive_semaphore.try_acquire_for(chrono::milliseconds(500)) == false)
		{
			continue;
		}	

		unique_ptr<Event_handle, ConcurrentObjectPool<Event_handle>::Deleter> pEvent = nullptr;

		if (receive_queue.try_dequeue(pEvent))
		{
			auto pEvent_send = server_handle.Construct_Event_handle(
				pEvent->socket,
				Event_Send,
				pEvent->buffer.len);
			send_queue.enqueue(pEvent_send);
			send_semaphore.release();
		}
	}
}
