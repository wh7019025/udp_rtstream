cmake_minimum_required(VERSION 3.16)
project(udp_rtstream_receiver LANGUAGES C CXX)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavcodec libavutil libswscale)
pkg_check_modules(JPEG REQUIRED IMPORTED_TARGET libjpeg)

# ==================== 阶段 1：读取项目唯一配置 ====================
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../config.env" CONFIG_LINES)
foreach(CONFIG_LINE IN LISTS CONFIG_LINES)
  if(CONFIG_LINE MATCHES "^([A-Z0-9_]+)=(.*)$")
    set(${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
  endif()
endforeach()

# ==================== 阶段 2：构建共享接收底层与 NVDEC ====================
add_library(receiver_core STATIC
  ../receiver_core/receiver_core.c
  ../receiver_core/delay_probe.c
  ../receiver_core/nvdec_dec.c)
target_include_directories(receiver_core PUBLIC ../receiver_core ..)
target_link_libraries(receiver_core PUBLIC PkgConfig::FFMPEG)

# ==================== 阶段 3：构建 ROS 2 薄封装节点 ====================
add_executable(receiver_node
  src/receiver_node.cpp
  src/jpeg_encoder.cpp)
target_compile_features(receiver_node PRIVATE cxx_std_17)
target_compile_definitions(receiver_node PRIVATE
  DEFAULT_UDP_PORT=${UDP_PORT}
  MAX_CAM=${CAMERA_COUNT}
  DELAY_PROBE_HOST=\"${REMOTE_HOST}\"
  DELAY_PROBE_PORT=${DELAY_PROBE_PORT}
  DELAY_CHECK_THRESHOLD_US=${DELAY_CHECK_THRESHOLD_US}
  DELAY_CHECK_SAMPLES=${DELAY_CHECK_SAMPLES}
  DELAY_CHECK_INTERVAL_MS=${DELAY_CHECK_INTERVAL_MS})
target_link_libraries(receiver_node receiver_core PkgConfig::JPEG)
ament_target_dependencies(receiver_node rclcpp sensor_msgs)

install(TARGETS receiver_node DESTINATION lib/${PROJECT_NAME})
ament_package()
