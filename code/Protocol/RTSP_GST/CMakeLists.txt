# /Protocol/RTSP_GST/CMakeLists.txt
#
# GStreamer 기반 RTSP receiver/client (Phase 1: rtspsrc + avdec_h264 + appsink).
# happytimesoft 외부 RTSP 라이브러리 (Protocol/RTSP/) 의 대체.

cmake_minimum_required(VERSION 3.17)

set(LIBRARY_NAME RTSP_GST)
message("LB | ${LIBRARY_NAME}")

# ---------------------------------------------------------------------------
# GStreamer 의존성 (pkg-config)
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-video-1.0    # GstRtspReceiver.cpp 의 gst_video_info_from_caps / GST_VIDEO_INFO_FORMAT 등 (#include <gst/video/video.h>)
)

# ---------------------------------------------------------------------------
# 소스 파일
# ---------------------------------------------------------------------------
file(GLOB_RECURSE RTSP_GST_SOURCES CONFIGURE_DEPENDS
    "src/*.cpp"
)

# ---------------------------------------------------------------------------
# 라이브러리 생성 (SHARED)
# ---------------------------------------------------------------------------
add_library(${LIBRARY_NAME} SHARED ${RTSP_GST_SOURCES})

include(GenerateExportHeader)
generate_export_header(${LIBRARY_NAME})

# ---------------------------------------------------------------------------
# Include 경로
# ---------------------------------------------------------------------------
target_include_directories(${LIBRARY_NAME}
PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ---------------------------------------------------------------------------
# 링크
#  - GStreamer (pkg-config IMPORTED_TARGET)
#  - BasicLibs (SafeQueue, MgenLogger 등)
#  - FFmpeg (AVFrame 정의 위해)
# ---------------------------------------------------------------------------
if(FFMPEG_FOUND)
    target_include_directories(${LIBRARY_NAME} PUBLIC ${FFMPEG_INCLUDE_DIRS})
endif()

target_link_libraries(${LIBRARY_NAME}
PUBLIC
    PkgConfig::GST
    BasicLibs
    ${FFMPEG_LIBRARIES}
PRIVATE
    Threads::Threads
)
