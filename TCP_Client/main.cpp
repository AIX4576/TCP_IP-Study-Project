#include<iostream>
#include<thread>
#include<chrono>

using namespace std;

#define WIN32_LEAN_AND_MEAN
#include<WS2tcpip.h>

#pragma comment(lib,"ws2_32.lib")

int main()
{
	WORD version = MAKEWORD(2, 2);//指定Socket版本
	WSADATA data;
	WSAStartup(version, &data);

	//创建一个ipv4，数据流，tcp协议的socket套接字
	SOCKET socket_client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (socket_client == INVALID_SOCKET)
	{
		cout << "Error: create client socket fail" << endl;
		WSACleanup();

		return -1;
	}

	// connect 连接服务器
	int result;
	sockaddr_in address_server{};

	address_server.sin_family = AF_INET;
	address_server.sin_port = htons(8080);
	inet_pton(AF_INET, "192.168.137.1", &address_server.sin_addr);

	result = connect(socket_client, (sockaddr*)&address_server, sizeof(address_server));
	if (result == SOCKET_ERROR)
	{
		cout << "Error: connect to server fail" << endl;
		closesocket(socket_client);
		WSACleanup();

		return -1;
	}
	else
	{
		cout << "Connect succeed" << endl;
	}

	int size;
	char send_buffer[256]{};
	char receive_buffer[256]{};

	while (1)
	{
		memset(send_buffer, 0, sizeof(send_buffer));
		memset(receive_buffer, 0, sizeof(receive_buffer));

		cin >> send_buffer;
		if (memcmp(send_buffer, "exit", strlen("exit")) == 0)
		{
			break;
		}

		size = strnlen(send_buffer, sizeof(send_buffer));

		//send 向服务器发送数据，对于客户端，与服务器建立连接后使用 send() 发送数据时填入的是自己的socket
		send(socket_client, send_buffer, size, 0);

		//recv 接受服务器发来的数据，对于客户端，与服务器建立连接后使用 recv() 接收数据时填入的是自己的socket
		size = recv(socket_client, receive_buffer, sizeof(receive_buffer), 0);
		if (size <= 0)
		{
			cout << "Server close" << endl;
			break;
		}
		else
		{
			cout << "Message from server is " << receive_buffer << endl;
		}
	}

	//关闭客户端的socket
	closesocket(socket_client);

	WSACleanup();

	return 0;
}

