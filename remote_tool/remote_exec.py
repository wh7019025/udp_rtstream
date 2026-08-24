#!/usr/bin/env python3
"""编译并在远端电脑上运行 capture/udp_rtstream。"""

from __future__ import annotations

import shlex

from remote_compile import compile_remote
from remote_utils import load_config, remote_workspace, run_remote


def main() -> None:
    # ==================== 阶段 1：同步并在远端编译 ====================
    compile_remote()

    # ==================== 阶段 2：在远端运行发送程序 ====================
    config = load_config()
    udp_port = int(config["UDP_PORT"])
    remote_capture = shlex.quote(f"{remote_workspace()}/capture")
    remote_command = (
        f"cd {remote_capture} && "
        "receiver_ip=${SSH_CLIENT%% *} && "
        f"exec ./udp_rtstream \"$receiver_ip\" {udp_port}"
    )
    print(f"远端开始发送到本机 UDP {udp_port}；按 Ctrl+C 停止。", flush=True)
    run_remote(remote_command)


if __name__ == "__main__":
    main()
