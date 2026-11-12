/* tests/vr_glue: host stand-in for the NDK's <android/log.h> (the shell's logging). */
#ifndef VRGLUE_STUB_ANDROID_LOG_H
#define VRGLUE_STUB_ANDROID_LOG_H
enum { ANDROID_LOG_VERBOSE = 2, ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR };
int __android_log_print(int prio, const char *tag, const char *fmt, ...);
#endif
