#pragma once

#include <adwaita.h>
#include "gsr-info.h"
#include "gsr-config.h"

G_BEGIN_DECLS

#define GSR_TYPE_CONFIG_PAGE (gsr_config_page_get_type())
G_DECLARE_FINAL_TYPE(GsrConfigPage, gsr_config_page, GSR, CONFIG_PAGE, AdwPreferencesPage)

GsrConfigPage *gsr_config_page_new          (const GsrInfo *info);
void           gsr_config_page_set_advanced  (GsrConfigPage *self,
                                              gboolean       advanced);
void           gsr_config_page_apply_config  (GsrConfigPage *self,
                                              const GsrConfig *config);
void           gsr_config_page_read_config   (GsrConfigPage *self,
                                              GsrConfig     *config);

/* ── Command-line helpers (Phase 5) ──────────────────────────────── */

/**
 * Get the record area ID currently selected.
 * Borrowed pointer, do NOT free.
 */
const char    *gsr_config_page_get_record_area_id  (GsrConfigPage *self);

/**
 * Get video codec ID. Borrowed pointer, do NOT free.
 */
const char    *gsr_config_page_get_video_codec_id  (GsrConfigPage *self);

/**
 * Build audio "-a" arguments into a GPtrArray of strings.
 * Each string is one "-a" value (e.g. "device:xxx" or merged pipe-delimited).
 * merge_tracks: if TRUE, merge all tracks into one pipe-delimited string.
 * Caller must free the array with g_ptr_array_unref() (strings owned by array).
 */
GPtrArray     *gsr_config_page_build_audio_args    (GsrConfigPage *self,
                                                    gboolean       merge_tracks);

/**
 * Check if app_audio_inverted is enabled.
 */
gboolean       gsr_config_page_get_app_audio_inverted(GsrConfigPage *self);

/* ── Scalar getters for command-line building ──────────────────── */

int            gsr_config_page_get_fps                (GsrConfigPage *self);

/**
 * Get quality ID: "custom", "medium", "high", "very_high", "ultra".
 */
const char    *gsr_config_page_get_quality_id         (GsrConfigPage *self);

int            gsr_config_page_get_video_bitrate      (GsrConfigPage *self);

/**
 * Get color range: "limited" or "full".
 */
const char    *gsr_config_page_get_color_range_id     (GsrConfigPage *self);

/**
 * Get audio codec: "opus" or "aac".
 */
const char    *gsr_config_page_get_audio_codec_id     (GsrConfigPage *self);

/**
 * Get framerate mode: "auto", "cfr", or "vfr".
 */
const char    *gsr_config_page_get_framerate_mode_id  (GsrConfigPage *self);

gboolean       gsr_config_page_get_record_cursor      (GsrConfigPage *self);
gboolean       gsr_config_page_get_overclock           (GsrConfigPage *self);
gboolean       gsr_config_page_get_restore_portal_session(GsrConfigPage *self);
gboolean       gsr_config_page_get_change_video_resolution(GsrConfigPage *self);
int            gsr_config_page_get_video_width         (GsrConfigPage *self);
int            gsr_config_page_get_video_height        (GsrConfigPage *self);
int            gsr_config_page_get_area_width          (GsrConfigPage *self);
int            gsr_config_page_get_area_height         (GsrConfigPage *self);
gboolean       gsr_config_page_get_split_audio         (GsrConfigPage *self);
gboolean       gsr_config_page_get_notify_started      (GsrConfigPage *self);
gboolean       gsr_config_page_get_notify_stopped      (GsrConfigPage *self);
gboolean       gsr_config_page_get_notify_saved        (GsrConfigPage *self);

/* ── Window picker ──────────────────────────────────────────────── */

/**
 * Get the X11 window ID selected via the "Select window" picker.
 * Returns 0 if no window has been selected.
 */
unsigned long  gsr_config_page_get_selected_window     (GsrConfigPage *self);

/**
 * Check if we have a valid window selection.
 * Returns TRUE if not in "window" mode, or if a window has been picked.
 * Returns FALSE if in "window" mode but no window selected yet.
 */
gboolean       gsr_config_page_has_valid_window_selection(GsrConfigPage *self);

G_END_DECLS
