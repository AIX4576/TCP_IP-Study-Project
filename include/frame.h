#pragma once

#define Buffer_Size 1024

#define Start_Bytes0 0xaa
#define Start_Bytes1 0xbb
#define Start_Bytes2 0xcc
#define Start_Bytes3 0xdd
#define Start_4Bytes ((uint32_t)((Start_Bytes3<<24)|(Start_Bytes2<<16)|(Start_Bytes1<<8)|Start_Bytes0))

#define User_Name_Size 32
#define Password_Size 32

enum Command
{
	CMD_Login,
	CMD_Login_Result,
	CMD_Logout,
	CMD_Logout_Result,
};

struct Frame_Header
{
	union
	{
		uint8_t bytes[4];
		uint32_t bytes4;
	}start_4bytes;
	int command;
	int frame_length;//整个数据帧的长度大小（包括帧数据头和帧数据体）
};

struct Frame_Login :protected Frame_Header
{
	void Initialize_Header()
	{
		start_4bytes.bytes4 = Start_4Bytes;
		command = CMD_Login;
		frame_length = sizeof(Frame_Login);
	}
	Frame_Login()
	{
		Initialize_Header();
	}
	Frame_Login(const char* user_name, const char* password)
	{
		Initialize_Header();
		strncpy_s(this->user_name, user_name, User_Name_Size - 1);
		strncpy_s(this->password, password, Password_Size - 1);
	}

	char user_name[User_Name_Size]{};
	char password[Password_Size]{};
};

struct Frame_Login_Result :protected Frame_Header
{
	void Initialize_Header()
	{
		start_4bytes.bytes4 = Start_4Bytes;
		command = CMD_Login_Result;
		frame_length = sizeof(Frame_Login_Result);
	}
	Frame_Login_Result()
	{
		Initialize_Header();
		result = 0;
	}
	Frame_Login_Result(int result)
	{
		Initialize_Header();
		this->result = result;
	}

	int result;
};
