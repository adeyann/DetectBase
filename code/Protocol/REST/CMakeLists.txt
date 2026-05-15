cmake_minimum_required(VERSION 3.17)

set(LIBRARY_NAME REST)
message("LB | ${LIBRARY_NAME}")

# ---------------------------------------------------------------------------
# 소스 파일 수집
# ---------------------------------------------------------------------------
file(GLOB REST_SOURCES "src/*.cpp")

# ---------------------------------------------------------------------------
# 라이브러리 생성 (STATIC)
# ---------------------------------------------------------------------------
# Root에서 CMAKE_POSITION_INDEPENDENT_CODE ON 설정을 했으므로,
# STATIC 라이브러리라도 자동으로 -fPIC가 적용됩니다.
add_library(${LIBRARY_NAME} STATIC ${REST_SOURCES})

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
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ---------------------------------------------------------------------------
# 외부 라이브러리 경로 (NPU 단일 구성)
# ---------------------------------------------------------------------------
set(REST_CLIENT_LIB_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lib/NPU/librestclient-cpp.a)
set(CURL_LIB_PATH        ${CMAKE_CURRENT_SOURCE_DIR}/lib/NPU/libcurl.a)

# ---------------------------------------------------------------------------
# 라이브러리 링크
# ---------------------------------------------------------------------------
target_link_libraries(${LIBRARY_NAME}
PRIVATE
    # Internal Libs
    BasicLibs

    # Prebuilt Static Libs
    ${REST_CLIENT_LIB_PATH}
    ${CURL_LIB_PATH}

    # System Libs
    Threads::Threads  # pthread 대신 Modern CMake 방식 사용
    z                 # zlib
)
