# vim: ts=2 sw=2
# [FindFFmpeg.cmake]
# Try to find the required ffmpeg components(default: AVFORMAT, AVUTIL, AVCODEC)
# Improved for DetectBase Project with unified logging and modern targets.

include(FindPackageHandleStandardArgs)

message(STATUS "[Find::FFmpeg] Starting automatic FFmpeg detection...")

# ---------------------------------------------------------------------------
# 1. Setup Components
# ---------------------------------------------------------------------------
if (NOT FFmpeg_FIND_COMPONENTS)
  set(FFmpeg_FIND_COMPONENTS AVCODEC AVFORMAT AVUTIL)
endif ()

# pkg-config is mandatory for this script
find_package(PkgConfig REQUIRED)

# ---------------------------------------------------------------------------
# 2. Macro: find_component
# ---------------------------------------------------------------------------
macro(find_component _component _pkgconfig _library _header)

  # 1. Use pkg-config to get hints
  pkg_check_modules(PC_${_component} QUIET ${_pkgconfig})

  # 2. Find Include Dir
  find_path(${_component}_INCLUDE_DIRS ${_header}
    HINTS
      ${PC_${_component}_INCLUDEDIR}
      ${PC_${_component}_INCLUDE_DIRS}
    PATH_SUFFIXES
      ffmpeg
  )

  # 3. Find Library
  find_library(${_component}_LIBRARIES NAMES ${_library}
    HINTS
      ${PC_${_component}_LIBDIR}
      ${PC_${_component}_LIBRARY_DIRS}
  )

  # 4. Result Handling & Target Creation
  if (${_component}_LIBRARIES AND ${_component}_INCLUDE_DIRS)
    set(${_component}_FOUND TRUE)
    set(${_component}_DEFINITIONS ${PC_${_component}_CFLAGS_OTHER} CACHE STRING "The ${_component} CFLAGS.")
    set(${_component}_VERSION ${PC_${_component}_VERSION} CACHE STRING "The ${_component} version number.")

    message(STATUS "[Find::FFmpeg] Found component: ${_component} (Version: ${${_component}_VERSION})")
    # message(STATUS "    [Lib] ${${_component}_LIBRARIES}")

    # Create Modern Target: FFmpeg::<Component> (e.g., FFmpeg::AVCODEC)
    if(NOT TARGET FFmpeg::${_component})
      add_library(FFmpeg::${_component} UNKNOWN IMPORTED)
      set_target_properties(FFmpeg::${_component} PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${${_component}_INCLUDE_DIRS}"
        IMPORTED_LOCATION "${${_component}_LIBRARIES}"
        INTERFACE_COMPILE_OPTIONS "${${_component}_DEFINITIONS}"
      )
    endif()

  else ()
    # Optional components shouldn't error out immediately here
    # message(STATUS "[Find::FFmpeg] Component ${_component} NOT found.")
    set(${_component}_FOUND FALSE)
  endif ()

  mark_as_advanced(
    ${_component}_INCLUDE_DIRS
    ${_component}_LIBRARIES
    ${_component}_DEFINITIONS
    ${_component}_VERSION
  )

endmacro()

# ---------------------------------------------------------------------------
# 3. Main Loop
# ---------------------------------------------------------------------------

# Check for all possible components
find_component(AVCODEC    libavcodec    avcodec  libavcodec/avcodec.h)
find_component(AVFORMAT   libavformat   avformat libavformat/avformat.h)
find_component(AVDEVICE   libavdevice   avdevice libavdevice/avdevice.h)
find_component(AVUTIL     libavutil     avutil   libavutil/avutil.h)
find_component(AVFILTER   libavfilter   avfilter libavfilter/avfilter.h)
find_component(SWSCALE    libswscale    swscale  libswscale/swscale.h)
find_component(POSTPROC   libpostproc   postproc libpostproc/postprocess.h)
find_component(SWRESAMPLE libswresample swresample libswresample/swresample.h)

# Aggregate results for requested components
set(FFMPEG_LIBRARIES "")
set(FFMPEG_INCLUDE_DIRS "")
set(FFMPEG_DEFINITIONS "")

foreach (_component ${FFmpeg_FIND_COMPONENTS})
  if (${_component}_FOUND)
    list(APPEND FFMPEG_LIBRARIES ${${_component}_LIBRARIES})
    list(APPEND FFMPEG_INCLUDE_DIRS ${${_component}_INCLUDE_DIRS})
    list(APPEND FFMPEG_DEFINITIONS ${${_component}_DEFINITIONS})
  endif ()
endforeach ()

if (FFMPEG_INCLUDE_DIRS)
  list(REMOVE_DUPLICATES FFMPEG_INCLUDE_DIRS)
endif ()

if (FFMPEG_LIBRARIES)
    list(REMOVE_DUPLICATES FFMPEG_LIBRARIES)
endif ()

# ---------------------------------------------------------------------------
# 4. Final Validation
# ---------------------------------------------------------------------------
# Compile the list of required vars
set(_FFmpeg_REQUIRED_VARS FFMPEG_LIBRARIES FFMPEG_INCLUDE_DIRS)
foreach (_component ${FFmpeg_FIND_COMPONENTS})
  list(APPEND _FFmpeg_REQUIRED_VARS ${_component}_LIBRARIES ${_component}_INCLUDE_DIRS)
endforeach ()

find_package_handle_standard_args(FFmpeg DEFAULT_MSG ${_FFmpeg_REQUIRED_VARS})