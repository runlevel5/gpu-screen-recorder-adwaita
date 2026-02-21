#pragma once

/*
 * gsr-x11-hotkeys.h — X11 global hotkey grabbing for GTK4.
 *
 * GTK4 removed gdk_window_add_filter(), so we poll the X connection fd
 * via a GLib GSource to receive KeyRelease events for grabbed combos.
 */

#include <stdbool.h>
#include <X11/Xlib.h>

typedef void (*GsrX11HotkeyCallback)(unsigned int modifiers, KeySym keysym, void *userdata);

typedef struct {
    unsigned int modifiers;   /* X11 modifier mask (ControlMask, Mod1Mask, etc.) */
    KeySym       keysym;      /* e.g. XK_1, XK_2 */
} GsrX11HotkeyCombo;

typedef struct _GsrX11Hotkeys GsrX11Hotkeys;

/**
 * Create a new X11 hotkey watcher on the given display.
 * The callback fires on KeyRelease matching any grabbed combo.
 * Returns NULL on failure.
 */
GsrX11Hotkeys *gsr_x11_hotkeys_new(Display *display,
                                    GsrX11HotkeyCallback callback,
                                    void *userdata);

/**
 * Destroy the hotkey watcher and ungrab all keys.
 */
void gsr_x11_hotkeys_free(GsrX11Hotkeys *self);

/**
 * Ungrab all currently grabbed keys.
 */
void gsr_x11_hotkeys_ungrab_all(GsrX11Hotkeys *self);

/**
 * Grab a key combo. The combo is added to the internal list and
 * grabbed with NumLock/CapsLock variants.
 * Returns true on success, false if XGrabKey failed.
 */
bool gsr_x11_hotkeys_grab(GsrX11Hotkeys *self, GsrX11HotkeyCombo combo);
