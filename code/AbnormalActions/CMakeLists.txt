# /AbnormalActions/CMakeLists.txt

cmake_minimum_required(VERSION 3.17)

set(LIBRARY_NAME AbnormalActions)
message("LB | ${LIBRARY_NAME}")

# ---------------------------------------------------------------------------
# 소스 파일 수집 (자동 탐색)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE ABNORMAL_SOURCES CONFIGURE_DEPENDS
    "src/*.cpp"
)

# ---------------------------------------------------------------------------
# 라이브러리 생성 (STATIC)
# ---------------------------------------------------------------------------
# Root의 CMAKE_POSITION_INDEPENDENT_CODE ON 설정으로 인해 -fPIC가 자동 적용됩니다.
add_library(${LIBRARY_NAME} STATIC ${ABNORMAL_SOURCES})

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
    ${OpenCV_INCLUDE_DIRS}
)

# ---------------------------------------------------------------------------
# 라이브러리 링크
# ---------------------------------------------------------------------------
target_link_libraries(${LIBRARY_NAME}
PUBLIC
    BasicLibs
    Management
    Tracker
    ${OpenCV_LIBS}
)