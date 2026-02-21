#pragma once

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

G_BEGIN_DECLS

/* ── Hotkey ──────────────────────────────────────────────────────── */

typedef struct {
    int64_t  keysym;
    uint32_t modifiers;
} GsrConfigHotkey;

/* ── Config struct ───────────────────────────────────────────────── */

typedef struct {
    /* Capture target */
    char    *record_area_option;   /* "window", "focused", "portal", or monitor name */
    int32_t  record_area_width;
    int32_t  record_area_height;
    int32_t  video_width;
    int32_t  video_height;

    /* Video */
    int32_t  fps;
    int32_t  video_bitrate;
    char    *color_range;          /* "limited", "full" */
    char    *quality;              /* "custom", "medium", "high", "very_high", "ultra" */
    char    *codec;                /* "auto", "h264", "hevc", etc. */
    char    *audio_codec;          /* "opus", "aac" */
    char    *framerate_mode;       /* "auto", "cfr", "vfr" */
    bool     overclock;
    bool     record_cursor;

    /* Audio */
    char   **audio_input;          /* NULL-terminated array, each "device:xxx" / "app:xxx" */
    int      n_audio_input;
    bool     merge_audio_tracks;
    bool     record_app_audio_inverted;

    /* Resolution */
    bool     change_video_resolution;

    /* Notifications */
    bool     show_recording_started_notifications;
    bool     show_recording_stopped_notifications;
    bool     show_recording_saved_notifications;

    /* UI state */
    bool     advanced_view;
    bool     hide_window_when_recording;
    bool     restore_portal_session;

    /* Warning flags (persisted, not shown in UI) */
    bool     software_encoding_warning_shown;
    bool     steam_deck_warning_shown;
    bool     hevc_amd_bug_warning_shown;
    bool     av1_amd_bug_warning_shown;

    /* Misc */
    bool     use_new_ui;
    int32_t  installed_gsr_global_hotkeys_version;
} GsrMainConfig;

typedef struct {
    char *streaming_service;   /* "twitch", "youtube", "custom" */
    char *youtube_stream_key;
    char *twitch_stream_key;
    char *custom_url;
    char *custom_container;    /* "mp4", "flv", "matroska", etc. */

    GsrConfigHotkey start_stop_hotkey;
} GsrStreamingConfig;

typedef struct {
    char *save_directory;
    char *container;

    GsrConfigHotkey start_stop_hotkey;
    GsrConfigHotkey pause_unpause_hotkey;
} GsrRecordConfig;

typedef struct {
    char    *save_directory;
    char    *container;
    int32_t  replay_time;

    GsrConfigHotkey start_stop_hotkey;
    GsrConfigHotkey save_hotkey;
} GsrReplayConfig;

typedef struct {
    GsrMainConfig      main_config;
    GsrStreamingConfig streaming_config;
    GsrRecordConfig    record_config;
    GsrReplayConfig    replay_config;
} GsrConfig;

/* ── API ─────────────────────────────────────────────────────────── */

/**
 * Initialize config to default values.
 * Must be called before gsr_config_read() or manual population.
 */
void gsr_config_init_defaults(GsrConfig *config);

/**
 * Read config from the standard file location.
 * Returns TRUE on success, FALSE if file not found or error.
 * On failure, config retains its default values.
 */
gboolean gsr_config_read(GsrConfig *config);

/**
 * Save config to the standard file location.
 * Creates config directory if needed.
 */
void gsr_config_save(const GsrConfig *config);

/**
 * Free all heap-allocated members (strings, arrays).
 * Does NOT free the GsrConfig struct itself.
 */
void gsr_config_clear(GsrConfig *config);

/**
 * Get the config directory path (e.g., ~/.config/gpu-screen-recorder).
 * Returns a newly allocated string; caller must g_free().
 */
char *gsr_config_get_dir(void);

/**
 * Get the default videos directory.
 * Returns a newly allocated string; caller must g_free().
 */
char *gsr_config_get_videos_dir(void);

/* ── Hotkey conversion utilities ─────────────────────────────────── */

/**
 * Convert a GsrConfigHotkey (custom modifier bitmask + keysym) to a
 * GTK accelerator string like "<Alt>1".
 * Returns a newly allocated string, or NULL if the hotkey is empty.
 */
char *gsr_config_hotkey_to_accel(const GsrConfigHotkey *hk);

/**
 * Convert a GTK accelerator string like "<Alt>1" to a GsrConfigHotkey.
 * Returns TRUE on success, FALSE if the accel string is invalid.
 * If accel is NULL, the hotkey is cleared (keysym=0, modifiers=0).
 */
gboolean gsr_config_hotkey_from_accel(GsrConfigHotkey *hk, const char *accel);

#ifdef HAVE_X11
/**
 * Convert a GsrConfigHotkey to an X11 modifier mask suitable for XGrabKey.
 * The keysym is returned unchanged in *out_keysym.
 */
void gsr_config_hotkey_to_x11(const GsrConfigHotkey *hk,
                               unsigned int *out_x11_modifiers,
                               uint64_t     *out_keysym);
#endif /* HAVE_X11 */

/**
 * Check if a hotkey is empty (no key assigned).
 */
static inline gboolean
gsr_config_hotkey_is_empty(const GsrConfigHotkey *hk)
{
    return hk->keysym == 0;
}

G_END_DECLS
