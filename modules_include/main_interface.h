#pragma once

#include<iostream>
#include<memory>
#include<shared_mutex>
#include<unordered_map>
using namespace std;

#include"ModuleHandle.h"

/**
 * @brief 主模块句柄（单例）：持有并管理所有业务模块句柄，级联启停。
 * 公有继承 Module_Handle，自身也具备回调能力，可作为全局事件汇入点。
 */
class Main_Handle : public Module_Handle
{
public:
	Main_Handle(const Main_Handle&) = delete;
	Main_Handle(Main_Handle&& other) = delete;
	Main_Handle& operator=(const Main_Handle&) = delete;
	Main_Handle& operator=(Main_Handle&& other) = delete;
	~Main_Handle() override;

	/**
	 * @brief 获取主模块单例引用（函数内静态对象，首次调用时构造，C++11 起构造线程安全）。
	 */
	static Main_Handle& instance();

	/**
	 * @brief 添加模块，由主模块接管其生命周期（start_run/stop_run 级联）。
	 * @param p_module 模块句柄（不能为空）；同名模块已存在时替换。
	 */
	void add_module(shared_ptr<Module_Handle> p_module);

	/**
	 * @brief 按名称移除模块；移除后 shared_ptr 释放，模块析构自动调用 stop_run。
	 */
	void remove_module(const string& name);

	/**
	 * @brief 按名称获取模块句柄。
	 * @return 模块句柄的 shared_ptr 拷贝；不存在时返回空 shared_ptr。
	 */
	shared_ptr<Module_Handle> get_module(const string& name) const;

protected:
	void start_run_impl() override;
	void stop_run_impl() override;

private:
	Main_Handle();

	// 模块句柄容器：按模块 name 索引（与 main.cpp 原存储方式一致）
	unordered_map<string, shared_ptr<Module_Handle>> module_handles;
	mutable shared_mutex mtx;	// 保护 module_handles（add/remove/get 可被其他模块线程调用）
};
