# f2fs_extract

F2FS image extractor with transparent decompression (LZ4/ZSTD/LZO/LZO-RLE),
xattr/SELinux/capabilities metadata export, and original timestamp
preservation. Built against structures verified from AOSP `f2fs-tools` and
the Linux kernel source, and tested against real Android partition images.

## Build — zero dependencies

```bash
git clone https://github.com/blackeangel/f2fs_unpacker.git
cd f2fs_unpacker
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/f2fs_extract -v -m system.img out/system
```

That's it — **no `apt install`, no manual dependency setup.** All three
compression libraries (LZ4, Zstd, LZO) are fetched as source via CMake's
`FetchContent` and statically linked into the binary at build time, the
same way `tar_repacker`/`md1img_repacker` vendor their dependencies. You
only need a C/C++ compiler, CMake ≥ 3.16, and internet access to
`github.com` during the *first* configure (subsequent builds reuse the
cached `build/_deps/` checkout, no network needed).

```bash
$ ldd build/f2fs_extract
        linux-vdso.so.1
        libstdc++.so.6 => ...
        libgcc_s.so.1  => ...
        libc.so.6      => ...
        libm.so.6      => ...
```

No `liblz4.so`, `libzstd.so`, or `liblzo2.so` — everything's baked in.

## Project layout

```
f2fs_extract/
├── CMakeLists.txt          ← build entry point, vendors all dependencies
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

## How dependencies are vendored

| Library | Source | Why this one |
|---|---|---|
| LZ4  | [github.com/lz4/lz4](https://github.com/lz4/lz4) `v1.9.4` | official upstream, ships a usable CMake-free static lib we compile directly from `lib/*.c` |
| Zstd | [github.com/facebook/zstd](https://github.com/facebook/zstd) `v1.5.5` | official upstream, has its own `build/cmake` subtree we point `FetchContent` at |
| LZO  | [github.com/nemequ/lzo](https://github.com/nemequ/lzo) (miniLZO subset) | the official LZO release only ships an autotools build; **miniLZO** is upstream's own single-file embed-friendly subset (`minilzo.c` + 3 headers) exposing the exact `lzo1x_1_compress`/`lzo1x_decompress_safe` API this project calls — no autotools needed |

`FetchContent` downloads each at configure time, builds it as a static
library inside `build/_deps/`, and links it privately into `f2fs_extract` —
nothing is installed system-wide, nothing leaks into your `PATH` or
`LD_LIBRARY_PATH`.

If a library's source can't be reached (offline build, restricted network),
CMake prints a `WARNING` for that one algorithm and still builds everything
else — the binary just refuses files needing that specific compression
algorithm at runtime ("`LZ4 support not compiled in`") instead of failing
to build.

### Opting out: use system packages instead

If you already have `liblz4-dev`/`libzstd-dev`/`liblzo2-dev` installed and
would rather avoid the first-build source download:

```bash
cmake -S . -B build -DF2FS_USE_SYSTEM_LIBS=ON
cmake --build build -j
```

Debian/Ubuntu: `sudo apt install liblz4-dev libzstd-dev liblzo2-dev`
Fedora/RHEL: `sudo dnf install lz4-devel libzstd-devel lzo-devel`
Arch: `sudo pacman -S lz4 zstd lzo`

### Skipping compression entirely

```bash
cmake -S . -B build -DF2FS_COMPRESS_NONE=ON
cmake --build build
```

Useful for a minimal/fast build when you know your target images don't use
the F2FS `compression` feature (check with
`dump.f2fs -d 2 image.img | grep feature`).

## Build — Windows (MinGW-w64)

```bash
# MSYS2 MinGW64 shell — just need the compiler toolchain + cmake
pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake

cd f2fs_unpacker
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build -j
```

Same FetchContent vendoring applies — no `mingw-w64-x86_64-lz4` /
`-zstd` / `-lzo2` packages needed by default.

## Build — Android (NDK)

```bash
cd f2fs_unpacker
cmake -S . -B build-android \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-26 \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build-android -j
```

All three libraries are cross-compiled against the NDK toolchain
automatically via the same FetchContent path. Repeat with
`-DANDROID_ABI=armeabi-v7a` / `x86_64` / `x86` for other ABIs.

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

## License

GPL-3.0 (see `LICENSE`). LZO/miniLZO is GPL-2.0-or-later, compatible with
this project's GPL-3.0 terms.
