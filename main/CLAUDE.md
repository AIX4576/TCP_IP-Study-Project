# CLAUDE.md

本文件为 `main` 启动项目的目录级指导，适用于 `main/` 目录下的所有文件；仓库级常驻规则（构建、模块化插件体系约定）见根目录 `CLAUDE.md`。

## 项目职责

`main` 是解决方案的启动项目（`main.exe`），负责创建各业务模块并驱动其生命周期。当前唯一模块为 Custom_Server：`main.cpp` 通过导入库 `library/Custom_Server.lib` 隐式链接，运行时由系统加载 `Custom_Server.dll`。

## 模块加载与生命周期

`main.cpp` 直接调用各模块接口头（`modules_include/`）声明的 `Initialize()`（`extern "C"` 导出函数），流程：

1. 调用 `Initialize()` 获得 `Module_Handle*`，存入 `unordered_map<string, unique_ptr<Module_Handle>>`（按模块 `name` 索引）
2. 依次调用各模块的 `start_run()` 启动
3. 主线程进入命令循环，输入 `exit` 退出
4. 退出前对每个模块调用 `stop_run()`，随后进程结束

- 新增模块需在 `main.cpp` 引入对应接口头并调用其 `Initialize()`；`Module_Handle` 的所有权由 `unique_ptr` 管理，禁止拷贝/移动
- 模块基类 `Module_Handle` 定义在 `public_component/ModuleHandle.h`，生命周期方法为 `start_run()` / `stop_run()`

## 目录文件说明

- `main.cpp` — 入口，模块加载与生命周期驱动
- `override_new_delete.cpp` — 全局 operator new/delete 覆盖实现
- `main.vcxproj(.filters)` — 项目文件，经属性表模板继承公共配置，链接 `library/` 下的导入库

## 注意

- 本文件随 main 开发同步维护；修改本目录代码后检查本文件和根目录 `CLAUDE.md` 是否需要更新
