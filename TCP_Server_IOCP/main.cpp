#include<iostream>
#include<thread>
#include<chrono>

using namespace std;

#include"frame.h"

#define WIN32_LEAN_AND_MEAN
#include<WS2tcpip.h>

#pragma comment(lib,"ws2_32.lib")

int main()
{
	WORD version = MAKEWORD(2, 2);
	WSADATA data;
	WSAStartup(version, &data);
	
	//创建一个支持重叠IO的TCP套接字, WSASocket()函数是Windows系统专用的,socket()函数是跨平台的
	SOCKET socket_server = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (socket_server == INVALID_SOCKET)
	{
		cout << "Error: create server socket fail" << endl;
		WSACleanup();

		return -1;
	}

	closesocket(socket_server);
	WSACleanup();

	return 0;
}



