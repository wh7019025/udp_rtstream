#!/usr/bin/env python3
"""远程编译和执行共用工具。"""

from __future__ import annotations

import os
import shlex
import subprocess
from pathlib import Path


def project_root() -> Path:
    return Path(__file__).resolve().parent.parent


def load_config() -> dict[str, str]:
    """读取根目录简洁的 KEY=VALUE 配置。"""
    config: dict[str, str] = {}
    config_path = project_root() / "config.env"
    for line_number, raw_line in enumerate(config_path.read_text().splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise ValueError(f"{config_path}:{line_number} 配置格式错误")
        key, value = line.split("=", 1)
        config[key.strip()] = value.strip()
    return config


def remote_workspace() -> str:
    return load_config()["REMOTE_WORKSPACE"]


def ssh_target() -> str:
    config = load_config()
    remote_user = config["REMOTE_USER"]
    remote_host = config["REMOTE_HOST"]
    return f"{remote_user}@{remote_host}"


def command_environment() -> dict[str, str]:
    environment = os.environ.copy()
    environment["SSHPASS"] = load_config()["REMOTE_PASSWORD"]
    return environment


def run_remote(command: str, *, capture_output: bool = False) -> str:
    """执行远端命令，并按需返回标准输出。"""
    ssh_command = [
        "sshpass", "-e", "ssh",
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", "ConnectTimeout=10",
        ssh_target(), command,
    ]
    completed = subprocess.run(
        ssh_command,
        env=command_environment(),
        check=True,
        capture_output=capture_output,
        text=True,
    )
    return completed.stdout.strip() if capture_output else ""


def sync_capture_sources() -> None:
    """同步 config 和 capture 源码，构建产物仍只存在于远端。"""
    root = project_root()
    local_capture = root / "capture"
    workspace = remote_workspace()
    remote_capture = f"{workspace}/capture"
    run_remote(f"mkdir -p {shlex.quote(remote_capture)}")

    source_files = sorted(
        path for path in local_capture.iterdir()
        if path.name == "Makefile" or path.suffix in {".c", ".h"}
    )
    if not source_files:
        raise RuntimeError(f"没有在 {local_capture} 中找到采集端源码")

    scp_base = [
        "sshpass", "-e", "scp",
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", "ConnectTimeout=10",
    ]
    environment = command_environment()
    subprocess.run(
        [*scp_base, str(root / "config.env"), f"{ssh_target()}:{workspace}/"],
        env=environment, check=True,
    )
    subprocess.run(
        [*scp_base, *(str(path) for path in source_files),
         f"{ssh_target()}:{remote_capture}/"],
        env=environment, check=True,
    )
