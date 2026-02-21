#include "gsr-config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>
#include <pwd.h>
#include <sys/stat.h>

#include <gtk/gtk.h>

/*
 * X keysym constants for the custom modifier bitmask encoding.
 * These are just integer values used for config file serialization —
 * they do NOT require X11 headers or libraries at runtime.
 */
#define XK_Shift_L    0xFFE1
#define XK_Shift_R    0xFFE2
#define XK_Control_L  0xFFE3
#define XK_Control_R  0xFFE4
#define XK_Meta_L     0xFFE7
#define XK_Meta_R     0xFFE8
#define XK_Alt_L      0xFFE9
#define XK_Alt_R      0xFFEA
#define XK_Super_L    0xFFEB
#define XK_Super_R    0xFFEC

#ifdef HAVE_X11
#include <X11/Xlib.h>   /* ControlMask, ShiftMask, Mod1Mask, Mod4Mask */
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  gsr-config.c — Config file read/write
 *
 *  File format: one key-value pair per line, space-separated.
 *  STRING_ARRAY keys appear multiple times (one per element).
 *  Backward-compatible with the C++ GTK3 version.
 * ═══════════════════════════════════════════════════════════════════ */

/* ── Helpers ─────────────────────────────────────────────────────── */

static char *
get_home_dir(void)
{
    const char *home = g_get_home_dir();
    if (home)
        return g_strdup(home);
    return g_strdup("/tmp");
}

char *
gsr_config_get_dir(void)
{
    const char *xdg = g_getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0])
        return g_build_filename(xdg, "gpu-screen-recorder", NULL);

    char *home = get_home_dir();
    char *result = g_build_filename(home, ".config", "gpu-screen-recorder", NULL);
    g_free(home);
    return result;
}

char *
gsr_config_get_videos_dir(void)
{
    const char *vdir = g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS);
    if (vdir)
        return g_strdup(vdir);
    char *home = get_home_dir();
    char *result = g_build_filename(home, "Videos", NULL);
    g_free(home);
    return result;
}

static int
create_directory_recursive(char *path)
{
    int path_len = (int)strlen(path);
    char *p = path;
    char *end = path + path_len;

    for (;;) {
        char *slash_p = strchr(p, '/');

        /* Skip leading '/' — don't try to mkdir root */
        if (slash_p == path) {
            ++p;
            continue;
        }

        if (!slash_p)
            slash_p = end;

        char prev = *slash_p;
        *slash_p = '\0';
        int err = mkdir(path, S_IRWXU);
        *slash_p = prev;

        if (err == -1 && errno != EEXIST)
            return err;

        if (slash_p == end)
            break;
        else
            p = slash_p + 1;
    }
    return 0;
}

/* ── Config value type dispatch ──────────────────────────────────── */

typedef enum {
    CFG_BOOL,
    CFG_STRING,
    CFG_I32,
    CFG_HOTKEY,
    CFG_STRING_ARRAY,
} CfgType;

typedef struct {
    const char *key;
    CfgType     type;
    size_t      offset;        /* offset into GsrConfig */
    size_t      count_offset;  /* for STRING_ARRAY: offset of n_audio_input */
} CfgEntry;

/* Helper macro to compute offset from a nested config member */
#define CFG_OFF(section, member)  offsetof(GsrConfig, section.member)

static const CfgEntry config_entries[] = {
    /* ── main ── */
    { "main.record_area_option",                  CFG_STRING,       CFG_OFF(main_config, record_area_option),       0 },
    { "main.record_area_width",                   CFG_I32,          CFG_OFF(main_config, record_area_width),        0 },
    { "main.record_area_height",                  CFG_I32,          CFG_OFF(main_config, record_area_height),       0 },
    { "main.video_width",                         CFG_I32,          CFG_OFF(main_config, video_width),              0 },
    { "main.video_height",                        CFG_I32,          CFG_OFF(main_config, video_height),             0 },
    { "main.fps",                                 CFG_I32,          CFG_OFF(main_config, fps),                      0 },
    { "main.video_bitrate",                       CFG_I32,          CFG_OFF(main_config, video_bitrate),            0 },
    { "main.merge_audio_tracks",                  CFG_BOOL,         CFG_OFF(main_config, merge_audio_tracks),       0 },
    { "main.record_app_audio_inverted",           CFG_BOOL,         CFG_OFF(main_config, record_app_audio_inverted),0 },
    { "main.change_video_resolution",             CFG_BOOL,         CFG_OFF(main_config, change_video_resolution),  0 },
    { "main.audio_input",                         CFG_STRING_ARRAY, CFG_OFF(main_config, audio_input),
                                                                    CFG_OFF(main_config, n_audio_input) },
    { "main.color_range",                         CFG_STRING,       CFG_OFF(main_config, color_range),              0 },
    { "main.quality",                             CFG_STRING,       CFG_OFF(main_config, quality),                  0 },
    { "main.codec",                               CFG_STRING,       CFG_OFF(main_config, codec),                    0 },
    { "main.audio_codec",                         CFG_STRING,       CFG_OFF(main_config, audio_codec),              0 },
    { "main.framerate_mode",                      CFG_STRING,       CFG_OFF(main_config, framerate_mode),           0 },
    { "main.advanced_view",                       CFG_BOOL,         CFG_OFF(main_config, advanced_view),            0 },
    { "main.overclock",                           CFG_BOOL,         CFG_OFF(main_config, overclock),                0 },
    { "main.show_recording_started_notifications",CFG_BOOL,         CFG_OFF(main_config, show_recording_started_notifications), 0 },
    { "main.show_recording_stopped_notifications",CFG_BOOL,         CFG_OFF(main_config, show_recording_stopped_notifications), 0 },
    { "main.show_recording_saved_notifications",  CFG_BOOL,         CFG_OFF(main_config, show_recording_saved_notifications),   0 },
    { "main.record_cursor",                       CFG_BOOL,         CFG_OFF(main_config, record_cursor),            0 },
    { "main.hide_window_when_recording",          CFG_BOOL,         CFG_OFF(main_config, hide_window_when_recording),0 },
    { "main.software_encoding_warning_shown",     CFG_BOOL,         CFG_OFF(main_config, software_encoding_warning_shown),0 },
    { "main.steam_deck_warning_shown",            CFG_BOOL,         CFG_OFF(main_config, steam_deck_warning_shown), 0 },
    { "main.hevc_amd_bug_warning_shown",          CFG_BOOL,         CFG_OFF(main_config, hevc_amd_bug_warning_shown),0 },
    { "main.av1_amd_bug_warning_shown",           CFG_BOOL,         CFG_OFF(main_config, av1_amd_bug_warning_shown),0 },
    { "main.restore_portal_session",              CFG_BOOL,         CFG_OFF(main_config, restore_portal_session),   0 },
    { "main.use_new_ui",                          CFG_BOOL,         CFG_OFF(main_config, use_new_ui),               0 },
    { "main.installed_gsr_global_hotkeys_version",CFG_I32,          CFG_OFF(main_config, installed_gsr_global_hotkeys_version),0 },

    /* ── streaming ── */
    { "streaming.service",                        CFG_STRING,       CFG_OFF(streaming_config, streaming_service),    0 },
    { "streaming.youtube.key",                    CFG_STRING,       CFG_OFF(streaming_config, youtube_stream_key),   0 },
    { "streaming.twitch.key",                     CFG_STRING,       CFG_OFF(streaming_config, twitch_stream_key),    0 },
    { "streaming.custom.url",                     CFG_STRING,       CFG_OFF(streaming_config, custom_url),           0 },
    { "streaming.custom.container",               CFG_STRING,       CFG_OFF(streaming_config, custom_container),     0 },
    { "streaming.start_stop_recording_hotkey",    CFG_HOTKEY,       CFG_OFF(streaming_config, start_stop_hotkey),    0 },

    /* ── record ── */
    { "record.save_directory",                    CFG_STRING,       CFG_OFF(record_config, save_directory),          0 },
    { "record.container",                         CFG_STRING,       CFG_OFF(record_config, container),               0 },
    { "record.start_stop_recording_hotkey",       CFG_HOTKEY,       CFG_OFF(record_config, start_stop_hotkey),       0 },
    { "record.pause_unpause_recording_hotkey",    CFG_HOTKEY,       CFG_OFF(record_config, pause_unpause_hotkey),    0 },

    /* ── replay ── */
    { "replay.save_directory",                    CFG_STRING,       CFG_OFF(replay_config, save_directory),          0 },
    { "replay.container",                         CFG_STRING,       CFG_OFF(replay_config, container),               0 },
    { "replay.time",                              CFG_I32,          CFG_OFF(replay_config, replay_time),             0 },
    { "replay.start_stop_recording_hotkey",       CFG_HOTKEY,       CFG_OFF(replay_config, start_stop_hotkey),       0 },
    { "replay.save_recording_hotkey",             CFG_HOTKEY,       CFG_OFF(replay_config, save_hotkey),             0 },
};

#define N_CONFIG_ENTRIES ((int)(sizeof(config_entries) / sizeof(config_entries[0])))

static const CfgEntry *
find_entry(const char *key, int key_len)
{
    for (int i = 0; i < N_CONFIG_ENTRIES; i++) {
        if ((int)strlen(config_entries[i].key) == key_len &&
            memcmp(config_entries[i].key, key, (size_t)key_len) == 0)
            return &config_entries[i];
    }
    return NULL;
}

/* ── Default initialization ──────────────────────────────────────── */

void
gsr_config_init_defaults(GsrConfig *config)
{
    memset(config, 0, sizeof(*config));

    GsrMainConfig *m = &config->main_config;
    m->record_area_option = g_strdup("");
    m->record_area_width = 0;
    m->record_area_height = 0;
    m->video_width = 0;
    m->video_height = 0;
    m->fps = 60;
    m->video_bitrate = 15000;
    m->merge_audio_tracks = true;
    m->record_app_audio_inverted = false;
    m->change_video_resolution = false;
    m->audio_input = NULL;
    m->n_audio_input = 0;
    m->color_range = g_strdup("limited");
    m->quality = g_strdup("very_high");
    m->codec = g_strdup("auto");
    m->audio_codec = g_strdup("opus");
    m->framerate_mode = g_strdup("auto");
    m->advanced_view = false;
    m->overclock = false;
    m->show_recording_started_notifications = false;
    m->show_recording_stopped_notifications = false;
    m->show_recording_saved_notifications = true;
    m->record_cursor = true;
    m->hide_window_when_recording = false;
    m->restore_portal_session = true;
    m->software_encoding_warning_shown = false;
    m->steam_deck_warning_shown = false;
    m->hevc_amd_bug_warning_shown = false;
    m->av1_amd_bug_warning_shown = false;
    m->use_new_ui = false;
    m->installed_gsr_global_hotkeys_version = 0;

    /* Default hotkeys: Alt+1 = start/stop, Alt+2 = pause/save
     * Custom bitmask: Alt_L = 1 << (XK_Alt_L - XK_Shift_L) = 1 << 8 = 256
     * XK_1 = 0x31 = 49, XK_2 = 0x32 = 50 */
    #define DEFAULT_HOTKEY_START_STOP  ((GsrConfigHotkey){ .keysym = 49, .modifiers = 256 })
    #define DEFAULT_HOTKEY_SECONDARY   ((GsrConfigHotkey){ .keysym = 50, .modifiers = 256 })

    GsrStreamingConfig *s = &config->streaming_config;
    s->streaming_service = g_strdup("twitch");
    s->youtube_stream_key = g_strdup("");
    s->twitch_stream_key = g_strdup("");
    s->custom_url = g_strdup("");
    s->custom_container = g_strdup("flv");
    s->start_stop_hotkey = DEFAULT_HOTKEY_START_STOP;

    GsrRecordConfig *r = &config->record_config;
    r->save_directory = gsr_config_get_videos_dir();
    r->container = g_strdup("mp4");
    r->start_stop_hotkey = DEFAULT_HOTKEY_START_STOP;
    r->pause_unpause_hotkey = DEFAULT_HOTKEY_SECONDARY;

    GsrReplayConfig *rp = &config->replay_config;
    rp->save_directory = gsr_config_get_videos_dir();
    rp->container = g_strdup("mp4");
    rp->replay_time = 30;
    rp->start_stop_hotkey = DEFAULT_HOTKEY_START_STOP;
    rp->save_hotkey = DEFAULT_HOTKEY_SECONDARY;

    #undef DEFAULT_HOTKEY_START_STOP
    #undef DEFAULT_HOTKEY_SECONDARY
}

/* ── Read ────────────────────────────────────────────────────────── */

gboolean
gsr_config_read(GsrConfig *config)
{
    char *config_dir = gsr_config_get_dir();
    char *config_path = g_build_filename(config_dir, "config", NULL);
    g_free(config_dir);

    gchar *contents = NULL;
    gsize length = 0;
    if (!g_file_get_contents(config_path, &contents, &length, NULL)) {
        g_free(config_path);
        return FALSE;
    }
    g_free(config_path);

    /* Parse line by line */
    char *p = contents;
    char *end = contents + length;

    while (p < end) {
        /* Find end of line */
        char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl)
            nl = end;

        int line_len = (int)(nl - p);

        /* Find space separator between key and value */
        char *sp = memchr(p, ' ', (size_t)line_len);
        if (sp && sp > p) {
            int key_len = (int)(sp - p);
            const char *val = sp + 1;
            int val_len = (int)(nl - val);

            if (key_len > 0 && val_len > 0) {
                const CfgEntry *entry = find_entry(p, key_len);
                if (entry) {
                    char *base = (char *)config;
                    switch (entry->type) {
                    case CFG_BOOL: {
                        bool *ptr = (bool *)(base + entry->offset);
                        *ptr = (val_len == 4 && memcmp(val, "true", 4) == 0);
                        break;
                    }
                    case CFG_STRING: {
                        char **ptr = (char **)(void *)(base + entry->offset);
                        g_free(*ptr);
                        *ptr = g_strndup(val, (gsize)val_len);
                        break;
                    }
                    case CFG_I32: {
                        int32_t *ptr = (int32_t *)(void *)(base + entry->offset);
                        char tmp[32];
                        int copy_len = val_len < 31 ? val_len : 31;
                        memcpy(tmp, val, (size_t)copy_len);
                        tmp[copy_len] = '\0';
                        if (sscanf(tmp, "%" PRIi32, ptr) != 1) {
                            *ptr = 0;
                        }
                        break;
                    }
                    case CFG_HOTKEY: {
                        GsrConfigHotkey *hk = (GsrConfigHotkey *)(void *)(base + entry->offset);
                        char tmp[64];
                        int copy_len = val_len < 63 ? val_len : 63;
                        memcpy(tmp, val, (size_t)copy_len);
                        tmp[copy_len] = '\0';
                        if (sscanf(tmp, "%" PRIi64 " %" PRIu32,
                                   &hk->keysym, &hk->modifiers) != 2) {
                            hk->keysym = 0;
                            hk->modifiers = 0;
                        }
                        break;
                    }
                    case CFG_STRING_ARRAY: {
                        char ***arr_ptr = (char ***)(void *)(base + entry->offset);
                        int *count_ptr = (int *)(void *)(base + entry->count_offset);
                        int n = *count_ptr;
                        *arr_ptr = g_realloc(*arr_ptr, sizeof(char *) * (gsize)(n + 2));
                        (*arr_ptr)[n] = g_strndup(val, (gsize)val_len);
                        (*arr_ptr)[n + 1] = NULL;
                        *count_ptr = n + 1;
                        break;
                    }
                    }
                }
            }
        }

        p = nl + 1;
    }

    g_free(contents);
    return TRUE;
}

/* ── Save ────────────────────────────────────────────────────────── */

void
gsr_config_save(const GsrConfig *config)
{
    char *config_dir = gsr_config_get_dir();
    char *config_path = g_build_filename(config_dir, "config", NULL);

    /* Ensure directory exists */
    if (create_directory_recursive(config_dir) != 0) {
        g_warning("Failed to create config directory: %s", config_dir);
        g_free(config_dir);
        g_free(config_path);
        return;
    }
    g_free(config_dir);

    FILE *file = fopen(config_path, "wb");
    if (!file) {
        g_warning("Failed to create config file: %s", config_path);
        g_free(config_path);
        return;
    }
    g_free(config_path);

    const char *base = (const char *)config;

    for (int i = 0; i < N_CONFIG_ENTRIES; i++) {
        const CfgEntry *e = &config_entries[i];
        switch (e->type) {
        case CFG_BOOL: {
            const bool *ptr = (const bool *)(base + e->offset);
            fprintf(file, "%s %s\n", e->key, *ptr ? "true" : "false");
            break;
        }
        case CFG_STRING: {
            const char *const *ptr = (const char *const *)(const void *)(base + e->offset);
            fprintf(file, "%s %s\n", e->key, *ptr ? *ptr : "");
            break;
        }
        case CFG_I32: {
            const int32_t *ptr = (const int32_t *)(const void *)(base + e->offset);
            fprintf(file, "%s %" PRIi32 "\n", e->key, *ptr);
            break;
        }
        case CFG_HOTKEY: {
            const GsrConfigHotkey *hk = (const GsrConfigHotkey *)(const void *)(base + e->offset);
            fprintf(file, "%s %" PRIi64 " %" PRIu32 "\n", e->key,
                    hk->keysym, hk->modifiers);
            break;
        }
        case CFG_STRING_ARRAY: {
            char *const *const *arr_ptr = (char *const *const *)(const void *)(base + e->offset);
            const int *count_ptr = (const int *)(const void *)(base + e->count_offset);
            int n = *count_ptr;
            char *const *arr = *arr_ptr;
            for (int j = 0; j < n && arr && arr[j]; j++) {
                fprintf(file, "%s %s\n", e->key, arr[j]);
            }
            break;
        }
        }
    }

    fclose(file);
}

/* ── Clear ───────────────────────────────────────────────────────── */

void
gsr_config_clear(GsrConfig *config)
{
    GsrMainConfig *m = &config->main_config;
    g_free(m->record_area_option);
    g_free(m->color_range);
    g_free(m->quality);
    g_free(m->codec);
    g_free(m->audio_codec);
    g_free(m->framerate_mode);

    if (m->audio_input) {
        for (int i = 0; i < m->n_audio_input; i++)
            g_free(m->audio_input[i]);
        g_free(m->audio_input);
    }

    GsrStreamingConfig *s = &config->streaming_config;
    g_free(s->streaming_service);
    g_free(s->youtube_stream_key);
    g_free(s->twitch_stream_key);
    g_free(s->custom_url);
    g_free(s->custom_container);

    g_free(config->record_config.save_directory);
    g_free(config->record_config.container);

    g_free(config->replay_config.save_directory);
    g_free(config->replay_config.container);

    memset(config, 0, sizeof(*config));
}

/* ── Hotkey conversion utilities ─────────────────────────────────── */

/*
 * The GTK3 code uses a custom bitmask for modifier keys:
 *   modkey_to_mask(keysym) = 1 << (keysym - XK_Shift_L)
 *
 * The relevant X keysyms (from <X11/keysym.h>):
 *   XK_Shift_L   = 0xFFE1 → bit 0
 *   XK_Shift_R   = 0xFFE2 → bit 1
 *   XK_Control_L = 0xFFE3 → bit 2
 *   XK_Control_R = 0xFFE4 → bit 3
 *   XK_Caps_Lock = 0xFFE5 → bit 4 (unused)
 *   XK_Shift_Lock= 0xFFE6 → bit 5 (unused)
 *   XK_Meta_L    = 0xFFE7 → bit 6
 *   XK_Meta_R    = 0xFFE8 → bit 7
 *   XK_Alt_L     = 0xFFE9 → bit 8
 *   XK_Alt_R     = 0xFFEA → bit 9
 *   XK_Super_L   = 0xFFEB → bit 10
 *   XK_Super_R   = 0xFFEC → bit 11
 */

#define CUSTOM_MASK(ks) (1u << ((ks) - XK_Shift_L))

#define MASK_SHIFT   (CUSTOM_MASK(XK_Shift_L)   | CUSTOM_MASK(XK_Shift_R))
#define MASK_CONTROL (CUSTOM_MASK(XK_Control_L)  | CUSTOM_MASK(XK_Control_R))
#define MASK_ALT     (CUSTOM_MASK(XK_Alt_L)      | CUSTOM_MASK(XK_Alt_R))
#define MASK_META    (CUSTOM_MASK(XK_Meta_L)     | CUSTOM_MASK(XK_Meta_R))
#define MASK_SUPER   (CUSTOM_MASK(XK_Super_L)    | CUSTOM_MASK(XK_Super_R))

char *
gsr_config_hotkey_to_accel(const GsrConfigHotkey *hk)
{
    if (!hk || hk->keysym == 0)
        return NULL;

    /* Convert custom modifier bitmask to GdkModifierType */
    GdkModifierType mods = 0;
    uint32_t m = hk->modifiers;

    if (m & MASK_SHIFT)   mods |= GDK_SHIFT_MASK;
    if (m & MASK_CONTROL) mods |= GDK_CONTROL_MASK;
    if (m & MASK_ALT)     mods |= GDK_ALT_MASK;
    if (m & (MASK_META | MASK_SUPER))
        mods |= GDK_SUPER_MASK;

    return gtk_accelerator_name((guint)hk->keysym, mods);
}

gboolean
gsr_config_hotkey_from_accel(GsrConfigHotkey *hk, const char *accel)
{
    if (!hk)
        return FALSE;

    if (!accel || !accel[0]) {
        hk->keysym = 0;
        hk->modifiers = 0;
        return TRUE;
    }

    guint keyval = 0;
    GdkModifierType mods = 0;

    if (!gtk_accelerator_parse(accel, &keyval, &mods))
        return FALSE;

    if (keyval == 0) {
        hk->keysym = 0;
        hk->modifiers = 0;
        return TRUE;
    }

    hk->keysym = (int64_t)keyval;

    /* Convert GdkModifierType back to custom bitmask.
     * We use the _L variant for each modifier pair. */
    uint32_t m = 0;
    if (mods & GDK_SHIFT_MASK)   m |= CUSTOM_MASK(XK_Shift_L);
    if (mods & GDK_CONTROL_MASK) m |= CUSTOM_MASK(XK_Control_L);
    if (mods & GDK_ALT_MASK)     m |= CUSTOM_MASK(XK_Alt_L);
    if (mods & GDK_SUPER_MASK)   m |= CUSTOM_MASK(XK_Super_L);
    hk->modifiers = m;

    return TRUE;
}

#ifdef HAVE_X11
void
gsr_config_hotkey_to_x11(const GsrConfigHotkey *hk,
                          unsigned int *out_x11_modifiers,
                          uint64_t     *out_keysym)
{
    if (!hk) {
        if (out_x11_modifiers) *out_x11_modifiers = 0;
        if (out_keysym) *out_keysym = 0;
        return;
    }

    if (out_keysym)
        *out_keysym = (uint64_t)hk->keysym;

    if (!out_x11_modifiers)
        return;

    /* Convert custom modifier bitmask to X11 mask.
     * This matches key_mod_mask_to_x11_mask() from the GTK3 code. */
    uint32_t m = hk->modifiers;
    unsigned int x11 = 0;

    if (m & MASK_CONTROL) x11 |= ControlMask;
    if (m & MASK_ALT)     x11 |= Mod1Mask;
    if (m & MASK_SHIFT)   x11 |= ShiftMask;
    if (m & (MASK_META | MASK_SUPER)) x11 |= Mod4Mask;

    *out_x11_modifiers = x11;
}
#endif /* HAVE_X11 */
