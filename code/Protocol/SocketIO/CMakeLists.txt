# Tier 2 Library
# 참조 가능 라이브러리 : BasicLibs

cmake_minimum_required(VERSION 3.17)

set(LIBRARY_NAME SocketIO)
message("LB | ${LIBRARY_NAME}")

# ---------------------------------------------------------------------------
# 소스 파일 수집
# ---------------------------------------------------------------------------
file(GLOB SIO_SOURCES "src/*.cpp")

# ---------------------------------------------------------------------------
# 라이브러리 생성 (STATIC)
# ---------------------------------------------------------------------------
# Root에서 CMAKE_POSITION_INDEPENDENT_CODE ON 설정했으므로 fPIC 자동 적용됨
add_library(${LIBRARY_NAME} STATIC ${SIO_SOURCES})

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
set(SIO_CLIENT_LIB_PATH     ${CMAKE_CURRENT_SOURCE_DIR}/lib/NPU/libsioclient.a)
set(SIO_CLIENT_TLS_LIB_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lib/NPU/libsioclient_tls.a)

# ---------------------------------------------------------------------------
# 라이브러리 링크
# ---------------------------------------------------------------------------
target_link_libraries(${LIBRARY_NAME}
PRIVATE
    # Internal Libs
    BasicLibs

    # Prebuilt Static Libs
    ${SIO_CLIENT_LIB_PATH}
    ${SIO_CLIENT_TLS_LIB_PATH}

    # System Libs
    Threads::Threads  # pthread
)
