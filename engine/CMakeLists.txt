cmake_minimum_required(VERSION 3.4.1)

project(argus_engine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "-pthread")

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
# OrangePi or other for development
    find_package(Boost REQUIRED COMPONENTS filesystem system)
    find_package(OpenCV REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-pbutils-1.0)

    include_directories(${GST_INCLUDE_DIRS})
    link_directories(${GST_LIBRARY_DIRS})

    set(MY_LIBS ${Boost_LIBRARIES} ${OpenCV_LIBS} ${GST_LIBRARIES})
else()
# cross-compile from x86 for rk3588/XT5, rk3568/LS5
    # Only apply cross-compile flags if we're actually cross-compiling (not building tests)
    if(NOT BUILD_TESTS)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-poison-system-directories")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wpoison-system-directories")
    endif()

    set(BOOST_ROOT ${CMAKE_SOURCE_DIR}/sdk/sysroots/aarch64-oe-linux/usr)

    include_directories(${OECORE_TARGET_SYSROOT}/usr/include)
    include_directories(${OECORE_TARGET_SYSROOT}/usr/include/opencv4)
    include_directories(${OECORE_TARGET_SYSROOT}/usr/include/opencv4/opencv2)
    include_directories(${OECORE_TARGET_SYSROOT}/usr/include/rga)
    include_directories(${OECORE_TARGET_SYSROOT}/usr/include/gstreamer-1.0)
    include_directories(${OECORE_TARGET_SYSROOT}/usr/include/glib-2.0)
    include_directories(${OECORE_TARGET_SYSROOT}/usr/lib/glib-2.0/include)

    link_directories(${OECORE_TARGET_SYSROOT}/usr/lib)
    set(MY_LIBS
        opencv_core opencv_aruco opencv_bgsegm opencv_calib3d
        opencv_ccalib opencv_datasets opencv_features2d opencv_gapi
        opencv_hfs opencv_imgcodecs opencv_img_hash opencv_imgproc
        opencv_line_descriptor opencv_optflow opencv_photo opencv_quality
        opencv_rapid opencv_rgbd opencv_saliency opencv_shape
        opencv_stereo opencv_stitching opencv_superres opencv_surface_matching
        opencv_tracking opencv_ts opencv_videoio opencv_video
        opencv_videostab opencv_xfeatures2d opencv_ximgproc opencv_xphoto
        gstreamer-1.0 gstapp-1.0 gstbase-1.0 gstvideo-1.0 gstpbutils-1.0 gobject-2.0 glib-2.0
    )
endif()

# ---- Find the SDK's ext-bundle tarball ----
# Primary (correct) location when the SDK is sourced:
set(EXT_BUNDLE_TGZ "${OECORE_TARGET_SYSROOT}/usr/share/ext-bundle/ext-bundle-payload.tar.gz")

# Fallback (in-tree SDK sysroot, mirrors argus's fallback)
if(NOT EXISTS "${EXT_BUNDLE_TGZ}")
  set(EXT_BUNDLE_TGZ "${CMAKE_SOURCE_DIR}/sdk/sysroots/aarch64-oe-linux/usr/share/ext-bundle/ext-bundle-payload.tar.gz")
endif()

if(NOT EXISTS "${EXT_BUNDLE_TGZ}")
  message(WARNING "ext-bundle tarball not found: ${EXT_BUNDLE_TGZ}\n"
                  "GStreamer runtime/plugins will NOT be bundled with the extension.")
endif()

# Set the build type to Debug to include debug symbols
set(CMAKE_BUILD_TYPE Debug)

# Add the -g flag to the compiler options
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")

# skip 3rd-party lib dependencies
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--allow-shlib-undefined")

# install target and libraries
if(NOT DEFINED TARGET_SOC)
  set(TARGET_SOC rk3588)
endif()

if(NOT DEFINED LIB_ARCH)
  set(LIB_ARCH aarch64)
endif()

string(TOLOWER ${CMAKE_SYSTEM_NAME} cmake_system_name_lower)
string(TOLOWER ${LIB_ARCH} lib_arch_lower)
set(CMAKE_INSTALL_PREFIX ${CMAKE_SOURCE_DIR}/install/${TARGET_SOC}_${cmake_system_name_lower}_${lib_arch_lower})

set(CMAKE_SKIP_INSTALL_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")

# rknn api
set(LIB_ARCH aarch64)
set(RKNN_RT_LIB ${CMAKE_SOURCE_DIR}/include/librknnrt.so)

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
  # Native build - try to find RGA, make optional for non-Rockchip boards
  find_library(RGA_LIB NAMES rga rga2 PATHS /usr/lib /usr/local/lib /usr/lib/aarch64-linux-gnu)
  if(RGA_LIB)
    message(STATUS "Found RGA: ${RGA_LIB}")
    set(HAVE_RGA TRUE)
  else()
    message(WARNING "RGA library not found - RGA acceleration disabled for native build")
    set(HAVE_RGA FALSE)
    set(RGA_LIB "")
  endif()
else()
  # Cross-compile - use SDK sysroot
  set(RGA_LIB ${OECORE_TARGET_SYSROOT}/usr/lib/librga.so)
  set(HAVE_RGA TRUE)
endif()

include_directories(${CMAKE_SOURCE_DIR}/include/3rdparty)
include_directories(${RGA_PATH}/include)

# opencv
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
  find_package(OpenCV REQUIRED)

  # TurboJPEG - Ubuntu doesn't provide CMake config, find manually
  find_path(TURBOJPEG_INCLUDE_DIR turbojpeg.h)
  find_library(TURBOJPEG_LIB turbojpeg)
  if(NOT TURBOJPEG_INCLUDE_DIR OR NOT TURBOJPEG_LIB)
    message(FATAL_ERROR "TurboJPEG not found. Install with: sudo apt install libturbojpeg0-dev")
  endif()
  include_directories(${TURBOJPEG_INCLUDE_DIR})
  message(STATUS "Found TurboJPEG: ${TURBOJPEG_LIB}")
else()
  message(STATUS "OECORE_TARGET_SYSROOT: ${OECORE_TARGET_SYSROOT}")
  include_directories(${OECORE_TARGET_SYSROOT}/usr/include)
  include_directories(${OECORE_TARGET_SYSROOT}/usr/include/opencv4)
  include_directories(${OECORE_TARGET_SYSROOT}/usr/include/rga)
  include_directories(${OECORE_TARGET_SYSROOT}/usr/include/turbojpeg)
endif()

set(CMAKE_INSTALL_RPATH "lib")

if(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
  get_filename_component(OECORE_TARGET_SYSROOT_ABS "${OECORE_TARGET_SYSROOT}" ABSOLUTE)
  message(STATUS "OECORE_TARGET_SYSROOT_ABS: ${OECORE_TARGET_SYSROOT_ABS}")
  set(OpenCV_LIBS
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_core.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_aruco.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_bgsegm.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_calib3d.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_ccalib.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_datasets.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_features2d.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_gapi.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_hfs.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_highgui.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_imgcodecs.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_img_hash.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_imgproc.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_line_descriptor.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_optflow.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_photo.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_quality.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_rapid.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_rgbd.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_saliency.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_shape.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_stereo.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_stitching.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_superres.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_surface_matching.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_tracking.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_ts.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_videoio.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_video.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_videostab.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_xfeatures2d.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_ximgproc.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_xphoto.so
  )
  set(TURBOJPEG_LIB ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libturbojpeg.so)
endif()

set(CMAKE_INSTALL_RPATH "lib")

include_directories(${CMAKE_SOURCE_DIR}/include)

# Enable MQTT publisher support
add_definitions(-DHAVE_MOSQUITTO=1)

# ON by default; pass -DENABLE_YOLOX=OFF for a gaze-only binary (no yolox_s.rknn required).
option(ENABLE_YOLOX "Include YOLOX object detection" ON)
if(NOT ENABLE_YOLOX)
    message(STATUS "YOLOX DISABLED - gaze-only build")
else()
    add_definitions(-DYOLOX_ENABLED=1)
    message(STATUS "YOLOX ENABLED")
endif()

add_executable(argus-engine
        src/main.cpp
        src/attention.cpp
        src/analysis/light_analyzer.cpp
        src/analysis/light_json.cpp
        src/file_utils.c
        src/image_utils.c
        src/image_drawing.c
        src/inference.cpp
        src/publisher.cpp
        src/retinaface.cc
        src/npu_core.cc
        src/utils.cc
        src/postprocess.cc
        src/queue.cpp
        # Config module
        src/config/configuration.cpp
        src/config/config_monitor.cpp
        src/config/model_spec.cpp
        src/config/processing_params.cpp
        src/config/publisher_config.cpp
        src/config/runtime_settings.cpp
        # Input module
        src/input/input_factory.cpp
        src/input/input_file.cpp
        src/input/input_rtsp.cpp
        src/input/input_usb.cpp
        src/input/media_probe.cpp
        # Common utilities
        src/common/device_info.cpp
        # Metrics module
        src/metrics/file_logger.cpp
        src/metrics/log_global.cpp
        # Models module
        src/models/model_factory.cpp
        src/models/model_runner_retinaface.cpp
        $<$<BOOL:${ENABLE_YOLOX}>:src/models/model_runner_yolox.cpp>
        # Orchestration module
        src/orchestration/inference_worker.cpp
        src/orchestration/orchestrator.cpp
        src/orchestration/visualization.cpp
        # Output module
        src/output/annotator.cpp
        src/output/async_publisher.cpp
        src/output/face_blur.cpp
        src/output/frame_writer.cpp
        src/output/mqtt_broker.cpp
        src/output/mqtt_publisher.cpp
        # Pipeline module
        src/pipeline/capture_worker.cpp
        src/pipeline/inference_stage.cpp
        src/pipeline/postprocess_stage.cpp
        src/pipeline/preprocess_stage.cpp
        # Resources module
        src/resources/mem_pool.cpp
        src/resources/resource_manager.cpp
        src/resources/rga_context.cpp
        src/resources/tensor_manager.cpp
        # Tracking module
        src/tracking/tracker.cpp
        src/tracking/byte_tracker_lite.cpp
        # Utility module
        src/util/util.cpp
        $<$<BOOL:${ENABLE_YOLOX}>:src/yolox.cc>
)

target_link_libraries(argus-engine
  ${RKNN_RT_LIB}
  ${OpenCV_LIBS}
  ${RGA_LIB}
  ${TURBOJPEG_LIB}
  ${MY_LIBS}
  mosquitto
)

# Convert TARGET_SOC to uppercase for SOC_DIR
string(TOUPPER ${TARGET_SOC} SOC_DIR)

# install target and libraries
set(CMAKE_INSTALL_PREFIX ${CMAKE_SOURCE_DIR}/install/${SOC_DIR})
install(TARGETS argus-engine DESTINATION ./)

# Unit tests (added in a later task; off by default until engine/tests/ exists)
option(BUILD_TESTS "Build unit tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
