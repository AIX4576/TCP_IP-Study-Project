#include "main.h"

//定义此宏，让编译器避免引入Windows早期的库，避免引起冲突
#define WIN32_LEAN_AND_MEAN
#include<Windows.h>
#include<WinSock2.h>

//显式告诉编译器（链接器）程序需要"ws2_32.lib"这个静态库，否则链接时找不到WSAStartup()和WSACleanup()这两个符号
//或者在附加依赖项里面添加
#pragma comment(lib,"ws2_32.lib")

int main()
{
	/*
	在Windows下使用Windows Sockets API，必须先调用WSAStartup()初始化Winsock服务，包括加载相关的运行库等操作
	程序退出前需要调用WSACleanup()函数
	*/
	WORD version = MAKEWORD(2, 2);//指定Socket版本
	WSADATA data;
	WSAStartup(version, &data);

	WSACleanup();

	return 0;
}

