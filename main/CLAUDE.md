# CLAUDE.md

本文件为 `main` 启动项目的目录级指导，适用于 `main/` 目录下的所有文件；仓库级常驻规则（构建、模块化插件体系约定）见根目录 `CLAUDE.md`。

## 项目职责

`main` 是解决方案的启动项目（`main.exe`），负责创建各业务模块并驱动其生命周期。当前唯一模块为 Custom_Server：`main.cpp` 通过导入库 `library/Custom_Server.lib` 隐式链接，运行时由系统加载 `Custom_Server.dll`。

## 模块加载与生命周期

`Main_Handle`（单例，定义见 `modules_include/main_interface.h`，实现于 `main/main_interface.cpp`）公有继承 `Module_Handle`，持有并管理所有业务模块句柄（`unordered_map<string, shared_ptr<Module_Handle>>`，按模块 `name` 索引，`shared_mutex` 保护），对外提供 `add_module()` / `remove_module()` / `get_module()`。

`main.cpp` 流程：

1. 调用各模块接口头（`modules_include/`）声明的 `Initialize()`（`extern "C"` 导出函数）获得 `Module_Handle*`，包装为 `shared_ptr` 后 `add_module()` 交给 `Main_Handle` 托管
2. `Main_Handle::start_run()` 级联启动自身与全部模块（`start_run_impl()` 快照容器后逐个 `start_run()`）
3. 主线程进入命令循环，输入 `exit` 退出
4. `Main_Handle::stop_run()` 级联停止全部模块与自身，随后进程结束

- 新增模块需在 `main.cpp` 引入对应接口头并调用其 `Initialize()`；`Module_Handle` 的所有权由 `shared_ptr` 管理，禁止拷贝/移动
- 级联启停采用"快照后解锁再逐个调用"：`stop_run()` 会 join 子模块线程，持锁调用会与子模块线程请求容器锁互相等待
- 模块基类 `Module_Handle` 定义在 `public_component/ModuleHandle.h`，生命周期方法为 `start_run()` / `stop_run()`

## 目录文件说明

- `main.cpp` — 入口，模块加载与生命周期驱动
- `main_interface.cpp` — `Main_Handle` 单例实现（类声明在 `modules_include/main_interface.h`）
- `override_new_delete.cpp` — 全局 operator new/delete 覆盖实现
- `main.vcxproj(.filters)` — 项目文件，经属性表模板继承公共配置，链接 `library/` 下的导入库

## 注意

- 本文件随 main 开发同步维护；修改本目录代码后检查本文件和根目录 `CLAUDE.md` 是否需要更新
