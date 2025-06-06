#include<iostream>
#include<thread>
#include<chrono>

using namespace std;

#include"frame.h"

#define WIN32_LEAN_AND_MEAN //让编译器避免引入Windows早期的库，避免引起冲突
#include<WS2tcpip.h>

//显式告诉编译器（链接器）程序需要"ws2_32.lib"这个静态库，否则链接时找不到WSAStartup()和WSACleanup()这两个符号定义
//或者在附加依赖项里面添加
#pragma comment(lib,"ws2_32.lib")

int main()
{
	/*
	在Windows下使用Windows Sockets API，必须先调用WSAStartup()初始化Winsock服务，包括加载相关的运行库等操作
	程序退出前需要调用WSACleanup()函数来清除相关资源
	*/
	WORD version = MAKEWORD(2, 2);//指定Socket版本
	WSADATA data;
	WSAStartup(version, &data);

	//创建一个ipv4，数据流，tcp协议的socket套接字
	SOCKET socket_server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (socket_server == INVALID_SOCKET)
	{
		cout << "Error: create server socket fail" << endl;
		WSACleanup();

		return -1;
	}

	//bind 绑定网络端口
	//sockaddr_in可以转换为sockaddr，反之亦然，因为它们在内存中的布局是一致的
	//sockaddr是一个更通用的套接字地址结构，可以表示各种类型的网络地址（如IPv4、IPv6等），而sockaddr_in专门用于IPv4地址
	int result;
	sockaddr_in address_server
	{
		.sin_family = AF_INET,//地址族:ipv4
		.sin_port = htons(8080),//端口号，需要从主机字节序（小端）转换成网络字节序（大端）
		.sin_addr = INADDR_ANY,//地址，或者用 inet_pton(AF_INET, "127.0.0.1", &address_server.sin_addr); 
	};

	result = bind(socket_server, (sockaddr*)&address_server, sizeof(address_server));
	if (result == SOCKET_ERROR)
	{
		cout << "Error: socket bind port fail" << endl;
		closesocket(socket_server);
		WSACleanup();
		return -1;
	}
	else
	{
		cout << "Socket bind port succeed" << endl;
	}

	//listen 监听网络端口
	result = listen(socket_server, 5);
	if (result == SOCKET_ERROR)
	{
		cout << "Error: socket listen fail" << endl;
		closesocket(socket_server);
		WSACleanup();
		return -1;
	}
	else
	{
		cout << "Socket listen succeed" << endl;
	}

	//等待接受客户端连接，accept()会阻塞直到有客户端连接进来
	sockaddr_in address_client;
	int address_length_client = sizeof(address_client);

	SOCKET socket_client = accept(socket_server, (sockaddr*)&address_client, &address_length_client);
	if (socket_client == INVALID_SOCKET)
	{
		cout << "Error: accept invalid socket" << endl;
	}
	else
	{
		char ip_str[INET_ADDRSTRLEN]{};
		inet_ntop(AF_INET, &address_client.sin_addr, ip_str, sizeof(ip_str));

		cout << "Accept id: " << (int)socket_client << " client socket, ip = " << ip_str << endl;
	}

	//recv 接收客户端发来的数据，对于服务器，接收哪一个客户端的数据就要填入对应客户端的socket
	//send 向客户端发送数据，对于服务器，向哪个客户端发送数据就要填入对应客户端的socket
	Frame_Header frame_header;
	int size = 0;

	while (socket_client != INVALID_SOCKET)
	{
		if (recv(socket_client, (char*)&frame_header.start_4bytes.bytes[0], 1, 0) > 0)
		{
			if (frame_header.start_4bytes.bytes[0] == Start_Bytes0)
			{
				if (recv(socket_client, (char*)&frame_header.start_4bytes.bytes[1], 3, MSG_PEEK) == 3)
				{
					if ((frame_header.start_4bytes.bytes[1] == Start_Bytes1) &&
						(frame_header.start_4bytes.bytes[2] == Start_Bytes2) &&
						(frame_header.start_4bytes.bytes[3] == Start_Bytes3))
					{
						if (recv(socket_client, (char*)&frame_header.start_4bytes.bytes[1], sizeof(Frame_Header) - 1, 0) == (sizeof(Frame_Header) - 1))
						{
							switch (frame_header.command)
							{
							case CMD_Login:
							{
								if (frame_header.frame_length == sizeof(Frame_Login))
								{
									Frame_Login frame_login;
									if (recv(socket_client, (char*)&frame_login.user_name, sizeof(Frame_Login) - sizeof(Frame_Header), 0) == (sizeof(Frame_Login) - sizeof(Frame_Header)))
									{
										Frame_Login_Result frame_login_result;
										if ((frame_login.password[0] && frame_login.user_name[0]) == 0)
										{
											frame_login_result.result = -1;
										}
										send(socket_client, (char*)&frame_login_result, sizeof(Frame_Login_Result), 0);
									}
								}
							}
							break;
							default:break;
							}
						}
					}
				}
			}
		}
		else
		{
			cout << "Client close" << endl;
			break;
		}
	}

	//关闭服务器的socket
	closesocket(socket_server);

	WSACleanup();

	return 0;
}

