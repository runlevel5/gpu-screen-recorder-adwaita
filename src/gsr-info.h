#pragma once

#include <glib.h>
#include <stdbool.h>

G_BEGIN_DECLS

/* ── Enums ───────────────────────────────────────────────────────── */

typedef enum {
    GSR_DISPLAY_SERVER_UNKNOWN,
    GSR_DISPLAY_SERVER_X11,
    GSR_DISPLAY_SERVER_WAYLAND,
} GsrDisplayServer;

typedef enum {
    GSR_GPU_VENDOR_UNKNOWN,
    GSR_GPU_VENDOR_AMD,
    GSR_GPU_VENDOR_INTEL,
    GSR_GPU_VENDOR_NVIDIA,
    GSR_GPU_VENDOR_BROADCOM,
} GsrGpuVendor;

typedef enum {
    GSR_INFO_EXIT_OK,
    GSR_INFO_EXIT_FAILED_TO_RUN,
    GSR_INFO_EXIT_OPENGL_FAILED,
    GSR_INFO_EXIT_NO_DRM_CARD,
} GsrInfoExitStatus;

/* ── Data structures ─────────────────────────────────────────────── */

typedef struct {
    GsrDisplayServer display_server;
    bool             supports_app_audio;
    bool             is_steam_deck;
} GsrSystemInfo;

typedef struct {
    GsrGpuVendor vendor;
} GsrGpuInfo;

typedef struct {
    bool h264;
    bool h264_software;
    bool hevc;
    bool hevc_hdr;
    bool hevc_10bit;
    bool av1;
    bool av1_hdr;
    bool av1_10bit;
    bool vp8;
    bool vp9;
} GsrSupportedVideoCodecs;

typedef struct {
    char *name;       /* owned */
    int   width;
    int   height;
} GsrMonitor;

typedef struct {
    bool         window;
    bool         focused;
    bool         portal;
    GsrMonitor  *monitors;    /* owned array */
    int          n_monitors;
} GsrSupportedCaptureOptions;

typedef struct {
    GsrSystemInfo               system_info;
    GsrGpuInfo                  gpu_info;
    GsrSupportedVideoCodecs     supported_video_codecs;
    GsrSupportedCaptureOptions  supported_capture_options;
} GsrInfo;

/* ── Audio devices ───────────────────────────────────────────────── */

typedef struct {
    char *name;         /* PulseAudio/PipeWire ID, owned */
    char *description;  /* human-readable label, owned */
} GsrAudioDevice;

/* ── Functions ───────────────────────────────────────────────────── */

GsrInfoExitStatus  gsr_info_load          (GsrInfo *info);
void               gsr_info_clear         (GsrInfo *info);

bool               gsr_info_is_codec_supported
                                           (const GsrInfo *info,
                                            const char    *codec_id);

bool               gsr_info_is_capture_option_enabled
                                           (const GsrInfo *info,
                                            const char    *option_id);

/**
 * Returns the first usable HW video codec name (h264 > hevc > av1 > vp8 > vp9),
 * or NULL if none supported.
 */
const char        *gsr_info_get_first_usable_hw_video_codec(const GsrInfo *info);

/* Audio device / application queries (separate commands) */
GsrAudioDevice    *gsr_audio_devices_get  (int *n_devices);
void               gsr_audio_devices_free (GsrAudioDevice *devices,
                                           int              n_devices);

char             **gsr_application_audio_get (int *n_apps);
void               gsr_application_audio_free(char **apps, int n_apps);

G_END_DECLS
