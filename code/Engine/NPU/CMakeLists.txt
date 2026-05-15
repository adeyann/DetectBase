cmake_minimum_required(VERSION 3.17)

set(LIBRARY_NAME EngineNPU)
message("LB | ${LIBRARY_NAME}")

# ---------------------------------------------------------------------------
# 컴파일 정의 (API_EXPORTS)
# ---------------------------------------------------------------------------
add_compile_definitions(API_EXPORTS)

# ---------------------------------------------------------------------------
# RKNN API 경로 설정
# ---------------------------------------------------------------------------
set(RKNN_API_PATH "${CMAKE_CURRENT_SOURCE_DIR}/librknn_api")

# 아키텍처에 따라 경로가 달라질 수 있으나, 현재 aarch64로 고정되어 있음
set(RKNN_RT_DIR   "${RKNN_API_PATH}/aarch64")
set(RKNNRT_SO     "${RKNN_RT_DIR}/librknnrt.so")
set(RKNNAPI_SO    "${RKNN_RT_DIR}/librknn_api.so")

# ---------------------------------------------------------------------------
# 소스 파일 수집 (자동 탐색)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE ENGINE_NPU_SOURCES CONFIGURE_DEPENDS
    "EngineBuilder/*.cpp"
    "YoloV5_Torch_Onnx_RKNN_NPU/*.cpp"
)

# ---------------------------------------------------------------------------
# 엔진 라이브러리 생성
# ---------------------------------------------------------------------------
add_library(${LIBRARY_NAME} SHARED ${ENGINE_NPU_SOURCES})

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
    ${RKNN_API_PATH}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/EngineBuilder
    ${CMAKE_CURRENT_SOURCE_DIR}/YoloV5_Torch_Onnx_RKNN_NPU

    # Root에서 찾은 OpenCV 경로 (NPU 전처리용)
    ${OpenCV_INCLUDE_DIRS}
)

# ---------------------------------------------------------------------------
# RKNN .so 를 Imported 타깃으로 선언
# ---------------------------------------------------------------------------
# 1. rknnrt
add_library(rknnrt SHARED IMPORTED GLOBAL)
set_target_properties(rknnrt PROPERTIES
    IMPORTED_LOCATION             "${RKNNRT_SO}"
    INTERFACE_INCLUDE_DIRECTORIES "${RKNN_API_PATH}/include"
)

# 2. rknnapi
add_library(rknnapi SHARED IMPORTED GLOBAL)
set_target_properties(rknnapi PROPERTIES
    IMPORTED_LOCATION             "${RKNNAPI_SO}"
)

# ---------------------------------------------------------------------------
# 라이브러리 링크
# ---------------------------------------------------------------------------
target_link_libraries(${LIBRARY_NAME}
PRIVATE
    BasicLibs
    EngineBase
    ${OpenCV_LIBS}
    Threads::Threads
    rknnrt
    rknnapi
)

# ---------------------------------------------------------------------------
# POST_BUILD: 빌드 산출물 폴더로 .so 복사
# ---------------------------------------------------------------------------
# NPU 실행 환경을 위해 RKNN 라이브러리를 실행 파일 옆으로 복사합니다.
add_custom_command(TARGET ${LIBRARY_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rknnrt>  $<TARGET_FILE_DIR:${LIBRARY_NAME}>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rknnapi> $<TARGET_FILE_DIR:${LIBRARY_NAME}>
)

# ---------------------------------------------------------------------------
# RPATH 설정
# ---------------------------------------------------------------------------
# libEngineNPU.so 위치($ORIGIN)에서 종속 .so 탐색
set_property(TARGET ${LIBRARY_NAME} PROPERTY BUILD_RPATH   "\$ORIGIN")
set_property(TARGET ${LIBRARY_NAME} PROPERTY INSTALL_RPATH "\$ORIGIN")
