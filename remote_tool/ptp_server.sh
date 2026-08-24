#!/usr/bin/env bash

set -euo pipefail

# ==================== 阶段 1：读取唯一配置 ====================
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/.." && pwd)"
source "$project_root/config.env"
remote_log_file="/tmp/udp_rtstream_ptpd_slave.log"
slave_ssh_pid=""

ssh_options=(
    -n -T
    -o StrictHostKeyChecking=accept-new
    -o ConnectTimeout=10
    -o ServerAliveInterval=5
    -o ServerAliveCountMax=2
)

run_remote() {
    sshpass -p "$REMOTE_PASSWORD" ssh "${ssh_options[@]}" \
        "${REMOTE_USER}@${REMOTE_HOST}" "$1"
}

# ==================== 阶段 2：检查两端运行条件 ====================
for dependency in ptpd sshpass; do
    command -v "$dependency" >/dev/null 2>&1 || {
        echo "错误：本机没有找到 $dependency。" >&2
        exit 1
    }
done

if [[ ! -d "/sys/class/net/$PTP_MASTER_INTERFACE" ]]; then
    echo "错误：本机网卡 $PTP_MASTER_INTERFACE 不存在。" >&2
    exit 1
fi

run_remote "command -v ptpd >/dev/null && test -d /sys/class/net/$PTP_SLAVE_INTERFACE" || {
    echo "错误：远端 ptpd 或网卡 $PTP_SLAVE_INTERFACE 不可用。" >&2
    exit 1
}

# ==================== 阶段 3：定义统一退出流程 ====================
stop_remote_slave() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    if [[ -n "$slave_ssh_pid" ]]; then
        kill "$slave_ssh_pid" 2>/dev/null
        wait "$slave_ssh_pid" 2>/dev/null
    fi
    echo "远端 PTP slave SSH 会话已停止。"
    exit "$status"
}

# ==================== 阶段 4：启动远端 slave ====================
remote_command="echo $REMOTE_PASSWORD | sudo -S ptpd -C -s -i $PTP_SLAVE_INTERFACE -V > $remote_log_file 2>&1"
run_remote "$remote_command" &
slave_ssh_pid=$!
trap stop_remote_slave EXIT INT TERM

echo "远端 ${REMOTE_HOST}:${PTP_SLAVE_INTERFACE} PTP slave 已启动。"
echo "远端日志：$remote_log_file"

# ==================== 阶段 5：运行本机 master ====================
echo "正在本机 $PTP_MASTER_INTERFACE 上启动 PTP master；按 Ctrl+C 同时停止两端。"
sudo ptpd -C -M -i "$PTP_MASTER_INTERFACE"
