#include<iostream>
#include<thread>
#include<chrono>
using namespace std;

#define WIN32_LEAN_AND_MEAN //让编译器避免引入Windows早期的库，避免引起冲突。必须先定义这个宏再包含 WS2tcpip.h 
#include<WS2tcpip.h>
//显式告诉编译器（链接器）程序需要"ws2_32.lib"这个静态库，否则链接时找不到WSAStartup()和WSACleanup()这两个符号定义
//或者在附加依赖项里面添加
#pragma comment(lib,"ws2_32.lib")

#include"frame.h"

int main()
{
	WORD version = MAKEWORD(2, 2);//指定Socket版本
	WSADATA data;
	int result = 0;
	result = WSAStartup(version, &data);
	if (result != NO_ERROR)
	{
		cout << "WSAStartup fail" << endl;

		return -1;
	}

	//创建一个ipv4，数据流，tcp协议的socket套接字
	SOCKET socket_client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (socket_client == INVALID_SOCKET)
	{
		cout << "Error: create client socket fail" << endl;
		WSACleanup();

		return -1;
	}

	// connect 连接服务器
	result = SOCKET_ERROR;
	sockaddr_in address_server{};

	address_server.sin_family = AF_INET;
	address_server.sin_port = htons(8080);
	inet_pton(AF_INET, "192.168.137.1", &address_server.sin_addr);

	while (result == SOCKET_ERROR)
	{
		result = connect(socket_client, (sockaddr*)&address_server, sizeof(address_server));
		if (result == SOCKET_ERROR)
		{
			cout << "Error: connect to server fail" << endl;
		}
		else
		{
			cout << "Connect succeed" << endl;
			break;
		}

		this_thread::sleep_for(chrono::seconds(5));
	}

	//send 向服务器发送数据，对于客户端，与服务器建立连接后使用 send() 发送数据时填入的是自己的socket
	//recv 接受服务器发来的数据，对于客户端，与服务器建立连接后使用 recv() 接收数据时填入的是自己的socket
	int size;
	char command[32];

	while (1)
	{
		cin >> command;
		if (memcmp(command, "exit", strlen("exit")) == 0)
		{
			break;
		}
		else if (memcmp(command, "login", strlen("login")) == 0)
		{
			Frame_Login frame_login{ "hhh", "1234567890" };
			Frame_Login_Result frame_login_result;

			send(socket_client, (char*)&frame_login, sizeof(Frame_Login), 0);
			size = recv(socket_client, (char*)&frame_login_result, sizeof(Frame_Login_Result), 0);
			if (size > 0)
			{
				if (size == sizeof(Frame_Login_Result))
				{
					cout << "login result is " << frame_login_result.result << endl;
				}
				else
				{
					cout << "Receive data size error" << endl;
				}
			}
			else
			{
				cout << "Server close" << endl;
				break;
			}
		}
		else
		{
			cout << "Error command, please input again" << endl;
		}
	}

	//关闭客户端的socket
	closesocket(socket_client);

	WSACleanup();

	return 0;
}

