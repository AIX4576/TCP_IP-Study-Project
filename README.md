# TCP/IP学习项目

## 解决方案概述

本项目是一个 Visual Studio 解决方案工程，用于学习 Windows 环境下网络编程及 IOCP（I/O Completion Port）的使用。涵盖以下学习目标：

- **Windows 网络编程**：基于 TCP/IP 协议栈的高性能服务器开发
- **IOCP 模型**：使用 I/O 完成端口实现高效并发
- **软件架构设计**：模块化、可扩展的架构实践
- **事件通知与数据同步**：多线程环境下的同步机制
- **AI 辅助编程**：探索 AI agent 在实际工程中的应用

## 工程结构

- **产物**：单进程多线程的可执行文件 `main.exe`
- **架构风格**：模块化设计，除 `main` 为启动项目外，其余项目均为功能模块
- **编译产出**：各模块编译生成 `.lib` 导入库和 `.dll` 动态库；`main.exe` 在运行时动态加载各模块的 DLL 并执行其逻辑

## 工程组成

### 核心项目

1. **main** — 启动项目
   - **功能**：程序入口，负责动态加载和启动各个模块
   - **输出**：`main.exe`
   - **依赖**：Custom_Server 模块

2. **Custom_Server** — 自定义服务器模块
   - **功能**：基于 IOCP 模型的高性能 TCP 服务器
   - **输出**：`Custom_Server.dll` + `Custom_Server.lib`
   - **核心组件**：
     - `iocp_server.h/cpp` — IOCP 服务器核心实现
     - `application.h/cpp` — 应用层业务逻辑
     - `custom_server_thread.h/cpp` — 自定义服务器线程管理

### 目录结构

```
TCP_IP学习项目/
│
├── main/                         # 启动项目：程序入口，负责加载并运行各模块
├── Custom_Server/                # 高性能 TCP 服务器模块
├── 3rd_party/                    # 未通过 vcpkg 管理的第三方库头文件
│   └── mimalloc/                 # 高效内存分配器
├── modules_include/              # 各模块对外暴露的接口头文件
├── public_component/             # 所有项目共享的公共组件头文件
├── library/                      # 编译产物和第三方库的 .lib 导入库文件
├── x64/                          # 解决方案编译生成的产物目录
├── document/                     # 文档目录（不上传 Git）
├── plans/                        # AI agent 生成的计划文档（不上传 Git）
│
├── PropertySheetTemplateDebug.props    # Debug 属性表模板
├── PropertySheetTemplateRelease.props  # Release 属性表模板
├── TCP_IP学习项目.sln                  # 解决方案文件
├── build_config.json                   # 构建配置文件（指定 MSBuild 路径，不上传 Git）
├── build.ps1                           # 构建脚本（主要供 AI agent 使用）
└── README.md                           # 本文件
```

> **说明**：`document/` 和 `plans/` 目录不提交至 Git 仓库，请勿在其中存放需版本控制的文件。

## 开发与构建

- **推荐方式**：使用 Visual Studio 打开 `.sln` 解决方案文件进行编码、配置、编译和调试
- **自动化构建**：项目根目录下的 `build.ps1` 脚本也可完成编译工作，该脚本主要面向 AI agent 使用，便于在非交互环境中触发构建

## 依赖管理

- 部分第三方库通过 [vcpkg](https://github.com/microsoft/vcpkg) 管理
- 其余第三方库的手动管理的头文件位于 `3rd_party/` 目录
