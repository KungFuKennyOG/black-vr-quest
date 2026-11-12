/* host_stubs.c - tests/game_render_x86: the Android system libraries the Quest build links that do not exist on an
 * x86-64 Linux host: liblog (to stderr), ASharedMemory (memfd always works here) and AAudio (no audio device, or with
 * GRX_AUDIO_WAV a fake device that records what the game plays to a WAV: see below). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>
#include <android/sharedmem.h>
#include <aaudio/AAudio.h>

static int s_min_prio = -1;

static int log_on(int prio)
{
    if (s_min_prio < 0) {
        const char *e = getenv("GRX_LOG_PRIO");          /* default: INFO and up */
        s_min_prio = e ? atoi(e) : ANDROID_LOG_INFO;
    }
    return prio >= s_min_prio;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    char line[2048];
    if (!log_on(prio)) return 0;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[%s] %s\n", tag, line);
    return 0;
}

int __android_log_write(int prio, const char *tag, const char *text)
{
    if (!log_on(prio)) return 0;
    fprintf(stderr, "[%s] %s\n", tag, text);
    return 0;
}

int ASharedMemory_create(const char *name, size_t size) { (void)name; (void)size; return -1; }

/* ---- AAudio ------------------------------------------------------------------------------------------------------
 * Without GRX_AUDIO_WAV: no audio device (openStream fails, the APU runs without host output).
 * With GRX_AUDIO_WAV=<path> (run.sh sets <frames>/audio.wav): a fake output device in place of the Quest's. The
 * production apu_aaudio.c is used unchanged -- its ring, prime/reprime gate and data callback -- and only the device
 * is swapped: a thread plays the "device" at the stream's rate on the wall clock, calling the data callback one burst
 * at a time exactly as AAudio's callback thread would, and appends what the callback returns to a 16-bit PCM WAV.
 * If the thread is late (2 busy cores) it catches up with back-to-back callbacks, so the file's timeline is the wall
 * clock: sample n was "played" at stream start + n / rate. The WAV header is rewritten every second and at exit, so a
 * killed run still leaves a valid file. Sidecar <path>.json: rate, channels, start time on the harness clock
 * (seconds after process start, which render_main's t= origin follows within milliseconds), callbacks, late bursts. */
#include <pthread.h>
#include <time.h>
#include <errno.h>

struct AAudioStreamBuilderStruct {
    int32_t rate, channels;
    aaudio_format_t format;
    AAudioStream_dataCallback cb;
    void *cb_user;
    AAudioStream_errorCallback ecb;
    void *ecb_user;
};

struct AAudioStreamStruct {
    int32_t rate, channels, burst, bufsize;
    aaudio_format_t format;
    AAudioStream_dataCallback cb;
    void *cb_user;
    pthread_t th;
    volatile int run;
    int started;
    FILE *wav;
    char path[1024];
    uint64_t frames, callbacks, late_bursts, max_late_frames;
    double start_t;
};

static double s_proc_t0;
static AAudioStream *s_wav_stream;
static pthread_mutex_t s_wav_lock = PTHREAD_MUTEX_INITIALIZER;

static double mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

__attribute__((constructor)) static void grx_audio_t0(void) { s_proc_t0 = mono_s(); }

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

/* the 44-byte RIFF/WAVE header for `frames` frames of 16-bit PCM */
static void wav_header(AAudioStream *s)
{
    uint8_t h[44];
    const uint32_t data = (uint32_t)(s->frames * (uint64_t)s->channels * 2u);
    memcpy(h, "RIFF", 4); put32(h + 4, 36 + data); memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16); put16(h + 20, 1); put16(h + 22, (uint16_t)s->channels); put32(h + 24, (uint32_t)s->rate);
    put32(h + 28, (uint32_t)s->rate * (uint32_t)s->channels * 2u); put16(h + 32, (uint16_t)(s->channels * 2));
    put16(h + 34, 16); memcpy(h + 36, "data", 4); put32(h + 40, data);
    fseek(s->wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), s->wav);
    fseek(s->wav, 0, SEEK_END);
    fflush(s->wav);
}

static void wav_sidecar(AAudioStream *s)
{
    char p[1100];
    FILE *f;
    snprintf(p, sizeof(p), "%s.json", s->path);
    if (!(f = fopen(p, "w"))) return;
    fprintf(f, "{\"rate\":%d,\"channels\":%d,\"format\":\"s16le\",\"burst\":%d,\"start_t\":%.6f,\"frames\":%llu,"
               "\"callbacks\":%llu,\"late_bursts\":%llu,\"max_late_ms\":%.1f}\n",
            s->rate, s->channels, s->burst, s->start_t, (unsigned long long)s->frames,
            (unsigned long long)s->callbacks, (unsigned long long)s->late_bursts,
            s->max_late_frames * 1000.0 / s->rate);
    fclose(f);
}

static void wav_finish_locked(void)
{
    if (!s_wav_stream || !s_wav_stream->wav) return;
    wav_header(s_wav_stream);
    wav_sidecar(s_wav_stream);
}

static void wav_atexit(void)
{
    pthread_mutex_lock(&s_wav_lock);
    wav_finish_locked();
    pthread_mutex_unlock(&s_wav_lock);
}

static void *device_thread(void *arg)
{
    AAudioStream *s = (AAudioStream *)arg;
    int16_t *buf = (int16_t *)calloc((size_t)s->burst * s->channels, sizeof(int16_t));
    double last_hdr = mono_s();
    const double t0 = mono_s();
    s->start_t = t0 - s_proc_t0;
    fprintf(stderr, "[AAUDIO-WAV] device started at t=%.3fs: %d Hz x%d, %d-frame bursts -> %s\n", s->start_t, s->rate,
            s->channels, s->burst, s->path);
    while (s->run && buf) {
        const double now = mono_s();
        const uint64_t due = (uint64_t)((now - t0) * s->rate);
        int n = 0;
        if (due >= s->frames + 2u * (uint64_t)s->burst) {         /* more than a burst behind the wall clock */
            const uint64_t late = due - s->frames;
            s->late_bursts++;
            if (late > s->max_late_frames) s->max_late_frames = late;
        }
        while (s->frames + (uint64_t)s->burst <= due && n++ < 64) {
            s->cb(s, s->cb_user, buf, s->burst);
            s->callbacks++;
            pthread_mutex_lock(&s_wav_lock);
            fwrite(buf, sizeof(int16_t) * s->channels, (size_t)s->burst, s->wav);
            s->frames += (uint64_t)s->burst;
            pthread_mutex_unlock(&s_wav_lock);
        }
        if (now - last_hdr >= 1.0) {
            pthread_mutex_lock(&s_wav_lock);
            wav_finish_locked();
            pthread_mutex_unlock(&s_wav_lock);
            last_hdr = now;
        }
        {
            /* sleep to the next burst boundary */
            double next = t0 + (double)(s->frames + (uint64_t)s->burst) / s->rate;
            struct timespec ts;
            ts.tv_sec = (time_t)next;
            ts.tv_nsec = (long)((next - (double)ts.tv_sec) * 1e9);
            while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
        }
    }
    free(buf);
    return NULL;
}

aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder **b)
{
    const char *path = getenv("GRX_AUDIO_WAV");
    if (!path || !*path) { *b = NULL; return AAUDIO_ERROR_UNAVAILABLE; }
    *b = (AAudioStreamBuilder *)calloc(1, sizeof(**b));
    return *b ? AAUDIO_OK : AAUDIO_ERROR_UNAVAILABLE;
}
const char *AAudio_convertResultToText(aaudio_result_t r)
{
    return r == AAUDIO_OK ? "AAUDIO_OK" : "AAUDIO_ERROR_UNAVAILABLE (x86 host test: GRX_AUDIO_WAV unset)";
}
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *b, int32_t v) { if (b) b->channels = v; }
void AAudioStreamBuilder_setContentType(AAudioStreamBuilder *b, aaudio_content_type_t v) { (void)b; (void)v; }
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder *b, aaudio_direction_t v) { (void)b; (void)v; }
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *b, aaudio_format_t v) { if (b) b->format = v; }
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *b, aaudio_performance_mode_t v) { (void)b; (void)v; }
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *b, int32_t v) { if (b) b->rate = v; }
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder *b, aaudio_sharing_mode_t v) { (void)b; (void)v; }
void AAudioStreamBuilder_setUsage(AAudioStreamBuilder *b, aaudio_usage_t v) { (void)b; (void)v; }
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder *b, AAudioStream_dataCallback cb, void *u)
{
    if (b) { b->cb = cb; b->cb_user = u; }
}
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder *b, AAudioStream_errorCallback cb, void *u)
{
    if (b) { b->ecb = cb; b->ecb_user = u; }
}
aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *b, AAudioStream **out)
{
    const char *path = getenv("GRX_AUDIO_WAV");
    AAudioStream *s;
    static int atexit_set;
    *out = NULL;
    if (!b || !b->cb || !path || !*path) return AAUDIO_ERROR_UNAVAILABLE;
    if (b->format != AAUDIO_FORMAT_PCM_I16 && b->format != AAUDIO_FORMAT_UNSPECIFIED) return AAUDIO_ERROR_UNAVAILABLE;
    if (!(s = (AAudioStream *)calloc(1, sizeof(*s)))) return AAUDIO_ERROR_UNAVAILABLE;
    s->rate = b->rate > 0 ? b->rate : 48000;
    s->channels = b->channels > 0 ? b->channels : 2;
    s->format = AAUDIO_FORMAT_PCM_I16;
    s->burst = 192;                       /* 4 ms at 48 kHz, a typical low-latency burst */
    s->bufsize = s->burst * 2;
    s->cb = b->cb; s->cb_user = b->cb_user;
    snprintf(s->path, sizeof(s->path), "%s", path);
    pthread_mutex_lock(&s_wav_lock);
    /* a reopened stream (device recovery) keeps appending to the same file */
    if (s_wav_stream && s_wav_stream->wav) {
        s->wav = s_wav_stream->wav; s->frames = s_wav_stream->frames;
        s_wav_stream->wav = NULL;
    } else {
        s->wav = fopen(path, "wb");
    }
    if (!s->wav) { pthread_mutex_unlock(&s_wav_lock); free(s); return AAUDIO_ERROR_UNAVAILABLE; }
    s_wav_stream = s;
    wav_header(s);
    pthread_mutex_unlock(&s_wav_lock);
    if (!atexit_set++) atexit(wav_atexit);
    *out = s;
    return AAUDIO_OK;
}
aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder *b) { free(b); return AAUDIO_OK; }
aaudio_result_t AAudioStream_requestStart(AAudioStream *s)
{
    if (!s) return AAUDIO_ERROR_UNAVAILABLE;
    if (s->started) return AAUDIO_OK;
    s->run = 1;
    if (pthread_create(&s->th, NULL, device_thread, s) != 0) { s->run = 0; return AAUDIO_ERROR_UNAVAILABLE; }
    s->started = 1;
    return AAUDIO_OK;
}
aaudio_result_t AAudioStream_requestStop(AAudioStream *s)
{
    if (s && s->started) { s->run = 0; pthread_join(s->th, NULL); s->started = 0; }
    return AAUDIO_OK;
}
aaudio_result_t AAudioStream_close(AAudioStream *s)
{
    if (!s) return AAUDIO_OK;
    AAudioStream_requestStop(s);
    pthread_mutex_lock(&s_wav_lock);
    if (s_wav_stream == s) { wav_finish_locked(); }
    pthread_mutex_unlock(&s_wav_lock);
    /* the file stays open (and s_wav_stream valid) for a reopen and for the exit-time header */
    return AAUDIO_OK;
}
int32_t AAudioStream_getBufferSizeInFrames(AAudioStream *s) { return s ? s->bufsize : 0; }
aaudio_result_t AAudioStream_setBufferSizeInFrames(AAudioStream *s, int32_t n) { if (s && n > 0) s->bufsize = n; return s ? s->bufsize : 0; }
int32_t AAudioStream_getChannelCount(AAudioStream *s) { return s ? s->channels : 2; }
aaudio_format_t AAudioStream_getFormat(AAudioStream *s) { return s ? s->format : AAUDIO_FORMAT_PCM_I16; }
int32_t AAudioStream_getFramesPerBurst(AAudioStream *s) { return s ? s->burst : 192; }
aaudio_performance_mode_t AAudioStream_getPerformanceMode(AAudioStream *s) { (void)s; return AAUDIO_PERFORMANCE_MODE_LOW_LATENCY; }
int32_t AAudioStream_getSampleRate(AAudioStream *s) { return s ? s->rate : 48000; }
int32_t AAudioStream_getXRunCount(AAudioStream *s) { (void)s; return 0; }
