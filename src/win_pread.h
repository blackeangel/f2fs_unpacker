#pragma once
/**
 * win_pread.h — pread() shim for Windows / MinGW-w64
 *
 * On POSIX systems pread() is declared in <unistd.h>.
 * On Windows we declare it here and implement it in win_pread.cpp.
 */

#if defined(_WIN32) || defined(__MINGW32__)

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stddef.h>

#ifndef _SSIZE_T_DEFINED
#include <basetsd.h>
typedef SSIZE_T ssize_t;
#define _SSIZE_T_DEFINED
#endif

#ifndef _OFF_T_DEFINED
typedef long long off_t;
#define _OFF_T_DEFINED
#endif

// ────────────────────────────────────────────────────────────────────────────
// POSIX st_mode test macros — MinGW's <sys/stat.h> only defines a subset
// (S_ISREG/S_ISDIR/S_ISCHR typically present; S_ISLNK/S_ISSOCK/S_ISFIFO are
// not, since the Windows CRT has no native concept of them). F2FS images
// still encode these bits in i_mode the same way regardless of the host OS
// extracting them, so we need working macros here independent of what the
// target CRT models natively.
// ────────────────────────────────────────────────────────────────────────────
#ifndef S_IFLNK
#  define S_IFLNK  0xA000
#endif
#ifndef S_IFSOCK
#  define S_IFSOCK 0xC000
#endif
#ifndef S_IFIFO
#  define S_IFIFO  0x1000
#endif
#ifndef S_ISLNK
#  define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#endif
#ifndef S_ISSOCK
#  define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#endif
#ifndef S_ISFIFO
#  define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#endif

// O_LARGEFILE is a Linux-specific open() flag with no Windows equivalent —
// the Win32/MinGW CRT already uses 64-bit off_t (see typedef above), so the
// flag simply isn't needed there.
#ifndef O_LARGEFILE
#  define O_LARGEFILE 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

ssize_t pread(int fd, void* buf, size_t count, off_t offset);

#ifdef __cplusplus
}
#endif

#else
// POSIX: pread is in <unistd.h>
#include <unistd.h>
#endif // _WIN32 || __MINGW32__
