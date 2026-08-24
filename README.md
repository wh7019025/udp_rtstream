# UDP RTStream

面向 Rockchip 采集端和 NVIDIA/ROS 2 接收端的四路低延迟视频传输程序。采集端通过
V4L2 获取 NV12 DMA-BUF，由 Rockchip MPP 编码为 H.264，再通过 UDP 分片发送；接收端
统一使用 `receiver_core/` 完成协议解析和帧重组。

## 目录

- `capture/`：远端相机采集、MPP 编码、UDP 发送及内置延迟探测 responder
- `common/`：采集端和接收端共享的线协议
- `receiver_core/`：UDP 重组、延迟统计和无 ROS 调试程序
- `ros_wrapper/`：基于 receiver core 的 ROS 2 节点
- `remote_tool/`：远端同步、编译、部署及时间设置工具
- `time_sync_test/`：60 Hz 四路相机同步测试图

## 依赖

本机远程工具需要：

```bash
sudo apt install sshpass ptpd
```

远端需要 `gcc`、`make`、Rockchip MPP 开发库和 `ptpd`。NVIDIA 接收端还需要 FFmpeg、
SDL2、CUDA/NVDEC 相关开发环境；ROS 模式需要 ROS 2 和 `colcon`。

## 配置

所有运行参数集中在项目根目录的 `config.env`：

| 配置 | 用途 |
|---|---|
| `REMOTE_HOST/USER/PASSWORD` | 远端 SSH 连接 |
| `REMOTE_WORKSPACE` | 远端编译和运行目录 |
| `PTP_MASTER_INTERFACE` | 本机 PTP master 网卡 |
| `PTP_SLAVE_INTERFACE` | 远端 PTP slave 网卡，当前为 `eth0` |
| `UDP_PORT` | H.264 视频接收端口 |
| `DELAY_PROBE_PORT` | 启动前四时间戳探测端口 |
| `DELAY_CHECK_THRESHOLD_US` | 同步与链路延迟保守上限，当前为 `500 us` |
| `DELAY_CHECK_SAMPLES` | 启动检查采样数 |
| `DELAY_CHECK_INTERVAL_MS` | 探测包发送间隔 |
| `CAMERA_COUNT` | 相机数量 |
| `VIDEO_WIDTH/HEIGHT/FPS` | 视频尺寸和帧率 |
| `VIDEO_BITRATE/GOP` | 单路 H.264 码率和 GOP |
| `VIDEO_ROTATION` | MPP 硬件旋转角度：`0/90/180/270` |
| `STATS_WINDOW_FRAMES` | 延迟统计窗口帧数 |

如果接收端地址固定，可额外设置：

```bash
RECEIVER_HOST=192.168.1.165
```

未设置时，远端部署工具从当前 SSH 连接自动识别接收端 IP。修改配置后需要重新编译对应端。

## 推荐启动流程

### 1. 部署远端服务

```bash
./remote_tool/remote_deploy.py
```

部署工具会：

1. 同步 `config.env`、共享协议和 `capture/` 源码；
2. 在远端增量编译；
3. 安装并启动 `udp-rtstream-ptpd-slave.service`；
4. 安装并启动 `udp-rtstream.service`；
5. 设置两个服务开机自启并显示状态。

`udp_rtstream` 主进程同时负责视频发送和 UDP 延迟探测，不需要单独的 probe 服务。

查看远端日志：

```bash
ssh pi@远端IP 'journalctl -u udp-rtstream.service -f'
ssh pi@远端IP 'journalctl -u udp-rtstream-ptpd-slave.service -f'
```

### 2. 启动本机 PTP master

```bash
sudo ptpd -C -M -i eno1
```

网卡名应与 `PTP_MASTER_INTERFACE` 一致。远端日志进入 `PTP_SLAVE` 并收到 Sync 后，
再启动接收程序。

### 3. 启动接收端

无 ROS NVIDIA 调试程序：

```bash
make -C receiver_core nvidia
./receiver_core/debug_receiver_nvidia
```

程序以 2×2 布局显示四路画面，在左上角显示采集时间戳；按 `q` 退出。

ROS 2 节点：

```bash
make -C receiver_core ros
source ros_wrapper/install/setup.bash
ros2 run udp_rtstream_receiver receiver_node
```

每路相机发布：

- `/cameraN/image_raw`：`sensor_msgs/msg/Image`，RGB8 调试图像
- `/cameraN/image/compressed`：`sensor_msgs/msg/CompressedImage`，JPEG 图像
- `/cameraN/h264_video_stream`：`sensor_msgs/msg/CompressedImage`，原始 H.264 Annex-B AU

所有消息的 `header.stamp` 都直接使用采集端传来的 `pts_ns`。

## 启动延迟检查

调试程序和 ROS 节点在初始化解码器、显示和 publisher 前，会主动访问远端
`DELAY_PROBE_PORT`。检查使用独立实现的 NTP 式四时间戳交换，不依赖视频帧，也不依赖
外部 delay 测试程序。

程序从最低 RTT 样本估算：

```text
保守上界 = |两端时钟偏差| + 最小 RTT / 2
```

保守上界必须小于 `DELAY_CHECK_THRESHOLD_US`，当前为 `0.5 ms`。服务不可达、有效样本
不足或超过阈值时，接收端会输出原因并直接退出。例如：

```text
启动检查失败：同步与链路延迟保守上界 820 us，要求小于 500 us；请检查两端 PTP 和网络链路
```

## 开发和临时运行

只同步并在远端编译：

```bash
./remote_tool/remote_compile.py
```

以前台方式临时运行远端采集程序：

```bash
./remote_tool/remote_exec.py
```

该脚本会先停止 `udp-rtstream.service`，避免两个进程同时占用相机。按 `Ctrl+C` 退出后
不会自动恢复后台服务；需要恢复时重新运行 `remote_deploy.py`。

如需在 PTP 建立前粗略设置远端系统时间：

```bash
./remote_tool/set_remote_time.py
```

## 延迟统计

调试接收端每路累计 `STATS_WINDOW_FRAMES` 帧后输出 `latency_range_us`，包括编码、发送、
网络、重组、接收、解码、显示转换和端到端延迟的最小值与最大值，单位为微秒。

历史编码测试数据：

| 相机 | 50 Mbps 平均/最大 | 30 Mbps 平均/最大 |
|---|---:|---:|
| cam0 | 16.9 / 34.9 ms | 14.4 / 25.5 ms |
| cam1 | 12.8 / 35.2 ms | 11.1 / 26.1 ms |
| cam2 | 24.0 / 39.0 ms | 16.7 / 33.9 ms |

## 时间戳和四路同步测试

PWM 触发来自 RK3588 的触发引脚。`pts_ns` 来源于相机驱动返回的
`V4L2 buf.timestamp`；在确认 `buf.flags` 和驱动实现前，不能断言它精确对应曝光事件。

`time_sync_test` 使用系统绝对时间生成每秒 60 个唯一相位，用于让四路相机同时拍摄显示器，
对比帧号、相位格和真实时间戳：

```bash
make -C time_sync_test
./time_sync_test/time_sync_test
```

运行前应完成 PTP 同步并将显示器刷新率设为 60 Hz。程序默认全屏，按 `F` 切换全屏，
按 `Q` 或 `Esc` 退出；低亮度红色竖线用于观察滚动快门方向。

## 常见问题

- 接收端提示延迟检查失败：确认本机 PTP master 正在运行，远端状态为 `PTP_SLAVE`。
- `DELAY_PROBE_PORT` 无响应：确认 `udp-rtstream.service` 正常；responder 已集成在该进程中。
- 某路出现 `DQBUF timeout`：检查对应 `/dev/videoN`、相机供电、连接和驱动状态。
- 前台调试后没有后台视频：重新运行 `./remote_tool/remote_deploy.py`。
