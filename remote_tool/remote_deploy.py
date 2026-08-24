#!/usr/bin/env python3
"""编译采集端并将其部署为远端 systemd 服务。"""

from __future__ import annotations

import ipaddress
import shlex
import tempfile
from pathlib import Path

from remote_compile import compile_remote
from remote_utils import (
    copy_to_remote,
    load_config,
    remote_workspace,
    run_remote,
)


STREAM_SERVICE_NAME = "udp-rtstream.service"
PTP_SERVICE_NAME = "udp-rtstream-ptpd-slave.service"
LEGACY_PROBE_SERVICE_NAME = "udp-rtstream-delay-probe.service"


def receiver_ip() -> str:
    """优先使用配置值，否则取本次 SSH 连接的客户端地址。"""
    configured = load_config().get("RECEIVER_HOST")
    address = configured or run_remote(
        "printf '%s' \"${SSH_CLIENT%% *}\"", capture_output=True
    )
    try:
        return str(ipaddress.ip_address(address))
    except ValueError as error:
        raise RuntimeError(f"无法确定有效的接收端 IP：{address!r}") from error


def stream_service_unit(address: str) -> str:
    config = load_config()
    user = config["REMOTE_USER"]
    workspace = remote_workspace()
    udp_port = int(config["UDP_PORT"])
    executable = f"{workspace}/capture/udp_rtstream"

    # systemd 的命令行语法接受双引号；反斜杠和双引号需要显式转义。
    def quote(value: str) -> str:
        return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'

    return f"""[Unit]
Description=UDP RTStream camera sender
Wants=network-online.target
After=network-online.target

[Service]
Type=simple
User={user}
WorkingDirectory={workspace}/capture
ExecStart={quote(executable)} {quote(address)} {udp_port}
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
"""


def ptp_service_unit(ptpd_path: str) -> str:
    interface = load_config()["PTP_SLAVE_INTERFACE"]
    return f"""[Unit]
Description=UDP RTStream PTPd slave on {interface}
Wants=network-online.target
After=network-online.target
ConditionPathExists=/sys/class/net/{interface}

[Service]
Type=simple
ExecStart={ptpd_path} -C -s -i {interface} -V
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
"""


def deploy() -> None:
    config = load_config()
    remote_host = config["REMOTE_HOST"]
    password = config["REMOTE_PASSWORD"]

    print(f"[1/5] 同步并编译 {remote_host}……", flush=True)
    compile_remote()

    print("[2/5] 检查远端 PTPd 和网卡……", flush=True)
    interface = config["PTP_SLAVE_INTERFACE"]
    ptpd_path = run_remote("command -v ptpd", capture_output=True)
    run_remote(f"test -d /sys/class/net/{shlex.quote(interface)}")
    print(f"      PTP slave 将监听 {interface}", flush=True)

    print("[3/5] 确定接收端 IP……", flush=True)
    address = receiver_ip()
    print(f"      视频将发送到 {address}:{config['UDP_PORT']}", flush=True)

    stream_staging = f"{remote_workspace()}/.{STREAM_SERVICE_NAME}.tmp"
    ptp_staging = f"{remote_workspace()}/.{PTP_SERVICE_NAME}.tmp"
    print("[4/5] 安装并启动 systemd 服务……", flush=True)
    with tempfile.TemporaryDirectory(prefix="udp-rtstream-") as temp_dir:
        stream_unit = Path(temp_dir) / STREAM_SERVICE_NAME
        stream_unit.write_text(stream_service_unit(address), encoding="utf-8")
        copy_to_remote(stream_unit, stream_staging)
        ptp_unit = Path(temp_dir) / PTP_SERVICE_NAME
        ptp_unit.write_text(ptp_service_unit(ptpd_path), encoding="utf-8")
        copy_to_remote(ptp_unit, ptp_staging)

    quoted_stream_staging = shlex.quote(stream_staging)
    quoted_ptp_staging = shlex.quote(ptp_staging)
    quoted_stream_service = shlex.quote(STREAM_SERVICE_NAME)
    quoted_ptp_service = shlex.quote(PTP_SERVICE_NAME)
    quoted_legacy_probe_service = shlex.quote(LEGACY_PROBE_SERVICE_NAME)
    run_remote(
        "sudo -S -p '' sh -c "
        + shlex.quote(
            f"systemctl disable --now {quoted_legacy_probe_service} "
            ">/dev/null 2>&1 || true; "
            f"rm -f /etc/systemd/system/{quoted_legacy_probe_service}; "
            f"install -m 0644 {quoted_stream_staging} "
            f"/etc/systemd/system/{quoted_stream_service} && "
            f"install -m 0644 {quoted_ptp_staging} "
            f"/etc/systemd/system/{quoted_ptp_service} && "
            "systemctl daemon-reload && "
            f"systemctl enable {quoted_stream_service} {quoted_ptp_service} && "
            f"systemctl restart {quoted_ptp_service} {quoted_stream_service}"
        ),
        input_text=password + "\n",
    )
    run_remote(f"rm -f {quoted_stream_staging} {quoted_ptp_staging}")
    run_remote(
        f"rm -f {shlex.quote(f'{remote_workspace()}/capture/delay_probe_server')}"
    )

    print("[5/5] 服务状态：", flush=True)
    status = run_remote(
        "systemctl --no-pager --full status "
        f"{quoted_ptp_service} {quoted_stream_service}",
        capture_output=True,
    )
    print(status)


if __name__ == "__main__":
    deploy()
