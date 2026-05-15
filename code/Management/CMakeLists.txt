# /Management/CMakeLists.txt

cmake_minimum_required( VERSION 3.17 )

# Tier 3 Library
# 참조 가능한 라이브러리: BasicLibs, VisionCommon, SocketIO, GRPC
set( LIBRARY_NAME Management )
message( "LB | ${LIBRARY_NAME}" )

# ---------------------------------------------------------------------------
# 소스 파일 수집 (자동 탐색)
# ---------------------------------------------------------------------------
file( GLOB_RECURSE MANAGEMENT_SOURCES CONFIGURE_DEPENDS
    "manager/*.cpp"
    "worker/*.cpp"
)

# ---------------------------------------------------------------------------
# 라이브러리 생성 (STATIC)
# ---------------------------------------------------------------------------
# Root 설정에 의해 -fPIC가 자동 적용됩니다.
add_library( ${LIBRARY_NAME} STATIC ${MANAGEMENT_SOURCES} )

# ---------------------------------------------------------------------------
# Export Header
# ---------------------------------------------------------------------------
include( GenerateExportHeader )
generate_export_header( ${LIBRARY_NAME} )

# ---------------------------------------------------------------------------
# Include 경로 설정
# ---------------------------------------------------------------------------
target_include_directories( ${LIBRARY_NAME}
PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/manager/include
    ${CMAKE_CURRENT_SOURCE_DIR}/manager/interface
    ${CMAKE_CURRENT_SOURCE_DIR}/worker/include
)

# ---------------------------------------------------------------------------
# 라이브러리 링크
# ---------------------------------------------------------------------------
target_link_libraries( ${LIBRARY_NAME}
PUBLIC
    SocketIO          # Management 헤더에서 SocketIO 타입을 노출할 가능성이 있음
    VisionCommon      # EngineClient가 전처리 타입을 사용하므로 PUBLIC 링크
PRIVATE
    BasicLibs
    GRPC
    REST
    RTSP
    Threads::Threads  # pthread

    # GCC < 9.0 filesystem support
    $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,9.0>>:stdc++fs>
)