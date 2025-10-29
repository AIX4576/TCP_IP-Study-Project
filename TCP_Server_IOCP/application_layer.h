#pragma once
#include<iostream>
using namespace std;

#include"iocp_server.h"

void application_thread(bool& run_flag,
	Server_Handle& server_handle,
	moodycamel::ConcurrentQueue<Event_handle*>& receive_queue,
	moodycamel::ConcurrentQueue<Event_handle*>& send_queue);
