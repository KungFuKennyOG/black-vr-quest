/**
 * Host audio output -- the contract between the APU monitor and a platform
 * audio API.
 *
 * apu_core.c produces 48 kHz interleaved stereo int16 in fixed packets of
 * xa2_get_buffer_size() frames (a multiple of the 256-frame mixer block, at
 * most 2048) and hands them over with xa2_submit_samples() on its monitor
 * thread. Everything it decides -- backpressure waits, the silent "prime"
 * packet after a tail, start retries -- reads only the values below, so a
 * backend is interchangeable as long as it keeps these semantics:
 *
 *   - init/shutdown/submit/stats run on one owner thread (the monitor);
 *     calls from any other thread are refused.
 *   - submit copies the PCM; ACCEPTED means the backend owns it.
 *     BACKPRESSURE means every packet slot is still queued (retry later);
 *     ERROR means the device is unavailable (drop, recovery is internal).
 *   - playback starts once two packets are queued, and after the queue runs
 *     dry the backend re-primes (waits for two again) rather than chasing a
 *     single packet. queued_buffers counts packets not yet fully played.
 *   - a device-level failure is recovered internally by recreating the
 *     output, at most once per second, counted in the recovery stats.
 *
 * Backends (exactly one is linked):
 *   _WIN32       apu_xaudio2.c   XAudio2 (source voice, 3 x 1024-frame buffers)
 *   __ANDROID__  apu_aaudio.c    AAudio low-latency stream, data callback
 *                                pulling from a 3 x 1024-frame SPSC ring
 *   other POSIX  apu_xaudio2.c   inactive stubs (apu_core then outputs nothing)
 *
 * The xa2_ prefix is historical: it is the name apu_core already calls, kept
 * so the Windows build is byte-for-byte unchanged.
 */
#ifndef APU_HOST_AUDIO_H
#define APU_HOST_AUDIO_H

#include "apu_xaudio2.h"

#define HOST_AUDIO_SAMPLE_RATE  48000
#define HOST_AUDIO_CHANNELS     2

#endif /* APU_HOST_AUDIO_H */
