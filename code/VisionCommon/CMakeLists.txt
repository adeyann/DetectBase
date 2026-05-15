# /VisionCommon/CMakeLists.txt

cmake_minimum_required( VERSION 3.17 )

# Tier 2 Library
# 참조 가능한 라이브러리: BasicLibs
set( LIBRARY_NAME VisionCommon )
message( "LB | ${LIBRARY_NAME}" )

# ---------------------------------------------------------------------------
# 소스 파일 수집 (자동 탐색)
# ---------------------------------------------------------------------------
file( GLOB_RECURSE VISION_COMMON_SOURCES CONFIGURE_DEPENDS
    "src/*.cpp"
)

# ---------------------------------------------------------------------------
# 라이브러리 생성 (SHARED)
# ---------------------------------------------------------------------------
# Root 설정에 의해 -fPIC가 자동 적용됩니다.
add_library( ${LIBRARY_NAME} SHARED ${VISION_COMMON_SOURCES} )

# ---------------------------------------------------------------------------
# Export Header 생성
# ---------------------------------------------------------------------------
include( GenerateExportHeader )
generate_export_header( ${LIBRARY_NAME} )

# ---------------------------------------------------------------------------
# Include 경로 설정
# ---------------------------------------------------------------------------
target_include_directories( ${LIBRARY_NAME}
PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
PRIVATE
    # Root에서 찾은 OpenCV 및 FFmpeg 헤더 경로 주입
    ${OpenCV_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
)

# ---------------------------------------------------------------------------
# 라이브러리 링크
# ---------------------------------------------------------------------------
target_link_libraries( ${LIBRARY_NAME}
PUBLIC
    BasicLibs         # 기초 타입(MgenTypes, EngineStreamTypes) 의존성
    ${OpenCV_LIBS}    # 영상 처리를 위한 OpenCV 라이브러리
    ${FFMPEG_LIBRARIES} # 포맷 변환 및 Resize를 위한 FFmpeg 라이브러리
PRIVATE
    Threads::Threads  # 멀티스레딩 지원 (필요 시)

    # GCC < 9.0 filesystem support (호환성 유지)
    $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,9.0>>:stdc++fs>
)