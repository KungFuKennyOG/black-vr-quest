/*
 * xr_vulkan.c - Vulkan through XR_KHR_vulkan_enable2: instance, device,
 * swapchains and the built-in smoke-test pattern (renders without game
 * files, so head tracking and stereo can be checked on the headset).
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "xr_shell.h"
#include "vr_math.h"

#include "testpattern.vert.inc"   /* generated: uint32_t testpattern_vert_spv[] */
#include "testpattern.frag.inc"   /* generated: uint32_t testpattern_frag_spv[] */

#include "xr_result_names.h"

#define VKCHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { XLOGE("%s failed: %d %s (%s:%d)", #x, r_, vk_result_name(r_), __FILE__, __LINE__); xrs_note_failure(#x, (int)r_, 1, __FILE__, __LINE__); return 0; } } while (0)
#define XRCHECK(x) do { XrResult r_ = (x); if (XR_FAILED(r_)) { XLOGE("%s failed: %d %s (%s:%d)", #x, r_, xr_result_name(r_), __FILE__, __LINE__); xrs_note_failure(#x, (int)r_, 0, __FILE__, __LINE__); return 0; } } while (0)

/* ---- renderer weak defaults: no external renderer -> test pattern ---------- */

__attribute__((weak)) void vr_renderer_get_requirements(VrVulkanRequirements *req)
{
    memset(req, 0, sizeof(*req));
    req->api_version = VK_MAKE_API_VERSION(0, 1, 1, 0);
}
__attribute__((weak)) int vr_renderer_init(const VrVulkanContext *ctx) { (void)ctx; return 0; }
__attribute__((weak)) void vr_renderer_shutdown(void) {}
__attribute__((weak)) void vr_mark_world_pass(uint32_t pushbuffer_va, int begin) { (void)pushbuffer_va; (void)begin; }
__attribute__((weak)) void vr_mark_viewmodel_mvp(const float mvp[16], int constant_register) { (void)mvp; (void)constant_register; }
__attribute__((weak)) void vr_mark_viewmodel_offhand(const float k[16]) { (void)k; }

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name)
{
    uint32_t i;
    for (i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

int xrs_vulkan_create(XrShell *s)
{
    PFN_xrGetVulkanGraphicsRequirements2KHR get_req = NULL;
    PFN_xrCreateVulkanInstanceKHR create_instance = NULL;
    PFN_xrGetVulkanGraphicsDevice2KHR get_device = NULL;
    PFN_xrCreateVulkanDeviceKHR create_device = NULL;
    XrGraphicsRequirementsVulkan2KHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
    VrVulkanRequirements rr;
    VkResult vr;
    uint32_t api, i, n = 0;

    XRCHECK(xrGetInstanceProcAddr(s->instance, "xrGetVulkanGraphicsRequirements2KHR", (PFN_xrVoidFunction *)&get_req));
    XRCHECK(xrGetInstanceProcAddr(s->instance, "xrCreateVulkanInstanceKHR", (PFN_xrVoidFunction *)&create_instance));
    XRCHECK(xrGetInstanceProcAddr(s->instance, "xrGetVulkanGraphicsDevice2KHR", (PFN_xrVoidFunction *)&get_device));
    XRCHECK(xrGetInstanceProcAddr(s->instance, "xrCreateVulkanDeviceKHR", (PFN_xrVoidFunction *)&create_device));
    XRCHECK(get_req(s->instance, s->system, &req));

    vr_renderer_get_requirements(&rr);
    api = rr.api_version ? rr.api_version : VK_MAKE_API_VERSION(0, 1, 1, 0);
    if (api < VK_MAKE_API_VERSION(0, 1, 1, 0)) api = VK_MAKE_API_VERSION(0, 1, 1, 0);
    if (XR_VERSION_MAJOR(req.minApiVersionSupported) == 1 &&
        VK_MAKE_API_VERSION(0, 1, XR_VERSION_MINOR(req.minApiVersionSupported), 0) > api)
        api = VK_MAKE_API_VERSION(0, 1, XR_VERSION_MINOR(req.minApiVersionSupported), 0);
    s->vk_api = api;

    {
        VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        XrVulkanInstanceCreateInfoKHR xci = { XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR };
        app.pApplicationName = "BLACK VR";
        app.applicationVersion = 1;
        app.pEngineName = "xboxrecomp";
        app.apiVersion = api;
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = rr.instance_extension_count;
        ici.ppEnabledExtensionNames = rr.instance_extensions;
        xci.systemId = s->system;
        xci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        xci.vulkanCreateInfo = &ici;
        XRCHECK(create_instance(s->instance, &xci, &s->vk_instance, &vr));
        VKCHECK(vr);
    }
    {
        XrVulkanGraphicsDeviceGetInfoKHR gi = { XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR };
        gi.systemId = s->system;
        gi.vulkanInstance = s->vk_instance;
        XRCHECK(get_device(s->instance, &gi, &s->vk_phys));
    }
    {
        VkQueueFamilyProperties qf[16];
        n = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(s->vk_phys, &n, qf);
        s->vk_queue_family = UINT32_MAX;
        for (i = 0; i < n; i++)
            if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { s->vk_queue_family = i; break; }
        if (s->vk_queue_family == UINT32_MAX) { XLOGE("no graphics queue"); xrs_note_failure("no graphics queue family", 0, 1, __FILE__, __LINE__); return 0; }
    }
    {
        VkPhysicalDeviceMultiviewFeatures mv = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES };
        VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        VkPhysicalDeviceFeatures want, on;
        VkExtensionProperties *ext;
        const char *exts[64];
        uint32_t ne = 0, en = 0;
        VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        XrVulkanDeviceCreateInfoKHR xdi = { XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR };
        VkPhysicalDeviceMultiviewFeatures mv_on = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES };
        const float prio = 1.0f;

        f2.pNext = &mv;
        vkGetPhysicalDeviceFeatures2(s->vk_phys, &f2);
        s->multiview_supported = mv.multiview == VK_TRUE;

        /* The renderer's VkPhysicalDeviceFeatures, limited to what the device has: asking for a missing feature
         * fails vkCreateDevice (VK_ERROR_FEATURE_NOT_PRESENT). The Kelvin backend needs robustBufferAccess and
         * depthClamp, takes samplerAnisotropy / textureCompressionBC when present (vr_renderer_kvk.c). */
        memset(&want, 0, sizeof(want));
        if (rr.device_features10) want = *(const VkPhysicalDeviceFeatures *)rr.device_features10;
        {
            const VkBool32 *w = (const VkBool32 *)&want, *have = (const VkBool32 *)&f2.features;
            VkBool32 *o = (VkBool32 *)&on;
            const uint32_t nf = (uint32_t)(sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32));
            for (i = 0; i < nf; i++) {
                o[i] = w[i] && have[i] ? VK_TRUE : VK_FALSE;
                if (w[i] && !have[i]) XLOGW("renderer asked for device feature #%u, which the device lacks", i);
            }
        }

        vkEnumerateDeviceExtensionProperties(s->vk_phys, NULL, &ne, NULL);
        ext = (VkExtensionProperties *)calloc(ne ? ne : 1, sizeof(*ext));
        vkEnumerateDeviceExtensionProperties(s->vk_phys, NULL, &ne, ext);
        for (i = 0; i < rr.device_extension_count && en < 60; i++) {
            if (has_ext(ext, ne, rr.device_extensions[i])) exts[en++] = rr.device_extensions[i];
            else XLOGW("renderer asked for missing device extension %s", rr.device_extensions[i]);
        }
        /* Quest: foveation through the compositor needs these when present */
        if (has_ext(ext, ne, "VK_KHR_external_memory") && en < 60) exts[en++] = "VK_KHR_external_memory";
        /* XR_FB_foveation_vulkan: an eye swapchain created with XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB
         * requires VK_EXT_fragment_density_map "supported and enabled" on this device (xr.xml); without it a runtime
         * may refuse the swapchain, which would end the launch. No extension, no foveation. */
        if (s->has_foveation) {
            if (has_ext(ext, ne, "VK_EXT_fragment_density_map") && en < 60) {
                exts[en++] = "VK_EXT_fragment_density_map";
            } else {
                s->has_foveation = 0;
                XLOGW("foveation off: the GPU has no VK_EXT_fragment_density_map (XR_FB_foveation_vulkan needs it)");
            }
        }
        free(ext);

        q.queueFamilyIndex = s->vk_queue_family;
        q.queueCount = 1;
        q.pQueuePriorities = &prio;
        /* multiview (core in 1.1) is always enabled when present: the renderer's stereo needs it */
        mv_on.multiview = s->multiview_supported ? VK_TRUE : VK_FALSE;
        mv_on.pNext = (void *)rr.device_features_pnext;
        dci.pNext = &mv_on;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &q;
        dci.enabledExtensionCount = en;
        dci.ppEnabledExtensionNames = exts;
        dci.pEnabledFeatures = &on;
        xdi.systemId = s->system;
        xdi.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        xdi.vulkanPhysicalDevice = s->vk_phys;
        xdi.vulkanCreateInfo = &dci;
        XRCHECK(create_device(s->instance, &xdi, &s->vk_device, &vr));
        VKCHECK(vr);
        vkGetDeviceQueue(s->vk_device, s->vk_queue_family, 0, &s->vk_queue);

        memset(&s->vk_features, 0, sizeof(s->vk_features));
        s->vk_features.api_version = api;
        s->vk_features.multiview = (uint32_t)s->multiview_supported;
        s->vk_features.robust_buffer_access = on.robustBufferAccess;
        s->vk_features.depth_clamp = on.depthClamp;
        s->vk_features.sampler_anisotropy = on.samplerAnisotropy;
        s->vk_features.texture_compression_bc = on.textureCompressionBC;
        /* the swapchains are created MUTABLE_FORMAT (xrs_swapchain_create) */
        s->vk_features.swapchain_mutable_format = 1;
        s->vk_features.swapchain_format_list = (uint32_t)s->has_format_list;
        XLOGI("device features: multiview %u robustBufferAccess %u depthClamp %u anisotropy %u BC %u",
              s->vk_features.multiview, on.robustBufferAccess, on.depthClamp, on.samplerAnisotropy,
              on.textureCompressionBC);
    }
    XLOGI("Vulkan %u.%u device ready, multiview %s", VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api),
          s->multiview_supported ? "supported" : "unsupported");
    return 1;
}

/* ---- swapchains ------------------------------------------------------------- */

static VkFormat unorm_of(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return f;
    }
}

int xrs_swapchain_create(XrShell *s, XrsSwapchain *sc, uint32_t w, uint32_t h, uint32_t layers)
{
    return xrs_swapchain_create_ex(s, sc, w, h, layers, NULL);
}

int xrs_swapchain_create_ex(XrShell *s, XrsSwapchain *sc, uint32_t w, uint32_t h, uint32_t layers, const void *next)
{
    XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    XrSwapchainImageVulkan2KHR *imgs;
    XrVulkanSwapchainFormatListCreateInfoKHR fl = { XR_TYPE_VULKAN_SWAPCHAIN_FORMAT_LIST_CREATE_INFO_KHR };
    VkFormat view_formats[2];
    uint32_t i, n = 0;
    memset(sc, 0, sizeof(*sc));
    /* MUTABLE_FORMAT lets us take UNORM views of the sRGB image: the Xbox frame is
     * already display-referred, so the bytes are written as they are. With
     * XR_KHR_vulkan_swapchain_format_list the two formats are named, so the driver
     * can keep framebuffer compression (UBWC on Adreno) for a mutable image. */
    view_formats[0] = s->color_format;
    view_formats[1] = unorm_of(s->color_format);
    ci.next = next;
    if (s->has_format_list && view_formats[1] != view_formats[0]) {
        fl.viewFormatCount = 2;
        fl.viewFormats = view_formats;
        fl.next = next;
        ci.next = &fl;
    }
    ci.createFlags = 0;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                    XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
    ci.format = s->color_format;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = layers;
    ci.mipCount = 1;
    XRCHECK(xrCreateSwapchain(s->session, &ci, &sc->handle));
    sc->width = w; sc->height = h; sc->layers = layers;
    XRCHECK(xrEnumerateSwapchainImages(sc->handle, 0, &n, NULL));
    if (n == 0 || n > XR_MAX_SWAPCHAIN_IMAGES) {
        /* the whole list has to be fetched (capacity == count), but only XR_MAX_SWAPCHAIN_IMAGES are tracked */
        XLOGE("swapchain has %u images (supported 1..%d)", n, XR_MAX_SWAPCHAIN_IMAGES);
        xrs_note_failure("xrEnumerateSwapchainImages: image count", 0, 0, __FILE__, __LINE__);
        return 0;
    }
    imgs = (XrSwapchainImageVulkan2KHR *)calloc(n, sizeof(*imgs));
    if (!imgs) return 0;
    for (i = 0; i < n; i++) { imgs[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR; imgs[i].next = NULL; }
    XRCHECK(xrEnumerateSwapchainImages(sc->handle, n, &n, (XrSwapchainImageBaseHeader *)imgs));   /* failure ends the startup */
    sc->image_count = n;
    for (i = 0; i < n; i++) sc->images[i] = imgs[i].image;
    free(imgs);
    return xrs_swapchain_views(s, sc);
}

int xrs_swapchain_views(XrShell *s, XrsSwapchain *sc)
{
    uint32_t i, l;
    const VkFormat view_format = unorm_of(s->color_format);
    for (i = 0; i < sc->image_count; i++) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = sc->images[i];
        vi.format = view_format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        for (l = 0; l < sc->layers && l < 2; l++) {
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.subresourceRange.baseArrayLayer = l;
            vi.subresourceRange.layerCount = 1;
            VKCHECK(vkCreateImageView(s->vk_device, &vi, NULL, &sc->layer_views[i][l]));
        }
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vi.subresourceRange.baseArrayLayer = 0;
        vi.subresourceRange.layerCount = sc->layers;
        VKCHECK(vkCreateImageView(s->vk_device, &vi, NULL, &sc->array_views[i]));
    }
    return 1;
}

void xrs_swapchain_destroy(XrShell *s, XrsSwapchain *sc)
{
    uint32_t i, l;
    if (!sc->handle) return;
    for (i = 0; i < sc->image_count; i++) {
        for (l = 0; l < 2; l++) {
            if (sc->framebuffers[i][l]) vkDestroyFramebuffer(s->vk_device, sc->framebuffers[i][l], NULL);
            if (sc->layer_views[i][l]) vkDestroyImageView(s->vk_device, sc->layer_views[i][l], NULL);
        }
        if (sc->array_views[i]) vkDestroyImageView(s->vk_device, sc->array_views[i], NULL);
    }
    xrDestroySwapchain(sc->handle);
    memset(sc, 0, sizeof(*sc));
}

/* ---- test pattern --------------------------------------------------------------- */

typedef struct {
    float c0[4], cx[4], cy[4], eye[4], h0o[4], h0d[4], h1o[4], h1d[4];
} TpPush;

static VkShaderModule shader(VkDevice d, const uint32_t *code, size_t bytes)
{
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule m = VK_NULL_HANDLE;
    ci.codeSize = bytes;
    ci.pCode = code;
    if (vkCreateShaderModule(d, &ci, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

int xrs_testpattern_init(XrShell *s)
{
    VkAttachmentDescription att = { 0 };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { 0 };
    VkRenderPassCreateInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(TpPush) };
    VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineShaderStageCreateInfo st[2];
    VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba = { 0 };
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkCommandPoolCreateInfo cp = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkShaderModule vs, fs;
    int i;

    att.format = unorm_of(s->color_format);
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;   /* what the runtime expects at release */
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    VKCHECK(vkCreateRenderPass(s->vk_device, &rp, NULL, &s->tp_pass));

    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    VKCHECK(vkCreatePipelineLayout(s->vk_device, &pl, NULL, &s->tp_layout));

    vs = shader(s->vk_device, testpattern_vert_spv, sizeof(testpattern_vert_spv));
    fs = shader(s->vk_device, testpattern_frag_spv, sizeof(testpattern_frag_spv));
    if (!vs || !fs) { XLOGE("test pattern shaders failed"); return 0; }
    memset(st, 0, sizeof(st));
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    cba.colorWriteMask = 0xF;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    gp.stageCount = 2;
    gp.pStages = st;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = s->tp_layout;
    gp.renderPass = s->tp_pass;
    VKCHECK(vkCreateGraphicsPipelines(s->vk_device, VK_NULL_HANDLE, 1, &gp, NULL, &s->tp_pipeline));
    vkDestroyShaderModule(s->vk_device, vs, NULL);
    vkDestroyShaderModule(s->vk_device, fs, NULL);

    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = s->vk_queue_family;
    VKCHECK(vkCreateCommandPool(s->vk_device, &cp, NULL, &s->tp_pool));
    ca.commandPool = s->tp_pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = VR_EYE_COUNT;
    VKCHECK(vkAllocateCommandBuffers(s->vk_device, &ca, s->tp_cmd));
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (i = 0; i < VR_EYE_COUNT; i++) VKCHECK(vkCreateFence(s->vk_device, &fi, NULL, &s->tp_fence[i]));
    s->tp_ready = 1;
    return 1;
}

static VkFramebuffer framebuffer(XrShell *s, XrsSwapchain *sc, uint32_t image, uint32_t layer)
{
    VkFramebufferCreateInfo fc = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    if (sc->framebuffers[image][layer]) return sc->framebuffers[image][layer];
    fc.renderPass = s->tp_pass;
    fc.attachmentCount = 1;
    fc.pAttachments = &sc->layer_views[image][layer];
    fc.width = sc->width;
    fc.height = sc->height;
    fc.layers = 1;
    if (vkCreateFramebuffer(s->vk_device, &fc, NULL, &sc->framebuffers[image][layer]) != VK_SUCCESS) return VK_NULL_HANDLE;
    return sc->framebuffers[image][layer];
}

static VrPose pose_of(const XrPosef *p)
{
    VrPose r;
    r.orientation = vrm_q(p->orientation.x, p->orientation.y, p->orientation.z, p->orientation.w);
    r.position = vrm_v3(p->position.x, p->position.y, p->position.z);
    return r;
}

void xrs_testpattern_draw(XrShell *s, int slot, XrsSwapchain *sc, uint32_t image, uint32_t layer,
                          const XrPosef *eye, const XrFovf *fov, double time_s)
{
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkViewport v = { 0 };
    VkRect2D sr = { { 0, 0 }, { 0, 0 } };
    VkFramebuffer fb;
    VkCommandBuffer cmd = s->tp_cmd[slot];
    TpPush pc;
    VrPose ep = pose_of(eye), hand[2];
    VrVec3 c0, cx, cy;
    int valid[2];
    float trig[2];
    VrPadState pad;

    if (!s->tp_ready || !(fb = framebuffer(s, sc, image, layer))) return;
    vkWaitForFences(s->vk_device, 1, &s->tp_fence[slot], VK_TRUE, UINT64_MAX);
    vkResetFences(s->vk_device, 1, &s->tp_fence[slot]);

    /* ray directions at the image corners, world space */
    {
        const float tl = tanf(fov->angleLeft), tr = tanf(fov->angleRight);
        const float td = tanf(fov->angleDown), tu = tanf(fov->angleUp);
        const VrVec3 bl = vrm_q_rotate(ep.orientation, vrm_v3(tl, td, -1));
        const VrVec3 br = vrm_q_rotate(ep.orientation, vrm_v3(tr, td, -1));
        const VrVec3 tlv = vrm_q_rotate(ep.orientation, vrm_v3(tl, tu, -1));
        c0 = bl; cx = vrm_sub(br, bl); cy = vrm_sub(tlv, bl);
    }
    memset(&pc, 0, sizeof(pc));
    pc.c0[0] = c0.x; pc.c0[1] = c0.y; pc.c0[2] = c0.z; pc.c0[3] = (float)time_s;
    pc.cx[0] = cx.x; pc.cx[1] = cx.y; pc.cx[2] = cx.z; pc.cx[3] = s->game_files_present ? 0.0f : 1.0f;
    pc.cy[0] = cy.x; pc.cy[1] = cy.y; pc.cy[2] = cy.z; pc.cy[3] = s->floor_y;
    vrb_touch_debug(&pad);
    pc.eye[0] = ep.position.x; pc.eye[1] = ep.position.y; pc.eye[2] = ep.position.z; pc.eye[3] = (float)pad.buttons;
    vrb_head_for_testpattern(hand, valid, trig);
    {
        float *o[2] = { pc.h0o, pc.h1o }, *d[2] = { pc.h0d, pc.h1d };
        int h;
        for (h = 0; h < 2; h++) {
            const VrVec3 f = vrm_q_rotate(hand[h].orientation, vrm_v3(0, 0, -1));
            o[h][0] = hand[h].position.x; o[h][1] = hand[h].position.y; o[h][2] = hand[h].position.z;
            o[h][3] = valid[h] ? 1.0f : 0.0f;
            d[h][0] = f.x; d[h][1] = f.y; d[h][2] = f.z; d[h][3] = trig[h];
        }
    }

    vkResetCommandBuffer(cmd, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    rb.renderPass = s->tp_pass;
    rb.framebuffer = fb;
    rb.renderArea.extent.width = sc->width;
    rb.renderArea.extent.height = sc->height;
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    v.width = (float)sc->width;
    v.height = (float)sc->height;
    v.maxDepth = 1.0f;
    sr.extent.width = sc->width;
    sr.extent.height = sc->height;
    vkCmdSetViewport(cmd, 0, 1, &v);
    vkCmdSetScissor(cmd, 0, 1, &sr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s->tp_pipeline);
    vkCmdPushConstants(cmd, s->tp_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vr_queue_lock();
    vkQueueSubmit(s->vk_queue, 1, &si, s->tp_fence[slot]);
    vr_queue_unlock();
}

void xrs_vulkan_destroy(XrShell *s)
{
    int i;
    if (!s->vk_device) return;
    vkDeviceWaitIdle(s->vk_device);
    for (i = 0; i < VR_EYE_COUNT; i++) xrs_swapchain_destroy(s, &s->eye[i]);
    xrs_swapchain_destroy(s, &s->flat);
    xrs_swapchain_destroy(s, &s->hud);
    if (s->tp_ready) {
        for (i = 0; i < VR_EYE_COUNT; i++) vkDestroyFence(s->vk_device, s->tp_fence[i], NULL);
        vkDestroyCommandPool(s->vk_device, s->tp_pool, NULL);
        vkDestroyPipeline(s->vk_device, s->tp_pipeline, NULL);
        vkDestroyPipelineLayout(s->vk_device, s->tp_layout, NULL);
        vkDestroyRenderPass(s->vk_device, s->tp_pass, NULL);
        s->tp_ready = 0;
    }
    vkDestroyDevice(s->vk_device, NULL);
    vkDestroyInstance(s->vk_instance, NULL);
    s->vk_device = VK_NULL_HANDLE;
    s->vk_instance = VK_NULL_HANDLE;
}
