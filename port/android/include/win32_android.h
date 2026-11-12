/*
 * win32_android.h - Win32 calls that BLACK's sources and the xboxrecomp input
 * layer use on the Quest (aarch64 Android) build beyond what the runtime's
 * POSIX Win32 layer (platform/win32_compat.h) provides.
 *
 * Only Android builds have port/android/include on their include path, and
 * every includer guards the #include with __ANDROID__, so the Windows and
 * desktop-Linux builds never see this file.
 *
 * Each addition is a macro over a black_w32_* function (or a guarded macro),
 * so that if win32_compat.h grows a real declaration of the same name later,
 * nothing conflicts: win32_compat.h is included first and the macro only
 * redirects the code that follows.
 */
#ifndef WIN32_ANDROID_H
#define WIN32_ANDROID_H

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform/win32_compat.h"

/* ---- file system: CreateDirectoryA / DeleteFileA / MoveFileExA ----------
 * Same results and GetLastError codes as Win32 for the cases callers test
 * (ERROR_FILE_NOT_FOUND, ERROR_ALREADY_EXISTS). */
#ifndef MOVEFILE_REPLACE_EXISTING
#define MOVEFILE_REPLACE_EXISTING 0x00000001u
#endif
#ifndef MOVEFILE_COPY_ALLOWED
#define MOVEFILE_COPY_ALLOWED     0x00000002u
#endif
#ifndef MOVEFILE_WRITE_THROUGH
#define MOVEFILE_WRITE_THROUGH    0x00000008u
#endif

static inline DWORD black_w32_error(int e)
{
    switch (e) {
    case 0:       return ERROR_SUCCESS;
    case ENOENT:  return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EEXIST:  return ERROR_ALREADY_EXISTS;
    case EACCES:
    case EPERM:
    case EROFS:   return ERROR_ACCESS_DENIED;
    case ENOMEM:  return ERROR_NOT_ENOUGH_MEMORY;
    default:      return ERROR_GEN_FAILURE;
    }
}

static inline BOOL black_w32_CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES sa)
{
    (void)sa;
    if (mkdir(path, 0770) == 0)
        return TRUE;
    SetLastError(black_w32_error(errno));
    return FALSE;
}

static inline BOOL black_w32_DeleteFileA(LPCSTR path)
{
    if (unlink(path) == 0)
        return TRUE;
    SetLastError(black_w32_error(errno));
    return FALSE;
}

/* rename(2) replaces the target atomically, which is MOVEFILE_REPLACE_EXISTING;
 * without that flag Win32 refuses an existing target. MOVEFILE_WRITE_THROUGH:
 * the callers flush the data themselves; the new directory entry is made
 * durable with an fsync of the target's directory. */
static inline BOOL black_w32_MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags)
{
    struct stat st;
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && stat(to, &st) == 0) {
        SetLastError(ERROR_ALREADY_EXISTS);
        return FALSE;
    }
    if (rename(from, to) != 0) {
        SetLastError(black_w32_error(errno));
        return FALSE;
    }
    if (flags & MOVEFILE_WRITE_THROUGH) {
        char dir[512];
        const char *slash = strrchr(to, '/');
        size_t n = slash ? (size_t)(slash - to) : 0;
        int fd;
        if (n == 0 || n >= sizeof(dir)) {
            dir[0] = slash == to ? '/' : '.';
            dir[1] = 0;
        } else {
            memcpy(dir, to, n);
            dir[n] = 0;
        }
        fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd >= 0) {
            fsync(fd);
            close(fd);
        }
    }
    return TRUE;
}

#define CreateDirectoryA black_w32_CreateDirectoryA
#define DeleteFileA      black_w32_DeleteFileA
#define MoveFileExA      black_w32_MoveFileExA

/* ---- interlocked bit operations (return the previous value, full barrier) */
#ifndef InterlockedOr
#define InterlockedOr(p, v)  __atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST)
#endif
#ifndef InterlockedAnd
#define InterlockedAnd(p, v) __atomic_fetch_and((p), (v), __ATOMIC_SEQ_CST)
#endif

/* ---- keyboard layout: there is no keyboard on the headset. pc_input asks
 * the layout only to label keys; 0 means "no layout name", so it falls back to
 * its own scancode names. */
#ifndef MAPVK_VK_TO_VSC
#define MAPVK_VK_TO_VSC    0u
#define MAPVK_VSC_TO_VK    1u
#define MAPVK_VK_TO_CHAR   2u
#define MAPVK_VSC_TO_VK_EX 3u
#endif
static inline UINT black_w32_MapVirtualKeyW(UINT code, UINT type)
{
    (void)code;
    (void)type;
    return 0;
}
#define MapVirtualKeyW black_w32_MapVirtualKeyW

#endif /* WIN32_ANDROID_H */
