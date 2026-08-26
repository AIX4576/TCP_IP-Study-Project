#pragma once

#include<iostream>
#include<thread>
#include<list>
using namespace std;

#include"ModuleHandle.h"

class Custom_Server_Handle : public Module_Handle
{
public:
	Custom_Server_Handle(const string& name);
	Custom_Server_Handle(const Custom_Server_Handle&) = delete;
	Custom_Server_Handle(Custom_Server_Handle&& other) = delete;
	Custom_Server_Handle& operator=(const Custom_Server_Handle&) = delete;
	Custom_Server_Handle& operator=(Custom_Server_Handle&& other) = delete;
	~Custom_Server_Handle() override;
protected:
	void start_run_impl() override;
	void stop_run_impl() override;
};

extern "C"
{
	__declspec(dllexport) Module_Handle* Initialize();
}


