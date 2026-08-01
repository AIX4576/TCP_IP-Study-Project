#include"application.h"

void application_thread(bool& run_flag,
	Server_Handle& server_handle,
	moodycamel::ConcurrentQueue<unique_ptr<Event_handle, ConcurrentObjectPool<Event_handle>::Deleter>>& receive_queue,
	moodycamel::ConcurrentQueue<Event_handle*>& send_queue)
{
	while (run_flag)
	{
		unique_ptr<Event_handle, ConcurrentObjectPool<Event_handle>::Deleter> pEvent = nullptr;

		while (receive_queue.try_dequeue(pEvent))
		{
			auto pEvent_send = server_handle.Construct_Event_handle(
				pEvent->socket,
				Event_Send,
				pEvent->buffer.len);
			send_queue.enqueue(pEvent_send);
		}

		this_thread::sleep_for(chrono::milliseconds(5));
	}
}
