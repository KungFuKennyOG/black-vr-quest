/*
 * Stand-ins for everything the arm64 boot test does not run: the GPU (Kelvin,
 * the Vulkan backend), USB (OHCI), the PC video/menu modules, the XR shell's
 * vr_* contract, and the Android shared libraries a static executable cannot
 * link (liblog, libaaudio, libandroid).
 *
 * GPU entry points the title reaches from game code count as the first GPU
 * touch (boot_main.c stops there). kelvin_start/kelvin_active are called by
 * the runtime itself during MemoryLayoutInit, so they report Kelvin inactive,
 * which makes the runtime's NV2A ack thread stand in for the GPU (it echoes
 * DMA PUT into GET) exactly as on a build without RECOMP_NV2A_KELVIN.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pc_video.h"
#include "pc_video_menu.h"
#include "pc_menu.h"
#include "pc_movie_skip.h"
#include "pc_photo.h"
#include "pc_split.h"
#include "pc_crosshair.h"
#include "pc_debug.h"
#include "vr_bridge.h"
#include <aaudio/AAudio.h>

volatile int boot_gpu_touched;
/* Set once xbox_MemoryLayoutInit has mapped the NV2A aperture (it calls kelvin_start
 * after mapping it); the monitor must not read registers before then. */
volatile int boot_nv2a_mapped;
const char *volatile boot_gpu_first;

static void gpu_touch(const char *what)
{
    if (!boot_gpu_first)
        boot_gpu_first = what;
    boot_gpu_touched = 1;
}

/* ---- Kelvin / GPU ---------------------------------------------------------------------------- */
int  kelvin_active(void) { return 0; }
void kelvin_start(void) { __atomic_store_n(&boot_nv2a_mapped, 1, __ATOMIC_RELEASE); fprintf(stderr, "[BOOT-STUB] kelvin_start: GPU engine stubbed (ack thread stands in)\n"); }
int  kelvin_irq_asserted(void) { return 0; }
void *kelvin_irq_event(void) { return NULL; }
void kelvin_vblank(void) {}
int  kelvin_owns_register(unsigned int offset) { (void)offset; return 0; }
int  kelvin_mark_frame_aspect(uint32_t va, uint32_t w, uint32_t h) { (void)va; (void)w; (void)h; gpu_touch("kelvin_mark_frame_aspect"); return 0; }
int  kelvin_mark_view(uint32_t va, int mode, int slot, int x0, int y0, int x1, int y1)
{ (void)va; (void)mode; (void)slot; (void)x0; (void)y0; (void)x1; (void)y1; gpu_touch("kelvin_mark_view"); return 0; }
int  kgpu_capture_result(void) { return 0; }
void kgpu_request_capture(const char *path) { (void)path; }
void kgpu_set_ao_camera(float hx, float hy) { (void)hx; (void)hy; }

/* ---- USB, framebuffer, FMV ------------------------------------------------------------------- */
void xbox_OhciInit(void) { fprintf(stderr, "[BOOT-STUB] xbox_OhciInit: USB stubbed (no pad)\n"); }
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; gpu_touch("xbox_FramebufferWindowSet"); }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferGammaRamp(const uint8_t *r, const uint8_t *g, const uint8_t *b) { (void)r; (void)g; (void)b; }
int  xbox_FramebufferRequestClose(void) { return 0; }
int  xbox_VideoPlayFile(const char *host_path) { fprintf(stderr, "[BOOT-STUB] video %s skipped\n", host_path); return 0; }
int  xbox_VideoIsPlaying(void) { return 0; }

/* ---- PC video settings / menus / photo / split / crosshair / debug --------------------------- */
void pc_video_get(PcVideoSettings *out) { memset(out, 0, sizeof(*out)); }
int  pc_video_set(const PcVideoSettings *wanted) { (void)wanted; return 0; }
unsigned pc_video_aspect(void) { return 0; }
const char *pc_video_aspect_name(unsigned aspect) { (void)aspect; return "original"; }
double pc_video_fov(void) { return 0.0; }
int  pc_video_motion_blur_off(void) { return 0; }
void pc_video_menu_page_seen(void) {}
void pc_video_menu_set_native_opener(void (*request)(void)) { (void)request; }
int  pc_menu_has_font(int slot) { (void)slot; return 0; }
int  pc_menu_load_bundle_images(const uint8_t *d, size_t n, const char *movie, const uint32_t *ids, const int *slots, int c)
{ (void)d; (void)n; (void)movie; (void)ids; (void)slots; (void)c; return 0; }
int  pc_menu_load_font_file(int slot, const uint8_t *d, size_t n) { (void)slot; (void)d; (void)n; return 0; }
void pc_menu_request_video(void) {}
void pc_menu_set_default_definition(const char *text) { (void)text; }
void pc_menu_set_replaced_defaults(const uint64_t *fnv1a, int count) { (void)fnv1a; (void)count; }
int  pc_menu_take_input(void) { return 0; }
int  pc_menu_tick(const char *page, int ready, const PcMenuHost *host) { (void)page; (void)ready; (void)host; return 0; }
void pc_movie_skip_clear(void) {}
void pc_movie_skip_subtitle_target(uint64_t s, const char *stem) { (void)s; (void)stem; }
int  pc_movie_skip_take_request(uint64_t s) { (void)s; return 0; }
void pc_movie_skip_update(uint64_t s, int kind, int playing) { (void)s; (void)kind; (void)playing; }
int  pc_photo_active(void) { return 0; }
int  pc_photo_camera(float m[4][3], float half_y) { (void)m; (void)half_y; return 0; }
void pc_photo_set_host(const PcPhotoHost *host) { (void)host; }
int  pc_photo_weapon_hidden(void) { return 0; }
float pc_photo_zoom(void) { return 1.0f; }
void pc_split_camera(float m[4][3], const float p1[4][3]) { (void)m; (void)p1; }
int  pc_split_enabled(void) { return 0; }
void pc_split_rect(int player, int r[4]) { (void)player; r[0] = r[1] = 0; r[2] = 640; r[3] = 480; }
void pc_crosshair_hit_for(int player, int kind) { (void)player; (void)kind; }
void pc_crosshair_tick(int c, float s, unsigned n, float z) { (void)c; (void)s; (void)n; (void)z; }
void pc_crosshair_tick_for(int p, int c, float s, unsigned n, float z) { (void)p; (void)c; (void)s; (void)n; (void)z; }
void pc_debug_game_row(unsigned slot, const char *label, const char *fmt, ...) { (void)slot; (void)label; (void)fmt; }

/* ---- the XR shell's contract (port/quest/vr_bridge.h): no headset, flat ---------------------- */
static VrFrameState s_frame;
static VrGameConfig s_cfg;
const VrFrameState *vr_frame_state(void) { return &s_frame; }
const VrFrameState *vr_latch_frame_state(void) { return &s_frame; }
int  vr_active(void) { return 0; }
void vr_publish_game_camera(const VrGameCamera *cam) { (void)cam; }
void vr_publish_game_camera_ex(const VrGameCamera *cam, const VrGameCameraExt *ext) { (void)cam; (void)ext; }
void vr_set_present_mode(int mode) { (void)mode; }
int  vr_present_mode(void) { return 0; }
void vr_get_recentre(VrPose *origin) { memset(origin, 0, sizeof(*origin)); origin->orientation.w = 1.0f; }
double vr_consume_turn(void) { return 0.0; }
const VrGameConfig *vr_game_config(void) { return &s_cfg; }
void vr_set_rumble(uint16_t l, uint16_t r) { (void)l; (void)r; }
void vr_game_haptic(int hand, float strength, uint32_t ms) { (void)hand; (void)strength; (void)ms; }
int  vr_get_pad(VrPadState *out) { memset(out, 0, sizeof(*out)); return 0; }
void vr_mark_world_pass(uint32_t va, int begin) { (void)va; (void)begin; }
/* gun latch (port/notes/gun-latch.md) */
void vr_publish_game_gun(const VrGameGun *gun) { (void)gun; }
void vr_mark_viewmodel_mvp(const float mvp[16], int constant_register) { (void)mvp; (void)constant_register; }
void vr_mark_viewmodel_offhand(const float a[16], const float c[3]) { (void)a; (void)c; }
void vr_grip_correction(float c[3]) { c[0] = c[1] = c[2] = 0.0f; }
void vr_set_gun_barrel(const float d[3], int valid) { (void)d; (void)valid; }
void vr_set_gun_laser(const float m[3], const float d[3], float l, int v) { (void)m; (void)d; (void)l; (void)v; }
void vr_grip_lock(float d[16]) { int i; for (i = 0; i < 16; i++) d[i] = (i % 5 == 0) ? 1.0f : 0.0f; }
void vr_note_shot(void) { }
/* VR controls (port/notes/xr-shell.md "Input") */
void vr_set_action_resolver(int (*resolve)(int slot)) { (void)resolve; }
void vr_publish_game_input(const VrGameInputState *s) { (void)s; }
int  vr_game_aim_request(void) { return -1; }
/* VR SETTINGS in the game's Options menu (port/notes/settings-panel.md): no shell, the stock pages */
int  vr_menu_native(void) { return 0; }
void vr_menu_enter(void) {}
int  vr_menu_rows(int section, VrMenuRow *out, int max) { (void)section; (void)out; (void)max; return 0; }
int  vr_menu_activate(int section, int row, int dir) { (void)section; (void)row; (void)dir; return VR_MENU_NOTHING; }
const char *vr_menu_title(int section) { (void)section; return "VR SETTINGS"; }
void vr_menu_leave(void) {}

/* ---- liblog / libandroid / libaaudio (shared-only in the NDK) -------------------------------- */
int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%d %s] ", prio, tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return 0;
}
int __android_log_write(int prio, const char *tag, const char *text)
{
    fprintf(stderr, "[%d %s] %s\n", prio, tag, text);
    return 0;
}
int ASharedMemory_create(const char *name, size_t size) { (void)name; (void)size; return -1; }

/* No audio device: AAudio refuses to build a stream, the APU runs without host output. */
aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder **b) { *b = NULL; return AAUDIO_ERROR_UNAVAILABLE; }
const char *AAudio_convertResultToText(aaudio_result_t r) { (void)r; return "AAUDIO_ERROR_UNAVAILABLE (boot test)"; }
#define BUILDER_SET(name, type) void AAudioStreamBuilder_##name(AAudioStreamBuilder *b, type v) { (void)b; (void)v; }
BUILDER_SET(setChannelCount, int32_t)
BUILDER_SET(setContentType, aaudio_content_type_t)
BUILDER_SET(setDirection, aaudio_direction_t)
BUILDER_SET(setFormat, aaudio_format_t)
BUILDER_SET(setPerformanceMode, aaudio_performance_mode_t)
BUILDER_SET(setSampleRate, int32_t)
BUILDER_SET(setSharingMode, aaudio_sharing_mode_t)
BUILDER_SET(setUsage, aaudio_usage_t)
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder *b, AAudioStream_dataCallback cb, void *u) { (void)b; (void)cb; (void)u; }
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder *b, AAudioStream_errorCallback cb, void *u) { (void)b; (void)cb; (void)u; }
aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *b, AAudioStream **s) { (void)b; *s = NULL; return AAUDIO_ERROR_UNAVAILABLE; }
aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder *b) { (void)b; return AAUDIO_OK; }
aaudio_result_t AAudioStream_close(AAudioStream *s) { (void)s; return AAUDIO_OK; }
aaudio_result_t AAudioStream_requestStart(AAudioStream *s) { (void)s; return AAUDIO_ERROR_UNAVAILABLE; }
aaudio_result_t AAudioStream_requestStop(AAudioStream *s) { (void)s; return AAUDIO_OK; }
int32_t AAudioStream_getBufferSizeInFrames(AAudioStream *s) { (void)s; return 0; }
aaudio_result_t AAudioStream_setBufferSizeInFrames(AAudioStream *s, int32_t n) { (void)s; return n; }
int32_t AAudioStream_getChannelCount(AAudioStream *s) { (void)s; return 2; }
aaudio_format_t AAudioStream_getFormat(AAudioStream *s) { (void)s; return AAUDIO_FORMAT_PCM_I16; }
int32_t AAudioStream_getFramesPerBurst(AAudioStream *s) { (void)s; return 192; }
aaudio_performance_mode_t AAudioStream_getPerformanceMode(AAudioStream *s) { (void)s; return AAUDIO_PERFORMANCE_MODE_LOW_LATENCY; }
int32_t AAudioStream_getSampleRate(AAudioStream *s) { (void)s; return 48000; }
int32_t AAudioStream_getXRunCount(AAudioStream *s) { (void)s; return 0; }
