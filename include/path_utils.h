#pragma once                     // 预处理器指令：确保此头文件在编译时只被包含一次，避免重复定义

#include <string>                // 包含标准字符串库，用于std::string类型
#include <vector>                // 包含标准向量库，用于std::vector容器

// 获取环境变量的值，如果环境变量不存在则返回空字符串
std::string get_env_or_empty(const char *name);

// 获取当前可执行文件所在的目录路径
std::string executable_dir();

// 返回默认的搜索根路径列表（通常用于查找配置文件或资源）
std::vector<std::string> default_search_roots();

// 根据给定的相对路径候选列表和环境变量，解析出一个存在的绝对路径
// relative_candidates: 相对路径候选列表
// env_var: 可选的环境变量名，用于指定基础目录
std::string resolve_existing_path(const std::vector<std::string> &relative_candidates,
                                  const char *env_var = nullptr);

// 根据文件名和环境变量解析配置文件的实际路径
// filename: 配置文件名
// env_var: 可选的环境变量名，用于指定搜索路径
std::string resolve_config_path(const std::string &filename, const char *env_var = nullptr);