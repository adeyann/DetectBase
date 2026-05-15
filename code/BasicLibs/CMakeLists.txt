# /BasicLibs/CMakeLists.txt
# Tier 1 Library - 참조 가능한 상위 라이브러리 없음

cmake_minimum_required(VERSION 3.17)

set(LIBRARY_NAME BasicLibs)
message("LB | ${LIBRARY_NAME}")

# ---------------------------------------------------------------------------
# 소스 파일 수집 (현재 CMakeList 이하 폴더에서만 재귀 탐색)
# ---------------------------------------------------------------------------
file(
    GLOB_RECURSE        # 현재 CMakeList 이하 폴더에서만 재귀 탐색
    BASIC_LIBS_SOURCES  # 탐색 후 파일 목록
    CONFIGURE_DEPENDS   # 변경 시 자동 재탐색 플래그

    # 파일 탐색 조건
    "core/*.cpp"
    "profile/*.cpp"
    "utils/*.cpp"
)

# ---------------------------------------------------------------------------
# 라이브러리 생성
# ---------------------------------------------------------------------------
add_library(${LIBRARY_NAME} SHARED ${BASIC_LIBS_SOURCES})

# ---------------------------------------------------------------------------
# DLL/SO Export Header 생성
# ---------------------------------------------------------------------------
include(GenerateExportHeader)
generate_export_header(${LIBRARY_NAME})

# ---------------------------------------------------------------------------
# 외부 라이브러리 링크 (NPU 단일 구성)
# P3 (2026-05-14): vendored yaml-cpp 제거. ClassChecker → nlohmann::json 마이그레이션.
# ---------------------------------------------------------------------------
target_link_libraries(${LIBRARY_NAME}
PUBLIC
    # STL
    Threads::Threads
    $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,9.0>>:stdc++fs>
PRIVATE
    # P54: Prometheus exporter. PRIVATE 로 link → 다른 사용자에 의존성 누설 안 함.
    # MetricsRegistry 헤더는 pImpl 이라 prometheus 헤더에 의존 안 함.
    prometheus-cpp-core
    prometheus-cpp-pull
)

# ---------------------------------------------------------------------------
# 헤더 포함 경로
# ---------------------------------------------------------------------------
target_include_directories(${LIBRARY_NAME}
PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core/logger
    ${CMAKE_CURRENT_SOURCE_DIR}/core/metrics
    ${CMAKE_CURRENT_SOURCE_DIR}/core/parser
    ${CMAKE_CURRENT_SOURCE_DIR}/core/structure
    ${CMAKE_CURRENT_SOURCE_DIR}/core/types
    ${CMAKE_CURRENT_SOURCE_DIR}/profile
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
)
