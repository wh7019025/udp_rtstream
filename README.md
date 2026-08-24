# udp RTStream

## 带宽与MPP



| 相机 | 50 Mbps平均/最大 | 30 Mbps平均/最大 |
|---|---:|---:|
| cam0 | 16.9 / 34.9 ms | 14.4 / 25.5 ms |
| cam1 | 12.8 / 35.2 ms | 11.1 / 26.1 ms |
| cam2 | 24.0 / 39.0 ms | 16.7 / 33.9 ms |


## 时间同步

pwm 触发来自于 3588 的触发引脚

pts_ns 来源于相机驱动的 V4L2 buf.timestamp；但在检查 buf.flags 和相机驱动实现之前，不能断言它是精确的曝光事件时间戳。



## 配置

项目只有一个配置文件：`config.env`。远端连接、PTP 网卡、UDP 端口、相机路数、
分辨率、帧率、码率、GOP 和统计窗口都只在这里修改。Shell 脚本直接读取它，
Python 工具会读取并同步它，采集端和接收端构建系统会把它编译进程序。

```bash
# 修改配置后重新编译两端
./remote_tool/remote_compile.py
make -C receiver_core nvidia
```

```

./remote_tool/ptp_server.sh
```

`remote_tool/ptp_server.sh` 会同时启动本机 `eno1` PTP master 和远端 `10.42.0.244` 的
`eth1` PTP slave；按 `Ctrl+C` 会同时停止两端。远端日志位于
`/tmp/udp_rtstream_ptpd_slave.log`。

## 设置远程电脑时间

脚本默认通过 `pi` 用户和 `pi` 密码连接 `10.42.0.244`，并将远程电脑设置为本机当前时间。

```bash
sudo apt install sshpass
./remote_tool/set_remote_time.py
```

## 在远端编译和运行采集端

本机是唯一源码工作区。下面的脚本会把本机 `capture/` 源文件同步到远端
`pi@10.42.0.244:/home/pi/udp_rtstream/capture/`，编译和程序执行都发生在远端。

```bash
# 只同步并在远端编译
./remote_tool/remote_compile.py

# 同步、远端编译，然后远端执行
./remote_tool/remote_exec.py
```

`remote_tool/remote_exec.py` 会从 SSH 连接自动获得本机 IP，并让发送程序向本机 UDP `5000`
端口发送 `/dev/video0`～`/dev/video3` 四路画面。发送端默认每路 30 Mbps、
GOP=1（每帧独立关键帧），用于降低四路编码和 UDP 突发压力。程序保持在前台运行，可按
`Ctrl+C` 停止。

## 接收端：receiver core 与 ROS wrapper

`receiver_core/` 是唯一的 UDP 协议解析、分片重组和解码底层，并包含无 ROS 调试程序。
`ros_wrapper/` 只负责调用该底层并将图像包装成 ROS 2 消息，不重复实现接收逻辑。

无 ROS NVIDIA 调试程序以 2×2 布局显示四路画面，在每路画面左上角叠加摄像头采集时间戳
（Unix `秒.毫秒`），按 `q` 退出：

```bash
cd receiver_core
make nvidia
./debug_receiver_nvidia
```

调试程序每路累计 300 帧后会打印一条 `latency_range_us`，给出编码、发送、网络、
重组、接收、解码、显示转换和端到端各阶段在该窗口内的最小/最大延迟，单位为微秒。

日常 ROS 2 使用：

```bash
cd receiver_core
make ros
source ../ros_wrapper/install/setup.bash
ros2 run udp_rtstream_receiver receiver_node
```

每路相机发布三类话题，所有消息的 `header.stamp` 都直接使用采集端传来的 `pts_ns`：

- `/cameraN/image_raw`：`sensor_msgs/msg/Image`，RGB8 调试图像
- `/cameraN/image/compressed`：`sensor_msgs/msg/CompressedImage`，JPEG 图像
- `/cameraN/h264_video_stream`：`sensor_msgs/msg/CompressedImage`，`format=h264`，
  数据为采集端原始 H.264 Annex-B AU，不进行二次编码





# 厂商资料总结


## 快速查看相机是否工作正常


```bash

```

## 60 Hz 四路时间同步测试图

测试图使用系统绝对时间生成每秒 60 个唯一相位。四路相机同时拍摄显示器后，
对比画面右上角帧号、60 格位置和真实时间戳，即可检查是否采到同一帧。
运行前应先通过 PTP/PTPd 同步系统时间，并将显示器刷新率设置为 60 Hz。

```bash
cd time_sync_test
make
./time_sync_test
```

程序默认全屏运行，按 `F` 切换窗口/全屏，按 `Q`（或 `Esc`）退出。低亮度红色竖线用于观察
滚动快门方向。程序采用稳定低亮度背景，不进行整屏闪光。画面底部显示当前
测试帧的真实 Unix 时间戳，格式为 `秒.毫秒`。
