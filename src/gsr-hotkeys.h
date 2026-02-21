#pragma once

/*
 * gsr-hotkeys.h — Unified hotkey manager for X11 and Wayland.
 *
 * Detects the display server at runtime and uses either:
 *   - X11: XGrabKey + GSource polling (gsr-x11-hotkeys)
 *   - Wayland: XDG GlobalShortcuts portal (global_shortcuts)
 *
 * Dispatches hotkey actions to the appropriate page based on the
 * currently visible tab (page-scoped dispatch).
 */

#include <stdbool.h>
#include "gsr-info.h"

/* Forward declare — the hotkeys manager calls back into the window */
typedef struct _GsrWindow GsrWindow;

typedef struct _GsrHotkeys GsrHotkeys;

/**
 * Create a hotkey manager for the given display server type.
 * The window pointer is used for page-scoped dispatch.
 * Returns NULL on failure.
 */
GsrHotkeys *gsr_hotkeys_new(GsrDisplayServer display_server,
                              GsrWindow       *window);

/**
 * Destroy the hotkey manager and release all resources.
 */
void gsr_hotkeys_free(GsrHotkeys *self);

/**
 * Re-grab hotkeys for the visible page (X11 only).
 * On Wayland this is a no-op since the portal handles page dispatch
 * in the deactivated callback.
 *
 * Call this whenever the visible tab changes.
 */
#ifdef HAVE_X11
void gsr_hotkeys_regrab_for_visible_page(GsrHotkeys *self);
#endif

/**
 * On Wayland (GNOME), bind shortcuts if not yet done.
 * Should be called once when first navigating to an action page.
 */
#ifdef HAVE_WAYLAND
void gsr_hotkeys_register_wayland_shortcuts_once(GsrHotkeys *self);
#endif
