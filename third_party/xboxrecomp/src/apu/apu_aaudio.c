/**
 * AAudio host audio backend (Android / Meta Quest).
 *
 * Implements the host-audio contract in apu_host_audio.h with an AAudio
 * output stream: 48 kHz, stereo, int16, PERFORMANCE_MODE_LOW_LATENCY, shared
 * mode, USAGE_GAME. AAudio pulls from a data callback on its own high-priority
 * thread; the APU monitor pushes fixed packets into a single-producer /
 * single-consumer ring of APU_AA_NUM_BUFS slots. The ring is the analogue of
 * XAudio2's source-voice buffer queue, so apu_core's pacing, backpressure and
 * prime logic behave exactly as on Windows:
 *
 *   producer (monitor thread)            consumer (AAudio callback)
 *   write slot[w % N], w++               read slot[r % N] from offset, r++
 *   BACKPRESSURE when w - r == N         silence + empty_queue when w == r
 *
 * The callback never blocks, logs or allocates. The "started" gate gives the
 * XAudio2 Start/Stop semantics without stopping the stream: the callback emits
 * silence until the producer has queued two packets, and drops back to that
 * state (a reprime) when it drains the ring.
 *
 * Device errors (AAUDIO_ERROR_DISCONNECTED when the Quest switches between
 * the built-in speakers and a headset, for instance) arrive on the error
 * callback, which may not close the stream. It only raises a flag; the owner
 * thread closes and reopens on its next submit, at most once a second.
 */
#if defined(__ANDROID__)

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <aaudio/AAudio.h>
#include <android/log.h>
#include "apu_host_audio.h"

#define APU_AA_RATE           HOST_AUDIO_SAMPLE_RATE
#define APU_AA_CHANNELS       HOST_AUDIO_CHANNELS
#define APU_AA_BUF_SAMPLES    1024   /* frames per packet, ~21 ms, as XAudio2 */
#define APU_AA_NUM_BUFS       3
#define APU_AA_RETRY_MS       1000

#define AA_LOG(...) __android_log_print(ANDROID_LOG_INFO, "xboxrecomp-audio", __VA_ARGS__)
#define AA_ERR(...) __android_log_print(ANDROID_LOG_ERROR, "xboxrecomp-audio", __VA_ARGS__)

typedef struct {
    int16_t  pcm[APU_AA_BUF_SAMPLES][APU_AA_CHANNELS];
    uint32_t frames;
} AaSlot;

static AaSlot          s_ring[APU_AA_NUM_BUFS];
static uint64_t        s_w;                 /* producer count (owner) */
static uint64_t        s_r;                 /* consumer count (callback) */
static uint32_t        s_read_off;          /* frames used of slot s_r (callback) */
static int             s_started;           /* callback-visible gate */
static uint64_t        s_played;            /* frames played (callback) */
static uint64_t        s_underruns;         /* empty-queue callbacks while started */
static uint64_t        s_reprimes;
static int             s_error_pending;
static int32_t         s_error_status;
static unsigned        s_error_count;

static AAudioStream   *s_stream;
static int             s_initialized;
static pthread_t       s_owner;
static int             s_frames_written;
static uint64_t        s_retry_at_ms;
static Xa2OutputStats  s_stats;
static uint64_t        s_played_base;       /* frames played by earlier streams */
static uint64_t        s_generation_submitted;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int is_owner(void)
{
    return s_initialized && pthread_equal(s_owner, pthread_self());
}

/* ---- AAudio callbacks (AAudio's thread) -------------------------------- */

static aaudio_data_callback_result_t aa_data(AAudioStream *stream, void *user,
                                             void *audio, int32_t frames)
{
    int16_t (*out)[APU_AA_CHANNELS] = audio;
    int32_t done = 0;
    (void)stream; (void)user;

    if (__atomic_load_n(&s_started, __ATOMIC_ACQUIRE)) {
        while (done < frames) {
            uint64_t w = __atomic_load_n(&s_w, __ATOMIC_ACQUIRE);
            uint64_t r = s_r;
            if (r == w) {
                /* Ran dry: XAudio2 would report BuffersQueued == 0 and the
                 * producer would Stop() and re-prime. Do the same here. */
                __atomic_store_n(&s_started, 0, __ATOMIC_RELEASE);
                __atomic_add_fetch(&s_underruns, 1, __ATOMIC_RELAXED);
                __atomic_add_fetch(&s_reprimes, 1, __ATOMIC_RELAXED);
                break;
            }
            AaSlot *slot = &s_ring[r % APU_AA_NUM_BUFS];
            uint32_t avail = slot->frames - s_read_off;
            uint32_t take = (uint32_t)(frames - done) < avail ? (uint32_t)(frames - done) : avail;
            memcpy(out[done], slot->pcm[s_read_off], take * sizeof(out[0]));
            done += (int32_t)take;
            s_read_off += take;
            __atomic_add_fetch(&s_played, take, __ATOMIC_RELAXED);
            if (s_read_off == slot->frames) {
                s_read_off = 0;
                __atomic_store_n(&s_r, r + 1, __ATOMIC_RELEASE);   /* slot free */
            }
        }
    }
    if (done < frames)
        memset(out[done], 0, (size_t)(frames - done) * sizeof(out[0]));
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void aa_error(AAudioStream *stream, void *user, aaudio_result_t error)
{
    (void)stream; (void)user;
    __atomic_store_n(&s_error_status, (int32_t)error, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_error_count, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&s_error_pending, 1, __ATOMIC_RELEASE);
}

/* ---- stream lifetime (owner thread) ------------------------------------ */

static void aa_close(void)
{
    if (s_stream) {
        AAudioStream_requestStop(s_stream);
        AAudioStream_close(s_stream);      /* waits for the callback to finish */
        s_stream = NULL;
    }
    {
        uint64_t played = __atomic_load_n(&s_played, __ATOMIC_ACQUIRE);
        uint64_t w = s_w, r = __atomic_load_n(&s_r, __ATOMIC_ACQUIRE);
        uint64_t queued_frames = 0;
        for (uint64_t i = r; i < w; i++)
            queued_frames += s_ring[i % APU_AA_NUM_BUFS].frames;
        queued_frames -= s_read_off;
        s_stats.discarded_samples += queued_frames;
        s_played_base += played;
    }
    /* No callback can run now: reset the ring for the next stream. */
    __atomic_store_n(&s_played, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_started, 0, __ATOMIC_RELAXED);
    s_w = s_r = 0;
    s_read_off = 0;
    s_generation_submitted = 0;
}

static int aa_open(void)
{
    AAudioStreamBuilder *b = NULL;
    aaudio_result_t rc = AAudio_createStreamBuilder(&b);
    if (rc != AAUDIO_OK || !b) {
        AA_ERR("createStreamBuilder: %s", AAudio_convertResultToText(rc));
        return 0;
    }
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, APU_AA_CHANNELS);
    AAudioStreamBuilder_setSampleRate(b, APU_AA_RATE);
#if __ANDROID_API__ >= 28
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_SONIFICATION);
#endif
    AAudioStreamBuilder_setDataCallback(b, aa_data, NULL);
    AAudioStreamBuilder_setErrorCallback(b, aa_error, NULL);
    rc = AAudioStreamBuilder_openStream(b, &s_stream);
    AAudioStreamBuilder_delete(b);
    if (rc != AAUDIO_OK || !s_stream) {
        AA_ERR("openStream: %s", AAudio_convertResultToText(rc));
        s_stream = NULL;
        return 0;
    }
    if (AAudioStream_getSampleRate(s_stream) != APU_AA_RATE ||
        AAudioStream_getChannelCount(s_stream) != APU_AA_CHANNELS ||
        AAudioStream_getFormat(s_stream) != AAUDIO_FORMAT_PCM_I16) {
        AA_ERR("stream is %d Hz x%d fmt %d, need 48000 x2 i16",
               AAudioStream_getSampleRate(s_stream),
               AAudioStream_getChannelCount(s_stream),
               (int)AAudioStream_getFormat(s_stream));
        AAudioStream_close(s_stream);
        s_stream = NULL;
        return 0;
    }
    /* Two bursts of device buffering is the low-latency recommendation; the
     * 3-packet ring above it is where the producer's jitter is absorbed. */
    {
        int32_t burst = AAudioStream_getFramesPerBurst(s_stream);
        if (burst > 0)
            AAudioStream_setBufferSizeInFrames(s_stream, burst * 2);
    }
    rc = AAudioStream_requestStart(s_stream);
    if (rc != AAUDIO_OK) {
        AA_ERR("requestStart: %s", AAudio_convertResultToText(rc));
        ++s_stats.start_failed;
        AAudioStream_close(s_stream);
        s_stream = NULL;
        return 0;
    }
    __atomic_store_n(&s_error_pending, 0, __ATOMIC_RELEASE);
    return 1;
}

static int aa_recover_if_needed(void)
{
    if (__atomic_load_n(&s_error_pending, __ATOMIC_ACQUIRE)) {
        AA_ERR("stream error %d (%s); reopening",
               s_error_status, AAudio_convertResultToText(s_error_status));
        aa_close();
        __atomic_store_n(&s_error_pending, 0, __ATOMIC_RELEASE);
        s_retry_at_ms = 0;
    }
    if (s_stream) return 1;
    if (now_ms() < s_retry_at_ms) return 0;
    ++s_stats.recovery_attempts;
    if (!aa_open()) {
        ++s_stats.recovery_failed;
        s_retry_at_ms = now_ms() + APU_AA_RETRY_MS;
        return 0;
    }
    ++s_stats.recoveries;
    s_retry_at_ms = now_ms() + APU_AA_RETRY_MS;
    AA_LOG("output recovered; priming fresh PCM");
    return 1;
}

/* ---- contract ---------------------------------------------------------- */

int xa2_init(void)
{
    if (s_initialized)
        return is_owner();
    memset(&s_stats, 0, sizeof(s_stats));
    s_played_base = 0;
    s_frames_written = 0;
    s_retry_at_ms = 0;
    s_w = s_r = 0;
    s_read_off = 0;
    s_started = 0;
    s_played = s_underruns = s_reprimes = 0;
    s_error_pending = 0;
    s_error_count = 0;
    s_error_status = 0;
    if (!aa_open())
        return 0;
    s_owner = pthread_self();
    s_initialized = 1;
    AA_LOG("AAudio output: %d Hz stereo i16, %d x %d-frame packets, burst %d, "
           "buffer %d frames, perf mode %d",
           APU_AA_RATE, APU_AA_NUM_BUFS, APU_AA_BUF_SAMPLES,
           AAudioStream_getFramesPerBurst(s_stream),
           AAudioStream_getBufferSizeInFrames(s_stream),
           (int)AAudioStream_getPerformanceMode(s_stream));
    fprintf(stderr, "[AAUDIO] initialized (%d Hz stereo 16-bit, %d x %d-sample packets)\n",
            APU_AA_RATE, APU_AA_NUM_BUFS, APU_AA_BUF_SAMPLES);
    return 1;
}

void xa2_shutdown(void)
{
    if (!is_owner()) return;
    aa_close();
    s_initialized = 0;
    fprintf(stderr, "[AAUDIO] shut down (%d packets written)\n", s_frames_written);
}

int xa2_is_active(void)
{
    return s_initialized;
}

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    if (!is_owner() || !samples || num_samples <= 0) return XA2_SUBMIT_ERROR;
    if (!aa_recover_if_needed()) return XA2_SUBMIT_ERROR;

    uint64_t r = __atomic_load_n(&s_r, __ATOMIC_ACQUIRE);
    uint64_t queued = s_w - r;
    if (!queued && s_generation_submitted)
        ++s_stats.empty_queue;
    if (queued >= APU_AA_NUM_BUFS) {
        /* Start gate: a full ring is always enough to play. */
        __atomic_store_n(&s_started, 1, __ATOMIC_RELEASE);
        ++s_stats.queue_full;
        return XA2_SUBMIT_BACKPRESSURE;
    }
    {
        AaSlot *slot = &s_ring[s_w % APU_AA_NUM_BUFS];
        uint32_t n = num_samples > APU_AA_BUF_SAMPLES ? APU_AA_BUF_SAMPLES : (uint32_t)num_samples;
        memcpy(slot->pcm, samples, n * sizeof(slot->pcm[0]));
        slot->frames = n;
        __atomic_store_n(&s_w, s_w + 1, __ATOMIC_RELEASE);   /* publish */
        s_stats.submitted_samples += n;
        s_generation_submitted += n;
        s_frames_written++;
    }
    /* Prime two packets (~42.7 ms) before playback, as the XAudio2 backend
     * does, so ordinary scheduling jitter cannot drain a single packet. */
    if (queued + 1 >= 2)
        __atomic_store_n(&s_started, 1, __ATOMIC_RELEASE);
    return XA2_SUBMIT_ACCEPTED;
}

int xa2_get_buffer_size(void)
{
    return APU_AA_BUF_SAMPLES;
}

void xa2_get_output_stats(Xa2OutputStats *stats)
{
    *stats = s_stats;
    stats->played_samples = s_played_base + __atomic_load_n(&s_played, __ATOMIC_ACQUIRE);
    stats->reprimes = __atomic_load_n(&s_reprimes, __ATOMIC_RELAXED);
    stats->critical_errors = __atomic_load_n(&s_error_count, __ATOMIC_RELAXED);
    stats->critical_error_status = __atomic_load_n(&s_error_status, __ATOMIC_RELAXED);
    stats->queued_buffers = (unsigned)(s_w - __atomic_load_n(&s_r, __ATOMIC_ACQUIRE));
    stats->mmcss_registered = 0;
}

/* Device-side underruns (AAudio's own xrun counter), for the Android shell's
 * diagnostics overlay. Not part of Xa2OutputStats, whose fields keep their
 * XAudio2 meanings. */
int32_t apu_aaudio_xrun_count(void)
{
    return s_stream ? AAudioStream_getXRunCount(s_stream) : 0;
}

#endif /* __ANDROID__ */
