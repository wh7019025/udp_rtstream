#!/usr/bin/env python3
"""把远端计算机的系统时间设置为本机当前时间。"""

from __future__ import annotations

import shlex
import subprocess
import time

from remote_utils import load_config, run_remote, ssh_target


def main() -> None:
    # ==================== 阶段 1：读取统一配置 ====================
    config = load_config()
    password = config["REMOTE_PASSWORD"]
    target = ssh_target()

    # ==================== 阶段 2：记录本机时间 ====================
    local_epoch = int(time.time())
    print(f"正在把 {target} 的时间设置为本机时间……", flush=True)

    # ==================== 阶段 3：设置远端系统时间 ====================
    password_line = shlex.quote(f"{password}\n")
    set_time_command = (
        f"printf %s {password_line} | "
        f"sudo -S date -s @{local_epoch} >/dev/null"
    )
    run_remote(set_time_command)

    # ==================== 阶段 4：回读并显示结果 ====================
    remote_time = run_remote("date --iso-8601=ns", capture_output=True)
    print(f"设置完成，远端时间：{remote_time}")


if __name__ == "__main__":
    try:
        main()
    except KeyError as error:
        raise SystemExit(f"错误：config.env 缺少配置项 {error.args[0]}") from error
    except FileNotFoundError as error:
        if error.filename == "sshpass":
            raise SystemExit(
                "错误：未安装 sshpass，请先执行：sudo apt install sshpass"
            ) from error
        raise
    except subprocess.CalledProcessError as error:
        raise SystemExit(f"错误：远端时间设置失败（退出码 {error.returncode}）") from error
