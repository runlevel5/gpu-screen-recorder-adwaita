#include "gsr-hotkeys.h"
#include "gsr-window.h"
#include "gsr-config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * On X11 we use gsr-x11-hotkeys.h for XGrabKey + GSource polling.
 * On Wayland we use global_shortcuts.h for the D-Bus portal.
 *
 * Each backend is conditionally compiled via HAVE_X11 / HAVE_WAYLAND.
 */

#ifdef HAVE_X11
#include "gsr-x11-hotkeys.h"
#include <X11/keysym.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#endif
#endif /* HAVE_X11 */

#ifdef HAVE_WAYLAND
#include "global_shortcuts.h"
#endif

/* ── Shortcut IDs (Wayland portal) ────────────────────────────────── */

#ifdef HAVE_WAYLAND
#define SHORTCUT_ID_START_STOP       "gpu_screen_recorder_start_stop_recording"
#define SHORTCUT_ID_PAUSE_UNPAUSE    "gpu_screen_recorder_pause_unpause_recording"
#define SHORTCUT_ID_SAVE_REPLAY      "gpu_screen_recorder_save_replay"
#endif

/* ── Internal state ──────────────────────────────────────────────── */

typedef enum {
    HOTKEY_ACTION_START_STOP,
    HOTKEY_ACTION_PAUSE_UNPAUSE,
    HOTKEY_ACTION_SAVE_REPLAY,
} HotkeyAction;

#ifdef HAVE_X11
typedef struct {
    unsigned int x11_modifiers;
    KeySym       keysym;
    HotkeyAction action;
} GrabbedCombo;

#define MAX_ACTIVE_COMBOS 4
#endif /* HAVE_X11 */

struct _GsrHotkeys {
    GsrDisplayServer  display_server;
    GsrWindow        *window;          /* NOT owned */

#ifdef HAVE_X11
    /* X11 backend */
    GsrX11Hotkeys    *x11;

    /* Currently grabbed combos and what action they map to */
    GrabbedCombo      active_combos[MAX_ACTIVE_COMBOS];
    int               n_active_combos;
#endif /* HAVE_X11 */

#ifdef HAVE_WAYLAND
    /* Wayland backend */
    gsr_global_shortcuts  wayland;
    bool                  wayland_initialized;
    bool                  wayland_shortcuts_bound;
    bool                  wayland_shortcuts_received;
#endif /* HAVE_WAYLAND */
};

/* ── Forward declarations ────────────────────────────────────────── */

#ifdef HAVE_X11
static void on_x11_hotkey(unsigned int modifiers, KeySym keysym, void *userdata);
#endif

#ifdef HAVE_WAYLAND
static void on_wayland_init(bool success, void *userdata);
static void on_wayland_deactivated(const char *shortcut_id, void *userdata);
static void on_wayland_shortcut_changed(gsr_shortcut shortcut, void *userdata);
#endif

/* ── Hotkey action dispatch (shared by both backends) ────────────── */

/**
 * Get the name of the currently visible page in the AdwViewStack.
 * Returns a borrowed string like "stream", "record", "replay", "config".
 */
static const char *
get_visible_page_name(GsrHotkeys *self)
{
    return gsr_window_get_visible_page_name(self->window);
}

static void
dispatch_start_stop(GsrHotkeys *self)
{
    gsr_window_hotkey_start_stop(self->window);
}

static void
dispatch_pause_unpause(GsrHotkeys *self)
{
    gsr_window_hotkey_pause_unpause(self->window);
}

static void
dispatch_save_replay(GsrHotkeys *self)
{
    gsr_window_hotkey_save_replay(self->window);
}

/* ── X11 callback ────────────────────────────────────────────────── */

#ifdef HAVE_X11
static void
on_x11_hotkey(unsigned int modifiers, KeySym keysym, void *userdata)
{
    GsrHotkeys *self = userdata;

    /* Find which action this combo maps to */
    HotkeyAction action = HOTKEY_ACTION_START_STOP;
    bool found = false;

    for (int i = 0; i < self->n_active_combos; i++) {
        if (self->active_combos[i].keysym == keysym &&
            self->active_combos[i].x11_modifiers == modifiers)
        {
            action = self->active_combos[i].action;
            found = true;
            break;
        }
    }

    if (!found) {
        /* Fallback: match by keysym only (modifier might have extra bits
         * from NumLock/CapsLock that XGrabKey variants handle) */
        unsigned int base_mods = modifiers & (ControlMask | ShiftMask | Mod1Mask | Mod4Mask);
        for (int i = 0; i < self->n_active_combos; i++) {
            if (self->active_combos[i].keysym == keysym &&
                self->active_combos[i].x11_modifiers == base_mods)
            {
                action = self->active_combos[i].action;
                found = true;
                break;
            }
        }
    }

    if (!found)
        return;

    switch (action) {
    case HOTKEY_ACTION_START_STOP:
        dispatch_start_stop(self);
        break;
    case HOTKEY_ACTION_PAUSE_UNPAUSE:
        dispatch_pause_unpause(self);
        break;
    case HOTKEY_ACTION_SAVE_REPLAY:
        dispatch_save_replay(self);
        break;
    }
}
#endif /* HAVE_X11 */

/* ── Wayland callbacks ───────────────────────────────────────────── */

#ifdef HAVE_WAYLAND
static void
on_wayland_init(bool success, void *userdata)
{
    GsrHotkeys *self = userdata;
    self->wayland_initialized = success;
    if (success) {
        g_debug("Wayland global shortcuts session created");
        gsr_global_shortcuts_subscribe_activated_signal(
            &self->wayland, on_wayland_deactivated,
            on_wayland_shortcut_changed, self);
    } else {
        fprintf(stderr, "gsr warning: Wayland global shortcuts init failed\n");
    }

    /* Notify the window so it can update the UI */
    gsr_window_on_wayland_hotkeys_init(self->window, success);
}

static void
on_wayland_deactivated(const char *shortcut_id, void *userdata)
{
    GsrHotkeys *self = userdata;

    const char *page = get_visible_page_name(self);
    if (!page)
        return;

    /* The portal uses 3 shared IDs across all modes.
     * Dispatch based on the currently visible page. */

    if (strcmp(shortcut_id, SHORTCUT_ID_START_STOP) == 0) {
        if (g_str_equal(page, "stream") ||
            g_str_equal(page, "record") ||
            g_str_equal(page, "replay"))
        {
            dispatch_start_stop(self);
        }
    } else if (strcmp(shortcut_id, SHORTCUT_ID_PAUSE_UNPAUSE) == 0) {
        if (g_str_equal(page, "record"))
            dispatch_pause_unpause(self);
    } else if (strcmp(shortcut_id, SHORTCUT_ID_SAVE_REPLAY) == 0) {
        if (g_str_equal(page, "replay"))
            dispatch_save_replay(self);
    }
}

static void
on_wayland_shortcut_changed(gsr_shortcut shortcut, void *userdata)
{
    GsrHotkeys *self = userdata;
    self->wayland_shortcuts_received = true;
    g_debug("Wayland shortcut changed: id=%s trigger=%s",
            shortcut.id, shortcut.trigger_description);
}
#endif /* HAVE_WAYLAND */

/* ── Public API ──────────────────────────────────────────────────── */

GsrHotkeys *
gsr_hotkeys_new(GsrDisplayServer display_server, GsrWindow *window)
{
    GsrHotkeys *self = calloc(1, sizeof(GsrHotkeys));
    if (!self)
        return NULL;

    self->display_server = display_server;
    self->window = window;

#ifdef HAVE_X11
    if (display_server == GSR_DISPLAY_SERVER_X11) {
        /*
         * Get the X11 Display from GDK.
         * We use gdk_x11_display_get_xdisplay() from <gdk/x11/gdkx.h>.
         */
        GdkDisplay *gdk_dpy = gdk_display_get_default();
        if (!gdk_dpy) {
            fprintf(stderr, "gsr warning: no default GDK display\n");
            free(self);
            return NULL;
        }

        /* GTK4 X11 backend */
        Display *xdisplay = NULL;

        /* Use the GDK X11 function if available */
#ifdef GDK_WINDOWING_X11
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        xdisplay = gdk_x11_display_get_xdisplay(gdk_dpy);
        G_GNUC_END_IGNORE_DEPRECATIONS
#endif

        if (!xdisplay) {
            fprintf(stderr, "gsr warning: could not get X11 display from GDK\n");
            free(self);
            return NULL;
        }

        self->x11 = gsr_x11_hotkeys_new(xdisplay, on_x11_hotkey, self);
        if (!self->x11) {
            fprintf(stderr, "gsr warning: failed to create X11 hotkey watcher\n");
            free(self);
            return NULL;
        }

        g_debug("X11 hotkey manager initialized");

    }
#endif /* HAVE_X11 */

#ifdef HAVE_WAYLAND
    if (display_server == GSR_DISPLAY_SERVER_WAYLAND) {
        /* TODO: Skip Hyprland due to portal crash bug */
        if (!gsr_global_shortcuts_init(&self->wayland, on_wayland_init, self)) {
            fprintf(stderr, "gsr warning: failed to initialize Wayland global shortcuts\n");
            /* Non-fatal: hotkeys just won't work */
        }
    }
#endif /* HAVE_WAYLAND */

    return self;
}

void
gsr_hotkeys_free(GsrHotkeys *self)
{
    if (!self)
        return;

#ifdef HAVE_X11
    if (self->x11) {
        gsr_x11_hotkeys_free(self->x11);
        self->x11 = NULL;
    }
#endif

#ifdef HAVE_WAYLAND
    if (self->wayland_initialized) {
        gsr_global_shortcuts_deinit(&self->wayland);
        self->wayland_initialized = false;
    }
#endif

    free(self);
}

/* ── Helper: grab a hotkey from config and track its action ──────── */

#ifdef HAVE_X11
static void
grab_hotkey_from_config(GsrHotkeys *self,
                        const GsrConfigHotkey *hk,
                        HotkeyAction action)
{
    if (gsr_config_hotkey_is_empty(hk))
        return;

    unsigned int x11_mods = 0;
    uint64_t keysym = 0;
    gsr_config_hotkey_to_x11(hk, &x11_mods, &keysym);

    if (keysym == 0)
        return;

    GsrX11HotkeyCombo combo = { .modifiers = x11_mods, .keysym = (KeySym)keysym };
    if (!gsr_x11_hotkeys_grab(self->x11, combo)) {
        g_warning("Failed to grab hotkey (keysym=0x%lx, mods=0x%x)",
                  (unsigned long)keysym, x11_mods);
        return;
    }

    if (self->n_active_combos < MAX_ACTIVE_COMBOS) {
        self->active_combos[self->n_active_combos] = (GrabbedCombo){
            .x11_modifiers = x11_mods,
            .keysym = (KeySym)keysym,
            .action = action,
        };
        self->n_active_combos++;
    }
}

void
gsr_hotkeys_regrab_for_visible_page(GsrHotkeys *self)
{
    if (!self || self->display_server != GSR_DISPLAY_SERVER_X11 || !self->x11)
        return;

    /* Ungrab everything first */
    gsr_x11_hotkeys_ungrab_all(self->x11);
    self->n_active_combos = 0;

    const char *page = get_visible_page_name(self);
    if (!page)
        return;

    const GsrConfig *config = gsr_window_get_config(self->window);
    if (!config)
        return;

    /* Only grab hotkeys for action pages (not config) */
    if (g_str_equal(page, "stream")) {
        grab_hotkey_from_config(self,
            &config->streaming_config.start_stop_hotkey,
            HOTKEY_ACTION_START_STOP);

    } else if (g_str_equal(page, "record")) {
        grab_hotkey_from_config(self,
            &config->record_config.start_stop_hotkey,
            HOTKEY_ACTION_START_STOP);
        grab_hotkey_from_config(self,
            &config->record_config.pause_unpause_hotkey,
            HOTKEY_ACTION_PAUSE_UNPAUSE);

    } else if (g_str_equal(page, "replay")) {
        grab_hotkey_from_config(self,
            &config->replay_config.start_stop_hotkey,
            HOTKEY_ACTION_START_STOP);
        grab_hotkey_from_config(self,
            &config->replay_config.save_hotkey,
            HOTKEY_ACTION_SAVE_REPLAY);
    }
}
#endif /* HAVE_X11 */

#ifdef HAVE_WAYLAND
void
gsr_hotkeys_register_wayland_shortcuts_once(GsrHotkeys *self)
{
    if (!self || self->display_server != GSR_DISPLAY_SERVER_WAYLAND)
        return;

    if (!self->wayland_initialized)
        return;

    /* On GNOME: bind once. On KDE: the user clicks a button (deferred to Phase 7). */
    if (self->wayland_shortcuts_bound || self->wayland_shortcuts_received)
        return;

    self->wayland_shortcuts_bound = true;

    const gsr_bind_shortcut shortcuts[3] = {
        {
            .description = "Start/stop recording/replay/streaming",
            .shortcut = { .id = SHORTCUT_ID_START_STOP, .trigger_description = "ALT+1" },
        },
        {
            .description = "Pause/unpause recording",
            .shortcut = { .id = SHORTCUT_ID_PAUSE_UNPAUSE, .trigger_description = "ALT+2" },
        },
        {
            .description = "Save replay",
            .shortcut = { .id = SHORTCUT_ID_SAVE_REPLAY, .trigger_description = "ALT+3" },
        },
    };

    if (!gsr_global_shortcuts_bind_shortcuts(&self->wayland, shortcuts, 3,
                                              on_wayland_shortcut_changed, self))
    {
        fprintf(stderr, "gsr warning: failed to bind Wayland shortcuts\n");
    }
}
#endif /* HAVE_WAYLAND */
