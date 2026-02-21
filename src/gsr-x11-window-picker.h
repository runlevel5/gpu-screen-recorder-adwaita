#pragma once

/*
 * gsr-x11-window-picker.h — Interactive X11 window picker.
 *
 * Grabs the pointer with a crosshair cursor, waits for a click,
 * walks the X11 window tree to find the real toplevel, and reports
 * the result via callback.
 *
 * Uses its own X11 display connection + GLib GSource to avoid
 * interfering with GDK's event loop.
 *
 * X11 only — do NOT use on Wayland.
 */

#include <stdbool.h>
#include <X11/Xlib.h>

typedef struct _GsrX11WindowPicker GsrX11WindowPicker;

/**
 * Result of a window pick.  If window == None, the pick was cancelled
 * (Escape pressed or no valid window found).
 */
typedef struct {
    Window  window;     /* X11 window ID, or None on cancel */
    char   *name;       /* Window name (heap-allocated), or NULL on cancel.
                           Caller must g_free() / free(). */
} GsrX11WindowPickResult;

/**
 * Callback fired when the user has clicked a window or cancelled.
 * Called from the GLib main loop context.
 */
typedef void (*GsrX11WindowPickCallback)(const GsrX11WindowPickResult *result,
                                          void *userdata);

/**
 * Create a window picker.  Returns NULL on failure (e.g. cannot open
 * X display or grab pointer).
 *
 * The picker grabs the pointer immediately.  When the user clicks or
 * presses Escape, @callback is invoked and the picker is automatically
 * destroyed (do NOT call _free after the callback fires).
 *
 * @callback: result callback
 * @userdata: passed to callback
 */
GsrX11WindowPicker *gsr_x11_window_picker_new(GsrX11WindowPickCallback callback,
                                                void *userdata);

/**
 * Cancel and destroy an in-progress pick.
 * Safe to call if picker is NULL.
 */
void gsr_x11_window_picker_free(GsrX11WindowPicker *self);
