/* Static arm64 test executables cannot link liblog/libandroid (shared only): stand-ins for
 * what win32_compat.c and kelvin_mmio.c call. ashmem "fails", so memfd is what is tested. */
#if defined(__ANDROID__)
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
int ASharedMemory_create(const char *name, size_t size) { (void)name; (void)size; return -1; }
int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    int n;
    (void)prio;
    fprintf(stderr, "[%s] ", tag ? tag : "log");
    va_start(ap, fmt);
    n = vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    return n;
}
#endif
