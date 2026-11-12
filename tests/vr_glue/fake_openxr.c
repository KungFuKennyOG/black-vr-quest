/* fake_openxr.c - see fake_openxr.h. Single instance, single session; handles are small integers. */
#include "fake_openxr.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

FxVulkan fx_vk;
volatile int64_t fx_now = 1000000000LL;
int64_t fx_period = 13888889;
void (*fx_trajectory)(int64_t t, XrPosef *head, XrPosef eye[2], XrFovf fov[2]);
FxEndFrame fx_last_end;
volatile uint64_t fx_end_count;
pthread_mutex_t *fx_queue_lock;
int fx_check_locks;
int fx_lock_violations;
void (*fx_on_release)(XrSwapchain sc, uint32_t index);
void (*fx_on_end_frame)(const FxEndFrame *f);
int (*fx_input_bool)(const char *action, XrPath subaction, XrBool32 *state);
int (*fx_input_vec2)(const char *action, XrPath subaction, XrVector2f *state);
int (*fx_input_float)(const char *action, XrPath subaction, float *state);
int (*fx_hand_pose)(int hand, int grip, XrTime t, XrPosef *pose);
int (*fx_hand_velocity)(int hand, int grip, XrTime t, XrVector3f *linear);

static FxSwapchain s_sc[FX_MAX_SWAPCHAINS];
static XrEventDataSessionStateChanged s_events[16];
static int s_nevents;
static XrTime s_last_wait;

FxSwapchain *fx_swapchain(XrSwapchain h)
{
    const uintptr_t i = (uintptr_t)h;
    return i >= 1 && i <= FX_MAX_SWAPCHAINS && s_sc[i - 1].used ? &s_sc[i - 1] : NULL;
}

void fx_push_session_state(XrSessionState st)
{
    XrEventDataSessionStateChanged *e = &s_events[s_nevents++];
    memset(e, 0, sizeof(*e));
    e->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    e->session = (XrSession)(uintptr_t)1;
    e->state = st;
}

/* ---- queue external synchronisation check ---- */

static void *trylock_thread(void *m)
{
    const int r = pthread_mutex_trylock((pthread_mutex_t *)m);
    if (r == 0) pthread_mutex_unlock((pthread_mutex_t *)m);
    return (void *)(intptr_t)r;
}

static void check_queue_lock(const char *fn)
{
    pthread_t th;
    void *r = NULL;
    if (!fx_check_locks || !fx_queue_lock) return;
    pthread_create(&th, NULL, trylock_thread, fx_queue_lock);
    pthread_join(th, &r);
    if ((intptr_t)r != EBUSY) {
        fx_lock_violations++;
        printf("FAIL: %s called without the VkQueue lock\n", fn);
    }
}

/* ---- Vulkan through XR_KHR_vulkan_enable2 ---- */

static XrResult XRAPI_CALL fx_get_reqs2(XrInstance i, XrSystemId s, XrGraphicsRequirementsVulkanKHR *r)
{
    (void)i; (void)s;
    r->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
    r->maxApiVersionSupported = XR_MAKE_VERSION(1, 3, 0);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_create_vk_instance(XrInstance i, const XrVulkanInstanceCreateInfoKHR *ci, VkInstance *out,
                                                 VkResult *vr)
{
    (void)i;
    fx_vk.instance_api = ci->vulkanCreateInfo->pApplicationInfo ? ci->vulkanCreateInfo->pApplicationInfo->apiVersion : 0;
    *vr = vkCreateInstance(ci->vulkanCreateInfo, NULL, out);
    fx_vk.instance = *out;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_get_vk_device2(XrInstance i, const XrVulkanGraphicsDeviceGetInfoKHR *gi, VkPhysicalDevice *out)
{
    uint32_t n = 1;
    (void)i;
    if (vkEnumeratePhysicalDevices(gi->vulkanInstance, &n, out) < 0 || !n) return XR_ERROR_RUNTIME_FAILURE;
    fx_vk.phys = *out;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_create_vk_device(XrInstance i, const XrVulkanDeviceCreateInfoKHR *ci, VkDevice *out, VkResult *vr)
{
    const VkDeviceCreateInfo *d = ci->vulkanCreateInfo;
    const VkBaseInStructure *p;
    VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    (void)i;
    for (p = (const VkBaseInStructure *)d->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES)
            fx_vk.dev_multiview |= ((const VkPhysicalDeviceMultiviewFeatures *)p)->multiview != 0;
    {
        uint32_t k;
        for (k = 0; k < d->enabledExtensionCount; k++)
            if (!strcmp(d->ppEnabledExtensionNames[k], "VK_EXT_fragment_density_map")) fx_vk.dev_fdm = 1;
    }
    if (d->pEnabledFeatures) {
        fx_vk.dev_robust = d->pEnabledFeatures->robustBufferAccess != 0;
        fx_vk.dev_depth_clamp = d->pEnabledFeatures->depthClamp != 0;
        fx_vk.dev_bc = d->pEnabledFeatures->textureCompressionBC != 0;
        fx_vk.dev_aniso = d->pEnabledFeatures->samplerAnisotropy != 0;
    }
    *vr = vkCreateDevice(ci->vulkanPhysicalDevice, d, NULL, out);
    if (*vr != VK_SUCCESS) return XR_SUCCESS;
    fx_vk.device = *out;
    fx_vk.queue_family = d->pQueueCreateInfos[0].queueFamilyIndex;
    vkGetDeviceQueue(*out, fx_vk.queue_family, 0, &fx_vk.queue);
    pi.queueFamilyIndex = fx_vk.queue_family;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(*out, &pi, NULL, &fx_vk.pool);
    return XR_SUCCESS;
}

XrResult fx_convert_time(XrInstance instance, const struct timespec *ts, XrTime *t)
{
    (void)instance; (void)ts;
    *t = fx_now;
    return XR_SUCCESS;
}

/* ---- XR_FB_foveation / _configuration / XR_FB_swapchain_update_state ---- */
int fx_no_foveation, fx_foveation_profiles, fx_foveation_profiles_destroyed, fx_foveation_updates;
typedef struct FxProfile { int used, level; } FxProfile;
static FxProfile s_prof[16];

static XrResult XRAPI_CALL fx_create_foveation_profile(XrSession s, const XrFoveationProfileCreateInfoFB *ci, XrFoveationProfileFB *out)
{
    const XrBaseInStructure *p;
    int k, level = -1;
    (void)s;
    if (!ci || ci->type != XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB || !out) return XR_ERROR_VALIDATION_FAILURE;
    for (p = (const XrBaseInStructure *)ci->next; p; p = p->next)
        if (p->type == XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB) {
            const XrFoveationLevelProfileCreateInfoFB *l = (const XrFoveationLevelProfileCreateInfoFB *)p;
            if (l->level < XR_FOVEATION_LEVEL_NONE_FB || l->level > XR_FOVEATION_LEVEL_HIGH_FB) return XR_ERROR_VALIDATION_FAILURE;
            level = (int)l->level;
        }
    for (k = 0; k < 16; k++)
        if (!s_prof[k].used) {
            s_prof[k].used = 1; s_prof[k].level = level < 0 ? 0 : level;
            *out = (XrFoveationProfileFB)(uintptr_t)(k + 1);
            fx_foveation_profiles++;
            return XR_SUCCESS;
        }
    return XR_ERROR_LIMIT_REACHED;
}

static XrResult XRAPI_CALL fx_destroy_foveation_profile(XrFoveationProfileFB h)
{
    const uintptr_t k = (uintptr_t)h;
    if (!k || k > 16 || !s_prof[k - 1].used) return XR_ERROR_HANDLE_INVALID;
    s_prof[k - 1].used = 0;
    fx_foveation_profiles_destroyed++;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_update_swapchain(XrSwapchain h, const XrSwapchainStateBaseHeaderFB *st)
{
    FxSwapchain *sc = fx_swapchain(h);
    if (!sc || !st) return XR_ERROR_HANDLE_INVALID;
    if (st->type == XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB) {
        const XrSwapchainStateFoveationFB *f = (const XrSwapchainStateFoveationFB *)st;
        const uintptr_t k = (uintptr_t)f->profile;
        /* XR_FB_foveation: only a swapchain created with foveation flags takes a profile */
        if (!sc->foveation_flags) return XR_ERROR_VALIDATION_FAILURE;
        if (!k || k > 16 || !s_prof[k - 1].used) return XR_ERROR_HANDLE_INVALID;
        sc->foveation_level = s_prof[k - 1].level;
        fx_foveation_updates++;
        return XR_SUCCESS;
    }
    return XR_ERROR_VALIDATION_FAILURE;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char *name, PFN_xrVoidFunction *fn)
{
    (void)instance;
    *fn = NULL;
    if (!strcmp(name, "xrGetVulkanGraphicsRequirements2KHR")) *fn = (PFN_xrVoidFunction)fx_get_reqs2;
    else if (!strcmp(name, "xrCreateVulkanInstanceKHR")) *fn = (PFN_xrVoidFunction)fx_create_vk_instance;
    else if (!strcmp(name, "xrGetVulkanGraphicsDevice2KHR")) *fn = (PFN_xrVoidFunction)fx_get_vk_device2;
    else if (!strcmp(name, "xrCreateVulkanDeviceKHR")) *fn = (PFN_xrVoidFunction)fx_create_vk_device;
    else if (!strcmp(name, "xrConvertTimespecTimeToTimeKHR")) *fn = (PFN_xrVoidFunction)fx_convert_time;
    else if (!fx_no_foveation && !strcmp(name, "xrCreateFoveationProfileFB")) *fn = (PFN_xrVoidFunction)fx_create_foveation_profile;
    else if (!fx_no_foveation && !strcmp(name, "xrDestroyFoveationProfileFB")) *fn = (PFN_xrVoidFunction)fx_destroy_foveation_profile;
    else if (!fx_no_foveation && !strcmp(name, "xrUpdateSwapchainFB")) *fn = (PFN_xrVoidFunction)fx_update_swapchain;
    return *fn ? XR_SUCCESS : XR_ERROR_FUNCTION_UNSUPPORTED;
}

/* ---- instance, system, session ---- */

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateInstanceExtensionProperties(const char *layer, uint32_t cap, uint32_t *n, XrExtensionProperties *p)
{
    static const char *const ext[] = {
        XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME,
        XR_KHR_VULKAN_SWAPCHAIN_FORMAT_LIST_EXTENSION_NAME,
        XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME, XR_FB_FOVEATION_EXTENSION_NAME,
        XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME, XR_FB_FOVEATION_VULKAN_EXTENSION_NAME,
    };
    const uint32_t total = fx_no_foveation ? 3u : (uint32_t)(sizeof(ext) / sizeof(ext[0]));
    uint32_t i;
    (void)layer;
    *n = total;
    if (!cap || !p) return XR_SUCCESS;
    if (cap < total) return XR_ERROR_SIZE_INSUFFICIENT;
    for (i = 0; i < total; i++) {
        snprintf(p[i].extensionName, XR_MAX_EXTENSION_NAME_SIZE, "%s", ext[i]);
        p[i].extensionVersion = 1;
    }
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateInstance(const XrInstanceCreateInfo *ci, XrInstance *out) { (void)ci; *out = (XrInstance)(uintptr_t)1; return XR_SUCCESS; }
XRAPI_ATTR XrResult XRAPI_CALL xrDestroyInstance(XrInstance i) { (void)i; return XR_SUCCESS; }
XRAPI_ATTR XrResult XRAPI_CALL xrGetSystem(XrInstance i, const XrSystemGetInfo *gi, XrSystemId *s) { (void)i; (void)gi; *s = 1; return XR_SUCCESS; }
XRAPI_ATTR XrResult XRAPI_CALL xrGetSystemProperties(XrInstance i, XrSystemId s, XrSystemProperties *p) { (void)i; (void)s; (void)p; return XR_SUCCESS; }
XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProperties(XrInstance i, XrInstanceProperties *p)
{
    (void)i;
    snprintf(p->runtimeName, sizeof(p->runtimeName), "tests/vr_glue fake_openxr");
    p->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateViewConfigurationViews(XrInstance i, XrSystemId s, XrViewConfigurationType t, uint32_t cap,
                                                                uint32_t *n, XrViewConfigurationView *v)
{
    (void)i; (void)s; (void)t; (void)cap; (void)v;
    *n = 2;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateSession(XrInstance i, const XrSessionCreateInfo *ci, XrSession *out)
{
    const XrGraphicsBindingVulkan2KHR *gb = (const XrGraphicsBindingVulkan2KHR *)ci->next;
    (void)i;
    if (!gb || gb->type != XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR || gb->device != fx_vk.device) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    *out = (XrSession)(uintptr_t)1;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroySession(XrSession s) { (void)s; return XR_SUCCESS; }
XRAPI_ATTR XrResult XRAPI_CALL xrBeginSession(XrSession s, const XrSessionBeginInfo *bi) { (void)s; (void)bi; return XR_SUCCESS; }
XRAPI_ATTR XrResult XRAPI_CALL xrEndSession(XrSession s) { (void)s; return XR_SUCCESS; }
XRAPI_ATTR XrResult XRAPI_CALL xrPollEvent(XrInstance i, XrEventDataBuffer *ev)
{
    (void)i;
    if (!s_nevents) return XR_EVENT_UNAVAILABLE;
    memcpy(ev, &s_events[0], sizeof(s_events[0]));
    memmove(&s_events[0], &s_events[1], sizeof(s_events[0]) * (size_t)(--s_nevents));
    return XR_SUCCESS;
}

/* ---- spaces, views ---- */

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateReferenceSpaces(XrSession s, uint32_t cap, uint32_t *n, XrReferenceSpaceType *t)
{
    (void)s;
    *n = 3;
    if (cap > 0 && cap < 3) return XR_ERROR_SIZE_INSUFFICIENT;
    if (cap >= 3) { t[0] = XR_REFERENCE_SPACE_TYPE_VIEW; t[1] = XR_REFERENCE_SPACE_TYPE_LOCAL; t[2] = XR_REFERENCE_SPACE_TYPE_STAGE; }
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateReferenceSpace(XrSession s, const XrReferenceSpaceCreateInfo *ci, XrSpace *out)
{
    (void)s;
    *out = ci->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE ? FX_SPACE_STAGE
         : ci->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW ? FX_SPACE_VIEW : FX_SPACE_LOCAL;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroySpace(XrSpace s) { (void)s; return XR_SUCCESS; }

/* action spaces (xrCreateActionSpace below): which hand, aim or grip */
#define FX_ACTION_SPACE0 0x20
#define FX_MAX_ACTION_SPACES 16
static struct { int hand, grip; } s_aspace[FX_MAX_ACTION_SPACES];
static int action_space_kind(XrSpace sp, int *hand, int *grip);

XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpace(XrSpace space, XrSpace base, XrTime t, XrSpaceLocation *loc)
{
    XrPosef head, eye[2];
    XrFovf fov[2];
    int hand, grip;
    loc->locationFlags = 0;
    if (base == FX_SPACE_STAGE && fx_hand_pose && action_space_kind(space, &hand, &grip)) {
        /* hands: tracked only when the test gives a pose */
        XrSpaceVelocity *vel = loc->next && ((XrSpaceVelocity *)loc->next)->type == XR_TYPE_SPACE_VELOCITY ?
                               (XrSpaceVelocity *)loc->next : NULL;
        if (vel) vel->velocityFlags = 0;
        if (fx_hand_pose(hand, grip, t, &loc->pose)) {
            loc->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                 XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
            if (vel && fx_hand_velocity && fx_hand_velocity(hand, grip, t, &vel->linearVelocity))
                vel->velocityFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
        }
        return XR_SUCCESS;
    }
    if (space != FX_SPACE_VIEW || base != FX_SPACE_STAGE || !fx_trajectory) return XR_SUCCESS;   /* hands: not tracked */
    fx_trajectory(t, &head, eye, fov);
    loc->pose = head;
    loc->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                         XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrLocateViews(XrSession s, const XrViewLocateInfo *li, XrViewState *vs, uint32_t cap, uint32_t *n, XrView *v)
{
    XrPosef head, eye[2];
    XrFovf fov[2];
    int i;
    (void)s;
    *n = 2;
    if (cap < 2 || !fx_trajectory || li->space != FX_SPACE_STAGE) return XR_ERROR_SIZE_INSUFFICIENT;
    fx_trajectory(li->displayTime, &head, eye, fov);
    for (i = 0; i < 2; i++) { v[i].pose = eye[i]; v[i].fov = fov[i]; }
    vs->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    return XR_SUCCESS;
}

/* ---- swapchains: real VkImages ---- */

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainFormats(XrSession s, uint32_t cap, uint32_t *n, int64_t *f)
{
    /* like Quest 3's runtime: more than 64 formats, the preferred sRGB ones near the end, and the two-call idiom
     * enforced (a non-zero capacity below the count is XR_ERROR_SIZE_INSUFFICIENT) */
    enum { FX_FORMATS = 100 };
    uint32_t k;
    (void)s;
    *n = FX_FORMATS;
    if (cap == 0) return XR_SUCCESS;
    if (cap < FX_FORMATS) return XR_ERROR_SIZE_INSUFFICIENT;
    for (k = 0; k < FX_FORMATS - 3; k++) f[k] = (k & 1) ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    f[FX_FORMATS - 3] = VK_FORMAT_R8G8B8A8_UNORM; f[FX_FORMATS - 2] = VK_FORMAT_B8G8R8A8_SRGB; f[FX_FORMATS - 1] = VK_FORMAT_R8G8B8A8_SRGB;
    return XR_SUCCESS;
}

static void one_shot_layout(VkImage img, uint32_t layers)
{
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkCommandBuffer cb;
    ai.commandPool = fx_vk.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(fx_vk.device, &ai, &cb);
    vkBeginCommandBuffer(cb, &bi);
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = layers;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vkEndCommandBuffer(cb);
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    if (fx_queue_lock) pthread_mutex_lock(fx_queue_lock);
    vkQueueSubmit(fx_vk.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(fx_vk.queue);
    if (fx_queue_lock) pthread_mutex_unlock(fx_queue_lock);
    vkFreeCommandBuffers(fx_vk.device, fx_vk.pool, 1, &cb);
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateSwapchain(XrSession s, const XrSwapchainCreateInfo *ci, XrSwapchain *out)
{
    const XrBaseInStructure *p;
    FxSwapchain *sc = NULL;
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkImageFormatListCreateInfo fl = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };
    int i, k;
    (void)s;
    for (k = 0; k < FX_MAX_SWAPCHAINS && !sc; k++) if (!s_sc[k].used) sc = &s_sc[k];
    if (!sc) return XR_ERROR_LIMIT_REACHED;
    memset(sc, 0, sizeof(*sc));
    sc->used = 1;
    sc->ci = *ci;
    sc->foveation_level = -1;
    for (p = (const XrBaseInStructure *)ci->next; p; p = p->next)
        if (p->type == XR_TYPE_VULKAN_SWAPCHAIN_FORMAT_LIST_CREATE_INFO_KHR) {
            const XrVulkanSwapchainFormatListCreateInfoKHR *l = (const XrVulkanSwapchainFormatListCreateInfoKHR *)p;
            for (i = 0; i < (int)l->viewFormatCount && i < 4; i++) sc->view_formats[i] = l->viewFormats[i];
            sc->nview_formats = l->viewFormatCount;
        } else if (p->type == XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB) {
            if (fx_no_foveation) { sc->used = 0; return XR_ERROR_VALIDATION_FAILURE; }   /* extension not enabled */
            sc->foveation_flags = ((const XrSwapchainCreateInfoFoveationFB *)p)->flags;
            /* XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB: "the application must ensure ... that the
             * VK_EXT_fragment_density_map extension is supported and enabled" (xr.xml) */
            if ((sc->foveation_flags & XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB) && !fx_vk.dev_fdm) {
                sc->used = 0;
                return XR_ERROR_VALIDATION_FAILURE;
            }
        }
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = (VkFormat)ci->format;
    ii.extent.width = ci->width; ii.extent.height = ci->height; ii.extent.depth = 1;
    ii.mipLevels = 1; ii.arrayLayers = ci->arraySize;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;     /* the test reads the images back */
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) ii.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) ii.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) ii.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT) ii.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (sc->nview_formats) { fl.viewFormatCount = sc->nview_formats; fl.pViewFormats = sc->view_formats; ii.pNext = &fl; }
    for (i = 0; i < FX_IMAGES; i++) {
        VkMemoryRequirements mr;
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        VkPhysicalDeviceMemoryProperties mp;
        uint32_t t;
        if (vkCreateImage(fx_vk.device, &ii, NULL, &sc->img[i]) != VK_SUCCESS) return XR_ERROR_RUNTIME_FAILURE;
        vkGetImageMemoryRequirements(fx_vk.device, sc->img[i], &mr);
        vkGetPhysicalDeviceMemoryProperties(fx_vk.phys, &mp);
        for (t = 0; t < mp.memoryTypeCount; t++)
            if ((mr.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) break;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = t;
        if (vkAllocateMemory(fx_vk.device, &ai, NULL, &sc->mem[i]) != VK_SUCCESS) return XR_ERROR_RUNTIME_FAILURE;
        vkBindImageMemory(fx_vk.device, sc->img[i], sc->mem[i], 0);
        one_shot_layout(sc->img[i], ci->arraySize);   /* OpenXR hands out colour images in COLOR_ATTACHMENT_OPTIMAL */
    }
    *out = (XrSwapchain)(uintptr_t)(sc - s_sc + 1);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain h)
{
    FxSwapchain *sc = fx_swapchain(h);
    int i;
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    for (i = 0; i < FX_IMAGES; i++) { vkDestroyImage(fx_vk.device, sc->img[i], NULL); vkFreeMemory(fx_vk.device, sc->mem[i], NULL); }
    sc->used = 0;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainImages(XrSwapchain h, uint32_t cap, uint32_t *n, XrSwapchainImageBaseHeader *imgs)
{
    FxSwapchain *sc = fx_swapchain(h);
    uint32_t i;
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    *n = FX_IMAGES;
    if (cap > 0 && cap < FX_IMAGES) return XR_ERROR_SIZE_INSUFFICIENT;
    if (cap >= FX_IMAGES)
        for (i = 0; i < FX_IMAGES; i++) ((XrSwapchainImageVulkan2KHR *)imgs)[i].image = sc->img[i];
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain h, const XrSwapchainImageAcquireInfo *ai, uint32_t *index)
{
    FxSwapchain *sc = fx_swapchain(h);
    (void)ai;
    check_queue_lock("xrAcquireSwapchainImage");
    if (!sc || sc->acquired) return XR_ERROR_CALL_ORDER_INVALID;
    *index = sc->acquired_index = sc->next;
    sc->next = (sc->next + 1) % FX_IMAGES;
    sc->acquired = 1;
    sc->waited = 0;
    sc->acquires++;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain h, const XrSwapchainImageWaitInfo *wi)
{
    FxSwapchain *sc = fx_swapchain(h);
    (void)wi;
    if (!sc || !sc->acquired) return XR_ERROR_CALL_ORDER_INVALID;
    sc->waited = 1;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrReleaseSwapchainImage(XrSwapchain h, const XrSwapchainImageReleaseInfo *ri)
{
    FxSwapchain *sc = fx_swapchain(h);
    (void)ri;
    check_queue_lock("xrReleaseSwapchainImage");
    if (!sc || !sc->acquired || !sc->waited) return XR_ERROR_CALL_ORDER_INVALID;
    sc->acquired = 0;
    sc->last_released = sc->acquired_index;
    sc->ever_released = 1;
    sc->releases++;
    if (fx_on_release) fx_on_release(h, sc->last_released);
    return XR_SUCCESS;
}

/* ---- frames ---- */

XRAPI_ATTR XrResult XRAPI_CALL xrWaitFrame(XrSession s, const XrFrameWaitInfo *wi, XrFrameState *fs)
{
    XrTime t;
    (void)s; (void)wi;
    t = s_last_wait ? s_last_wait + fx_period : fx_now + fx_period;
    while (t < fx_now + fx_period / 2) t += fx_period;
    s_last_wait = t;
    fs->predictedDisplayTime = t;
    fs->predictedDisplayPeriod = fx_period;
    fs->shouldRender = XR_TRUE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrBeginFrame(XrSession s, const XrFrameBeginInfo *bi)
{
    (void)s; (void)bi;
    check_queue_lock("xrBeginFrame");
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEndFrame(XrSession s, const XrFrameEndInfo *ei)
{
    FxEndFrame f;
    uint32_t i;
    (void)s;
    check_queue_lock("xrEndFrame");
    memset(&f, 0, sizeof(f));
    f.display_time = ei->displayTime;
    f.nlayers = ei->layerCount;
    for (i = 0; i < ei->layerCount; i++) {
        const XrCompositionLayerBaseHeader *l = ei->layers[i];
        if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const XrCompositionLayerProjection *p = (const XrCompositionLayerProjection *)l;
            uint32_t v;
            f.has_proj = 1;
            f.proj = *p;
            for (v = 0; v < 2 && v < p->viewCount; v++) {
                FxSwapchain *sc = fx_swapchain(p->views[v].subImage.swapchain);
                f.views[v] = p->views[v];
                f.proj_image[v] = sc ? sc->last_released : 0;
            }
            f.proj.views = NULL;
        } else if (l->type == XR_TYPE_COMPOSITION_LAYER_QUAD && f.nquads < 2) {
            f.quads[f.nquads++] = *(const XrCompositionLayerQuad *)l;
        }
    }
    fx_last_end = f;
    fx_end_count++;
    if (fx_on_end_frame) fx_on_end_frame(&f);
    return XR_SUCCESS;
}

/* ---- actions (nothing tracked, nothing pressed) ---- */

XRAPI_ATTR XrResult XRAPI_CALL xrStringToPath(XrInstance i, const char *str, XrPath *p) { (void)i; *p = (XrPath)(strlen(str) + 1); return XR_SUCCESS; }
XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSet(XrInstance i, const XrActionSetCreateInfo *ci, XrActionSet *out) { (void)i; (void)ci; *out = (XrActionSet)(uintptr_t)1; return XR_SUCCESS; }
XRAPI_ATTR XrResult XRAPI_CALL xrDestroyActionSet(XrActionSet a) { (void)a; return XR_SUCCESS; }
#define FX_MAX_ACTIONS 32
static char s_action_names[FX_MAX_ACTIONS][XR_MAX_ACTION_NAME_SIZE];
XRAPI_ATTR XrResult XRAPI_CALL xrCreateAction(XrActionSet s, const XrActionCreateInfo *ci, XrAction *out)
{
    static uintptr_t n = 1;
    (void)s;
    if (n < FX_MAX_ACTIONS) snprintf(s_action_names[n], sizeof(s_action_names[n]), "%s", ci->actionName);
    *out = (XrAction)n++;
    return XR_SUCCESS;
}
static const char *action_name(XrAction a)
{
    const uintptr_t i = (uintptr_t)a;
    return i < FX_MAX_ACTIONS ? s_action_names[i] : "";
}
XRAPI_ATTR XrResult XRAPI_CALL xrSuggestInteractionProfileBindings(XrInstance i, const XrInteractionProfileSuggestedBinding *b) { (void)i; (void)b; return XR_SUCCESS; }
XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSpace(XrSession s, const XrActionSpaceCreateInfo *ci, XrSpace *out)
{
    static uintptr_t n = FX_ACTION_SPACE0;
    (void)s;
    if (n - FX_ACTION_SPACE0 < FX_MAX_ACTION_SPACES) {
        /* the fake's XrPath is strlen + 1: "/user/hand/left" 16, "/user/hand/right" 17 */
        s_aspace[n - FX_ACTION_SPACE0].hand = ci->subactionPath == 17 ? 1 : 0;
        s_aspace[n - FX_ACTION_SPACE0].grip = strcmp(action_name(ci->action), "grip_pose") == 0;
    }
    *out = (XrSpace)n++;
    return XR_SUCCESS;
}
static int action_space_kind(XrSpace sp, int *hand, int *grip)
{
    const uintptr_t i = (uintptr_t)sp;
    if (i < FX_ACTION_SPACE0 || i - FX_ACTION_SPACE0 >= FX_MAX_ACTION_SPACES) return 0;
    *hand = s_aspace[i - FX_ACTION_SPACE0].hand;
    *grip = s_aspace[i - FX_ACTION_SPACE0].grip;
    return 1;
}
XRAPI_ATTR XrResult XRAPI_CALL xrAttachSessionActionSets(XrSession s, const XrSessionActionSetsAttachInfo *ai) { (void)s; (void)ai; return XR_SUCCESS; }
XRAPI_ATTR XrResult XRAPI_CALL xrSyncActions(XrSession s, const XrActionsSyncInfo *si) { (void)s; (void)si; return XR_SUCCESS; }
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateFloat(XrSession s, const XrActionStateGetInfo *gi, XrActionStateFloat *st)
{
    (void)s;
    st->isActive = XR_FALSE; st->currentState = 0;
    if (fx_input_float && fx_input_float(action_name(gi->action), gi->subactionPath, &st->currentState)) st->isActive = XR_TRUE;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateBoolean(XrSession s, const XrActionStateGetInfo *gi, XrActionStateBoolean *st)
{
    (void)s;
    st->isActive = XR_FALSE; st->currentState = XR_FALSE;
    if (fx_input_bool && fx_input_bool(action_name(gi->action), gi->subactionPath, &st->currentState)) st->isActive = XR_TRUE;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateVector2f(XrSession s, const XrActionStateGetInfo *gi, XrActionStateVector2f *st)
{
    (void)s;
    st->isActive = XR_FALSE; st->currentState.x = st->currentState.y = 0.0f;
    if (fx_input_vec2 && fx_input_vec2(action_name(gi->action), gi->subactionPath, &st->currentState)) st->isActive = XR_TRUE;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrApplyHapticFeedback(XrSession s, const XrHapticActionInfo *hi, const XrHapticBaseHeader *h) { (void)s; (void)hi; (void)h; return XR_SUCCESS; }
XRAPI_ATTR XrResult XRAPI_CALL xrStopHapticFeedback(XrSession s, const XrHapticActionInfo *hi) { (void)s; (void)hi; return XR_SUCCESS; }
XRAPI_ATTR XrResult XRAPI_CALL xrRequestExitSession(XrSession s) { (void)s; return XR_SUCCESS; }
