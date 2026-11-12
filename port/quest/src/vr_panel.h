/*
 * vr_panel.h - the settings panel's Dear ImGui side (vr_panel_ui.cpp), called by xr_panel.c on the XR thread only:
 * the UI for one frame (settings model: vr_settings.h) and its Vulkan drawing into a panel swapchain image.
 * C interface; the implementation is C++ (Dear ImGui, third_party/imgui).
 */
#ifndef VR_PANEL_H
#define VR_PANEL_H

#include <stdint.h>
#include <vulkan/vulkan.h>
#include "vr_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VRP_MAX_THREADS 8

typedef struct {
    char name[20];
    int tid;
    float pct;                     /* % of one core over the last sample */
} VrpThreadUse;

/* The Info tab (filled by xr_panel.c about once a second while the panel is open) */
typedef struct {
    float game_fps;                /* frames the renderer released (eye images + flat), per second */
    float xr_fps;                  /* XR loop frames per second */
    float display_hz;              /* the display's refresh rate now */
    int nthreads;
    VrpThreadUse threads[VRP_MAX_THREADS];
    uint32_t eye_w, eye_h;         /* the eye swapchain */
    float res_scale;               /* the resolution scale it was made with */
    int multiview;
    char gpu[400];                 /* device name, driver */
    char gpu_name[256];
    char runtime[200];             /* OpenXR runtime name, version */
    char build[96];
    char ini_path[600];
    int foveation_applied;         /* VR_FOVEATION_* the eye swapchain has, 0 none */
    const char *mode;              /* "menu" / "gameplay" ... */
} VrpInfo;

typedef struct {
    uint32_t width, height;        /* panel pixels */
    float mouse_x, mouse_y;        /* pixels; mouse_valid 0: the laser is off the panel */
    int mouse_valid, mouse_down;
    float wheel;
    double dt;
    VrShellConfig *edit;           /* the panel's working copy (the UI changes it) */
    const VrShellConfig *boot;     /* as the process started (restart settings) */
    const VrShellConfig *saved;    /* as last saved / loaded (for "unsaved changes") */
    const VrsCaps *caps;
    const VrpInfo *info;
    const char *status;            /* the bottom line: "Saved to ..." / an error; NULL: none */
} VrpUiIn;

typedef struct {
    int changed;                   /* edit changed this frame */
    int save, cancel, reset, close, recalibrate;
} VrpUiOut;

typedef struct {
    VkPhysicalDevice phys;
    VkDevice device;
    VkFormat format;               /* the colour attachment (view) format of the panel swapchain: UNORM */
} VrpVkInit;

int  vrp_ui_init(const VrpVkInit *ci);     /* 1 ok */
void vrp_ui_shutdown(void);
/* Runs one UI frame (ImGui NewFrame .. Render) */
void vrp_ui_frame(const VrpUiIn *in, VrpUiOut *out);
/* Records the frame's texture uploads and draws into cmd, rendering into `view` (an image of the panel swapchain,
 * COLOR_ATTACHMENT_OPTIMAL before and after). slot: the frame-in-flight index (0/1) whose fence the caller waited.
 * Returns 1 when something was recorded. */
int  vrp_ui_record(VkCommandBuffer cmd, VkImageView view, uint32_t width, uint32_t height, int slot);
/* tests: where a control was drawn in the last frame, panel pixels (x0 y0 x1 y1). Names: "tab:Comfort",
 * "seg:<ini key>:<option index>" (enums and on/off), "plus:<key>" / "minus:<key>" (sliders), "btn:Save" / "btn:Cancel"
 * / "btn:Defaults" / "btn:Close" / "btn:Recalibrate". Returns 0 when it was not drawn. */
int  vrp_ui_test_rect(const char *name, float rect[4]);

#ifdef __cplusplus
}
#endif

#endif /* VR_PANEL_H */
