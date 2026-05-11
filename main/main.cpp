#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDICAPMASKS
#define NOSYSMETRICS
#define NOMCX
#define NOIME
#define NOSOUND
#define NOCOMM
#define NOKANJI
#include<windows.h>

#include<iostream>
#include<unordered_map>
using namespace std;

#include"ModuleHandle.h"
#include"custom_server_interface.h"

int main()
{
	unordered_map<string, unique_ptr<Module_Handle>> module_handles;
	module_handles.reserve(16);

	Module_Handle* pCustom_Server_Handle = Initialize();
	if (pCustom_Server_Handle)
	{
		module_handles.emplace(pCustom_Server_Handle->name, unique_ptr<Module_Handle>(pCustom_Server_Handle));
		pCustom_Server_Handle->start_run();
	}

	string command;
	while (true)
	{
		cin >> command;
		if (command == "exit")
		{
			break;
		}
	}

	auto it = module_handles.begin();
	while (it != module_handles.end())
	{
		it->second->stop_run();
		it++;
	}

	return 0;
}

