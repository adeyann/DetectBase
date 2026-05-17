# /Main/CMakeLists.txt

cmake_minimum_required(VERSION 3.17)

set(EXECUTABLE_NAME DetectBase)

message("------------------------------------------------------------------")
message(":: ${EXECUTABLE_NAME}")
message("------------------------------------------------------------------")

# ---------------------------------------------------------------------------
# 소스 파일 수집 (BASE + DETECTOR 단일 구성)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE BASE_SRCS     "BASE/src/*.cpp")
file(GLOB_RECURSE DETECTOR_SRCS "DETECTOR/src/*.cpp")

# ---------------------------------------------------------------------------
# 실행 파일 생성
# ---------------------------------------------------------------------------
add_executable(${EXECUTABLE_NAME}
    ${BASE_SRCS}
    ${DETECTOR_SRCS}
)

# ---------------------------------------------------------------------------
# Include 경로 설정
# ---------------------------------------------------------------------------
target_include_directories(${EXECUTABLE_NAME}
PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/BASE/include
    ${CMAKE_CURRENT_SOURCE_DIR}/DETECTOR/include
)

# ---------------------------------------------------------------------------
# 라이브러리 링크 설정 (NPU 단일 구성)
# ---------------------------------------------------------------------------
target_link_libraries(${EXECUTABLE_NAME}
    PRIVATE
        # MgenSolution Libraries
        BasicLibs
        VisionCommon
        AbnormalActions
        REST
        GRPC
        RTSP_GST          # Phase 1: GStreamer replaces happytimesoft RTSP
        SocketIO
        Management
        Tracker
        EngineBase
        EngineNPU

        # System / External Libraries
        Threads::Threads
)

# ---------------------------------------------------------------------------
# 빌드 후 작업 (Post Build)
# ---------------------------------------------------------------------------
add_custom_command(
    TARGET ${EXECUTABLE_NAME}
    POST_BUILD

    # 1. 버전 파일 생성
    COMMAND echo "---------------------------" >  ${CMAKE_BINARY_DIR}/VERSION
    COMMAND echo "Version : ${DetectBase_VERSION}"   >> ${CMAKE_BINARY_DIR}/VERSION
    COMMAND echo "---------------------------" >> ${CMAKE_BINARY_DIR}/VERSION

    # 2. 심볼릭 링크 생성 (선택 사항)
    # 빌드된 실행 파일을 /DetectBase/bin/DetectBase 로 링크
    # (권한 문제가 발생할 수 있으므로 || true 로 에러 무시 처리)
    COMMAND ln -sf $<TARGET_FILE:${EXECUTABLE_NAME}> /DetectBase/bin/${EXECUTABLE_NAME} || true

    COMMENT "Generating VERSION file and symbolic link..."
)
