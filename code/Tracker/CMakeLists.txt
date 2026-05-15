# /Tracker/CMakeLists.txt

cmake_minimum_required(VERSION 3.17)

set(LIBRARY_NAME Tracker)
message("LB | ${LIBRARY_NAME}")

# ---------------------------------------------------------------------------
# 소스 파일 수집 (자동 탐색)
# ---------------------------------------------------------------------------
# SORT 폴더 내의 cpp 파일들을 찾습니다.
file(GLOB_RECURSE TRACKER_SOURCES CONFIGURE_DEPENDS
    "SORT/*.cpp"
)

# ---------------------------------------------------------------------------
# 라이브러리 생성
# ---------------------------------------------------------------------------
add_library(${LIBRARY_NAME} SHARED ${TRACKER_SOURCES})

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
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/SORT
    ${CMAKE_CURRENT_SOURCE_DIR}/TrackerBase

    # Root에서 찾은 OpenCV 경로
    ${OpenCV_INCLUDE_DIRS}
)

# ---------------------------------------------------------------------------
# 라이브러리 링크
# ---------------------------------------------------------------------------
target_link_libraries(${LIBRARY_NAME}
PUBLIC
    BasicLibs
    ${OpenCV_LIBS}
)