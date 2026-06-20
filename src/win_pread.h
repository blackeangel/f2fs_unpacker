#pragma once
/**
 * win_pread.h — pread() shim for Windows / MinGW-w64
 *
 * On POSIX systems pread() is declared in <unistd.h>.
 * On Windows we declare it here and implement it in win_pread.cpp.
 */

#if defined(_WIN32) || defined(__MINGW32__)

#include <sys/types.h>
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
