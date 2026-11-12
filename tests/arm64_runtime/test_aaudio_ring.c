/*
 * apu_aaudio.c's packet ring and XAudio2-compatible semantics, exercised
 * without a device: this file supplies a fake AAudio (the NDK only has
 * libaaudio.so, which a static test cannot load, and the emulator has no
 * audio anyway) and drives the data callback by hand.
 *
 * Checked: accept/backpressure at 3 packets, the two-packet start gate,
 * silence while unprimed, chronological PCM out of the callback across
 * packet boundaries and odd burst sizes, reprime after an underrun, stats,
 * owner-thread refusal, and recovery from an error-callback disconnect.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <aaudio/AAudio.h>
#include "apu_xaudio2.h"

/* ---- fake AAudio ---- */
struct AAudioStreamStruct { int dummy; };
struct AAudioStreamBuilderStruct { AAudioStream_dataCallback data; AAudioStream_errorCallback err; };
static struct AAudioStreamBuilderStruct s_b;
static struct AAudioStreamStruct s_stream;
static AAudioStream_dataCallback s_data;
static AAudioStream_errorCallback s_err;
static int s_opens, s_closes, s_fail_open;

aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder **b) { *b = &s_b; return AAUDIO_OK; }
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder *b, aaudio_direction_t d) { (void)b; (void)d; }
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder *b, aaudio_sharing_mode_t m) { (void)b; (void)m; }
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *b, aaudio_performance_mode_t m) { (void)b; (void)m; }
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *b, aaudio_format_t f) { (void)b; (void)f; }
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *b, int32_t c) { (void)b; (void)c; }
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *b, int32_t r) { (void)b; (void)r; }
void AAudioStreamBuilder_setUsage(AAudioStreamBuilder *b, aaudio_usage_t u) { (void)b; (void)u; }
void AAudioStreamBuilder_setContentType(AAudioStreamBuilder *b, aaudio_content_type_t c) { (void)b; (void)c; }
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder *b, AAudioStream_dataCallback cb, void *u) { (void)u; b->data = cb; }
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder *b, AAudioStream_errorCallback cb, void *u) { (void)u; b->err = cb; }
aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *b, AAudioStream **s)
{ if (s_fail_open) return AAUDIO_ERROR_UNAVAILABLE; s_data = b->data; s_err = b->err; *s = &s_stream; s_opens++; return AAUDIO_OK; }
aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder *b) { (void)b; return AAUDIO_OK; }
int32_t AAudioStream_getSampleRate(AAudioStream *s) { (void)s; return 48000; }
int32_t AAudioStream_getChannelCount(AAudioStream *s) { (void)s; return 2; }
aaudio_format_t AAudioStream_getFormat(AAudioStream *s) { (void)s; return AAUDIO_FORMAT_PCM_I16; }
int32_t AAudioStream_getFramesPerBurst(AAudioStream *s) { (void)s; return 192; }
aaudio_result_t AAudioStream_setBufferSizeInFrames(AAudioStream *s, int32_t n) { (void)s; return n; }
int32_t AAudioStream_getBufferSizeInFrames(AAudioStream *s) { (void)s; return 384; }
aaudio_performance_mode_t AAudioStream_getPerformanceMode(AAudioStream *s) { (void)s; return AAUDIO_PERFORMANCE_MODE_LOW_LATENCY; }
aaudio_result_t AAudioStream_requestStart(AAudioStream *s) { (void)s; return AAUDIO_OK; }
aaudio_result_t AAudioStream_requestStop(AAudioStream *s) { (void)s; return AAUDIO_OK; }
aaudio_result_t AAudioStream_close(AAudioStream *s) { (void)s; s_closes++; s_data = NULL; return AAUDIO_OK; }
int32_t AAudioStream_getXRunCount(AAudioStream *s) { (void)s; return 0; }
const char *AAudio_convertResultToText(aaudio_result_t r) { (void)r; return "fake"; }

static int failures, checks;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static int16_t pkt[1024][2];
static void fill(int base) { for (int i = 0; i < 1024; i++) { pkt[i][0] = (int16_t)(base + i); pkt[i][1] = (int16_t)-(base + i); } }

/* Pull n frames through the callback; returns how many were non-silent and
 * checks they are the expected running sequence. */
static int s_expect;
static int pull(int n)
{
    int16_t out[4096][2];
    int got = 0;
    s_data(&s_stream, NULL, out, n);
    for (int i = 0; i < n; i++) {
        if (out[i][0] == 0 && out[i][1] == 0 && s_expect != 0) continue;
        if (out[i][0] == 0 && out[i][1] == 0) continue;
        if (out[i][0] != (int16_t)s_expect || out[i][1] != (int16_t)-s_expect) {
            if (failures < 20) printf("FAIL order: frame %d = %d, expected %d\n", i, out[i][0], s_expect);
            failures++;
        }
        s_expect++;
        got++;
    }
    return got;
}

static void *other_thread(void *arg) { (void)arg; return (void *)(intptr_t)xa2_submit_samples(&pkt[0][0], 1024); }

int main(void)
{
    Xa2OutputStats st;
    CHECK(xa2_init() == 1);
    CHECK(xa2_is_active());
    CHECK(xa2_get_buffer_size() == 1024);

    /* unprimed: silence */
    s_expect = 1;
    CHECK(pull(480) == 0);

    fill(1);
    CHECK(xa2_submit_samples(&pkt[0][0], 1024) == XA2_SUBMIT_ACCEPTED);
    CHECK(pull(480) == 0);                   /* one packet: still gated */
    fill(1025);
    CHECK(xa2_submit_samples(&pkt[0][0], 1024) == XA2_SUBMIT_ACCEPTED);
    fill(2049);
    CHECK(xa2_submit_samples(&pkt[0][0], 1024) == XA2_SUBMIT_ACCEPTED);
    CHECK(xa2_submit_samples(&pkt[0][0], 1024) == XA2_SUBMIT_BACKPRESSURE);
    xa2_get_output_stats(&st);
    CHECK(st.queued_buffers == 3 && st.queue_full == 1 && st.submitted_samples == 3072);

    /* other threads are refused */
    {
        pthread_t t; void *r;
        pthread_create(&t, NULL, other_thread, NULL);
        pthread_join(t, &r);
        CHECK((intptr_t)r == XA2_SUBMIT_ERROR);
    }

    /* drain in odd bursts across packet boundaries */
    CHECK(pull(500) == 500);
    CHECK(pull(700) == 700);                 /* crosses into packet 2 */
    xa2_get_output_stats(&st);
    CHECK(st.queued_buffers == 2 && st.played_samples == 1200);
    fill(3073);
    CHECK(xa2_submit_samples(&pkt[0][0], 1024) == XA2_SUBMIT_ACCEPTED);
    CHECK(pull(4000) == 4096 - 1200);        /* everything, then underrun */
    xa2_get_output_stats(&st);
    CHECK(st.queued_buffers == 0 && st.reprimes == 1 && st.played_samples == 4096);

    /* after an underrun: re-primes, one packet is not enough */
    fill(4097);
    CHECK(xa2_submit_samples(&pkt[0][0], 1024) == XA2_SUBMIT_ACCEPTED);
    xa2_get_output_stats(&st);
    CHECK(st.empty_queue == 1);
    CHECK(pull(256) == 0);
    fill(5121);
    CHECK(xa2_submit_samples(&pkt[0][0], 1024) == XA2_SUBMIT_ACCEPTED);
    CHECK(pull(256) == 256);

    /* device lost (e.g. headset switch): error callback, reopen on submit */
    s_err(&s_stream, NULL, AAUDIO_ERROR_DISCONNECTED);
    fill(1);
    s_expect = 1;
    CHECK(xa2_submit_samples(&pkt[0][0], 1024) == XA2_SUBMIT_ACCEPTED);
    xa2_get_output_stats(&st);
    CHECK(s_opens == 2 && s_closes == 1);
    CHECK(st.recoveries == 1 && st.critical_errors == 1 && st.queued_buffers == 1);
    CHECK(st.discarded_samples == 1792);     /* 2048 queued - 256 played */

    /* reopen failure is retried no sooner than a second later */
    s_err(&s_stream, NULL, AAUDIO_ERROR_DISCONNECTED);
    s_fail_open = 1;
    CHECK(xa2_submit_samples(&pkt[0][0], 1024) == XA2_SUBMIT_ERROR);
    s_fail_open = 0;
    CHECK(xa2_submit_samples(&pkt[0][0], 1024) == XA2_SUBMIT_ERROR);   /* within retry delay */
    xa2_get_output_stats(&st);
    CHECK(st.recovery_failed == 1);

    xa2_shutdown();
    CHECK(!xa2_is_active());
    printf("aaudio ring: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
