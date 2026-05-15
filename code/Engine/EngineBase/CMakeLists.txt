# /Engine/EngineBase/CMakeLists.txt

cmake_minimum_required(VERSION 3.17)

set(LIBRARY_NAME EngineBase)
message("LB | ${LIBRARY_NAME}")

# ---------------------------------------------------------------------------
# 소스 파일 수집 (자동 탐색)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE ENGINE_BASE_SOURCES CONFIGURE_DEPENDS
    "src/*.cpp"
)

# ---------------------------------------------------------------------------
# 라이브러리 생성 (STATIC)
# ---------------------------------------------------------------------------
# Root의 CMAKE_POSITION_INDEPENDENT_CODE ON 설정으로 -fPIC 자동 적용
add_library(${LIBRARY_NAME} STATIC ${ENGINE_BASE_SOURCES})

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
# 라이브러리 링크
# ---------------------------------------------------------------------------
target_link_libraries(${LIBRARY_NAME}
PUBLIC
    BasicLibs
PRIVATE
    Threads::Threads  # pthread 대신 Modern CMake

    # GCC < 9.0 filesystem support
    $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,9.0>>:stdc++fs>
)

# ---------------------------------------------------------------------------
# 컴파일 옵션 (개별 설정 유지)
# ---------------------------------------------------------------------------
# -O2, -g 등의 일반 옵션은 Root에서 상속받음.
# -faligned-new는 엔진 특성상 필요할 수 있으므로 유지.
target_compile_options(${LIBRARY_NAME} PRIVATE
    -faligned-new
)