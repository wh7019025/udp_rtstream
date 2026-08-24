#!/usr/bin/env python3
"""同步本机 capture 源码，并在远端电脑上编译。"""

from __future__ import annotations

import shlex

from remote_utils import (
    load_config,
    remote_workspace,
    run_remote,
    sync_capture_sources,
)


def compile_remote() -> None:
    # ==================== 阶段 1：同步唯一源码与配置 ====================
    config = load_config()
    remote_host = config["REMOTE_HOST"]
    print(f"[1/2] 同步到 {remote_host}……", flush=True)
    sync_capture_sources()

    # ==================== 阶段 2：在远端增量编译 ====================
    remote_capture = f"{remote_workspace()}/capture"
    print("[2/2] 在远端执行 make……", flush=True)
    quoted_capture = shlex.quote(remote_capture)
    run_remote(
        f"make -C {quoted_capture} all && "
        f"test -x {quoted_capture}/udp_rtstream"
    )
    print("远端编译完成。", flush=True)


if __name__ == "__main__":
    compile_remote()
