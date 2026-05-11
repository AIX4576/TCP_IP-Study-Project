#include"application.h"

void application_thread(bool& run_flag,
	Server_Handle& server_handle,
	moodycamel::ConcurrentQueue<Event_handle*>& receive_queue,
	moodycamel::ConcurrentQueue<Event_handle*>& send_queue)
{
	while (run_flag)
	{
		Event_handle* pEvent = NULL;
		Composite_Buffer_View buffer_view(server_handle);

		while (receive_queue.try_dequeue(pEvent))
		{
			buffer_view.add_event_handle(pEvent);
		}

		if (buffer_view.has_data())
		{
			vector<SOCKET> sockets = buffer_view.get_sockets();
			for (const SOCKET& socket : sockets)
			{
				string data = buffer_view.get_data(socket);
				cout << "socket [" << socket << "] received data length: " << data.length() << endl;
			}
		}

		this_thread::sleep_for(chrono::milliseconds(5));
	}
}
