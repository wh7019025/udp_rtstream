#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/compressed_image.hpp>
#include <sensor_msgs/msg/image.hpp>

#include "jpeg_encoder.h"
#include "nvdec_dec.h"
#include "receiver_core.h"

#ifndef DEFAULT_UDP_PORT
#error "UDP_PORT must be supplied by config.env through CMake"
#endif
#ifndef MAX_CAM
#error "CAMERA_COUNT must be supplied by config.env through CMake"
#endif

class ReceiverNode final : public rclcpp::Node {
public:
    ReceiverNode() : Node("udp_rtstream_receiver")
    {
        // ==================== 阶段 1：初始化共享底层和 ROS 发布器 ====================
        receiver_ = receiver_core_create(DEFAULT_UDP_PORT, MAX_CAM);
        if (!receiver_)
            throw std::runtime_error("receiver core init failed");

        decoders_.resize(MAX_CAM, nullptr);
        raw_publishers_.reserve(MAX_CAM);
        jpeg_publishers_.reserve(MAX_CAM);
        h264_publishers_.reserve(MAX_CAM);
        for (int cam_id = 0; cam_id < MAX_CAM; ++cam_id) {
            decoders_[cam_id] = nvdec_dec_create();
            if (!decoders_[cam_id])
                throw std::runtime_error("NVDEC init failed for cam" +
                                         std::to_string(cam_id));
            const std::string camera = "camera" + std::to_string(cam_id);
            raw_publishers_.push_back(create_publisher<sensor_msgs::msg::Image>(
                camera + "/image_raw",
                rclcpp::SensorDataQoS()));
            jpeg_publishers_.push_back(
                create_publisher<sensor_msgs::msg::CompressedImage>(
                camera + "/image/compressed", rclcpp::SensorDataQoS()));
            h264_publishers_.push_back(
                create_publisher<sensor_msgs::msg::CompressedImage>(
                camera + "/h264_video_stream",
                rclcpp::SensorDataQoS()));
        }

        running_ = true;
        receive_thread_ = std::thread([this] { receive_loop(); });
        RCLCPP_INFO(get_logger(), "listening UDP %d, publishing %d cameras",
                    DEFAULT_UDP_PORT, MAX_CAM);
    }

    ~ReceiverNode() override
    {
        // ==================== 阶段 4：停止线程并释放底层资源 ====================
        running_ = false;
        if (receive_thread_.joinable())
            receive_thread_.join();
        for (auto *decoder : decoders_)
            nvdec_dec_destroy(decoder);
        receiver_core_destroy(receiver_);
    }

private:
    static void frame_callback(const ReceiverFrame *frame, void *user)
    {
        static_cast<ReceiverNode *>(user)->publish_frame(*frame);
    }

    void receive_loop()
    {
        // ==================== 阶段 2：持续轮询同一套 UDP 重组底层 ====================
        while (running_ && rclcpp::ok()) {
            if (receiver_core_poll(receiver_, 50, frame_callback, this) < 0) {
                RCLCPP_ERROR(get_logger(), "receiver poll failed");
                break;
            }
        }
    }

    void publish_frame(const ReceiverFrame &frame)
    {
        // ==================== 阶段 3：直接发布原始 H.264 Annex-B AU ====================
        std_msgs::msg::Header header;
        header.stamp.sec = static_cast<int32_t>(frame.pts_ns / 1000000000ULL);
        header.stamp.nanosec = static_cast<uint32_t>(frame.pts_ns % 1000000000ULL);
        header.frame_id = "camera" + std::to_string(frame.cam_id);

        auto &h264_publisher = h264_publishers_[frame.cam_id];
        if (h264_publisher->get_subscription_count() > 0) {
            sensor_msgs::msg::CompressedImage h264_message;
            h264_message.header = header;
            h264_message.format = "h264";
            h264_message.data.assign(frame.data, frame.data + frame.size);
            h264_publisher->publish(std::move(h264_message));
        }

        // ==================== 阶段 4：解码一次并复用 RGB 图像 ====================
        const bool publish_raw =
            raw_publishers_[frame.cam_id]->get_subscription_count() > 0;
        const bool publish_jpeg =
            jpeg_publishers_[frame.cam_id]->get_subscription_count() > 0;
        if (!publish_raw && !publish_jpeg)
            return;

        int source_width = 0;
        int source_height = 0;
        if (nvdec_dec_decode(decoders_[frame.cam_id], frame.data, frame.size,
                             &source_width, &source_height) != 0)
            return;

        int width = 0;
        int height = 0;
        const uint8_t *rgb = nvdec_dec_rgb(decoders_[frame.cam_id],
                                           &width, &height);
        if (!rgb || width <= 0 || height <= 0)
            return;

        if (publish_raw) {
            sensor_msgs::msg::Image raw_message;
            raw_message.header = header;
            raw_message.height = static_cast<uint32_t>(height);
            raw_message.width = static_cast<uint32_t>(width);
            raw_message.encoding = "rgb8";
            raw_message.is_bigendian = false;
            raw_message.step = static_cast<uint32_t>(width * 3);
            raw_message.data.assign(rgb, rgb +
                static_cast<size_t>(raw_message.step) * raw_message.height);
            raw_publishers_[frame.cam_id]->publish(std::move(raw_message));
        }

        // ==================== 阶段 5：编码并发布 JPEG CompressedImage ====================
        if (publish_jpeg) {
            sensor_msgs::msg::CompressedImage jpeg_message;
            jpeg_message.header = header;
            jpeg_message.format = "jpeg";
            if (encode_rgb_to_jpeg(rgb, width, height, jpeg_message.data))
                jpeg_publishers_[frame.cam_id]->publish(std::move(jpeg_message));
        }
    }

    ReceiverCore *receiver_{nullptr};
    std::vector<NvDecCtx *> decoders_;
    std::vector<rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr>
        raw_publishers_;
    std::vector<rclcpp::Publisher<sensor_msgs::msg::CompressedImage>::SharedPtr>
        jpeg_publishers_;
    std::vector<rclcpp::Publisher<sensor_msgs::msg::CompressedImage>::SharedPtr>
        h264_publishers_;
    std::atomic_bool running_{false};
    std::thread receive_thread_;
};

int main(int argc, char **argv)
{
    rclcpp::init(argc, argv);
    try {
        rclcpp::spin(std::make_shared<ReceiverNode>());
    } catch (const std::exception &error) {
        std::fprintf(stderr, "receiver node failed: %s\n", error.what());
        rclcpp::shutdown();
        return 1;
    }
    rclcpp::shutdown();
    return 0;
}
