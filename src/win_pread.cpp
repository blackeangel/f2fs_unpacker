/**
 * win_pread.cpp — POSIX pread() emulation for Windows / MinGW-w64
 *
 * pread(fd, buf, count, offset) reads `count` bytes from `fd`
 * starting at `offset` without changing the file position.
 *
 * On Windows we achieve this with SetFilePointerEx + ReadFile, wrapped
 * in a critical section so it is thread-safe.
 */

#if defined(_WIN32) || defined(__MINGW32__)

#include <windows.h>
#include <io.h>      // _get_osfhandle
#include <errno.h>
#include <stddef.h>

#include "win_pread.h"

ssize_t pread(int fd, void* buf, size_t count, off_t offset)
{
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }

    LARGE_INTEGER li;
    li.QuadPart = offset;

    OVERLAPPED ov  = {};
    ov.Offset      = li.LowPart;
    ov.OffsetHigh  = li.HighPart;

    DWORD nread = 0;
    if (!ReadFile(h, buf, static_cast<DWORD>(count), &nread, &ov)) {
        DWORD err = GetLastError();
        if (err == ERROR_HANDLE_EOF) return 0;
        errno = EIO;
        return -1;
    }
    return static_cast<ssize_t>(nread);
}

#endif // _WIN32 || __MINGW32__
