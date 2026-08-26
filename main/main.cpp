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
using namespace std;

#include"ModuleHandle.h"
#include"main_interface.h"
#include"custom_server_interface.h"

int main()
{
	auto& main_handle = Main_Handle::instance();

	Module_Handle* pCustom_Server_Handle = Initialize();
	if (pCustom_Server_Handle)
	{
		main_handle.add_module(shared_ptr<Module_Handle>(pCustom_Server_Handle));
	}
	main_handle.start_run();	// 级联启动自身与全部模块

	string command;
	while (true)
	{
		cin >> command;
		if (command == "exit")
		{
			break;
		}
	}

	main_handle.stop_run();		// 级联停止全部模块与自身

	return 0;
}

