#include"application_layer.h"

void application_thread(bool& run_flag,
	Server_Handle& server_handle,
	moodycamel::ConcurrentQueue<Event_handle*>& receive_queue,
	moodycamel::ConcurrentQueue<Event_handle*>& send_queue)
{
	while (run_flag)
	{
		Event_handle* pEvent = NULL;

		while (receive_queue.try_dequeue(pEvent))
		{
			if (pEvent->buffer.len)
			{
				send_queue.enqueue(pEvent);
			}
			else
			{
				//长度为0，表示pEvent->socket连接关闭，销毁该事件对象
				server_handle.Destory_Event_handle(pEvent);
			}
		}

		this_thread::sleep_for(chrono::milliseconds(5));
	}
}
