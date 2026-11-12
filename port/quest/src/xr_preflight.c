/*
 * xr_preflight.c - the shell's part of the device preflight (bvr_diag.h has the process-level part): Vulkan before
 * and after OpenXR, the OpenXR runtime, AAudio, ActivityManager's memory figures. One "PREFLIGHT <name>: OK|WARN|
 * FAIL|SKIP ..." line each, tag BlackVR; port/notes/device-preflight.md explains them.
 *
 * Compiled into libblackvr.so and into tests/game_render_x86 (fake OpenXR runtime, SwiftShader; no AAudio or JNI
 * there: those report SKIP off Android).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "xr_shell.h"
#include "bvr_diag.h"
#include "xr_result_names.h"
#if defined(__ANDROID__)
#include <aaudio/AAudio.h>
#endif

/* ---- process checks (bvr_diag.c), with what only the shell knows ------------------------------------------------ */

#if defined(__ANDROID__)
/* ActivityManager.getMemoryClass / getLargeMemoryClass / getMemoryInfo, through JNI on this (native) thread */
static int jni_memory(XrShell *s, long *cls, long *large, long long *avail, long long *threshold, int *low)
{
    JavaVM *vm = s->app && s->app->activity ? s->app->activity->vm : NULL;
    jobject act = s->app && s->app->activity ? s->app->activity->clazz : NULL;
    JNIEnv *env = NULL;
    int ok = 0, attached = 0;
    if (!vm || !act) return 0;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK || !env) return 0;
        attached = 1;
    }
    {
        jclass ctx = (*env)->GetObjectClass(env, act);
        jmethodID gss = ctx ? (*env)->GetMethodID(env, ctx, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;") : NULL;
        jstring name = (*env)->NewStringUTF(env, "activity");
        jobject am = gss && name ? (*env)->CallObjectMethod(env, act, gss, name) : NULL;
        jclass amc = am ? (*env)->GetObjectClass(env, am) : NULL;
        jclass mic = (*env)->FindClass(env, "android/app/ActivityManager$MemoryInfo");
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        if (amc && mic) {
            jmethodID gmc = (*env)->GetMethodID(env, amc, "getMemoryClass", "()I");
            jmethodID glc = (*env)->GetMethodID(env, amc, "getLargeMemoryClass", "()I");
            jmethodID gmi = (*env)->GetMethodID(env, amc, "getMemoryInfo", "(Landroid/app/ActivityManager$MemoryInfo;)V");
            jmethodID ctor = (*env)->GetMethodID(env, mic, "<init>", "()V");
            jfieldID fa = (*env)->GetFieldID(env, mic, "availMem", "J");
            jfieldID ft = (*env)->GetFieldID(env, mic, "threshold", "J");
            jfieldID fl = (*env)->GetFieldID(env, mic, "lowMemory", "Z");
            if (gmc && glc && gmi && ctor && fa && ft && fl) {
                jobject mi = (*env)->NewObject(env, mic, ctor);
                *cls = (*env)->CallIntMethod(env, am, gmc);
                *large = (*env)->CallIntMethod(env, am, glc);
                if (mi) {
                    (*env)->CallVoidMethod(env, am, gmi, mi);
                    *avail = (*env)->GetLongField(env, mi, fa);
                    *threshold = (*env)->GetLongField(env, mi, ft);
                    *low = (*env)->GetBooleanField(env, mi, fl) ? 1 : 0;
                    ok = 1;
                }
            }
        }
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); ok = 0; }
    }
    if (attached) (*vm)->DetachCurrentThread(vm);
    return ok;
}
#endif

void xrp_process(XrShell *s)
{
    long cls = -1, large = -1;
    long long avail = 0, threshold = 0;
    int low = 0;
#if defined(__ANDROID__)
    if (!jni_memory(s, &cls, &large, &avail, &threshold, &low)) cls = -1;
#else
    (void)s;
#endif
    bvr_pf_memory(cls, large, avail, threshold, low);
    bvr_pf_addrspace();
    bvr_pf_sigsegv("sigsegv");
    bvr_pf_jit();
    bvr_pf_syscalls();
    {
        const size_t alt = bvr_thread_altstack(256 * 1024);
        bvr_pf_report("altstack", alt >= 256 * 1024 ? BVR_PF_OK : BVR_PF_WARN, 0, "this thread's alternate signal stack "
                      "%zu KB (the game thread and every runtime thread get 256 KB at start; bionic's default is 32 KB)",
                      alt / 1024);
    }
}

/* ---- AAudio ------------------------------------------------------------------------------------------------------ */
#if defined(__ANDROID__)
static volatile int s_aa_callbacks;
static aaudio_data_callback_result_t pf_aa_data(AAudioStream *st, void *user, void *audio, int32_t frames)
{
    (void)st; (void)user;
    memset(audio, 0, (size_t)frames * 2 * sizeof(int16_t));
    s_aa_callbacks++;
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
#endif

void xrp_audio(void)
{
#if defined(__ANDROID__)
    AAudioStreamBuilder *b = NULL;
    AAudioStream *st = NULL;
    aaudio_result_t r = AAudio_createStreamBuilder(&b);
    int i, rate, ch, fmt, burst, cap, pm, sm;
    if (r != AAUDIO_OK) {
        bvr_pf_report("audio", BVR_PF_FAIL, 0, "AAudio_createStreamBuilder: %s -- no sound", AAudio_convertResultToText(r));
        return;
    }
    /* what the runtime's backend asks for (third_party/xboxrecomp/src/apu/apu_aaudio.c) */
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, 48000);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_SONIFICATION);
    AAudioStreamBuilder_setDataCallback(b, pf_aa_data, NULL);
    r = AAudioStreamBuilder_openStream(b, &st);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK || !st) {
        bvr_pf_report("audio", BVR_PF_FAIL, 0, "AAudioStreamBuilder_openStream (48 kHz stereo i16, low latency, shared): %s "
                      "-- no sound; the game still runs", AAudio_convertResultToText(r));
        return;
    }
    rate = AAudioStream_getSampleRate(st);
    ch = AAudioStream_getChannelCount(st);
    fmt = (int)AAudioStream_getFormat(st);
    burst = AAudioStream_getFramesPerBurst(st);
    cap = AAudioStream_getBufferCapacityInFrames(st);
    pm = (int)AAudioStream_getPerformanceMode(st);
    sm = (int)AAudioStream_getSharingMode(st);
    s_aa_callbacks = 0;
    r = AAudioStream_requestStart(st);
    for (i = 0; i < 40 && r == AAUDIO_OK && s_aa_callbacks < 2; i++) usleep(10000);
    AAudioStream_requestStop(st);
    AAudioStream_close(st);
    if (rate != 48000 || ch != 2 || fmt != AAUDIO_FORMAT_PCM_I16)
        bvr_pf_report("audio", BVR_PF_FAIL, 0, "stream opened as %d Hz, %d ch, format %d: the runtime's AAudio backend "
                      "refuses anything but 48000/2/i16 -- no sound", rate, ch, fmt);
    else if (r != AAUDIO_OK || s_aa_callbacks < 1)
        bvr_pf_report("audio", BVR_PF_WARN, 0, "48 kHz stereo opened (burst %d, capacity %d) but %s%s: no data callback "
                      "in 400 ms", burst, cap, r != AAUDIO_OK ? "requestStart: " : "",
                      r != AAUDIO_OK ? AAudio_convertResultToText(r) : "started");
    else
        bvr_pf_report("audio", BVR_PF_OK, 0, "48000 Hz stereo i16, burst %d frames, capacity %d, performance mode %s, %s, "
                      "%d callbacks in the test", burst, cap, pm == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY ? "LOW_LATENCY" : "other",
                      sm == AAUDIO_SHARING_MODE_SHARED ? "shared" : "exclusive", s_aa_callbacks);
#else
    bvr_pf_report("audio", BVR_PF_SKIP, 0, "not Android (no AAudio)");
#endif
}

/* ---- Vulkan, standalone (before OpenXR: separates a GPU problem from a runtime one) ----------------------------- */
static int has_vk_ext(const VkExtensionProperties *e, uint32_t n, const char *name)
{
    uint32_t i;
    for (i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

static void driver_version(const VkPhysicalDeviceProperties *p, char *out, size_t cap)
{
    const uint32_t v = p->driverVersion;
    if (p->vendorID == 0x10DE)       /* NVIDIA */
        snprintf(out, cap, "%u.%u (0x%08X)", v >> 22, (v >> 14) & 0xFF, v);
    else                             /* Qualcomm (512.x.y) and most others: VK_MAKE_VERSION packing */
        snprintf(out, cap, "%u.%u.%u (0x%08X)", v >> 22, (v >> 12) & 0x3FF, v & 0xFFF, v);
}

#define KVK_SURF_NEED (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | \
                       VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)

void xrp_vulkan_probe(void)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pd[4];
    uint32_t npd = 4, iver = VK_API_VERSION_1_0, ne = 0, nq = 16, i;
    VkResult r;
    char missing[600] = "", optional[600] = "", drv[64];
    app.pApplicationName = "BLACK VR preflight";
    app.apiVersion = VK_API_VERSION_1_1;
    ci.pApplicationInfo = &app;
    vkEnumerateInstanceVersion(&iver);
    r = vkCreateInstance(&ci, NULL, &inst);
    if (r != VK_SUCCESS) {
        bvr_pf_report("vulkan", BVR_PF_FAIL, 0, "vkCreateInstance (API 1.1, no layers): %d %s; loader %u.%u", (int)r,
                      vk_result_name((int)r), VK_API_VERSION_MAJOR(iver), VK_API_VERSION_MINOR(iver));
        return;
    }
    r = vkEnumeratePhysicalDevices(inst, &npd, pd);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || npd == 0) {
        bvr_pf_report("vulkan", BVR_PF_FAIL, 0, "no physical device (%d %s)", (int)r, vk_result_name((int)r));
        vkDestroyInstance(inst, NULL);
        return;
    }
    {
        VkPhysicalDeviceProperties p;
        VkPhysicalDeviceMultiviewProperties mvp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        VkPhysicalDeviceMultiviewFeatures mv = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES };
        VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        VkQueueFamilyProperties qf[16];
        VkExtensionProperties *ext;
        const VkPhysicalDeviceLimits *L;
        int gfx = -1, depth = 0;
        size_t ml, ol;
        vkGetPhysicalDeviceProperties(pd[0], &p);
        p2.pNext = &mvp;
        if (VK_API_VERSION_MINOR(p.apiVersion) >= 1 || VK_API_VERSION_MAJOR(p.apiVersion) > 1) {
            vkGetPhysicalDeviceProperties2(pd[0], &p2);
            f2.pNext = &mv;
            vkGetPhysicalDeviceFeatures2(pd[0], &f2);
        } else {
            vkGetPhysicalDeviceFeatures(pd[0], &f2.features);
        }
        L = &p.limits;
        vkGetPhysicalDeviceQueueFamilyProperties(pd[0], &nq, qf);
        for (i = 0; i < nq && gfx < 0; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) gfx = (int)i;
        vkEnumerateDeviceExtensionProperties(pd[0], NULL, &ne, NULL);
        ext = (VkExtensionProperties *)calloc(ne ? ne : 1, sizeof(*ext));
        vkEnumerateDeviceExtensionProperties(pd[0], NULL, &ne, ext);
        driver_version(&p, drv, sizeof(drv));
#define MISS(...) do { ml = strlen(missing); if (ml && ml + 2 < sizeof(missing)) { strcat(missing, ", "); ml += 2; } \
                          snprintf(missing + ml, sizeof(missing) - ml, __VA_ARGS__); } while (0)
#define OPT(...)  do { ol = strlen(optional); if (ol && ol + 2 < sizeof(optional)) { strcat(optional, ", "); ol += 2; } \
                          snprintf(optional + ol, sizeof(optional) - ol, __VA_ARGS__); } while (0)
        /* required: the renderer refuses (test pattern only) without these */
        if (p.apiVersion < VK_API_VERSION_1_1) MISS("Vulkan 1.1 (device has %u.%u)", VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion));
        if (!mv.multiview) MISS("multiview");
        if (mvp.maxMultiviewViewCount && mvp.maxMultiviewViewCount < 2) MISS("2 multiview views");
        if (gfx < 0) MISS("a graphics queue");
        if (!f2.features.robustBufferAccess) MISS("robustBufferAccess");
        {
            static const struct { VkFormat f; const char *n; } col[] = {
                { VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM" }, { VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM" } };
            static const VkFormat dep[2] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT };
            const VkFormatFeatureFlags dneed = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            VkFormatProperties fp;
            for (i = 0; i < 2; i++) {
                vkGetPhysicalDeviceFormatProperties(pd[0], col[i].f, &fp);
                if ((fp.optimalTilingFeatures & KVK_SURF_NEED) != KVK_SURF_NEED) MISS("%s colour attachment+blend+sampled", col[i].n);
            }
            for (i = 0; i < 2 && !depth; i++) {
                vkGetPhysicalDeviceFormatProperties(pd[0], dep[i], &fp);
                if ((fp.optimalTilingFeatures & dneed) == dneed) depth = i == 0 ? 24 : 32;
            }
            if (!depth) MISS("a D24S8 or D32S8 depth-stencil attachment");
        }
        if (L->maxPushConstantsSize < 64) MISS("64 B push constants (%u)", L->maxPushConstantsSize);
        if (L->maxBoundDescriptorSets < 2) MISS("2 descriptor sets (%u)", L->maxBoundDescriptorSets);
        if (L->maxPerStageDescriptorUniformBuffers < 5 || L->maxDescriptorSetUniformBuffersDynamic < 5) MISS("5 dynamic UBOs");
        if (L->maxPerStageDescriptorStorageBuffers < 1 || L->maxDescriptorSetStorageBuffersDynamic < 1) MISS("1 dynamic SSBO");
        if (L->maxPerStageDescriptorSamplers < 4 || L->maxPerStageDescriptorSampledImages < 4) MISS("4 samplers per stage");
        if (L->maxUniformBufferRange < 3072) MISS("UBO range 3072 (%u)", L->maxUniformBufferRange);
        if (L->maxStorageBufferRange < (4u << 20)) MISS("SSBO range 4 MiB (%u)", L->maxStorageBufferRange);
        /* optional: the backend degrades without them */
        if (!f2.features.depthClamp) OPT("no depthClamp (clamped draws clip instead)");
        if (!f2.features.samplerAnisotropy) OPT("no samplerAnisotropy");
        if (!f2.features.textureCompressionBC) OPT("no BC textures (DXT decoded on the CPU, expected on Adreno)");
        if (!has_vk_ext(ext, ne, "VK_EXT_fragment_density_map")) OPT("no VK_EXT_fragment_density_map");
        if (!has_vk_ext(ext, ne, "VK_KHR_external_memory")) OPT("no VK_KHR_external_memory");
#undef MISS
#undef OPT
        free(ext);
        if (missing[0])
            bvr_pf_report("vulkan", BVR_PF_FAIL, 0, "%s, driver %s, API %u.%u: MISSING %s -- the Kelvin renderer cannot run "
                          "(test pattern only)%s%s", p.deviceName, drv, VK_API_VERSION_MAJOR(p.apiVersion),
                          VK_API_VERSION_MINOR(p.apiVersion), missing, optional[0] ? "; optional: " : "", optional);
        else
            bvr_pf_report("vulkan", BVR_PF_OK, 0, "%s (vendor 0x%04X device 0x%04X), driver %s, API %u.%u.%u, loader %u.%u; "
                          "multiview (%u views), D%dS8, graphics queue family %d, %u device extensions, %u memory "
                          "allocations max; %s%s", p.deviceName, p.vendorID, p.deviceID, drv,
                          VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
                          VK_API_VERSION_PATCH(p.apiVersion), VK_API_VERSION_MAJOR(iver), VK_API_VERSION_MINOR(iver),
                          mvp.maxMultiviewViewCount, depth, gfx, ne, L->maxMemoryAllocationCount,
                          optional[0] ? "degraded, not required: " : "every optional feature present", optional);
    }
    vkDestroyInstance(inst, NULL);
}

/* ---- OpenXR --------------------------------------------------------------------------------------------------- */
static const char *xr_hint(int code)
{
    switch (code) {
    case XR_ERROR_RUNTIME_UNAVAILABLE: return "no OpenXR runtime found by the loader: Quest OS too old for the Khronos "
                                              "loader's runtime broker, or the manifest's <queries> are missing";
    case XR_ERROR_RUNTIME_FAILURE: return "the runtime failed internally (reboot the headset; check for an OS update)";
    case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "the headset is not ready (put it on: the proximity sensor; Link/AirLink "
                                                  "active?)";
    case XR_ERROR_EXTENSION_NOT_PRESENT: return "a required extension is missing (see the extension list)";
    case XR_ERROR_INITIALIZATION_FAILED: return "the loader could not start (JavaVM/Activity handed over wrongly?)";
    case XR_ERROR_API_VERSION_UNSUPPORTED: return "the runtime refuses OpenXR 1.0";
    case XR_ERROR_GRAPHICS_DEVICE_INVALID: return "the Vulkan device does not match what the runtime picked";
    case XR_ERROR_VALIDATION_FAILURE: return "invalid parameters (a shell bug: send this log)";
    default: return "";
    }
}

void xrp_openxr(XrShell *s, int ok)
{
    if (!ok) {
        const int code = xrs_failure_code();
        bvr_pf_report("openxr", BVR_PF_FAIL, 1, "instance/system creation failed at %s%s%s -- the app exits (nothing can be "
                      "shown without OpenXR)", xrs_failure_text()[0] ? xrs_failure_text() : "?", xr_hint(code)[0] ? ": " : "",
                      xr_hint(code));
        return;
    }
    {
        XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
        XrSystemProperties sp = { XR_TYPE_SYSTEM_PROPERTIES };
        XrExtensionProperties *ext;
        uint32_t n = 0, i;
        char have[700] = "", miss[400] = "";
        static const struct { const char *n; int req; const char *what; } want[] = {
            { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, 1, "Vulkan" },
            { XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, 0, "Android instance" },
            { XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME, 0, "pose prediction for the game thread" },
            { XR_KHR_VULKAN_SWAPCHAIN_FORMAT_LIST_EXTENSION_NAME, 0, "sRGB+UNORM views" },
            { XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME, 0, "refresh_hz" },
            { XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME, 0, "CPU/GPU levels" },
            { XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME, 0, "thread hints" },
            { XR_FB_FOVEATION_VULKAN_EXTENSION_NAME, 0, "foveation" },
            { XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME, 0, "Touch Plus bindings" },
        };
        int req_missing = 0;
        if (XR_FAILED(xrGetInstanceProperties(s->instance, &ip))) snprintf(ip.runtimeName, sizeof(ip.runtimeName), "?");
        xrGetSystemProperties(s->instance, s->system, &sp);
        xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL);
        ext = (XrExtensionProperties *)calloc(n ? n : 1, sizeof(*ext));
        for (i = 0; i < n; i++) ext[i].type = XR_TYPE_EXTENSION_PROPERTIES;
        if (XR_FAILED(xrEnumerateInstanceExtensionProperties(NULL, n, &n, ext))) n = 0;
        for (i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
            uint32_t k;
            int present = 0;
            for (k = 0; k < n; k++) if (!strcmp(ext[k].extensionName, want[i].n)) present = 1;
            if (present) {
                size_t l = strlen(have);
                snprintf(have + l, sizeof(have) - l, "%s%s", l ? " " : "", want[i].n);
            } else {
                size_t l = strlen(miss);
                snprintf(miss + l, sizeof(miss) - l, "%s%s (%s)", l ? ", " : "", want[i].n, want[i].what);
                if (want[i].req) req_missing = 1;
            }
        }
        {   /* information only (the foveation work asked whether the runtime offers them): not used, so not in "used" */
            int rec = 0, vsc = 0;
            uint32_t k;
            for (k = 0; k < n; k++) {
                if (!strcmp(ext[k].extensionName, "XR_META_recommended_layer_resolution")) rec = 1;
                if (!strcmp(ext[k].extensionName, "XR_META_vulkan_swapchain_create_info")) vsc = 1;
            }
            XLOGI("PREFLIGHT info: XR_META_recommended_layer_resolution %s, XR_META_vulkan_swapchain_create_info %s",
                  rec ? "offered" : "not offered", vsc ? "offered" : "not offered");
        }
        free(ext);
        bvr_pf_report("openxr", req_missing ? BVR_PF_FAIL : miss[0] ? BVR_PF_WARN : BVR_PF_OK, req_missing,
                      "runtime \"%s\" %u.%u.%u, system \"%s\" (vendor 0x%X), eye %ux%u recommended (max %ux%u), %u "
                      "extensions offered; used: %s%s%s", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
                      XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion), sp.systemName,
                      sp.vendorId, s->config_views[0].recommendedImageRectWidth,
                      s->config_views[0].recommendedImageRectHeight, s->config_views[0].maxImageRectWidth,
                      s->config_views[0].maxImageRectHeight, n, have, miss[0] ? "; missing: " : "", miss);
    }
}

void xrp_vulkan_device(XrShell *s, int ok)
{
    if (!ok) {
        bvr_pf_report("vulkan-device", BVR_PF_FAIL, 1, "the Vulkan instance/device through XR_KHR_vulkan_enable2 failed at "
                      "%s -- the app exits", xrs_failure_text()[0] ? xrs_failure_text() : "?");
        return;
    }
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(s->vk_phys, &p);
        bvr_pf_report("vulkan-device", s->multiview_supported ? BVR_PF_OK : BVR_PF_FAIL, 0,
                      "%s through XR_KHR_vulkan_enable2, Vulkan %u.%u, queue family %u; enabled: multiview %u, "
                      "robustBufferAccess %u, depthClamp %u, samplerAnisotropy %u, BC %u%s", p.deviceName,
                      VK_API_VERSION_MAJOR(s->vk_api), VK_API_VERSION_MINOR(s->vk_api), s->vk_queue_family,
                      s->vk_features.multiview, s->vk_features.robust_buffer_access, s->vk_features.depth_clamp,
                      s->vk_features.sampler_anisotropy, s->vk_features.texture_compression_bc,
                      s->multiview_supported ? "" : " -- no multiview: the renderer refuses, test pattern only");
        /* the fragment density map (the backend's own foveation, kvk_fdm.h): what the device offers and what was enabled */
        {
            const int usable = s->fdm_info.have_fdm && s->fdm_info.have_nonsubsampled;
            bvr_pf_report("fdm", s->vk_features.fdm ? BVR_PF_OK : BVR_PF_WARN, 0,
                          "density map %s: VK_EXT_fragment_density_map %s, _map2 %s, VK_QCOM_fragment_density_map_offset %s, "
                          "VK_KHR_fragment_shading_rate %s (%u device extensions); features: fragmentDensityMap %d, "
                          "dynamic %d, nonSubsampledImages %d, deferred %d; texel size min %ux%u max %ux%u, invocations %u; "
                          "XR_FB_foveation_vulkan %s; %s",
                          s->vk_features.fdm ? "ENABLED" : "off",
                          s->fdm_info.ext_fdm ? "yes" : "no", s->fdm_info.ext_fdm2 ? "yes" : "no",
                          s->fdm_info.ext_qcom_offset ? "yes" : "no", s->fdm_info.ext_fsr ? "yes" : "no", s->fdm_info.n_ext,
                          s->fdm_info.have_fdm, s->fdm_info.have_dynamic, s->fdm_info.have_nonsubsampled,
                          s->fdm_info.have_deferred, s->fdm_info.min_w, s->fdm_info.min_h, s->fdm_info.max_w,
                          s->fdm_info.max_h, s->fdm_info.invocations, s->has_foveation ? "yes" : "no",
                          s->vk_features.fdm ? "the Kelvin main passes use their own map (RECOMP_KVK_FDM=0 turns it off)"
                          : s->fdm_info.env_off ? "RECOMP_KVK_FDM=0: features left off"
                          : !s->fdm_info.ext_fdm ? "the GPU has no VK_EXT_fragment_density_map"
                          : !usable ? "the GPU lacks the map or non-subsampled images" : "no usable texel size");
        }
    }
}

void xrp_session(XrShell *s, int ok)
{
    if (!ok) {
        bvr_pf_report("openxr-session", BVR_PF_FAIL, 1, "session/swapchain/action setup failed at %s -- the app exits",
                      xrs_failure_text()[0] ? xrs_failure_text() : "?");
        return;
    }
    bvr_pf_report("openxr-session", BVR_PF_OK, 0, "world space %s, swapchain format %d%s, eye %ux%u (%s, resolution_scale "
                  "%.2f), foveation %s, renderer %s", s->world_is_stage ? "STAGE" : "LOCAL (no STAGE)",
                  (int)s->color_format, s->has_format_list ? " with sRGB+UNORM format list" : "", s->eye[0].width,
                  s->eye[0].height, s->multiview ? "2-layer multiview" : "two swapchains", s->cfg.resolution_scale,
                  !s->has_foveation ? "not used (see the foveation line)" : s->foveation_applied ? "set" : "off",
                  s->external_renderer ? "attached (Kelvin Vulkan backend)"
                  : s->want_renderer ? "REFUSED (see the vr-glue line above): test pattern"
                                     : "not wanted (no game): test pattern");
}
