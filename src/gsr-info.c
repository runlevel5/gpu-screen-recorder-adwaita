#include "gsr-info.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

/* ── Helpers ─────────────────────────────────────────────────────── */

static char *
read_command_output(const char *cmd, int *exit_code)
{
    FILE *f = popen(cmd, "r");
    if (!f) {
        if (exit_code) *exit_code = -1;
        return NULL;
    }

    size_t capacity = 8192;
    size_t len = 0;
    char *buf = g_malloc(capacity);

    for (;;) {
        size_t n = fread(buf + len, 1, capacity - len - 1, f);
        if (n == 0) break;
        len += n;
        if (len + 1 >= capacity) {
            capacity *= 2;
            buf = g_realloc(buf, capacity);
        }
    }
    buf[len] = '\0';

    int status = pclose(f);
    if (exit_code) {
        if (WIFEXITED(status))
            *exit_code = WEXITSTATUS(status);
        else
            *exit_code = -1;
    }
    return buf;
}

typedef void (*LineCallback)(const char *line, size_t len, void *user_data);

static void
for_each_line(const char *text, LineCallback cb, void *user_data)
{
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0)
            cb(p, len, user_data);
        if (!nl) break;
        p = nl + 1;
    }
}

static bool
str_eq(const char *s, size_t len, const char *literal)
{
    size_t llen = strlen(literal);
    return len == llen && memcmp(s, literal, llen) == 0;
}

static bool
str_starts_with(const char *s, size_t len, const char *prefix)
{
    size_t plen = strlen(prefix);
    return len >= plen && memcmp(s, prefix, plen) == 0;
}

/* ── Section parsing ─────────────────────────────────────────────── */

typedef enum {
    SECTION_UNKNOWN,
    SECTION_SYSTEM_INFO,
    SECTION_GPU_INFO,
    SECTION_VIDEO_CODECS,
    SECTION_CAPTURE_OPTIONS,
} InfoSection;

typedef struct {
    GsrInfo     *info;
    InfoSection  section;
    /* temporary growable monitor array */
    GsrMonitor  *monitors;
    int          n_monitors;
    int          monitors_capacity;
} ParseState;

static void
parse_system_info(ParseState *st, const char *line, size_t len)
{
    const char *sep = memchr(line, '|', len);
    if (!sep) return;

    size_t klen = (size_t)(sep - line);
    const char *val = sep + 1;
    size_t vlen = len - klen - 1;

    if (str_eq(line, klen, "display_server")) {
        if (str_eq(val, vlen, "x11"))
            st->info->system_info.display_server = GSR_DISPLAY_SERVER_X11;
        else if (str_eq(val, vlen, "wayland"))
            st->info->system_info.display_server = GSR_DISPLAY_SERVER_WAYLAND;
    } else if (str_eq(line, klen, "is_steam_deck")) {
        st->info->system_info.is_steam_deck = str_eq(val, vlen, "yes");
    } else if (str_eq(line, klen, "supports_app_audio")) {
        st->info->system_info.supports_app_audio = str_eq(val, vlen, "yes");
    }
}

static void
parse_gpu_info(ParseState *st, const char *line, size_t len)
{
    const char *sep = memchr(line, '|', len);
    if (!sep) return;

    size_t klen = (size_t)(sep - line);
    const char *val = sep + 1;
    size_t vlen = len - klen - 1;

    if (str_eq(line, klen, "vendor")) {
        if (str_eq(val, vlen, "amd"))
            st->info->gpu_info.vendor = GSR_GPU_VENDOR_AMD;
        else if (str_eq(val, vlen, "intel"))
            st->info->gpu_info.vendor = GSR_GPU_VENDOR_INTEL;
        else if (str_eq(val, vlen, "nvidia"))
            st->info->gpu_info.vendor = GSR_GPU_VENDOR_NVIDIA;
        else if (str_eq(val, vlen, "broadcom"))
            st->info->gpu_info.vendor = GSR_GPU_VENDOR_BROADCOM;
    }
}

static void
parse_video_codecs(ParseState *st, const char *line, size_t len)
{
    GsrSupportedVideoCodecs *vc = &st->info->supported_video_codecs;

    if (str_eq(line, len, "h264"))               vc->h264 = true;
    else if (str_eq(line, len, "h264_software"))  vc->h264_software = true;
    else if (str_eq(line, len, "hevc"))           vc->hevc = true;
    else if (str_eq(line, len, "hevc_hdr"))       vc->hevc_hdr = true;
    else if (str_eq(line, len, "hevc_10bit"))     vc->hevc_10bit = true;
    else if (str_eq(line, len, "av1"))            vc->av1 = true;
    else if (str_eq(line, len, "av1_hdr"))        vc->av1_hdr = true;
    else if (str_eq(line, len, "av1_10bit"))      vc->av1_10bit = true;
    else if (str_eq(line, len, "vp8"))            vc->vp8 = true;
    else if (str_eq(line, len, "vp9"))            vc->vp9 = true;
}

static void
parse_capture_options(ParseState *st, const char *line, size_t len)
{
    if (str_eq(line, len, "window")) {
        st->info->supported_capture_options.window = true;
    } else if (str_eq(line, len, "focused")) {
        st->info->supported_capture_options.focused = true;
    } else if (str_eq(line, len, "portal")) {
        st->info->supported_capture_options.portal = true;
    } else if (str_eq(line, len, "region") || (len > 0 && line[0] == '/')) {
        /* skip "region" and DRM card paths */
    } else {
        /* monitor entry: name|WxH */
        if (st->n_monitors >= st->monitors_capacity) {
            st->monitors_capacity = st->monitors_capacity ? st->monitors_capacity * 2 : 8;
            st->monitors = g_realloc_n(st->monitors, (size_t)st->monitors_capacity,
                                       sizeof(GsrMonitor));
        }
        GsrMonitor *m = &st->monitors[st->n_monitors];
        memset(m, 0, sizeof(*m));

        const char *sep = memchr(line, '|', len);
        if (sep) {
            size_t name_len = (size_t)(sep - line);
            m->name = g_strndup(line, name_len);
            sscanf(sep + 1, "%dx%d", &m->width, &m->height);
        } else {
            m->name = g_strndup(line, len);
        }
        st->n_monitors++;
    }
}

static void
info_line_cb(const char *line, size_t len, void *user_data)
{
    ParseState *st = user_data;

    if (str_starts_with(line, len, "section=")) {
        const char *name = line + 8;
        size_t nlen = len - 8;
        if (str_eq(name, nlen, "system_info"))
            st->section = SECTION_SYSTEM_INFO;
        else if (str_eq(name, nlen, "gpu_info"))
            st->section = SECTION_GPU_INFO;
        else if (str_eq(name, nlen, "video_codecs"))
            st->section = SECTION_VIDEO_CODECS;
        else if (str_eq(name, nlen, "capture_options"))
            st->section = SECTION_CAPTURE_OPTIONS;
        else
            st->section = SECTION_UNKNOWN;
        return;
    }

    switch (st->section) {
    case SECTION_SYSTEM_INFO:     parse_system_info(st, line, len);     break;
    case SECTION_GPU_INFO:        parse_gpu_info(st, line, len);        break;
    case SECTION_VIDEO_CODECS:    parse_video_codecs(st, line, len);    break;
    case SECTION_CAPTURE_OPTIONS: parse_capture_options(st, line, len); break;
    case SECTION_UNKNOWN:         break;
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

GsrInfoExitStatus
gsr_info_load(GsrInfo *info)
{
    memset(info, 0, sizeof(*info));

    int exit_code = -1;
    char *output = read_command_output("gpu-screen-recorder --info", &exit_code);
    if (!output) {
        fprintf(stderr, "error: 'gpu-screen-recorder --info' failed to run\n");
        return GSR_INFO_EXIT_FAILED_TO_RUN;
    }

    ParseState st = {
        .info = info,
        .section = SECTION_UNKNOWN,
    };
    for_each_line(output, info_line_cb, &st);
    g_free(output);

    /* Transfer monitors */
    info->supported_capture_options.monitors = st.monitors;
    info->supported_capture_options.n_monitors = st.n_monitors;

    switch (exit_code) {
    case 0:  return GSR_INFO_EXIT_OK;
    case 22: return GSR_INFO_EXIT_OPENGL_FAILED;
    case 23: return GSR_INFO_EXIT_NO_DRM_CARD;
    default: return GSR_INFO_EXIT_FAILED_TO_RUN;
    }
}

void
gsr_info_clear(GsrInfo *info)
{
    for (int i = 0; i < info->supported_capture_options.n_monitors; i++)
        g_free(info->supported_capture_options.monitors[i].name);
    g_free(info->supported_capture_options.monitors);
    memset(info, 0, sizeof(*info));
}

bool
gsr_info_is_codec_supported(const GsrInfo *info, const char *codec_id)
{
    if (strcmp(codec_id, "auto") == 0)        return true;

    const GsrSupportedVideoCodecs *vc = &info->supported_video_codecs;
    if (strcmp(codec_id, "h264") == 0)           return vc->h264;
    if (strcmp(codec_id, "h264_software") == 0)  return vc->h264_software;
    if (strcmp(codec_id, "hevc") == 0)           return vc->hevc;
    if (strcmp(codec_id, "hevc_hdr") == 0)       return vc->hevc_hdr;
    if (strcmp(codec_id, "hevc_10bit") == 0)     return vc->hevc_10bit;
    if (strcmp(codec_id, "av1") == 0)            return vc->av1;
    if (strcmp(codec_id, "av1_hdr") == 0)        return vc->av1_hdr;
    if (strcmp(codec_id, "av1_10bit") == 0)      return vc->av1_10bit;
    if (strcmp(codec_id, "vp8") == 0)            return vc->vp8;
    if (strcmp(codec_id, "vp9") == 0)            return vc->vp9;
    return false;
}

bool
gsr_info_is_capture_option_enabled(const GsrInfo *info, const char *option_id)
{
    if (info->system_info.display_server == GSR_DISPLAY_SERVER_WAYLAND) {
        if (strcmp(option_id, "window") == 0 || strcmp(option_id, "focused") == 0)
            return false;
    }
    if (strcmp(option_id, "portal") == 0)
        return info->supported_capture_options.portal;
    return true;
}

const char *
gsr_info_get_first_usable_hw_video_codec(const GsrInfo *info)
{
    const GsrSupportedVideoCodecs *vc = &info->supported_video_codecs;
    if (vc->h264) return "h264";
    if (vc->hevc) return "hevc";
    if (vc->av1)  return "av1";
    if (vc->vp8)  return "vp8";
    if (vc->vp9)  return "vp9";
    return NULL;
}

/* ── Audio device queries ────────────────────────────────────────── */

GsrAudioDevice *
gsr_audio_devices_get(int *n_devices)
{
    *n_devices = 0;

    int exit_code = -1;
    char *output = read_command_output("gpu-screen-recorder --list-audio-devices", &exit_code);
    if (!output) return NULL;

    /* Count lines */
    int capacity = 16;
    GsrAudioDevice *devs = g_new0(GsrAudioDevice, capacity);
    int count = 0;

    const char *p = output;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0) {
            const char *sep = memchr(p, '|', len);
            if (sep) {
                if (count >= capacity) {
                    capacity *= 2;
                    devs = g_realloc_n(devs, (size_t)capacity, sizeof(GsrAudioDevice));
                }
                devs[count].name = g_strndup(p, (size_t)(sep - p));
                devs[count].description = g_strndup(sep + 1, len - (size_t)(sep - p) - 1);
                count++;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }

    g_free(output);
    *n_devices = count;
    return devs;
}

void
gsr_audio_devices_free(GsrAudioDevice *devices, int n_devices)
{
    for (int i = 0; i < n_devices; i++) {
        g_free(devices[i].name);
        g_free(devices[i].description);
    }
    g_free(devices);
}

char **
gsr_application_audio_get(int *n_apps)
{
    *n_apps = 0;

    int exit_code = -1;
    char *output = read_command_output("gpu-screen-recorder --list-application-audio", &exit_code);
    if (!output) return NULL;

    int capacity = 16;
    char **apps = g_new0(char *, capacity + 1);
    int count = 0;

    const char *p = output;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0) {
            if (count >= capacity) {
                capacity *= 2;
                apps = g_realloc_n(apps, (size_t)(capacity + 1), sizeof(char *));
            }
            apps[count] = g_strndup(p, len);
            count++;
        }
        if (!nl) break;
        p = nl + 1;
    }
    apps[count] = NULL;

    g_free(output);
    *n_apps = count;
    return apps;
}

void
gsr_application_audio_free(char **apps, int n_apps)
{
    for (int i = 0; i < n_apps; i++)
        g_free(apps[i]);
    g_free(apps);
}
