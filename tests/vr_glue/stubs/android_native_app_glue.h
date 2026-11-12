/* tests/vr_glue: host stand-in for the NDK's native_app_glue (only the fields the shell's session code reads). */
#ifndef VRGLUE_STUB_NATIVE_APP_GLUE_H
#define VRGLUE_STUB_NATIVE_APP_GLUE_H
#include <stdint.h>
#include <jni.h>
typedef struct ANativeActivity {
    JavaVM *vm;
    jobject clazz;
    const char *internalDataPath, *externalDataPath;
} ANativeActivity;
struct android_app {
    void *userData;
    void (*onAppCmd)(struct android_app *app, int32_t cmd);
    ANativeActivity *activity;
    int destroyRequested;
};
#endif
