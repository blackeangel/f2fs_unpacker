cmake_minimum_required(VERSION 3.16)
project(f2fs_extract
    VERSION 1.3.0
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
# Dependency strategy
#
# DEFAULT (no flags needed): every compression library is fetched as source
# via FetchContent and statically linked in. No system packages required —
# `cmake -S . -B build && cmake --build build` works on a bare toolchain
# with nothing but internet access to github.com.
#
# F2FS_USE_SYSTEM_LIBS=ON: opt out of FetchContent and use system-installed
# liblz4/libzstd/liblzo2 instead (faster local rebuilds, no re-download).
# F2FS_COMPRESS_NONE=ON:   skip compression support entirely.
# ────────────────────────────────────────────────────────────────────────────
option(F2FS_USE_SYSTEM_LIBS "Use system-installed compression libs instead of vendoring via FetchContent" OFF)
option(F2FS_COMPRESS_NONE   "Build without compression support" OFF)

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

    # CMake 4.x hard-errors on subprojects whose cmake_minimum_required()
    # predates 3.5 (zstd's bundled build/cmake/CMakeLists.txt still says
    # VERSION 2.8.12). This is the documented escape hatch: it tells CMake
    # to evaluate those old version checks as if run under this version
    # instead of refusing outright. Harmless on CMake < 3.31, where the
    # variable is simply ignored.
    if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    endif()

    # ── LZ4 ─────────────────────────────────────────────────────────────────
    if(NOT F2FS_USE_SYSTEM_LIBS)
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
        message(STATUS "f2fs_extract: LZ4 vendored via FetchContent (lz4/lz4 v1.9.4)")
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
            message(STATUS "f2fs_extract: LZ4 found on system (${LZ4_LIBRARY})")
        endif()
    endif()

    if(LZ4_FOUND)
        target_compile_definitions(f2fs_extract PRIVATE F2FS_HAVE_LZ4)
        target_link_libraries(f2fs_extract PRIVATE ${LZ4_TARGET})
    else()
        message(WARNING "f2fs_extract: LZ4 not available — LZ4-compressed F2FS files will fail to extract")
    endif()

    # ── ZSTD ────────────────────────────────────────────────────────────────
    if(NOT F2FS_USE_SYSTEM_LIBS)
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
        # zstd's own build/cmake/lib/CMakeLists.txt does not propagate
        # zstd.h's directory via target_include_directories for in-tree
        # (non-installed) consumers — without this, the build would silently
        # fall through to whatever zstd.h happens to be on the system
        # default include path (or fail outright if there isn't one),
        # defeating the entire point of vendoring it. Caught by testing a
        # MinGW cross-build on a machine that had no system libzstd-dev.
        # Wrapped in $<BUILD_INTERFACE:...>: zstd's own CMakeLists.txt
        # installs libzstd_static by default (no toggle to disable it), and
        # CMake's Generate step validates that INTERFACE_INCLUDE_DIRECTORIES
        # on any installable target doesn't contain a bare build-tree path —
        # it must be wrapped to mark it "valid only pre-install". We don't
        # actually install/export libzstd_static ourselves, but CMake still
        # validates it transitively because install(TARGETS f2fs_extract)
        # links against it. Without this, `cmake --build` fails at the
        # Generate step with "which is prefixed in the build directory".
        target_include_directories(libzstd_static PUBLIC
            $<BUILD_INTERFACE:${zstd_src_SOURCE_DIR}/lib>)
        set(ZSTD_FOUND TRUE)
        set(ZSTD_TARGET libzstd_static)
        message(STATUS "f2fs_extract: Zstd vendored via FetchContent (facebook/zstd v1.5.5)")
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
            message(STATUS "f2fs_extract: Zstd found on system (${ZSTD_LIBRARY})")
        endif()
    endif()

    if(ZSTD_FOUND)
        target_compile_definitions(f2fs_extract PRIVATE F2FS_HAVE_ZSTD)
        target_link_libraries(f2fs_extract PRIVATE ${ZSTD_TARGET})
    else()
        message(WARNING "f2fs_extract: Zstd not available — Zstd-compressed F2FS files will fail to extract")
    endif()

    # ── LZO (via miniLZO — see src/third_party/minilzo/README.md) ────────────
    # The full LZO2 distribution only ships an autotools build system, which
    # is painful to wrap with FetchContent. miniLZO is the upstream-maintained
    # subset (one .c file + headers) explicitly designed for embedding into
    # other projects' source trees — same on-disk format, same
    # lzo1x_1_compress/lzo1x_decompress_safe API we already call, just without
    # the lzo_init()/version-check ceremony the full library wants. Vendored
    # from https://github.com/nemequ/lzo (the standard unofficial GitHub
    # mirror of the oberhumer.com LZO release), GPL-2.0-or-later — compatible
    # with this project's GPL-3.0 license.
    if(NOT F2FS_USE_SYSTEM_LIBS)
        FetchContent_Declare(lzo_src
            GIT_REPOSITORY https://github.com/nemequ/lzo.git
            GIT_TAG        master
            GIT_SHALLOW    TRUE
        )
        FetchContent_GetProperties(lzo_src)
        if(NOT lzo_src_POPULATED)
            FetchContent_Populate(lzo_src)
            add_library(minilzo_static STATIC
                ${lzo_src_SOURCE_DIR}/minilzo/minilzo.c
            )
            target_include_directories(minilzo_static PUBLIC
                ${lzo_src_SOURCE_DIR}/minilzo
                ${lzo_src_SOURCE_DIR}/include/lzo
            )
            add_library(lzo::lzo2 ALIAS minilzo_static)
        endif()
        set(LZO_FOUND TRUE)
        target_compile_definitions(f2fs_extract PRIVATE F2FS_HAVE_LZO F2FS_LZO_IS_MINILZO)
        message(STATUS "f2fs_extract: LZO vendored via FetchContent (miniLZO from nemequ/lzo)")
    else()
        find_path(LZO_INCLUDE_DIR  NAMES lzo/lzo1x.h)
        find_library(LZO_LIBRARY   NAMES lzo2)
        if(LZO_INCLUDE_DIR AND LZO_LIBRARY)
            set(LZO_FOUND TRUE)
            add_library(lzo::lzo2 IMPORTED STATIC GLOBAL)
            set_target_properties(lzo::lzo2 PROPERTIES
                IMPORTED_LOCATION       "${LZO_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${LZO_INCLUDE_DIR}")
            target_compile_definitions(f2fs_extract PRIVATE F2FS_HAVE_LZO)
            message(STATUS "f2fs_extract: LZO found on system (${LZO_LIBRARY})")
        endif()
    endif()

    if(LZO_FOUND)
        target_link_libraries(f2fs_extract PRIVATE lzo::lzo2)
    else()
        message(WARNING "f2fs_extract: LZO not available — LZO/LZO-RLE-compressed F2FS files will fail to extract")
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
    if(NOT F2FS_USE_SYSTEM_LIBS)
        target_compile_definitions(f2fs_compress_test PRIVATE F2FS_LZO_IS_MINILZO)
    endif()
    target_link_libraries(f2fs_compress_test PRIVATE lzo::lzo2)
endif()

add_test(NAME compress_roundtrip COMMAND f2fs_compress_test)

# ════════════════════════════════════════════════════════════════════════════
# f2fs_pack — repacker, built on AOSP's f2fs-tools sload.f2fs
#
# Vendored from https://android.googlesource.com/platform/external/f2fs-tools
# (via github.com/AOSPA/android_external_f2fs-tools mirror) into
# third_party/aosp-f2fs-tools/. This is AOSP's own fork, NOT the
# jaegeuk/f2fs-tools kernel-community tree — it has full working -C
# (fs_config), -s (file_contexts via standard libselinux selabel_open),
# and -c/-a/-L (LZ4/LZO compression writing) support that the generic
# upstream tree lacks entirely (verified: zero compression code anywhere
# in jaegeuk/f2fs-tools).
#
# Built WITHOUT -DWITH_ANDROID: that path requires libcutils/libbase
# (canned_fs_config, real AOSP-toolchain-only headers) which we don't want
# to vendor. Instead fs_config_func is pointed at our own small parser
# (see third_party/aosp-f2fs-tools/fsck/f2fs_pack_fsconfig.c) reading the
# exact same AOSP canned_fs_config text format our own fs_config.txt
# writer already uses.
#
# Currently Linux only. Windows is excluded matching AOSP's own Android.bp
# ("windows: { enabled: false }" for this codebase — it's fairly
# POSIX-heavy: scandir, mntent.h, ioctl, readlink). Android NDK is ALSO
# excluded here despite AOSP building this same code as part of a normal
# Android.bp host/device target: that build goes through Bionic libc with
# Android's own full toolchain, which isn't the same as cross-compiling
# with the NDK the way this project's f2fs_extract does (Bionic has known
# gaps in less-common POSIX APIs like getmntent()/mntent.h). This hasn't
# been tested at all yet, so it's scoped out here rather than silently
# shipping an unverified cross-compile. Both are candidate follow-ups.
# ════════════════════════════════════════════════════════════════════════════

if(NOT WIN32 AND NOT MINGW AND NOT ANDROID)
    set(AOSP_F2FS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/aosp-f2fs-tools)

    set(F2FS_PACK_VENDORED_SOURCES
        ${AOSP_F2FS_DIR}/fsck/dir.c
        ${AOSP_F2FS_DIR}/fsck/dict.c
        ${AOSP_F2FS_DIR}/fsck/mkquota.c
        ${AOSP_F2FS_DIR}/fsck/quotaio.c
        ${AOSP_F2FS_DIR}/fsck/quotaio_tree.c
        ${AOSP_F2FS_DIR}/fsck/quotaio_v2.c
        ${AOSP_F2FS_DIR}/fsck/node.c
        ${AOSP_F2FS_DIR}/fsck/segment.c
        ${AOSP_F2FS_DIR}/fsck/xattr.c
        ${AOSP_F2FS_DIR}/fsck/main.c
        ${AOSP_F2FS_DIR}/fsck/mount.c
        ${AOSP_F2FS_DIR}/fsck/dump.c
        ${AOSP_F2FS_DIR}/fsck/resize.c
        ${AOSP_F2FS_DIR}/fsck/defrag.c
        ${AOSP_F2FS_DIR}/fsck/fsck.c
        ${AOSP_F2FS_DIR}/fsck/sload.c
        ${AOSP_F2FS_DIR}/fsck/f2fs_pack_fsconfig.c
        ${AOSP_F2FS_DIR}/fsck/compress.c
        ${AOSP_F2FS_DIR}/lib/libf2fs.c
        ${AOSP_F2FS_DIR}/lib/libf2fs_io.c
        ${AOSP_F2FS_DIR}/lib/libf2fs_zoned.c
        ${AOSP_F2FS_DIR}/lib/nls_utf8.c
    )

    add_executable(f2fs_pack ${F2FS_PACK_VENDORED_SOURCES})

    target_include_directories(f2fs_pack PRIVATE
        ${AOSP_F2FS_DIR}/include
        ${AOSP_F2FS_DIR}/mkfs
        ${AOSP_F2FS_DIR}/fsck
    )

    # Feature-test macros this codebase checks via #ifdef, set to match a
    # standard modern Linux/glibc environment (verified present on our
    # CI's ubuntu-latest runners). Deliberately NOT defined:
    # HAVE_SELINUX_ANDROID_H (needs AOSP-only libcutils), HAVE_SPARSE_SPARSE_H
    # (needs Android's libsparse; we always write raw non-sparse images),
    # HAVE_ATTR_XATTR_H / HAVE_SYS_ACL_H (need separate libattr1-dev/
    # libacl1-dev packages we don't want as new dependencies — the code's
    # own OR-gated fallbacks to HAVE_LINUX_XATTR_H / HAVE_LINUX_POSIX_ACL_H
    # cover the same functionality using only kernel-provided headers).
    target_compile_definitions(f2fs_pack PRIVATE
        F2FS_MAJOR_VERSION=1
        F2FS_MINOR_VERSION=15
        F2FS_TOOLS_VERSION="1.15.0"
        F2FS_TOOLS_DATE="2022-05-20"
        HAVE_BYTESWAP_H
        HAVE_CLOCK_BOOTTIME
        HAVE_CLOCK_GETTIME
        HAVE_FALLOCATE
        HAVE_FALLOC_H
        HAVE_FCNTL_H
        HAVE_FSETXATTR
        HAVE_FSTAT
        HAVE_FSYNC
        HAVE_GETMNTENT
        HAVE_LIBUUID
        HAVE_LINUX_FIEMAP_H
        HAVE_LINUX_FS_H
        HAVE_LINUX_HDREG_H
        HAVE_LINUX_LIMITS_H
        HAVE_LINUX_POSIX_ACL_H
        HAVE_LINUX_TYPES_H
        HAVE_LINUX_XATTR_H
        HAVE_LSEEK64
        HAVE_MEMSET
        HAVE_MNTENT_H
        HAVE_POSIX_ACL_H
        HAVE_SCSI_SG_H
        HAVE_SETMNTENT
        HAVE_STDLIB_H
        HAVE_STRING_H
        HAVE_SYS_IOCTL_H
        HAVE_SYS_MOUNT_H
        HAVE_SYS_STAT_H
        HAVE_SYS_SYSMACROS_H
        HAVE_SYS_UTSNAME_H
        HAVE_SYS_XATTR_H
        HAVE_UNISTD_H
        HAVE_UUID_UUID_H
    )

    target_link_libraries(f2fs_pack PRIVATE uuid)

    target_compile_definitions(f2fs_pack PRIVATE HAVE_LIBSELINUX)
    find_library(SELINUX_LIBRARY NAMES selinux)
    find_path(SELINUX_INCLUDE_DIR NAMES selinux/selinux.h)
    if(SELINUX_LIBRARY AND SELINUX_INCLUDE_DIR)
        target_include_directories(f2fs_pack PRIVATE ${SELINUX_INCLUDE_DIR})
        target_link_libraries(f2fs_pack PRIVATE ${SELINUX_LIBRARY})
        message(STATUS "f2fs_extract: f2fs_pack -s file_contexts support enabled (${SELINUX_LIBRARY})")
    else()
        message(WARNING "f2fs_extract: libselinux not found — f2fs_pack -s file_contexts will not work. Install libselinux1-dev (Debian/Ubuntu) or equivalent.")
    endif()

    # Reuse the SAME vendored LZ4 already fetched above for f2fs_extract
    # — compress.c uses the identical standard <lz4.h> API.
    # Only LZ4 for compression writing — AOSP's compress.c calls
    # lzo1x_1_15_compress(), a symbol only present in the FULL liblzo2
    # distribution (no official CMake build system), not in miniLZO (the
    # embed-friendly subset we vendor for the extractor, which only needs
    # the simpler lzo1x_1_compress/lzo1x_decompress_safe pair). All
    # real-world Samsung images seen in this project use LZ4 exclusively;
    # LZO repack support can be added later by vendoring full liblzo2
    # if ever needed.
    if(LZ4_FOUND)
        target_compile_definitions(f2fs_pack PRIVATE HAVE_LIBLZ4)
        target_link_libraries(f2fs_pack PRIVATE ${LZ4_TARGET})
    endif()

    target_compile_options(f2fs_pack PRIVATE
        -Wno-unused-function -Wno-sign-compare -Wno-pointer-arith
        -Wno-missing-field-initializers -Wno-format)
else()
    message(STATUS "f2fs_extract: f2fs_pack skipped (Linux-only for now; see CMakeLists.txt comment above)")
endif()
