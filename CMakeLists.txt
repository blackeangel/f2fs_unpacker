cmake_minimum_required(VERSION 3.16)
project(f2fs_extract
    VERSION 1.1.0
    LANGUAGES C CXX
    DESCRIPTION "F2FS image extractor with transparent decompression"
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# ────────────────────────────────────────────────────────────────────────────
# Platform detection
# ────────────────────────────────────────────────────────────────────────────
if(ANDROID)
    message(STATUS "f2fs_extract: Android NDK build")
elseif(WIN32 OR MINGW)
    message(STATUS "f2fs_extract: Windows/MinGW-w64 build")
else()
    message(STATUS "f2fs_extract: Linux/macOS build")
endif()

# ────────────────────────────────────────────────────────────────────────────
# Compression library option (auto / system / fetch / none)
# ────────────────────────────────────────────────────────────────────────────
option(F2FS_COMPRESS_FETCH  "Force FetchContent for compression libs (Android/MinGW)" OFF)
option(F2FS_COMPRESS_NONE   "Build without compression support" OFF)

if(ANDROID OR MINGW)
    set(F2FS_COMPRESS_FETCH ON CACHE BOOL "" FORCE)
endif()

# ────────────────────────────────────────────────────────────────────────────
# Sources
# ────────────────────────────────────────────────────────────────────────────
set(SOURCES
    src/f2fs_extract.cpp
    src/f2fs_compress.cpp
    src/f2fs_metadata.cpp
    src/main.cpp
)
set(HEADERS
    include/f2fs_fs.h
    src/f2fs_extract.h
    src/f2fs_compress.h
)

if(WIN32 OR MINGW)
    list(APPEND SOURCES src/win_pread.cpp)
endif()

add_executable(f2fs_extract ${SOURCES} ${HEADERS})

target_include_directories(f2fs_extract PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ────────────────────────────────────────────────────────────────────────────
# Compression libraries
# ────────────────────────────────────────────────────────────────────────────
if(NOT F2FS_COMPRESS_NONE)
    include(FetchContent)
    set(FETCHCONTENT_QUIET ON)

    # ── LZ4 ─────────────────────────────────────────────────────────────────
    if(F2FS_COMPRESS_FETCH)
        FetchContent_Declare(lz4_src
            GIT_REPOSITORY https://github.com/lz4/lz4.git
            GIT_TAG        v1.9.4
            GIT_SHALLOW    TRUE
        )
        FetchContent_GetProperties(lz4_src)
        if(NOT lz4_src_POPULATED)
            FetchContent_Populate(lz4_src)
            add_library(lz4_static STATIC
                ${lz4_src_SOURCE_DIR}/lib/lz4.c
                ${lz4_src_SOURCE_DIR}/lib/lz4hc.c
            )
            target_include_directories(lz4_static PUBLIC
                ${lz4_src_SOURCE_DIR}/lib)
            add_library(lz4::lz4 ALIAS lz4_static)
        endif()
        set(LZ4_FOUND TRUE)
        set(LZ4_TARGET lz4::lz4)
    else()
        find_path(LZ4_INCLUDE_DIR  NAMES lz4.h)
        find_library(LZ4_LIBRARY   NAMES lz4)
        if(LZ4_INCLUDE_DIR AND LZ4_LIBRARY)
            set(LZ4_FOUND TRUE)
            add_library(lz4::lz4 IMPORTED STATIC GLOBAL)
            set_target_properties(lz4::lz4 PROPERTIES
                IMPORTED_LOCATION       "${LZ4_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${LZ4_INCLUDE_DIR}")
            set(LZ4_TARGET lz4::lz4)
        endif()
    endif()

    if(LZ4_FOUND)
        target_compile_definitions(f2fs_extract PRIVATE F2FS_HAVE_LZ4)
        target_link_libraries(f2fs_extract PRIVATE ${LZ4_TARGET})
        message(STATUS "f2fs_extract: LZ4 enabled")
    else()
        message(WARNING "f2fs_extract: LZ4 not found — LZ4-compressed F2FS files will fail")
    endif()

    # ── ZSTD ────────────────────────────────────────────────────────────────
    if(F2FS_COMPRESS_FETCH)
        FetchContent_Declare(zstd_src
            GIT_REPOSITORY https://github.com/facebook/zstd.git
            GIT_TAG        v1.5.5
            GIT_SHALLOW    TRUE
            SOURCE_SUBDIR  build/cmake
        )
        set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(ZSTD_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
        set(ZSTD_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(zstd_src)
        set(ZSTD_FOUND TRUE)
        set(ZSTD_TARGET libzstd_static)
    else()
        find_path(ZSTD_INCLUDE_DIR  NAMES zstd.h)
        find_library(ZSTD_LIBRARY   NAMES zstd)
        if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
            set(ZSTD_FOUND TRUE)
            add_library(zstd::zstd IMPORTED STATIC GLOBAL)
            set_target_properties(zstd::zstd PROPERTIES
                IMPORTED_LOCATION       "${ZSTD_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${ZSTD_INCLUDE_DIR}")
            set(ZSTD_TARGET zstd::zstd)
        endif()
    endif()

    if(ZSTD_FOUND)
        target_compile_definitions(f2fs_extract PRIVATE F2FS_HAVE_ZSTD)
        target_link_libraries(f2fs_extract PRIVATE ${ZSTD_TARGET})
        message(STATUS "f2fs_extract: ZSTD enabled")
    else()
        message(WARNING "f2fs_extract: ZSTD not found")
    endif()

    # ── LZO2 ────────────────────────────────────────────────────────────────
    # LZO has no official CMakeLists.txt, so we always use the system library.
    # On Android/MinGW, you can vendor lzo-2.10 sources separately.
    find_path(LZO_INCLUDE_DIR  NAMES lzo/lzo1x.h)
    find_library(LZO_LIBRARY   NAMES lzo2)
    if(LZO_INCLUDE_DIR AND LZO_LIBRARY)
        set(LZO_FOUND TRUE)
        add_library(lzo::lzo2 IMPORTED STATIC GLOBAL)
        set_target_properties(lzo::lzo2 PROPERTIES
            IMPORTED_LOCATION       "${LZO_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${LZO_INCLUDE_DIR}")
        target_compile_definitions(f2fs_extract PRIVATE F2FS_HAVE_LZO)
        target_link_libraries(f2fs_extract PRIVATE lzo::lzo2)
        message(STATUS "f2fs_extract: LZO enabled (LZO + LZO-RLE)")
    else()
        message(WARNING "f2fs_extract: liblzo2 not found — LZO/LZO-RLE compressed files will fail")
    endif()
endif()

# ────────────────────────────────────────────────────────────────────────────
# Compiler flags
# ────────────────────────────────────────────────────────────────────────────
target_compile_options(f2fs_extract PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>: -Wall -Wextra -Wno-unused-parameter>
    $<$<AND:$<CXX_COMPILER_ID:GNU,Clang>,$<CONFIG:Release>>: -O2>
)

# ────────────────────────────────────────────────────────────────────────────
# Platform-specific flags
# ────────────────────────────────────────────────────────────────────────────
if(WIN32 OR MINGW)
    target_compile_definitions(f2fs_extract PRIVATE
        _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

if(UNIX)
    target_compile_definitions(f2fs_extract PRIVATE
        _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
       CMAKE_CXX_COMPILER_VERSION VERSION_LESS "9.0")
        target_link_libraries(f2fs_extract PRIVATE stdc++fs)
    endif()
endif()

if(ANDROID)
    target_link_libraries(f2fs_extract PRIVATE log)
endif()

# ────────────────────────────────────────────────────────────────────────────
# Install
# ────────────────────────────────────────────────────────────────────────────
install(TARGETS f2fs_extract RUNTIME DESTINATION bin)

# ────────────────────────────────────────────────────────────────────────────
# Unit test: compression round-trip
# ────────────────────────────────────────────────────────────────────────────
enable_testing()

add_executable(f2fs_compress_test
    test/test_compress.cpp
    src/f2fs_compress.cpp
)
target_include_directories(f2fs_compress_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(f2fs_compress_test PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>: -Wall -Wextra -Wno-unused-parameter>
)
if(LZ4_FOUND)
    target_compile_definitions(f2fs_compress_test PRIVATE F2FS_HAVE_LZ4)
    target_link_libraries(f2fs_compress_test PRIVATE ${LZ4_TARGET})
endif()
if(ZSTD_FOUND)
    target_compile_definitions(f2fs_compress_test PRIVATE F2FS_HAVE_ZSTD)
    target_link_libraries(f2fs_compress_test PRIVATE ${ZSTD_TARGET})
endif()
if(LZO_FOUND)
    target_compile_definitions(f2fs_compress_test PRIVATE F2FS_HAVE_LZO)
    target_link_libraries(f2fs_compress_test PRIVATE lzo::lzo2)
endif()

add_test(NAME compress_roundtrip COMMAND f2fs_compress_test)
