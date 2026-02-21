#pragma once

#include <adwaita.h>
#include "gsr-info.h"
#include "gsr-config.h"

G_BEGIN_DECLS

#define GSR_TYPE_STREAM_PAGE (gsr_stream_page_get_type())
G_DECLARE_FINAL_TYPE(GsrStreamPage, gsr_stream_page, GSR, STREAM_PAGE, AdwPreferencesPage)

GsrStreamPage *gsr_stream_page_new          (const GsrInfo *info);
void           gsr_stream_page_apply_config  (GsrStreamPage   *self,
                                              const GsrConfig *config);
void           gsr_stream_page_read_config   (GsrStreamPage *self,
                                              GsrConfig     *config);

/* Process management API (Phase 5) */
void           gsr_stream_page_set_active    (GsrStreamPage *self,
                                              gboolean       active);
void           gsr_stream_page_update_timer  (GsrStreamPage *self,
                                              const char    *text);

/* Get the stream URL for -o argument. Caller must g_free(). */
char          *gsr_stream_page_get_stream_url(GsrStreamPage *self);

/* Get the container ID for -c argument. Caller must g_free(). */
char          *gsr_stream_page_get_container (GsrStreamPage *self);

/* Hotkey: programmatically toggle start/stop. */
void           gsr_stream_page_activate_start_stop(GsrStreamPage *self);

/* Wayland: show/hide hotkey-not-supported banner. */
#ifdef HAVE_WAYLAND
void           gsr_stream_page_set_wayland_hotkeys_supported(
                   GsrStreamPage *self, gboolean supported);
#endif

G_END_DECLS
