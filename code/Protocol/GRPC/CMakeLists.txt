# /Protocol/GRPC/CMakeLists.txt
cmake_minimum_required(VERSION 3.17)

set(LIBRARY_NAME GRPC)
message("LB | ${LIBRARY_NAME}")

# ---------------------------------------------------------------------------
# 1. 패키지 링크 준비
# ---------------------------------------------------------------------------
if(NOT TARGET Protobuf::protobuf)
    find_package(Protobuf REQUIRED)
endif()
if(NOT TARGET gRPC::grpc++)
    find_package(gRPC REQUIRED)
endif()

# ---------------------------------------------------------------------------
# 2. 미리 생성된 소스 파일 지정 (Pre-generated)
# ---------------------------------------------------------------------------
set(PROTO_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/protos")

# 사용자가 미리 만들어둔 파일 목록
set(PROTO_SRCS
    "${PROTO_SRC_DIR}/MgenProto.pb.cc"
    "${PROTO_SRC_DIR}/MgenProto.grpc.pb.cc"
)

# 헤더 파일 경로 (Include Path용)
set(PROTO_HDR_DIR "${PROTO_SRC_DIR}")

# ---------------------------------------------------------------------------
# 3. 프로토버퍼 Object Library
# ---------------------------------------------------------------------------
# add_custom_command 없이, 파일들을 바로 컴파일합니다.
add_library(protobuf_objs OBJECT ${PROTO_SRCS})

# Shared Library 링크를 위한 fPIC
set_target_properties(protobuf_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)

# .pb.cc 파일 내부에서 .pb.h를 찾을 수 있도록 경로 설정
target_include_directories(protobuf_objs PUBLIC ${PROTO_HDR_DIR})

# .pb.cc 컴파일 시 Protobuf 헤더를 찾을 수 있도록 연결
# (OBJECT 라이브러리도 target_link_libraries를 통해 Include Path를 상속받을 수 있습니다)
target_link_libraries(protobuf_objs PUBLIC Protobuf::protobuf)

# ---------------------------------------------------------------------------
# 4. GRPC 라이브러리 정의
# ---------------------------------------------------------------------------
add_library(${LIBRARY_NAME} SHARED
    src/GrpcEventClientBase.cpp
    src/GrpcEventServerBase.cpp
    src/GrpcProtoMaker.cpp
)

include(GenerateExportHeader)
generate_export_header(${LIBRARY_NAME})

# ---------------------------------------------------------------------------
# 5. Include Directories
# ---------------------------------------------------------------------------
target_include_directories(${LIBRARY_NAME}
PUBLIC
    ${PROJECT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROTO_HDR_DIR}
PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ---------------------------------------------------------------------------
# 6. Link Libraries
# ---------------------------------------------------------------------------
target_link_libraries(${LIBRARY_NAME}
PUBLIC
    protobuf_objs
    BasicLibs

    # Root나 FindgRPC에서 찾은 타겟 연결
    Protobuf::protobuf
    gRPC::grpc++

    # Reflection (옵션)
    $<$<TARGET_EXISTS:gRPC::grpc++_reflection>:gRPC::grpc++_reflection>
)