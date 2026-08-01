# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 协作入口

适用于仓库根目录及未被更深层 `CLAUDE.md` 覆盖的路径。本文件只写该工程仓库级常驻规则；长文档、细节规范和历史决策下沉到 `README.md`、`document/`、`plans/` 和对应目录级 `CLAUDE.md`

## 项目概况

Windows 平台基于 IOCP（I/O 完成端口）的高性能 TCP 服务器学习项目。Visual Studio 解决方案（VS 18），C++20，仅支持 x64 平台。单进程多线程架构：`main.exe` 动态加载各模块 DLL，目前仅有 `Custom_Server` 一个模块。

## 构建

### 命令行构建（推荐 AI agent 使用）

```powershell
# 语法：build.ps1 <Action> <Configuration> <Platform>
./build.ps1 build debug x64      # 编译（Debug|x64）
./build.ps1 rebuild release x64  # 重新编译（Release|x64）
./build.ps1 clean debug x64      # 清理
```

- 参数 `Action` 必填（build/rebuild/clean）；`Configuration` 默认 Debug，`Platform` 默认 x64
- 脚本依赖 `build_config.json`（**已 gitignore，不提交**），需手动创建，内容为 `MSBuild_path` 和 `solution_file` 两个字段。缺失或路径无效时脚本会输出模板并终止
- 产物输出到 `library/`（Custom_Server.lib）和 `x64/`，MSBuild 日志在 `build.log`
- 也可用 Visual Studio 直接打开 `TCP_IP学习项目.sln` 编译

### 项目配置约定

- Debug/Release 共用两个属性表模板：`PropertySheetTemplateDebug.props` / `PropertySheetTemplateRelease.props`，由各 `.vcxproj` 在 `Debug|x64`/`Release|x64` 条件下导入
- 属性表定义：`stdcpp20` 标准、`/utf-8`、警告级别 Level3、公共 include 目录（`modules_include`、`3rd_party`、`public_component`、`$(ProjectDir)`）、库目录 `library`
- 依赖库：boost（object_pool、circular_buffer）、folly（F14Map/F14FastMap）、moodycamel::ConcurrentQueue 来自 vcpkg；mimalloc（`mimalloc.dll.lib`）、concurrentqueue.h 为手动管理的第三方库

## 架构

### 模块化插件体系

- `main/main.cpp` 是启动项目，运行时通过 `Initialize()`（`extern "C"` 导出函数）动态加载模块
- 模块基类 `Module_Handle` 定义在 `public_component/ModuleHandle.h`，生命周期方法为 `start_run()` / `stop_run()`，所有派生类禁用拷贝/移动
- 模块接口头文件（如 `custom_server_interface.h`，声明 `Custom_Server_Handle` 和 `Initialize()`）集中放在 `modules_include/`，模块内部实现自行包含
- 新增模块需遵循：接口头放入 `modules_include/`，编译为 DLL，导出 `Initialize()` 返回 `Module_Handle*`

### 模块内部细节（目录级文档）

- `Custom_Server/CLAUDE.md` — Custom_Server 模块的线程拓扑、数据流与 Event_handle 生命周期（易错点）
- `main/CLAUDE.md` — main 启动项目的模块加载与生命周期机制

## 目录约定

- `3rd_party/` — 手动管理的第三方头文件（concurrentqueue.h、mimalloc/）
- `modules_include/` — 各模块对外接口头文件
- `public_component/` — 跨模块共享组件（ModuleHandle.h、ConcurrentObjectPool.h）
- `library/` — 生成的 .lib 导入库
- `document/`、`plans/` — 文档与 AI 计划，**均被 gitignore**，不提交

执行plan任务时，需要生成对应的 `xxx.md` 文档放在 `plans/` 中以供用户查阅，并在与用户的沟通中不断修改，直至用户确认计划。用户确认后按此计划执行。 `xxx.md` 需包含该计划文档生成时间

## 注意

- `build_config.json` 被 gitignore，但脚本运行必须存在，机器上未提交时需向用户说明并引导创建
- README.md 记录了面向用户的目录结构与构建说明，修改目录结构时应同步更新
- 本文件应随着工程开发而不断修改，并不是一成不变的。每次修改工程后应检查对应目录级和根目录的 `CLAUDE.md` 和 `README.md` 是否需要更新
