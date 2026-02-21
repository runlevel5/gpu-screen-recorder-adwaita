#pragma once

#include <adwaita.h>
#include "gsr-info.h"
#include "gsr-config.h"

G_BEGIN_DECLS

#define GSR_TYPE_RECORD_PAGE (gsr_record_page_get_type())
G_DECLARE_FINAL_TYPE(GsrRecordPage, gsr_record_page, GSR, RECORD_PAGE, AdwPreferencesPage)

GsrRecordPage *gsr_record_page_new          (const GsrInfo *info);
void           gsr_record_page_apply_config  (GsrRecordPage   *self,
                                              const GsrConfig *config);
void           gsr_record_page_read_config   (GsrRecordPage *self,
                                              GsrConfig     *config);

/* Process management API (Phase 5) */
void           gsr_record_page_set_active    (GsrRecordPage *self,
                                              gboolean       active);
void           gsr_record_page_set_paused    (GsrRecordPage *self,
                                              gboolean       paused);
void           gsr_record_page_update_timer  (GsrRecordPage *self,
                                              const char    *text);

/* Get the save directory. Borrowed pointer, do NOT free. */
const char    *gsr_record_page_get_save_dir  (GsrRecordPage *self);

/* Get the container ID for -c argument. Caller must g_free(). */
char          *gsr_record_page_get_container (GsrRecordPage *self);

/* Hotkey: programmatically toggle start/stop. */
void           gsr_record_page_activate_start_stop(GsrRecordPage *self);

/* Hotkey: programmatically toggle pause/unpause. */
void           gsr_record_page_activate_pause(GsrRecordPage *self);

/* Wayland: show/hide hotkey-not-supported banner. */
#ifdef HAVE_WAYLAND
void           gsr_record_page_set_wayland_hotkeys_supported(
                   GsrRecordPage *self, gboolean supported);
#endif

G_END_DECLS
