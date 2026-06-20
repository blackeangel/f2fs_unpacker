# f2fs_extract

F2FS image extractor with transparent decompression (LZ4/ZSTD/LZO/LZO-RLE),
xattr/SELinux/capabilities metadata export, and original timestamp
preservation. Built against structures verified from AOSP `f2fs-tools` and
the Linux kernel source, and tested against real Android partition images.

## Project layout

```
f2fs_extract/
├── CMakeLists.txt          ← build entry point, fetches/finds dependencies
├── include/
│   └── f2fs_fs.h            on-disk F2FS structures
├── src/
│   ├── f2fs_extract.h/.cpp  core extractor (superblock/NAT/CP/node traversal)
│   ├── f2fs_compress.h/.cpp cluster decompression (LZ4/ZSTD/LZO/LZO-RLE)
│   ├── f2fs_metadata.h/.cpp xattr parsing + JSON/fs_config/file_contexts writers
│   ├── main.cpp              CLI entry point
│   └── win_pread.h/.cpp      pread() shim for Windows/MinGW (POSIX systems don't need it)
└── test/
    └── test_compress.cpp     standalone compression round-trip unit test
```

## Dependencies

| Library | Used for | Required? |
|---|---|---|
| LZ4    | decompressing `algo=1` files | optional — extraction works without it, just fails on LZ4-compressed files |
| Zstd   | decompressing `algo=2` files | optional, same caveat |
| LZO2   | decompressing `algo=0`/`algo=3` files | optional, same caveat |

**You don't need to install anything for files that aren't compressed with
that particular algorithm.** Most Android partitions only use LZ4, so on
Linux installing just `liblz4-dev` is usually enough. If a library is
missing, CMake prints a `WARNING` at configure time and builds everything
else — the binary will just refuse files needing that one algorithm
("`LZ4 support not compiled in`") instead of failing to build.

`CMakeLists.txt` finds these **automatically** — you do not vendor or
download anything yourself:

- **Linux/macOS**: uses `find_library`/`find_path` against your system
  package manager's libs (see install commands below).
- **Android NDK / Windows MinGW**: CMake's `FetchContent` automatically
  downloads and statically builds LZ4 + Zstd source from GitHub during
  configure (no system packages needed, since cross-compiled libs usually
  aren't available). LZO still needs to be available as a system/vendored
  library for these targets (see note below).

No other third-party code is vendored or required — everything else
(filesystem traversal, xattr parsing, JSON writer) is plain C++17/STL.

## Build — Linux

```bash
# Install compression libs (skip whichever you don't need)
sudo apt install liblz4-dev libzstd-dev liblzo2-dev cmake g++

cd f2fs_extract
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/f2fs_extract -v -m system.img out/system
```

Fedora/RHEL: `sudo dnf install lz4-devel libzstd-devel lzo-devel`
Arch: `sudo pacman -S lz4 zstd lzo`

## Build — Windows (MinGW-w64)

Using MSYS2:

```bash
pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake \
          mingw-w64-x86_64-lz4 mingw-w64-x86_64-zstd mingw-w64-x86_64-lzo2

cd f2fs_extract
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build -j
```

If you don't have the mingw-w64 packages for LZ4/Zstd available, pass
`-DF2FS_COMPRESS_FETCH=ON` to force CMake to download and statically build
them from source instead:

```bash
cmake -S . -B build -G "MinGW Makefiles" -DF2FS_COMPRESS_FETCH=ON
```

LZO has no CMake-based source build in this project (see note below) — for
MinGW you'll need the `mingw-w64-x86_64-lzo2` package, or vendor lzo-2.10
sources yourself and point `find_library`/`find_path` at them.

## Build — Android (NDK)

```bash
cd f2fs_extract
cmake -S . -B build-android \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-26 \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build-android -j
```

`F2FS_COMPRESS_FETCH` is forced `ON` automatically for Android builds — LZ4
and Zstd are downloaded and statically compiled against the NDK toolchain,
no system packages needed. LZO again needs a vendored source tree if you
need `algo=0`/`algo=3` support on-device (see below).

Repeat with `-DANDROID_ABI=armeabi-v7a` / `x86_64` / `x86` for other ABIs.

## Building without compression support

If you only need plain (uncompressed) F2FS images:

```bash
cmake -S . -B build -DF2FS_COMPRESS_NONE=ON
cmake --build build
```

This skips all three libraries entirely — useful for a minimal/fast build
when you know your target images don't use the `compression` feature
(check with `dump.f2fs -d 2 image.img | grep feature`).

## Note on LZO

LZO2 doesn't ship an official CMake build system, so unlike LZ4/Zstd this
project always looks for it via `find_library(lzo2)` — it is **not**
fetched/built from source automatically even with `F2FS_COMPRESS_FETCH=ON`.
For Android/Windows cross-builds where no system `liblzo2` exists, you have
two options:
1. Vendor `lzo-2.10` source yourself and add a small `CMakeLists.txt` for
   it, pointing `LZO_INCLUDE_DIR`/`LZO_LIBRARY` at the result.
2. Skip LZO support — most real-world Android images use LZ4, not LZO, so
   this is rarely a blocker in practice.

## Running the compression unit test

```bash
cmake --build build --target f2fs_compress_test
./build/f2fs_compress_test
```

Exercises round-trip compress→decompress for all four algorithms
(LZO/LZ4/Zstd/LZO-RLE) using the exact same `f2fs_decompress_cluster()`
function the extractor uses — useful as a smoke test after changing
compiler/toolchain/library versions.

## Usage

```
f2fs_extract [-v|-q] [-m|-M <meta_dir>] <f2fs_image> <output_dir>

  -v            verbose output  (INFO + WARN + ERR)
  -q            quiet output    (ERR only)
  -m            save metadata to <output_dir>/_metadata/
  -M <dir>      save metadata to <dir>
```

Metadata files written with `-m`/`-M`:
- `metadata.json` — mode/uid/gid/size/symlink-target/xattrs (raw, base64 for
  binary values)
- `fs_config.txt` — AOSP `fs_config` format (`path uid gid mode caps`)
- `file_contexts.txt` — SELinux labels (`path  label`), only for entries
  that actually carry a `security.selinux` xattr

Original `atime`/`mtime` are applied directly to extracted files/dirs (not
written to any metadata file) — always, regardless of `-m`.
