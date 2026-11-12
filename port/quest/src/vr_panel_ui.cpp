/*
 * vr_panel_ui.cpp - the settings panel's Dear ImGui UI and its Vulkan renderer (vr_panel.h; port/notes/settings-panel.md).
 *
 * Runs on the XR thread only, and only while the panel is open (xr_panel.c). The renderer is self-contained: its own
 * pipeline, font texture, descriptor pool and per-frame buffers on the shell's VkDevice; it records into the command
 * buffer the caller submits (under vr_queue_lock, with its own fence), so it never touches the queue itself and never
 * waits on it. Font atlas uploads (ImGuiBackendFlags_RendererHasTextures) are copies recorded before the render pass;
 * the barriers order them after any earlier submission that sampled the atlas.
 *
 * Colours: ImGui's colours are display (sRGB-encoded) values; the panel swapchain image is sRGB and written through a
 * UNORM view (as every shell swapchain), so the bytes are the colours as authored. The target is cleared to
 * (0,0,0,0) and blended "over" with alpha ONE / ONE_MINUS_SRC_ALPHA: premultiplied colour + coverage, as the
 * compositor expects for a quad layer with XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT.
 */
#include <float.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "imgui.h"
#include "vr_panel.h"

#include "panel.vert.inc"           /* generated: panel_vert_spv[] */
#include "panel.frag.inc"           /* generated: panel_frag_spv[] */
#include "roboto_medium.inc"        /* generated: roboto_medium_ttf[], third_party/imgui/misc/fonts (Apache 2.0) */

namespace {

struct Tex {
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
    VkDescriptorSet set;
    int w, h;
};
struct Buf {
    VkBuffer buf;
    VkDeviceMemory mem;
    VkDeviceSize size;
    void *map;
};
struct Fb { VkImageView view; VkFramebuffer fb; uint32_t w, h; };
struct Doomed { Tex *t; int frames; };

struct Ui {
    int ready;
    ImGuiContext *ctx;
    ImFont *font;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkFormat format;
    VkSampler sampler;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool pool;
    VkPipelineLayout layout;
    VkRenderPass pass;
    VkPipeline pipe;
    Buf vtx[2], idx[2], stage[2];
    Fb fbs[16];
    Doomed doomed[16];
    int ndoomed;
} U;

/* palette: BLACK's menus are black, white and a hard red */
const ImVec4 kBg       (0.045f, 0.047f, 0.052f, 0.97f);
const ImVec4 kPanel    (0.090f, 0.095f, 0.105f, 1.00f);
const ImVec4 kFrame    (0.140f, 0.148f, 0.160f, 1.00f);
const ImVec4 kFrameHi  (0.200f, 0.210f, 0.225f, 1.00f);
const ImVec4 kRed      (0.760f, 0.075f, 0.090f, 1.00f);
const ImVec4 kRedHi    (0.880f, 0.140f, 0.150f, 1.00f);
const ImVec4 kRedLo    (0.560f, 0.050f, 0.065f, 1.00f);
const ImVec4 kText     (0.930f, 0.930f, 0.925f, 1.00f);
const ImVec4 kDim      (0.580f, 0.600f, 0.620f, 1.00f);
const ImVec4 kAmber    (0.980f, 0.690f, 0.180f, 1.00f);
const ImVec4 kGreen    (0.400f, 0.820f, 0.480f, 1.00f);
const ImVec4 kBorder   (0.240f, 0.250f, 0.270f, 1.00f);

const float kFontSize = 28.0f, kSmall = 21.0f, kTitle = 40.0f;

uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(U.phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

void buf_free(Buf *b)
{
    if (b->buf) vkDestroyBuffer(U.dev, b->buf, NULL);
    if (b->mem) { vkUnmapMemory(U.dev, b->mem); vkFreeMemory(U.dev, b->mem, NULL); }
    memset(b, 0, sizeof(*b));
}

/* host-visible, coherent, persistently mapped; grown (the slot's previous use has finished: its fence was waited) */
int buf_ensure(Buf *b, VkDeviceSize need, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements mr;
    if (b->buf && b->size >= need) return 1;
    buf_free(b);
    if (need < 65536) need = 65536;
    need = need + need / 2;
    bi.size = need;
    bi.usage = usage;
    if (vkCreateBuffer(U.dev, &bi, NULL, &b->buf) != VK_SUCCESS) return 0;
    vkGetBufferMemoryRequirements(U.dev, b->buf, &mr);
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(U.dev, &ai, NULL, &b->mem) != VK_SUCCESS) { buf_free(b); return 0; }
    vkBindBufferMemory(U.dev, b->buf, b->mem, 0);
    if (vkMapMemory(U.dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map) != VK_SUCCESS) { buf_free(b); return 0; }
    b->size = need;
    return 1;
}

VkShaderModule shader(const uint32_t *code, size_t bytes)
{
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule m = VK_NULL_HANDLE;
    ci.codeSize = bytes;
    ci.pCode = code;
    vkCreateShaderModule(U.dev, &ci, NULL, &m);
    return m;
}

int vk_init()
{
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.25f;
    if (vkCreateSampler(U.dev, &si, NULL, &U.sampler) != VK_SUCCESS) return 0;

    VkDescriptorSetLayoutBinding b = {};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dl.bindingCount = 1;
    dl.pBindings = &b;
    if (vkCreateDescriptorSetLayout(U.dev, &dl, NULL, &U.dsl) != VK_SUCCESS) return 0;

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = 16;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(U.dev, &pi, NULL, &U.pool) != VK_SUCCESS) return 0;

    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &U.dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(U.dev, &pl, NULL, &U.layout) != VK_SUCCESS) return 0;

    VkAttachmentDescription att = {};
    att.format = U.format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;   /* what the runtime expects at release */
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    if (vkCreateRenderPass(U.dev, &rp, NULL, &U.pass) != VK_SUCCESS) return 0;

    VkShaderModule vs = shader(panel_vert_spv, sizeof(panel_vert_spv)), fs = shader(panel_frag_spv, sizeof(panel_frag_spv));
    if (!vs || !fs) return 0;
    VkPipelineShaderStageCreateInfo st[2] = {};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkVertexInputBindingDescription vb = { 0, sizeof(ImDrawVert), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[3] = {
        { 0, 0, VK_FORMAT_R32G32_SFLOAT, (uint32_t)offsetof(ImDrawVert, pos) },
        { 1, 0, VK_FORMAT_R32G32_SFLOAT, (uint32_t)offsetof(ImDrawVert, uv) },
        { 2, 0, VK_FORMAT_R8G8B8A8_UNORM, (uint32_t)offsetof(ImDrawVert, col) },
    };
    VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &vb;
    vin.vertexAttributeDescriptionCount = 3;
    vin.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba = {};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.stageCount = 2;
    gp.pStages = st;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = U.layout;
    gp.renderPass = U.pass;
    VkResult r = vkCreateGraphicsPipelines(U.dev, VK_NULL_HANDLE, 1, &gp, NULL, &U.pipe);
    vkDestroyShaderModule(U.dev, vs, NULL);
    vkDestroyShaderModule(U.dev, fs, NULL);
    return r == VK_SUCCESS;
}

void tex_destroy(Tex *t)
{
    if (!t) return;
    if (t->set) vkFreeDescriptorSets(U.dev, U.pool, 1, &t->set);
    if (t->view) vkDestroyImageView(U.dev, t->view, NULL);
    if (t->image) vkDestroyImage(U.dev, t->image, NULL);
    if (t->mem) vkFreeMemory(U.dev, t->mem, NULL);
    IM_DELETE(t);
}

Tex *tex_create(int w, int h)
{
    Tex *t = IM_NEW(Tex)();
    memset(t, 0, sizeof(*t));
    t->w = w; t->h = h;
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent.width = (uint32_t)w; ii.extent.height = (uint32_t)h; ii.extent.depth = 1;
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(U.dev, &ii, NULL, &t->image) != VK_SUCCESS) { tex_destroy(t); return NULL; }
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(U.dev, t->image, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, 0);
    if (vkAllocateMemory(U.dev, &ai, NULL, &t->mem) != VK_SUCCESS) { tex_destroy(t); return NULL; }
    vkBindImageMemory(U.dev, t->image, t->mem, 0);
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = t->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ii.format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(U.dev, &vi, NULL, &t->view) != VK_SUCCESS) { tex_destroy(t); return NULL; }
    VkDescriptorSetAllocateInfo da = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    da.descriptorPool = U.pool;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &U.dsl;
    if (vkAllocateDescriptorSets(U.dev, &da, &t->set) != VK_SUCCESS) { t->set = VK_NULL_HANDLE; tex_destroy(t); return NULL; }
    VkDescriptorImageInfo dii = { U.sampler, t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr.dstSet = t->set;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &dii;
    vkUpdateDescriptorSets(U.dev, 1, &wr, 0, NULL);
    return t;
}

void barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da,
             VkPipelineStageFlags ss, VkPipelineStageFlags ds)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = sa; b.dstAccessMask = da;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

/* ImGui's texture requests: create / update (copies recorded into cmd) / destroy (deferred past the frames in flight) */
void textures(VkCommandBuffer cmd, ImDrawData *dd, int slot)
{
    int i;
    for (i = 0; i < U.ndoomed;)                          /* two frames later: no submission uses it any more */
        if (--U.doomed[i].frames <= 0) { tex_destroy(U.doomed[i].t); U.doomed[i] = U.doomed[--U.ndoomed]; }
        else i++;
    if (!dd->Textures) return;
    VkDeviceSize need = 0;
    for (ImTextureData *t : *dd->Textures)
        if (t->Status == ImTextureStatus_WantCreate) need += (VkDeviceSize)t->Width * t->Height * 4;
        else if (t->Status == ImTextureStatus_WantUpdates) need += (VkDeviceSize)t->UpdateRect.w * t->UpdateRect.h * 4;
    if (need && !buf_ensure(&U.stage[slot], need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return;
    VkDeviceSize off = 0;
    for (ImTextureData *t : *dd->Textures) {
        if (t->Status == ImTextureStatus_WantDestroy && t->UnusedFrames >= 2) {
            Tex *bt = (Tex *)t->BackendUserData;
            if (bt && U.ndoomed < 16) { U.doomed[U.ndoomed].t = bt; U.doomed[U.ndoomed].frames = 3; U.ndoomed++; }
            else if (bt) tex_destroy(bt);              /* never: 16 textures doomed at once */
            t->BackendUserData = NULL;
            t->SetTexID(ImTextureID_Invalid);
            t->SetStatus(ImTextureStatus_Destroyed);
            continue;
        }
        if (t->Status != ImTextureStatus_WantCreate && t->Status != ImTextureStatus_WantUpdates) continue;
        IM_ASSERT(t->Format == ImTextureFormat_RGBA32);
        int x = 0, y = 0, w = t->Width, h = t->Height;
        Tex *bt = (Tex *)t->BackendUserData;
        VkImageLayout from = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (t->Status == ImTextureStatus_WantCreate) {
            if (bt && U.ndoomed < 16) { U.doomed[U.ndoomed].t = bt; U.doomed[U.ndoomed].frames = 3; U.ndoomed++; }
            bt = tex_create(t->Width, t->Height);
            if (!bt) continue;
            t->BackendUserData = bt;
            t->SetTexID((ImTextureID)(uintptr_t)bt->set);
            from = VK_IMAGE_LAYOUT_UNDEFINED;
        } else {
            if (!bt) continue;
            x = t->UpdateRect.x; y = t->UpdateRect.y; w = t->UpdateRect.w; h = t->UpdateRect.h;
        }
        if (w <= 0 || h <= 0) { t->SetStatus(ImTextureStatus_OK); continue; }
        uint8_t *dst = (uint8_t *)U.stage[slot].map + off;
        for (int r = 0; r < h; r++) memcpy(dst + (size_t)r * w * 4, t->GetPixelsAt(x, y + r), (size_t)w * 4);
        barrier(cmd, bt->image, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy c = {};
        c.bufferOffset = off;
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.layerCount = 1;
        c.imageOffset.x = x; c.imageOffset.y = y;
        c.imageExtent.width = (uint32_t)w; c.imageExtent.height = (uint32_t)h; c.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(cmd, U.stage[slot].buf, bt->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        barrier(cmd, bt->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        off += (VkDeviceSize)w * h * 4;
        t->SetStatus(ImTextureStatus_OK);
    }
}

VkFramebuffer framebuffer(VkImageView view, uint32_t w, uint32_t h)
{
    int i, free_i = -1;
    for (i = 0; i < 16; i++) {
        if (U.fbs[i].view == view && U.fbs[i].w == w && U.fbs[i].h == h) return U.fbs[i].fb;
        if (!U.fbs[i].view && free_i < 0) free_i = i;
    }
    if (free_i < 0) return VK_NULL_HANDLE;
    VkFramebufferCreateInfo fc = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fc.renderPass = U.pass;
    fc.attachmentCount = 1;
    fc.pAttachments = &view;
    fc.width = w; fc.height = h; fc.layers = 1;
    if (vkCreateFramebuffer(U.dev, &fc, NULL, &U.fbs[free_i].fb) != VK_SUCCESS) return VK_NULL_HANDLE;
    U.fbs[free_i].view = view; U.fbs[free_i].w = w; U.fbs[free_i].h = h;
    return U.fbs[free_i].fb;
}

void style()
{
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(26, 20);
    s.FramePadding = ImVec2(16, 10);
    s.ItemSpacing = ImVec2(12, 12);
    s.ItemInnerSpacing = ImVec2(10, 8);
    s.WindowRounding = 14.0f;
    s.ChildRounding = 8.0f;
    s.FrameRounding = 7.0f;
    s.GrabRounding = 6.0f;
    s.TabRounding = 7.0f;
    s.ScrollbarSize = 22.0f;
    s.ScrollbarRounding = 8.0f;
    s.GrabMinSize = 14.0f;
    s.WindowBorderSize = 2.0f;
    s.FrameBorderSize = 0.0f;
    s.TabBorderSize = 0.0f;
    s.AntiAliasedLines = s.AntiAliasedFill = true;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kDim;
    c[ImGuiCol_WindowBg] = kBg;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = kPanel;
    c[ImGuiCol_Border] = kBorder;
    c[ImGuiCol_FrameBg] = kFrame;
    c[ImGuiCol_FrameBgHovered] = kFrameHi;
    c[ImGuiCol_FrameBgActive] = kFrameHi;
    c[ImGuiCol_Button] = kFrame;
    c[ImGuiCol_ButtonHovered] = kFrameHi;
    c[ImGuiCol_ButtonActive] = kRedLo;
    c[ImGuiCol_SliderGrab] = kRed;
    c[ImGuiCol_SliderGrabActive] = kRedHi;
    c[ImGuiCol_CheckMark] = kRed;
    c[ImGuiCol_Header] = kFrame;
    c[ImGuiCol_HeaderHovered] = kFrameHi;
    c[ImGuiCol_HeaderActive] = kRedLo;
    c[ImGuiCol_Separator] = kBorder;
    c[ImGuiCol_Tab] = kPanel;
    c[ImGuiCol_TabHovered] = kFrameHi;
    c[ImGuiCol_TabSelected] = kRed;
    c[ImGuiCol_TabSelectedOverline] = kRedHi;
    c[ImGuiCol_TabDimmed] = kPanel;
    c[ImGuiCol_TabDimmedSelected] = kRedLo;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = kFrameHi;
    c[ImGuiCol_ScrollbarGrabHovered] = kDim;
    c[ImGuiCol_ScrollbarGrabActive] = kRed;
}

/* ---- where each control was drawn last frame (tests/vr_glue aims the laser with it: vrp_ui_test_rect) ---- */
struct Mark { char name[48]; float r[4]; };
Mark g_marks[2][160];
int g_nmarks[2], g_mark_cur;

void mark(const char *fmt, ...) IM_FMTARGS(1);
void mark(const char *fmt, ...)
{
    Mark *m;
    va_list ap;
    if (g_nmarks[g_mark_cur] >= 160) return;
    m = &g_marks[g_mark_cur][g_nmarks[g_mark_cur]++];
    va_start(ap, fmt);
    vsnprintf(m->name, sizeof(m->name), fmt, ap);
    va_end(ap);
    m->r[0] = ImGui::GetItemRectMin().x; m->r[1] = ImGui::GetItemRectMin().y;
    m->r[2] = ImGui::GetItemRectMax().x; m->r[3] = ImGui::GetItemRectMax().y;
}

/* ---- widgets sized for a laser at arm's length ---- */

bool seg_button(const char *label, bool on, float w)
{
    ImGui::PushStyleColor(ImGuiCol_Button, on ? kRed : kFrame);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? kRedHi : kFrameHi);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kRedLo);
    const bool r = ImGui::Button(label, ImVec2(w, 0));
    ImGui::PopStyleColor(3);
    return r;
}

/* one setting's control; returns 1 when the value changed */
int control(VrShellConfig *edit, int id, const VrsCaps *caps, float width)
{
    const VrSettingDesc *d = &k_vrs[id];
    const float v = vrs_get(edit, id);
    float nv = v;
    const bool avail = vrs_available(id, caps) == VRS_AVAILABLE;
    if (!avail) ImGui::BeginDisabled();
    ImGui::PushID(id);
    if (d->type == VRS_BOOL) {
        const float w = (width - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (seg_button("Off", v == 0.0f, w)) nv = 0.0f;
        mark("seg:%s:0", d->key);
        ImGui::SameLine();
        if (seg_button("On", v != 0.0f, w)) nv = 1.0f;
        mark("seg:%s:1", d->key);
    } else if (d->type == VRS_ENUM) {
        const float sp = ImGui::GetStyle().ItemSpacing.x * 0.5f;
        const float w = (width - sp * (float)(d->count - 1)) / (float)d->count;
        /* four or five options: a smaller face keeps every label whole */
        const bool many = d->count >= 4;
        if (many) ImGui::PushFont(NULL, 23.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp, ImGui::GetStyle().ItemSpacing.y));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, ImGui::GetStyle().FramePadding.y + (many ? 2.5f : 0.0f)));
        for (int i = 0; i < d->count; i++) {
            if (i) ImGui::SameLine();
            if (seg_button(d->labels[i], (int)v == i, w)) nv = (float)i;
            mark("seg:%s:%d", d->key, i);
        }
        ImGui::PopStyleVar(2);
        if (many) ImGui::PopFont();
    } else {
        const float bw = ImGui::GetFrameHeight() * 1.25f, sp = ImGui::GetStyle().ItemSpacing.x * 0.5f;
        char fmt[32];
        float f = v;
        snprintf(fmt, sizeof(fmt), "%s%s%s", d->fmt, d->unit ? " " : "", d->unit ? d->unit : "");
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp, ImGui::GetStyle().ItemSpacing.y));
        if (ImGui::Button("-", ImVec2(bw, 0))) nv = vrs_clamp(id, v - d->step);
        mark("minus:%s", d->key);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(width - 2.0f * (bw + sp));
        if (ImGui::SliderFloat("##v", &f, d->lo, d->hi, fmt, ImGuiSliderFlags_AlwaysClamp)) nv = vrs_clamp(id, f);
        ImGui::SameLine();
        if (ImGui::Button("+", ImVec2(bw, 0))) nv = vrs_clamp(id, v + d->step);
        mark("plus:%s", d->key);
        ImGui::PopStyleVar();
    }
    ImGui::PopID();
    if (!avail) ImGui::EndDisabled();
    if (nv != v) { vrs_set(edit, id, nv); return 1; }
    return 0;
}

void badge(const char *text, ImVec4 col)
{
    ImGui::PushFont(NULL, kSmall);
    ImGui::TextColored(col, "%s", text);
    ImGui::PopFont();
}

int rows(const VrpUiIn *in, int tab)
{
    int changed = 0;
    const float full = ImGui::GetContentRegionAvail().x;
    const float label_w = full * 0.36f, ctrl_w = full - label_w - 124.0f;
    if (ImGui::BeginTable("rows", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthFixed, label_w);
        ImGui::TableSetupColumn("c", ImGuiTableColumnFlags_WidthFixed, ctrl_w);
        ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch);
        for (int id = 0; id < VRS_COUNT; id++) {
            const VrSettingDesc *d = &k_vrs[id];
            if (d->tab != tab) continue;
            ImGui::TableNextRow(0, 0);
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(d->label);
            ImGui::PushFont(NULL, kSmall);
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + label_w - 12.0f);
            ImGui::TextUnformatted(d->help);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::TableSetColumnIndex(1);
            changed |= control(in->edit, id, in->caps, ctrl_w - 8.0f);
            ImGui::TableSetColumnIndex(2);
            ImGui::AlignTextToFramePadding();
            if (vrs_available(id, in->caps) != VRS_AVAILABLE) badge("N/A", kDim);
            else if (vrs_is_live(id, in->caps)) badge("LIVE", kGreen);
            else if (vrs_pending_restart(in->boot, in->edit, in->caps, id)) badge("RESTART\nto apply", kAmber);
            else badge("RESTART", kDim);
        }
        ImGui::EndTable();
    }
    return changed;
}

void info_line(const char *k, const char *fmt, ...) IM_FMTARGS(2);
void info_line(const char *k, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(kDim, "%s", k);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextWrapped("%s", buf);
}

void info_tab(const VrpInfo *i)
{
    if (!i) return;
    if (ImGui::BeginTable("info", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 250.0f);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
        info_line("Game", "%.1f fps", i->game_fps);
        if (i->display_hz > 0.0f) info_line("XR loop", "%.1f fps (display %.0f Hz)", i->xr_fps, i->display_hz);
        else info_line("XR loop", "%.1f fps (display rate not reported)", i->xr_fps);
        info_line("Eye images", "%u x %u per eye (scale x%.2f, %s)", i->eye_w, i->eye_h, i->res_scale,
                  i->multiview ? "multiview" : "2 swapchains");
        info_line("Foveation", "%s", i->foveation_applied >= 1 && i->foveation_applied <= 4 ?
                  k_vrs_fov_labels[i->foveation_applied] : "none");
        info_line("GPU", "%s", i->gpu);
        info_line("OpenXR", "%s", i->runtime);
        info_line("Build", "%s", i->build);
        info_line("Settings file", "%s", i->ini_path);
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextColored(kDim, "Threads (%% of one core)");
    if (ImGui::BeginTable("threads", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("n", ImGuiTableColumnFlags_WidthFixed, 250.0f);
        ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("p", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        for (int k = 0; k < i->nthreads; k++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s", i->threads[k].name);
            ImGui::TableSetColumnIndex(1);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, i->threads[k].pct > 85.0f ? kRed : kDim);
            ImGui::ProgressBar(i->threads[k].pct / 100.0f > 1.0f ? 1.0f : i->threads[k].pct / 100.0f,
                               ImVec2(-1, ImGui::GetTextLineHeight()), "");
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.0f%%", i->threads[k].pct);
        }
        if (!i->nthreads) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(kDim, "sampling...");
        }
        ImGui::EndTable();
    }
}

} // namespace

extern "C" int vrp_ui_init(const VrpVkInit *ci)
{
    if (U.ready) return 1;
    memset(&U, 0, sizeof(U));
    U.phys = ci->phys;
    U.dev = ci->device;
    U.format = ci->format;
    IMGUI_CHECKVERSION();
    U.ctx = ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.LogFilename = NULL;
    io.BackendRendererName = "blackvr_panel_vk";
    io.BackendPlatformName = "blackvr_panel_xr";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.MouseDrawCursor = false;
    {
        ImFontConfig fc;
        fc.FontDataOwnedByAtlas = false;          /* the array is static */
        fc.OversampleH = 2;
        snprintf(fc.Name, sizeof(fc.Name), "Roboto-Medium");
        U.font = io.Fonts->AddFontFromMemoryTTF((void *)roboto_medium_ttf, (int)roboto_medium_ttf_len, kFontSize, &fc);
    }
    style();
    ImGui::GetStyle().FontSizeBase = kFontSize;
    if (!U.font || !vk_init()) { vrp_ui_shutdown(); return 0; }
    U.ready = 1;
    return 1;
}

extern "C" void vrp_ui_shutdown(void)
{
    if (U.dev) {
        if (U.ctx) {
            for (ImTextureData *t : ImGui::GetPlatformIO().Textures) {
                tex_destroy((Tex *)t->BackendUserData);
                t->BackendUserData = NULL;
                t->SetTexID(ImTextureID_Invalid);
                t->SetStatus(ImTextureStatus_Destroyed);
            }
        }
        for (int i = 0; i < U.ndoomed; i++) tex_destroy(U.doomed[i].t);
        for (int i = 0; i < 16; i++) if (U.fbs[i].fb) vkDestroyFramebuffer(U.dev, U.fbs[i].fb, NULL);
        for (int s = 0; s < 2; s++) { buf_free(&U.vtx[s]); buf_free(&U.idx[s]); buf_free(&U.stage[s]); }
        if (U.pipe) vkDestroyPipeline(U.dev, U.pipe, NULL);
        if (U.pass) vkDestroyRenderPass(U.dev, U.pass, NULL);
        if (U.layout) vkDestroyPipelineLayout(U.dev, U.layout, NULL);
        if (U.pool) vkDestroyDescriptorPool(U.dev, U.pool, NULL);
        if (U.dsl) vkDestroyDescriptorSetLayout(U.dev, U.dsl, NULL);
        if (U.sampler) vkDestroySampler(U.dev, U.sampler, NULL);
    }
    if (U.ctx) ImGui::DestroyContext(U.ctx);
    memset(&U, 0, sizeof(U));
}

extern "C" void vrp_ui_frame(const VrpUiIn *in, VrpUiOut *out)
{
    memset(out, 0, sizeof(*out));
    if (!U.ready) return;
    ImGui::SetCurrentContext(U.ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)in->width, (float)in->height);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    io.DeltaTime = in->dt > 0.0 && in->dt < 0.5 ? (float)in->dt : 1.0f / 72.0f;
    if (in->mouse_valid) io.AddMousePosEvent(in->mouse_x, in->mouse_y);
    else io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    io.AddMouseButtonEvent(0, in->mouse_down != 0);
    if (in->wheel != 0.0f) io.AddMouseWheelEvent(0.0f, in->wheel);
    ImGui::NewFrame();
    g_mark_cur ^= 1;
    g_nmarks[g_mark_cur] = 0;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("BLACK VR settings", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    /* header: the title, a red rule, Close */
    {
        ImGui::PushFont(NULL, kTitle);
        ImGui::TextColored(kText, "BLACK");
        ImGui::SameLine(0, 14);
        ImGui::TextColored(kRed, "VR");
        ImGui::SameLine(0, 14);
        ImGui::TextColored(kDim, "SETTINGS");
        ImGui::PopFont();
        const float bw = 170.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - bw - ImGui::GetStyle().WindowPadding.x);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);
        if (ImGui::Button("Close  B", ImVec2(bw, 0))) out->close = 1;
        mark("btn:Close");
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y + 3.0f),
                          ImGui::GetColorU32(kRed));
        ImGui::Dummy(ImVec2(1, 8));
    }
    const float footer = ImGui::GetFrameHeightWithSpacing() + 26.0f + ImGui::GetStyle().ItemSpacing.y * 2.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(24, 12));
    if (ImGui::BeginTabBar("tabs", ImGuiTabBarFlags_FittingPolicyShrink)) {
        static const char *const names[] = { "Graphics", "Comfort", "Gestures", "Info" };
        for (int t = 0; t < 4; t++) {
            const bool open = ImGui::BeginTabItem(names[t]);
            mark("tab:%s", names[t]);
            if (!open) continue;
            ImGui::PopStyleVar();
            ImGui::BeginChild("content", ImVec2(0, -footer), ImGuiChildFlags_None);
            if (t < 3) out->changed |= rows(in, t);
            else info_tab(in->info);
            if (t == VRS_TAB_GESTURES) {
                ImGui::Spacing();
                if (ImGui::Button("Recalibrate height", ImVec2(360, 0))) out->recalibrate = 1;
                mark("btn:Recalibrate");
                ImGui::SameLine();
                ImGui::PushFont(NULL, kSmall);
                ImGui::TextColored(kDim, "Stand straight, look ahead: sets the\nstanding height and recentres the view");
                ImGui::PopFont();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(24, 12));
        }
        ImGui::EndTabBar();
    }
    ImGui::PopStyleVar();
    /* footer */
    {
        const float bw = 200.0f;
        int dirty = 0;
        for (int id = 0; id < VRS_COUNT; id++) dirty |= vrs_differs(in->saved, in->edit, id);
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Button, dirty ? kRed : kFrame);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, dirty ? kRedHi : kFrameHi);
        if (ImGui::Button("Save", ImVec2(bw, 0))) out->save = 1;
        mark("btn:Save");
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(bw, 0))) out->cancel = 1;
        mark("btn:Cancel");
        ImGui::SameLine();
        if (ImGui::Button("Defaults", ImVec2(bw, 0))) out->reset = 1;
        mark("btn:Defaults");
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::PushFont(NULL, kSmall);
        if (in->status && *in->status) ImGui::TextColored(kDim, "%s", in->status);
        else if (dirty) ImGui::TextColored(kAmber, "Unsaved changes");
        else ImGui::TextColored(kDim, "Saved");
        ImGui::PopFont();
    }
    ImGui::End();

    /* the laser's cursor */
    if (in->mouse_valid) {
        ImDrawList *fg = ImGui::GetForegroundDrawList();
        const ImVec2 m(in->mouse_x, in->mouse_y);
        fg->AddCircleFilled(m, in->mouse_down ? 8.0f : 10.0f, IM_COL32(255, 255, 255, 235), 24);
        fg->AddCircle(m, 13.0f, ImGui::GetColorU32(kRed), 24, 3.0f);
    }
    ImGui::Render();
}

extern "C" int vrp_ui_record(VkCommandBuffer cmd, VkImageView view, uint32_t width, uint32_t height, int slot)
{
    if (!U.ready) return 0;
    ImGui::SetCurrentContext(U.ctx);
    ImDrawData *dd = ImGui::GetDrawData();
    if (!dd) return 0;
    slot &= 1;
    VkFramebuffer fb = framebuffer(view, width, height);
    if (!fb) return 0;
    textures(cmd, dd, slot);
    if (dd->TotalVtxCount > 0) {
        if (!buf_ensure(&U.vtx[slot], (VkDeviceSize)dd->TotalVtxCount * sizeof(ImDrawVert), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
            !buf_ensure(&U.idx[slot], (VkDeviceSize)dd->TotalIdxCount * sizeof(ImDrawIdx), VK_BUFFER_USAGE_INDEX_BUFFER_BIT))
            return 0;
        ImDrawVert *vd = (ImDrawVert *)U.vtx[slot].map;
        ImDrawIdx *id = (ImDrawIdx *)U.idx[slot].map;
        for (ImDrawList *l : dd->CmdLists) {
            memcpy(vd, l->VtxBuffer.Data, (size_t)l->VtxBuffer.Size * sizeof(ImDrawVert));
            memcpy(id, l->IdxBuffer.Data, (size_t)l->IdxBuffer.Size * sizeof(ImDrawIdx));
            vd += l->VtxBuffer.Size;
            id += l->IdxBuffer.Size;
        }
    }
    VkClearValue clear;
    memset(&clear, 0, sizeof(clear));
    VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rb.renderPass = U.pass;
    rb.framebuffer = fb;
    rb.renderArea.extent.width = width;
    rb.renderArea.extent.height = height;
    rb.clearValueCount = 1;
    rb.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    if (dd->TotalVtxCount > 0) {
        VkViewport vp = { 0, 0, (float)width, (float)height, 0, 1 };
        const float pc[4] = { 2.0f / dd->DisplaySize.x, 2.0f / dd->DisplaySize.y,
                              -1.0f - dd->DisplayPos.x * 2.0f / dd->DisplaySize.x, -1.0f - dd->DisplayPos.y * 2.0f / dd->DisplaySize.y };
        VkDeviceSize zero = 0;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, U.pipe);
        vkCmdBindVertexBuffers(cmd, 0, 1, &U.vtx[slot].buf, &zero);
        vkCmdBindIndexBuffer(cmd, U.idx[slot].buf, 0, sizeof(ImDrawIdx) == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdPushConstants(cmd, U.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), pc);
        int vo = 0, io = 0;
        VkDescriptorSet bound = VK_NULL_HANDLE;
        for (ImDrawList *l : dd->CmdLists) {
            for (int c = 0; c < l->CmdBuffer.Size; c++) {
                const ImDrawCmd *dc = &l->CmdBuffer[c];
                if (dc->UserCallback) continue;            /* none are used */
                float x0 = dc->ClipRect.x - dd->DisplayPos.x, y0 = dc->ClipRect.y - dd->DisplayPos.y;
                float x1 = dc->ClipRect.z - dd->DisplayPos.x, y1 = dc->ClipRect.w - dd->DisplayPos.y;
                if (x0 < 0) x0 = 0;
                if (y0 < 0) y0 = 0;
                if (x1 > (float)width) x1 = (float)width;
                if (y1 > (float)height) y1 = (float)height;
                if (x1 <= x0 || y1 <= y0) continue;
                VkRect2D sc;
                sc.offset.x = (int32_t)x0; sc.offset.y = (int32_t)y0;
                sc.extent.width = (uint32_t)(x1 - x0); sc.extent.height = (uint32_t)(y1 - y0);
                vkCmdSetScissor(cmd, 0, 1, &sc);
                VkDescriptorSet set = (VkDescriptorSet)(uintptr_t)dc->GetTexID();
                if (!set) continue;
                if (set != bound) {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, U.layout, 0, 1, &set, 0, NULL);
                    bound = set;
                }
                vkCmdDrawIndexed(cmd, dc->ElemCount, 1, dc->IdxOffset + (uint32_t)io, (int32_t)(dc->VtxOffset + (uint32_t)vo), 0);
            }
            vo += l->VtxBuffer.Size;
            io += l->IdxBuffer.Size;
        }
    }
    vkCmdEndRenderPass(cmd);
    return 1;
}

extern "C" int vrp_ui_test_rect(const char *name, float rect[4])
{
    for (int i = 0; i < g_nmarks[g_mark_cur]; i++)
        if (!strcmp(g_marks[g_mark_cur][i].name, name)) { memcpy(rect, g_marks[g_mark_cur][i].r, sizeof(float) * 4); return 1; }
    return 0;
}
