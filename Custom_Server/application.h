#pragma once
#include<iostream>
using namespace std;

#include"iocp_server.h"

void application_thread(
	atomic<bool>& run_flag,
	Server_Handle &server_handle,
	moodycamel::ConcurrentQueue<unique_ptr<Event_handle, ConcurrentObjectPool<Event_handle>::Deleter>>& receive_queue,
	counting_semaphore<> &receive_semaphore,
	moodycamel::ConcurrentQueue<Event_handle*>& send_queue,
	counting_semaphore<> &send_semaphore);
