/* tests/game_render_x86: host stand-in for the NDK's <aaudio/AAudio.h> -- the subset apu_aaudio.c uses, with the
 * NDK's types and values. host_stubs.c implements it as "no audio device" (as tests/game_boot_arm64 does). */
#ifndef GRX_STUB_AAUDIO_H
#define GRX_STUB_AAUDIO_H
#include <stdint.h>
typedef int32_t aaudio_direction_t;
typedef int32_t aaudio_format_t;
typedef int32_t aaudio_result_t;
typedef int32_t aaudio_sharing_mode_t;
typedef int32_t aaudio_performance_mode_t;
typedef int32_t aaudio_usage_t;
typedef int32_t aaudio_content_type_t;
typedef int32_t aaudio_data_callback_result_t;
enum { AAUDIO_DIRECTION_OUTPUT = 0, AAUDIO_DIRECTION_INPUT = 1 };
enum { AAUDIO_FORMAT_INVALID = -1, AAUDIO_FORMAT_UNSPECIFIED = 0, AAUDIO_FORMAT_PCM_I16 = 1, AAUDIO_FORMAT_PCM_FLOAT = 2 };
enum { AAUDIO_OK = 0, AAUDIO_ERROR_BASE = -900, AAUDIO_ERROR_DISCONNECTED = -899, AAUDIO_ERROR_UNAVAILABLE = -889 };
enum { AAUDIO_SHARING_MODE_EXCLUSIVE = 0, AAUDIO_SHARING_MODE_SHARED = 1 };
enum { AAUDIO_PERFORMANCE_MODE_NONE = 10, AAUDIO_PERFORMANCE_MODE_POWER_SAVING = 11, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY = 12 };
enum { AAUDIO_USAGE_MEDIA = 1, AAUDIO_USAGE_GAME = 14 };
enum { AAUDIO_CONTENT_TYPE_SPEECH = 1, AAUDIO_CONTENT_TYPE_MUSIC = 2, AAUDIO_CONTENT_TYPE_MOVIE = 3, AAUDIO_CONTENT_TYPE_SONIFICATION = 4 };
enum { AAUDIO_CALLBACK_RESULT_CONTINUE = 0, AAUDIO_CALLBACK_RESULT_STOP };
typedef struct AAudioStreamStruct AAudioStream;
typedef struct AAudioStreamBuilderStruct AAudioStreamBuilder;
typedef aaudio_data_callback_result_t (*AAudioStream_dataCallback)(AAudioStream *stream, void *userData, void *audioData,
                                                                    int32_t numFrames);
typedef void (*AAudioStream_errorCallback)(AAudioStream *stream, void *userData, aaudio_result_t error);
aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder **builder);
const char *AAudio_convertResultToText(aaudio_result_t r);
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *b, int32_t v);
void AAudioStreamBuilder_setContentType(AAudioStreamBuilder *b, aaudio_content_type_t v);
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder *b, aaudio_direction_t v);
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *b, aaudio_format_t v);
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *b, aaudio_performance_mode_t v);
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *b, int32_t v);
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder *b, aaudio_sharing_mode_t v);
void AAudioStreamBuilder_setUsage(AAudioStreamBuilder *b, aaudio_usage_t v);
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder *b, AAudioStream_dataCallback cb, void *user);
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder *b, AAudioStream_errorCallback cb, void *user);
aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *b, AAudioStream **stream);
aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder *b);
aaudio_result_t AAudioStream_close(AAudioStream *s);
aaudio_result_t AAudioStream_requestStart(AAudioStream *s);
aaudio_result_t AAudioStream_requestStop(AAudioStream *s);
int32_t AAudioStream_getBufferSizeInFrames(AAudioStream *s);
aaudio_result_t AAudioStream_setBufferSizeInFrames(AAudioStream *s, int32_t n);
int32_t AAudioStream_getChannelCount(AAudioStream *s);
aaudio_format_t AAudioStream_getFormat(AAudioStream *s);
int32_t AAudioStream_getFramesPerBurst(AAudioStream *s);
aaudio_performance_mode_t AAudioStream_getPerformanceMode(AAudioStream *s);
int32_t AAudioStream_getSampleRate(AAudioStream *s);
int32_t AAudioStream_getXRunCount(AAudioStream *s);
#endif
