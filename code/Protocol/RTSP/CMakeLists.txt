# /Protocol/RTSP/CMakeLists.txt

cmake_minimum_required(VERSION 3.17)

set(LIBRARY_NAME RTSP)
message("LB | ${LIBRARY_NAME}")

# ---------------------------------------------------------------------------
# 소스 파일 수집 (자동 탐색)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE RTSP_SOURCES CONFIGURE_DEPENDS
    "core/src/*.cpp"
    "http/src/*.cpp"
    "media/src/*.cpp"
    "rtp/src/*.cpp"
    "rtsp/src/*.cpp"
)

# ---------------------------------------------------------------------------
# 라이브러리 생성
# ---------------------------------------------------------------------------
add_library(${LIBRARY_NAME} SHARED ${RTSP_SOURCES})

# ---------------------------------------------------------------------------
# Export Header
# ---------------------------------------------------------------------------
include(GenerateExportHeader)
generate_export_header(${LIBRARY_NAME})

# ---------------------------------------------------------------------------
# Include 경로 설정
# ---------------------------------------------------------------------------
target_include_directories(${LIBRARY_NAME}
PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR} # 전체 경로 포함 (상호 참조 편의성)
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/http/include
    ${CMAKE_CURRENT_SOURCE_DIR}/media/include
    ${CMAKE_CURRENT_SOURCE_DIR}/rtp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/rtsp/include
PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core/src
    ${CMAKE_CURRENT_SOURCE_DIR}/http/src
    ${CMAKE_CURRENT_SOURCE_DIR}/media/src
    ${CMAKE_CURRENT_SOURCE_DIR}/rtp/src
    ${CMAKE_CURRENT_SOURCE_DIR}/rtsp/src
)

# ---------------------------------------------------------------------------
# 라이브러리 링크
# ---------------------------------------------------------------------------
# Root에서 찾은 FFmpeg 변수 사용
if(FFMPEG_FOUND)
    target_include_directories(${LIBRARY_NAME} PUBLIC ${FFMPEG_INCLUDE_DIRS})
endif()

target_link_libraries(${LIBRARY_NAME}
PUBLIC
    BasicLibs
    ${FFMPEG_LIBRARIES}
PRIVATE
    x264
    x265
    opus
    asound
    Threads::Threads # pthread 대신 Modern CMake 방식
)