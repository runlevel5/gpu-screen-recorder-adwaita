#pragma once

#include <adwaita.h>
#include "gsr-info.h"
#include "gsr-config.h"

G_BEGIN_DECLS

#define GSR_TYPE_REPLAY_PAGE (gsr_replay_page_get_type())
G_DECLARE_FINAL_TYPE(GsrReplayPage, gsr_replay_page, GSR, REPLAY_PAGE, AdwPreferencesPage)

GsrReplayPage *gsr_replay_page_new          (const GsrInfo *info);
void           gsr_replay_page_apply_config  (GsrReplayPage   *self,
                                              const GsrConfig *config);
void           gsr_replay_page_read_config   (GsrReplayPage *self,
                                              GsrConfig     *config);

/* Process management API */
void           gsr_replay_page_set_active    (GsrReplayPage *self,
                                              gboolean       active);
void           gsr_replay_page_update_timer  (GsrReplayPage *self,
                                              const char    *text);

/* Get the save directory. Borrowed pointer, do NOT free. */
const char    *gsr_replay_page_get_save_dir  (GsrReplayPage *self);

/* Get the container ID for -c argument. Caller must g_free(). */
char          *gsr_replay_page_get_container (GsrReplayPage *self);

/* Get the replay time in seconds. */
int            gsr_replay_page_get_time      (GsrReplayPage *self);

/* Hotkey: programmatically toggle start/stop. */
void           gsr_replay_page_activate_start_stop(GsrReplayPage *self);

/* Hotkey: programmatically save replay. */
void           gsr_replay_page_activate_save(GsrReplayPage *self);

/* Wayland: show/hide hotkey-not-supported banner. */
#ifdef HAVE_WAYLAND
void           gsr_replay_page_set_wayland_hotkeys_supported(
                   GsrReplayPage *self, gboolean supported);
#endif

G_END_DECLS
