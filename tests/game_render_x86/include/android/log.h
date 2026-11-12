/* tests/game_render_x86: host stand-in for the NDK's <android/log.h> (game + runtime built with __ANDROID__ on
 * x86-64 Linux). host_stubs.c implements the two calls on stderr. */
#ifndef GRX_STUB_ANDROID_LOG_H
#define GRX_STUB_ANDROID_LOG_H
enum { ANDROID_LOG_UNKNOWN = 0, ANDROID_LOG_DEFAULT, ANDROID_LOG_VERBOSE, ANDROID_LOG_DEBUG, ANDROID_LOG_INFO,
       ANDROID_LOG_WARN, ANDROID_LOG_ERROR, ANDROID_LOG_FATAL, ANDROID_LOG_SILENT };
int __android_log_print(int prio, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int __android_log_write(int prio, const char *tag, const char *text);
#endif
