#include"application.h"

void application_thread(bool& run_flag,
	moodycamel::ConcurrentQueue<Event_handle*>& receive_queue,
	moodycamel::ConcurrentQueue<Event_handle*>& send_queue)
{
	while (run_flag)
	{
		Event_handle* pEvent = NULL;

		while (receive_queue.try_dequeue(pEvent))
		{
			//TODO, need to release pEvent
		}

		this_thread::sleep_for(chrono::milliseconds(5));
	}
}
