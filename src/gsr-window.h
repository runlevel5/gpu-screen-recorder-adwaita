#pragma once

#include <adwaita.h>
#include "gsr-config.h"

G_BEGIN_DECLS

#define GSR_TYPE_WINDOW (gsr_window_get_type())
G_DECLARE_FINAL_TYPE(GsrWindow, gsr_window, GSR, WINDOW, AdwApplicationWindow)

typedef enum {
    GSR_ACTIVE_MODE_NONE,
    GSR_ACTIVE_MODE_STREAM,
    GSR_ACTIVE_MODE_RECORD,
    GSR_ACTIVE_MODE_REPLAY,
} GsrActiveMode;

GsrWindow *gsr_window_new(AdwApplication *app);

void       gsr_window_set_recording_active(GsrWindow *self, gboolean active);

/* ── Process management (called from action pages) ───────────────── */

/**
 * Start gpu-screen-recorder for the given mode.
 * Returns TRUE on success, FALSE on fork failure.
 */
gboolean   gsr_window_start_process(GsrWindow    *self,
                                     GsrActiveMode mode);

/**
 * Stop the running gpu-screen-recorder process (SIGINT + waitpid).
 * Returns TRUE if the child exited successfully.
 * Sets *already_dead if the child was already gone.
 */
gboolean   gsr_window_stop_process (GsrWindow *self,
                                     gboolean  *already_dead);

/**
 * Send a signal to the running child process.
 * Used for SIGUSR1 (save replay) and SIGUSR2 (pause/unpause).
 */
void       gsr_window_send_signal  (GsrWindow *self, int sig);

/**
 * Notify replay saved (respects notify_saved config pref).
 */
void       gsr_window_notify_replay_saved(GsrWindow *self);

/**
 * Show a toast notification in the window.
 */
void       gsr_window_show_toast   (GsrWindow  *self,
                                     const char *message);

/**
 * Check if a process is currently running.
 */
gboolean   gsr_window_is_process_running(GsrWindow *self);

/**
 * Get the active mode.
 */
GsrActiveMode gsr_window_get_active_mode(GsrWindow *self);

/* ── Hotkey dispatch (called from gsr-hotkeys) ───────────────────── */

/**
 * Get the name of the currently visible page ("config", "stream",
 * "record", "replay"). Returns a borrowed string.
 */
const char *gsr_window_get_visible_page_name(GsrWindow *self);

/**
 * Hotkey: Start/Stop the active mode on the visible page.
 * Programmatically activates the start/stop button on the visible page.
 */
void       gsr_window_hotkey_start_stop   (GsrWindow *self);

/**
 * Hotkey: Pause/Unpause recording (record page only).
 */
void       gsr_window_hotkey_pause_unpause(GsrWindow *self);

/**
 * Hotkey: Save replay (replay page only).
 */
void       gsr_window_hotkey_save_replay  (GsrWindow *self);

/**
 * Called by gsr-hotkeys when Wayland global shortcuts init completes.
 * Updates hotkey UI on all action pages.
 */
#ifdef HAVE_WAYLAND
void       gsr_window_on_wayland_hotkeys_init(GsrWindow *self,
                                               gboolean   success);
#endif

/* ── Hotkey config changes (called from action pages) ────────────── */

/**
 * Called when the user changes a hotkey binding on an action page.
 * Saves config to disk and re-grabs X11 hotkeys.
 */
void       gsr_window_on_hotkey_changed(GsrWindow *self);

/**
 * Get a read-only pointer to the window's config struct.
 * Used by gsr-hotkeys to read dynamic hotkey bindings.
 */
const GsrConfig *gsr_window_get_config(GsrWindow *self);

G_END_DECLS
