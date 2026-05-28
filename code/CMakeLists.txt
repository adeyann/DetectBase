cmake_minimum_required( VERSION 3.17 )

# ---------------------------------------------------------------------------
# Project Information
# ---------------------------------------------------------------------------
project( DetectBase VERSION 0.1.27 LANGUAGES C CXX )

# ---------------------------------------------------------------------------
# 고정 빌드 파라미터 (NPU + DETECTOR 단일 구성)
# ---------------------------------------------------------------------------
set( SERVICE_IMPLEMENT_TYPE "DETECTOR" )

# ---------------------------------------------------------------------------
# Const Values
# ---------------------------------------------------------------------------
set( PROJECT_IMPL ${SERVICE_IMPLEMENT_TYPE} )
set( PROJECT_DESC "Mgensolution's AI Analytics" )
set( PROJECT_AUTH "KIM UnYoung" )

# ---------------------------------------------------------------------------
# C++ Language & Compiler Settings
# ---------------------------------------------------------------------------
set( CMAKE_CXX_STANDARD             17 )
set( CMAKE_CXX_STANDARD_REQUIRED     ON )
set( CMAKE_EXPORT_COMPILE_COMMANDS   ON )
set( CMAKE_POSITION_INDEPENDENT_CODE ON ) # fPIC 설정 (Shared Library 필수)

# ---------------------------------------------------------------------------
# 전역 컴파일 옵션 (모든 하위 디렉토리에 자동 전파됨)
# ---------------------------------------------------------------------------
add_compile_options(
    "$<$<CONFIG:Release>:-O2>"
    # NEW-7: generator expression 안 공백 분리 결함 fix.
    #        "-O0 -g" 한 문자열로 들어가면 cc1plus 가 -O 옵션 깨짐 ('-O' should be ...).
    #        세미콜론으로 분리하거나 별도 옵션으로 나눠야 함.
    "$<$<CONFIG:Debug>:-O0>"
    "$<$<CONFIG:Debug>:-g>"
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-deprecated-declarations
    -Wno-psabi  # GCC 10.1 std::pair ABI 변경 알림 — 단일 컴파일러 환경에선 무해
)

# ---------------------------------------------------------------------------
# Configure Header File
# ---------------------------------------------------------------------------
configure_file( ProgramConfig.h.in ProgramConfig.h )
include_directories( "${PROJECT_BINARY_DIR}" )

# ---------------------------------------------------------------------------
# Define Build Mode
# ---------------------------------------------------------------------------
if( CMAKE_BUILD_TYPE STREQUAL "Release" )
    add_compile_definitions( RELEASE_MODE )
elseif( CMAKE_BUILD_TYPE STREQUAL "Debug" )
    add_compile_definitions( DEBUG_MODE )
endif()

# ---------------------------------------------------------------------------
# Print Project Summary
# ---------------------------------------------------------------------------
message( "------------------------------------------------------------------" )
message( ".. ${PROJECT_DESC} | ${PROJECT_IMPL} | NPU | ${CMAKE_BUILD_TYPE}" )
message( "------------------------------------------------------------------" )
message( ".. ${PROJECT_AUTH}" )
message( "------------------------------------------------------------------" )

# ---------------------------------------------------------------------------
# Global Compile Definitions (RTSP)
# ---------------------------------------------------------------------------
add_compile_definitions(
    EPOLL
    PROXY
    RTSP_DEVICE
    RTSP_FILE
    RTSP_METADATA
)

# ---------------------------------------------------------------------------
# Custom CMake Modules (CMakeFinder)
# ---------------------------------------------------------------------------
list( APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMakeFinder" )

message( "------------------------------------------------------------------" )
message( ".. Find Packages Run" )
message( "------------------------------------------------------------------" )

# ---------------------------------------------------------------------------
# 공통 패키지
# ---------------------------------------------------------------------------
find_package( Threads REQUIRED )

# ---------------------------------------------------------------------------
# NPU 빌드 의존성
# ---------------------------------------------------------------------------
find_package( OpenCV REQUIRED )

# FFmpeg : RTSP/VisionCommon 모듈 + Main 실행파일이 av*/sws*/swr* 함수 사용
find_package( FFmpeg REQUIRED COMPONENTS AVCODEC AVUTIL AVFORMAT SWSCALE SWRESAMPLE )

message( "------------------------------------------------------------------" )
message( ".. Find Packages Fin" )
message( "------------------------------------------------------------------" )

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
# 1. 기초 라이브러리
add_subdirectory( ${CMAKE_CURRENT_SOURCE_DIR}/BasicLibs )

# 2. 영상 처리 공통 모듈
add_subdirectory( ${CMAKE_CURRENT_SOURCE_DIR}/VisionCommon )

# 3. 프로토콜
add_subdirectory( ${CMAKE_CURRENT_SOURCE_DIR}/Protocol/REST )
add_subdirectory( ${CMAKE_CURRENT_SOURCE_DIR}/Protocol/GRPC )
add_subdirectory( ${CMAKE_CURRENT_SOURCE_DIR}/Protocol/SocketIO )
add_subdirectory( ${CMAKE_CURRENT_SOURCE_DIR}/Protocol/RTSP_GST )    # Phase 1: GStreamer replaces happytimesoft RTSP module

# 4. 매니지먼트 및 알고리즘
add_subdirectory( ${CMAKE_CURRENT_SOURCE_DIR}/Management )
add_subdirectory( ${CMAKE_CURRENT_SOURCE_DIR}/Tracker )
add_subdirectory( ${CMAKE_CURRENT_SOURCE_DIR}/AbnormalActions )

# 5. 추론 엔진 (NPU 단일)
add_subdirectory( ${CMAKE_CURRENT_SOURCE_DIR}/Engine/EngineBase )
add_subdirectory( ${CMAKE_CURRENT_SOURCE_DIR}/Engine/NPU )

# 6. 메인
add_subdirectory( ${CMAKE_CURRENT_SOURCE_DIR}/Main )
