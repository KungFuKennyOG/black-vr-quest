/*
 * vr_settings_menu.h - the shell's VR settings state, shared by the native in-menu page (vr_bridge.h vr_menu_*) and
 * the floating panel (xr_panel.c). See vr_settings_menu.c.
 */
#ifndef VR_SETTINGS_MENU_H
#define VR_SETTINGS_MENU_H

#include "vr_settings.h"
#include "vr_panel.h"

typedef struct {
    VrShellConfig boot;            /* as the process started (restart settings) */
    VrShellConfig saved;           /* the stored values the edits started from (startup, then each Save) */
    VrShellConfig edit;            /* the working copy: live settings are copied from it into the running config */
    VrsCaps caps;
    VrpInfo info;
    char status[240];
    double status_t;
} VrSettingsState;

/* all on the state: take the (recursive) lock */
void vrsm_lock(void);
void vrsm_unlock(void);
VrSettingsState *vrsm(void);
void vrsm_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  vrsm_dirty(void);
#endif /* VR_SETTINGS_MENU_H */
