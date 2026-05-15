#pragma once

// ==============================
// MGEN Filesystem Compatibility
// ==============================

// Feature Detection
#if !defined(MGEN_HAS_FILESYSTEM) && !defined(MGEN_HAS_EXPERIMENTAL_FILESYSTEM)

  // Use __has_include if available
  #ifdef __has_include
    #if __has_include(<filesystem>)
      #define MGEN_HAS_FILESYSTEM 1
    #elif __has_include(<experimental/filesystem>)
      #define MGEN_HAS_EXPERIMENTAL_FILESYSTEM 1
    #endif
  #else
    // Fallback: assume experimental is available
    #define MGEN_HAS_EXPERIMENTAL_FILESYSTEM 1
  #endif

  // --- Platform & Compiler Specific Limitations ---

  // MinGW GCC 8: Filesystem 불가
  #if defined(__MINGW32__) && defined(__GNUC__) && __GNUC__ == 8
    #undef MGEN_HAS_FILESYSTEM
    #undef MGEN_HAS_EXPERIMENTAL_FILESYSTEM
  #endif

  // GCC 7.x: experimental only
  #if defined(__GNUC__) && !defined(__clang__)
    #if __GNUC__ < 8
      #undef MGEN_HAS_FILESYSTEM
      #ifndef MGEN_HAS_EXPERIMENTAL_FILESYSTEM
        #define MGEN_HAS_EXPERIMENTAL_FILESYSTEM 1
      #endif
    #endif
  #endif

  // Clang < 7: unsupported
  #if defined(__clang__) && __clang_major__ < 7
    #undef MGEN_HAS_FILESYSTEM
    #undef MGEN_HAS_EXPERIMENTAL_FILESYSTEM
  #endif

  // MSVC < 19.14: unsupported
  #if defined(_MSC_VER) && _MSC_VER < 1914
    #undef MGEN_HAS_FILESYSTEM
    #undef MGEN_HAS_EXPERIMENTAL_FILESYSTEM
  #endif

  // iOS < 13: unsupported
  #if defined(__IPHONE_OS_VERSION_MIN_REQUIRED) && __IPHONE_OS_VERSION_MIN_REQUIRED < 130000
    #undef MGEN_HAS_FILESYSTEM
    #undef MGEN_HAS_EXPERIMENTAL_FILESYSTEM
  #endif

  // macOS < 10.15: unsupported
  #if defined(__MAC_OS_X_VERSION_MIN_REQUIRED) && __MAC_OS_X_VERSION_MIN_REQUIRED < 101500
    #undef MGEN_HAS_FILESYSTEM
    #undef MGEN_HAS_EXPERIMENTAL_FILESYSTEM
  #endif

#endif // end feature detection

// Final default values (force define)
#ifndef MGEN_HAS_FILESYSTEM
  #define MGEN_HAS_FILESYSTEM 0
#endif

#ifndef MGEN_HAS_EXPERIMENTAL_FILESYSTEM
  #define MGEN_HAS_EXPERIMENTAL_FILESYSTEM 0
#endif

// ==============================
// Include the proper header
// ==============================

#if MGEN_HAS_FILESYSTEM
  #include <filesystem>
  namespace MGEN { namespace fs = std::filesystem; }

#elif MGEN_HAS_EXPERIMENTAL_FILESYSTEM
  #include <experimental/filesystem>
  namespace MGEN { namespace fs = std::experimental::filesystem; }

#else
  #error "No suitable <filesystem> support found for this compiler."
#endif
