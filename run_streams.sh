#!/usr/bin/env bash                # 使用 bash 解释器执行脚本
set -euo pipefail                  # 设置脚本选项：-e 遇到错误立即退出，-u 使用未定义变量时报错，-o pipefail 管道命令中任何命令失败都返回非零状态

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"  # 获取脚本所在目录的绝对路径，并赋值给 PROJECT_ROOT
RUN_DEMO_SH="${PROJECT_ROOT}/run_demo.sh"                     # 构造 run_demo.sh 的完整路径

if [[ -f "${PROJECT_ROOT}/demo.env" ]]; then                  # 如果存在 demo.env 文件
    # shellcheck disable=SC1091                               # 禁用 shellcheck 警告 SC1091（无法跟踪源文件）
    source "${PROJECT_ROOT}/demo.env"                         # 加载该环境变量文件
fi

if [[ ! -x "${RUN_DEMO_SH}" ]]; then                          # 如果 run_demo.sh 不可执行
    echo "Runner not found: ${RUN_DEMO_SH}"                   # 输出错误信息
    exit 1                                                    # 退出脚本，返回状态码1
fi

MODE="${1:-all}"                                              # 获取第一个命令行参数，如果没有则默认为 "all"
if [[ $# -gt 0 ]]; then                                       # 如果参数个数大于0
    shift                                                     # 移除第一个参数，后续参数留给 run_demo.sh
fi

export MYDEMO_RTMP_URL="${MYDEMO_RTMP_URL:-rtmp://192.168.43.11/live/livestream}"  # 设置 RTMP URL 环境变量，若未设置则使用默认值
export MYDEMO_RTSP_URL="${MYDEMO_RTSP_URL:-rtsp://192.168.43.11:8554/mystream}"   # 设置 RTSP URL 环境变量，若未设置则使用默认值
export MYDEMO_GB_CONFIG="${MYDEMO_GB_CONFIG:-${PROJECT_ROOT}/gb28181.conf}"       # 设置 GB28181 配置文件路径，若未设置则使用默认路径

export MYDEMO_ENABLE_RTMP=0                                   # 初始化 RTMP 开关为关闭
export MYDEMO_ENABLE_RTSP=0                                   # 初始化 RTSP 开关为关闭
export MYDEMO_ENABLE_GB28181=0                                # 初始化 GB28181 开关为关闭

# 定义函数 enable_mode，根据传入的模式设置对应的开关
enable_mode() {
    case "$1" in                                               # 根据第一个参数匹配
        rtmp)                                                  # 如果是 "rtmp"
            export MYDEMO_ENABLE_RTMP=1                        # 设置 RTMP 开关为1
            ;;
        rtsp)                                                  # 如果是 "rtsp"
            export MYDEMO_ENABLE_RTSP=1                        # 设置 RTSP 开关为1
            ;;
        gb28181)                                               # 如果是 "gb28181"
            export MYDEMO_ENABLE_GB28181=1                     # 设置 GB28181 开关为1
            ;;
        all)                                                   # 如果是 "all"
            export MYDEMO_ENABLE_RTMP=1                        # 开启 RTMP
            export MYDEMO_ENABLE_RTSP=1                        # 开启 RTSP
            export MYDEMO_ENABLE_GB28181=1                     # 开启 GB28181
            ;;
        none|"")                                               # 如果是 "none" 或空字符串
            ;;                                                 # 不做任何设置（所有开关保持0）
        *)                                                     # 其他情况
            echo "Unsupported mode: $1"                        # 输出不支持的 mode 信息
            echo "Usage: ./run_streams.sh [rtmp|rtsp|gb28181|all|rtmp,rtsp|rtmp,gb28181|rtsp,gb28181] [run_demo args...]"  # 显示用法
            exit 1                                             # 退出脚本，返回状态码1
            ;;
    esac
}

IFS=',' read -r -a MODES <<< "${MODE}"                         # 使用逗号分隔 MODE 变量，将结果存入数组 MODES
for item in "${MODES[@]}"; do                                  # 遍历数组中的每个元素
    enable_mode "${item}"                                      # 调用 enable_mode 函数处理每个模式
done

echo "Streaming config:"                                       # 输出配置信息标题
echo "  RTMP    : ${MYDEMO_ENABLE_RTMP} (${MYDEMO_RTMP_URL})"  # 显示 RTMP 开关状态和 URL
echo "  RTSP    : ${MYDEMO_ENABLE_RTSP} (${MYDEMO_RTSP_URL})"  # 显示 RTSP 开关状态和 URL
echo "  GB28181 : ${MYDEMO_ENABLE_GB28181} (${MYDEMO_GB_CONFIG})"  # 显示 GB28181 开关状态和配置文件路径
echo                                                          # 输出空行

exec "${RUN_DEMO_SH}" "$@"                                     # 用当前进程替换为执行 run_demo.sh，并传递剩余的所有参数