#include"application_layer.h"

void application_thread(bool& run_flag,
	Server_Handle& server_handle,
	moodycamel::ConcurrentQueue<Message>& receive_queue,
	moodycamel::ConcurrentQueue<Message>& send_queue)
{
	Message message;

	while (run_flag)
	{
		while (receive_queue.try_dequeue(message))
		{
			if(message.data.size())
			{
				send_queue.enqueue(move(message));
			}
		}

		this_thread::sleep_for(chrono::milliseconds(10));
	}
}
