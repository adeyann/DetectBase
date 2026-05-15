# CMakeFinder/FindgRPC.cmake
# Finds gRPC and creates Modern CMake targets (gRPC::grpc++, Protobuf::protobuf)
# Improved for DetectBase Project with unified logging.

message(STATUS "[Find::gRPC] Starting automatic gRPC detection...")

find_package(PkgConfig QUIET)

# 1. gRPC 검색 (pkg-config 우선)
pkg_check_modules(PC_GRPC QUIET grpc++)

find_path(gRPC_INCLUDE_DIR NAMES grpcpp/grpcpp.h
  HINTS ${PC_GRPC_INCLUDEDIR} ${PC_GRPC_INCLUDE_DIRS}
  PATH_SUFFIXES include
)

find_library(gRPC_LIBRARY NAMES grpc++
  HINTS ${PC_GRPC_LIBDIR} ${PC_GRPC_LIBRARY_DIRS}
)

find_library(gRPC_GRPC_LIBRARY NAMES grpc
  HINTS ${PC_GRPC_LIBDIR} ${PC_GRPC_LIBRARY_DIRS}
)

find_library(gRPC_GPR_LIBRARY NAMES gpr
  HINTS ${PC_GRPC_LIBDIR} ${PC_GRPC_LIBRARY_DIRS}
)

# Plugin 찾기
find_program(GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(gRPC
  REQUIRED_VARS gRPC_LIBRARY gRPC_INCLUDE_DIR
  VERSION_VAR PC_GRPC_VERSION
)

if(gRPC_FOUND)
  set(gRPC_LIBRARIES ${gRPC_LIBRARY} ${gRPC_GRPC_LIBRARY} ${gRPC_GPR_LIBRARY})
  set(gRPC_INCLUDE_DIRS ${gRPC_INCLUDE_DIR})

  message(STATUS "[Find::gRPC] Found gRPC: ${gRPC_LIBRARY}")

  # -----------------------------------------------------------------------
  # [Fix] gRPC Target 생성
  # -----------------------------------------------------------------------
  if(NOT TARGET gRPC::grpc++)
    add_library(gRPC::grpc++ UNKNOWN IMPORTED)
    set_target_properties(gRPC::grpc++ PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${gRPC_INCLUDE_DIRS}"
      IMPORTED_LOCATION "${gRPC_LIBRARY}"
    )
    message(STATUS "[Find::gRPC] Created Modern Target: gRPC::grpc++")
  endif()

  # Reflection (Optional)
  find_library(gRPC_REFLECTION_LIBRARY NAMES grpc++_reflection HINTS ${PC_GRPC_LIBDIR} ${PC_GRPC_LIBRARY_DIRS})
  if(gRPC_REFLECTION_LIBRARY AND NOT TARGET gRPC::grpc++_reflection)
     add_library(gRPC::grpc++_reflection UNKNOWN IMPORTED)
     set_target_properties(gRPC::grpc++_reflection PROPERTIES
       INTERFACE_INCLUDE_DIRECTORIES "${gRPC_INCLUDE_DIRS}"
       IMPORTED_LOCATION "${gRPC_REFLECTION_LIBRARY}"
     )
  endif()

  # -----------------------------------------------------------------------
  # [Crucial Fix] Protobuf::protobuf 타겟 강제 생성 (Shim)
  # gRPC 사용 시 Protobuf 타겟이 필수인데, Legacy 모드로 찾아지면 없을 수 있음.
  # -----------------------------------------------------------------------
  if(NOT TARGET Protobuf::protobuf)
    # 이미 시스템에서 찾은 변수가 있는지 확인
    if(Protobuf_LIBRARY OR PROTOBUF_LIBRARY)
        set(_PROTO_LIB "${Protobuf_LIBRARY}")
        if(NOT _PROTO_LIB)
            set(_PROTO_LIB "${PROTOBUF_LIBRARY}")
        endif()

        set(_PROTO_INC "${Protobuf_INCLUDE_DIR}")
        if(NOT _PROTO_INC)
            set(_PROTO_INC "${PROTOBUF_INCLUDE_DIR}")
        endif()

        message(STATUS "[Find::gRPC] Creating legacy shim for Protobuf::protobuf")
        add_library(Protobuf::protobuf UNKNOWN IMPORTED)
        set_target_properties(Protobuf::protobuf PROPERTIES
            IMPORTED_LOCATION "${_PROTO_LIB}"
            INTERFACE_INCLUDE_DIRECTORIES "${_PROTO_INC}"
        )
    else()
        # 변수조차 없으면 다시 찾음
        find_package(Protobuf REQUIRED)
        if(NOT TARGET Protobuf::protobuf)
             add_library(Protobuf::protobuf UNKNOWN IMPORTED)
             set_target_properties(Protobuf::protobuf PROPERTIES
                IMPORTED_LOCATION "${PROTOBUF_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${PROTOBUF_INCLUDE_DIR}"
            )
            message(STATUS "[Find::gRPC] Created fallback shim for Protobuf::protobuf")
        endif()
    endif()
  endif()

endif()