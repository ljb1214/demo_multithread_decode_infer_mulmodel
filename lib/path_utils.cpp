// 包含头文件 "path_utils.h"（声明本文件实现的函数）
#include "path_utils.h"

// 标准库头文件
#include <cstdlib>        // 提供 std::getenv
#include <filesystem>     // C++17 文件系统库
#include <limits.h>       // 提供 PATH_MAX 宏
#include <unistd.h>       // 提供 readlink 系统调用
#include <vector>         // 提供 std::vector

// 为 std::filesystem 创建别名，简化书写
namespace fs = std::filesystem;

// 匿名命名空间，内部函数仅在本文件可见，避免链接冲突
namespace
{
// 将规范化后的路径加入根目录列表，如果该路径尚未存在
void append_root_if_unique(std::vector<std::string> &roots, const fs::path &root)
{
    // 如果传入的路径为空，直接返回
    if (root.empty())
        return;

    // 错误码对象，用于接收文件系统操作中的错误而不抛异常
    std::error_code ec;
    // 尝试将路径转换为规范化的绝对路径（解析符号链接、消除 . 和 ..）
    fs::path normalized = fs::weakly_canonical(root, ec);
    // 如果转换失败（例如路径不存在），则仅进行词法规范化（不检查实际文件系统）
    if (ec)
        normalized = root.lexically_normal();

    // 获取规范化后的字符串形式
    const std::string value = normalized.string();
    // 如果字符串为空，返回
    if (value.empty())
        return;

    // 检查该路径是否已经在 roots 列表中
    for (const auto &existing : roots)
    {
        if (existing == value)
            return;   // 已存在，不重复添加
    }
    // 路径唯一，添加到列表末尾
    roots.push_back(value);
}
} // 匿名命名空间结束

// 获取环境变量值，若变量不存在或传入空指针则返回空字符串
std::string get_env_or_empty(const char *name)
{
    if (name == nullptr)
        return "";

    const char *value = std::getenv(name);
    // 如果获取到值则返回字符串，否则返回空字符串
    return value ? std::string(value) : std::string();
}

// 获取可执行文件所在的目录
std::string executable_dir()
{
    // 缓冲区用于存储路径，PATH_MAX 通常为 4096 或 1024
    char buffer[PATH_MAX] = {0};
    // 读取 /proc/self/exe 符号链接，得到当前可执行文件的绝对路径
    const ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    // 如果读取失败（len <= 0），返回当前工作目录
    if (len <= 0)
        return fs::current_path().string();

    // 添加字符串结束符
    buffer[len] = '\0';
    // 取路径的父目录（即可执行文件所在目录）
    return fs::path(buffer).parent_path().string();
}

// 返回默认的搜索根目录列表，按优先级排序
std::vector<std::string> default_search_roots()
{
    std::vector<std::string> roots;

    // 优先使用环境变量 MYDEMO_ASSET_ROOT 指定的根目录
    const std::string asset_root = get_env_or_empty("MYDEMO_ASSET_ROOT");
    if (!asset_root.empty())
        append_root_if_unique(roots, asset_root);

    // 添加当前工作目录及其上级目录（两级）
    const fs::path cwd = fs::current_path();
    append_root_if_unique(roots, cwd);
    append_root_if_unique(roots, cwd / "..");
    append_root_if_unique(roots, cwd / "../..");

    // 添加可执行文件所在目录及其上级目录（两级）
    const fs::path exe = executable_dir();
    append_root_if_unique(roots, exe);
    append_root_if_unique(roots, exe / "..");
    append_root_if_unique(roots, exe / "../..");

    return roots;
}

// 在多个候选路径和默认搜索根目录中查找存在的文件，返回第一个存在的绝对路径
std::string resolve_existing_path(const std::vector<std::string> &relative_candidates,
                                  const char *env_var)
{
    // 如果提供了环境变量名，优先检查环境变量指定的路径是否存在
    if (env_var != nullptr)
    {
        const std::string override_path = get_env_or_empty(env_var);
        if (!override_path.empty() && fs::exists(override_path))
            return override_path;   // 直接返回存在的环境变量路径
    }

    // 遍历每个候选路径（可以是相对路径或绝对路径）
    for (const auto &candidate : relative_candidates)
    {
        if (candidate.empty())
            continue;

        // 将候选路径转换为 fs::path 对象
        const fs::path raw(candidate);
        // 如果候选路径本身就是绝对路径且存在，直接返回
        if (raw.is_absolute() && fs::exists(raw))
            return raw.string();

        // 否则，在默认的搜索根目录下尝试拼接候选路径，并检查是否存在
        for (const auto &root : default_search_roots())
        {
            fs::path full = fs::path(root) / raw;
            if (fs::exists(full))
                // 返回规范化的绝对路径
                return fs::weakly_canonical(full).string();
        }
    }

    // 如果所有候选路径都不存在，则回退：使用第一个搜索根目录 + 第一个候选路径，并返回词法规范化后的路径（不检查存在性）
    if (!relative_candidates.empty())
    {
        const fs::path fallback = fs::path(default_search_roots().front()) / relative_candidates.front();
        return fallback.lexically_normal().string();
    }
    // 如果没有候选路径，返回空字符串
    return "";
}

// 解析配置文件路径（简化版本，只传入单个文件名）
std::string resolve_config_path(const std::string &filename, const char *env_var)
{
    // 将单个文件名放入候选列表，调用通用解析函数
    return resolve_existing_path({filename}, env_var);
}