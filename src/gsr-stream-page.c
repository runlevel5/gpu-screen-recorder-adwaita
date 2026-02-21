#include "gsr-stream-page.h"
#ifdef HAVE_X11
#include "gsr-shortcut-accel-dialog.h"
#endif
#include "gsr-window.h"
#include <time.h>

/* ═══════════════════════════════════════════════════════════════════
 *  GsrStreamPage — "Stream" tab
 *
 *  Groups: Service · Action · Status
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum {
    STREAM_SERVICE_TWITCH,
    STREAM_SERVICE_YOUTUBE,
    STREAM_SERVICE_CUSTOM,
} StreamService;

struct _GsrStreamPage {
    AdwPreferencesPage parent_instance;

    const GsrInfo *info;   /* borrowed */

    /* ── Hotkey group ─── */
    AdwPreferencesGroup *hotkey_group;
#ifdef HAVE_WAYLAND
    GtkWidget           *hotkey_not_supported_label; /* Wayland only */
    GtkWidget           *hotkey_info_row;            /* Wayland only */
#endif

#ifdef HAVE_X11
    /* X11 interactive hotkey rows */
    AdwActionRow        *x11_start_stop_row;
    GtkShortcutLabel    *x11_start_stop_label;
    char                *x11_start_stop_accel;       /* owned */
#endif

    /* ── Service group ─── */
    AdwPreferencesGroup *service_group;
    AdwComboRow         *service_row;
    AdwPasswordEntryRow *twitch_key_row;
    AdwPasswordEntryRow *youtube_key_row;
    AdwPasswordEntryRow *custom_url_row;
    AdwComboRow         *container_row;

    /* ── Action group ─── */
    AdwPreferencesGroup *action_group;
    GtkButton           *start_button;

    /* ── Status group ─── */
    AdwPreferencesGroup *status_group;
    GtkBox              *status_box;
    GtkImage            *record_icon;
    GtkLabel            *timer_label;

    gboolean             is_active;
    double               start_time;
    guint                timer_source_id;
};

G_DEFINE_FINAL_TYPE(GsrStreamPage, gsr_stream_page, ADW_TYPE_PREFERENCES_PAGE)

/* ── Helpers ─────────────────────────────────────────────────────── */

static StreamService
get_selected_service(GsrStreamPage *self)
{
    guint idx = adw_combo_row_get_selected(self->service_row);
    switch (idx) {
    case 0:  return STREAM_SERVICE_TWITCH;
    case 1:  return STREAM_SERVICE_YOUTUBE;
    case 2:  return STREAM_SERVICE_CUSTOM;
    default: return STREAM_SERVICE_TWITCH;
    }
}

static void
update_service_visibility(GsrStreamPage *self)
{
    StreamService svc = get_selected_service(self);
    gtk_widget_set_visible(GTK_WIDGET(self->twitch_key_row),
        svc == STREAM_SERVICE_TWITCH);
    gtk_widget_set_visible(GTK_WIDGET(self->youtube_key_row),
        svc == STREAM_SERVICE_YOUTUBE);
    gtk_widget_set_visible(GTK_WIDGET(self->custom_url_row),
        svc == STREAM_SERVICE_CUSTOM);
    gtk_widget_set_visible(GTK_WIDGET(self->container_row),
        svc == STREAM_SERVICE_CUSTOM);
}

/* ── Timer helpers ───────────────────────────────────────────────── */

static double
clock_get_monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void
format_timer(double seconds, char *buf, size_t buf_size)
{
    int total = (int)seconds;
    int h = total / 3600;
    int m = (total % 3600) / 60;
    int s = total % 60;
    snprintf(buf, buf_size, "%02d:%02d:%02d", h, m, s);
}

static gboolean
on_timer_tick(gpointer user_data)
{
    GsrStreamPage *self = GSR_STREAM_PAGE(user_data);
    double elapsed = clock_get_monotonic_seconds() - self->start_time;
    char buf[32];
    format_timer(elapsed, buf, sizeof(buf));
    gtk_label_set_text(self->timer_label, buf);
    return G_SOURCE_CONTINUE;
}

/* ── Callbacks ───────────────────────────────────────────────────── */

static void
on_service_changed(GObject    *obj G_GNUC_UNUSED,
                   GParamSpec *pspec G_GNUC_UNUSED,
                   gpointer    user_data)
{
    update_service_visibility(GSR_STREAM_PAGE(user_data));
}

static void
on_start_streaming_clicked(GtkButton *btn G_GNUC_UNUSED,
                           gpointer   user_data)
{
    GsrStreamPage *self = GSR_STREAM_PAGE(user_data);

    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
    GsrWindow *window = (root && GSR_IS_WINDOW(root)) ? GSR_WINDOW(root) : NULL;

    if (self->is_active) {
        /* ── Stop ─── */
        if (window) {
            gsr_window_stop_process(window, NULL);
            gsr_window_set_recording_active(window, FALSE);
        }
        gsr_stream_page_set_active(self, FALSE);
        /* set_active(FALSE) resets is_active and stops timer */
    } else {
        /* ── Start ─── */
        if (!window) return;

        gboolean ok = gsr_window_start_process(window, GSR_ACTIVE_MODE_STREAM);
        if (!ok) return;  /* fork failed — toast already shown by window */

        self->is_active = TRUE;
        gsr_stream_page_set_active(self, TRUE);
        gsr_window_set_recording_active(window, TRUE);

        /* Start display timer */
        self->start_time = clock_get_monotonic_seconds();
        self->timer_source_id = g_timeout_add(500, on_timer_tick, self);
    }
}

/* ── Build groups ────────────────────────────────────────────────── */

#ifdef HAVE_WAYLAND
static gboolean
is_kde_wayland(void)
{
    const char *desktop = g_getenv("XDG_CURRENT_DESKTOP");
    return desktop && g_strstr_len(desktop, -1, "KDE") != NULL;
}
#endif

/* ── X11 hotkey row callbacks ────────────────────────────────────── */

#ifdef HAVE_X11
static void
on_x11_start_stop_shortcut_set(GsrShortcutAccelDialog *dialog,
                                gpointer                user_data)
{
    GsrStreamPage *self = GSR_STREAM_PAGE(user_data);
    const char *accel = gsr_shortcut_accel_dialog_get_accelerator(dialog);

    g_free(self->x11_start_stop_accel);
    self->x11_start_stop_accel = g_strdup(accel);

    if (self->x11_start_stop_label)
        gtk_shortcut_label_set_accelerator(self->x11_start_stop_label,
            accel ? accel : "");

    /* Save config & regrab hotkeys */
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
    if (root && GSR_IS_WINDOW(root)) {
        gsr_window_on_hotkey_changed(GSR_WINDOW(root));
    }
}

static void
on_x11_start_stop_activated(AdwActionRow *row G_GNUC_UNUSED,
                             gpointer      user_data)
{
    GsrStreamPage *self = GSR_STREAM_PAGE(user_data);
    GsrShortcutAccelDialog *dialog = gsr_shortcut_accel_dialog_new(
        "Start/Stop streaming", self->x11_start_stop_accel);
    g_signal_connect(dialog, "shortcut-set",
        G_CALLBACK(on_x11_start_stop_shortcut_set), self);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(self));
}
#endif /* HAVE_X11 */

static void
build_hotkey_group(GsrStreamPage *self)
{
    self->hotkey_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(self->hotkey_group, "Hotkeys");

    GsrDisplayServer ds = self->info->system_info.display_server;

#ifdef HAVE_WAYLAND
    if (ds == GSR_DISPLAY_SERVER_WAYLAND) {
        /* "Not supported" label — initially hidden, shown if portal fails */
        self->hotkey_not_supported_label = gtk_label_new(
            "Your Wayland compositor doesn't support global hotkeys.\n"
            "Use X11 or KDE Plasma on Wayland if you want to use hotkeys.");
        gtk_label_set_wrap(GTK_LABEL(self->hotkey_not_supported_label), TRUE);
        gtk_widget_add_css_class(self->hotkey_not_supported_label, "dim-label");
        gtk_widget_set_margin_top(self->hotkey_not_supported_label, 6);
        gtk_widget_set_margin_bottom(self->hotkey_not_supported_label, 6);
        gtk_widget_set_visible(self->hotkey_not_supported_label, FALSE);
        adw_preferences_group_add(self->hotkey_group,
            self->hotkey_not_supported_label);

        /* Info row about Wayland hotkeys */
        AdwActionRow *info_row = ADW_ACTION_ROW(adw_action_row_new());
        self->hotkey_info_row = GTK_WIDGET(info_row);
        if (is_kde_wayland()) {
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(info_row),
                "Hotkeys are managed by KDE Plasma");
            adw_action_row_set_subtitle(info_row,
                "Click to configure hotkeys in system settings");
            GtkButton *change_btn = GTK_BUTTON(
                gtk_button_new_with_label("Change hotkeys"));
            gtk_widget_set_valign(GTK_WIDGET(change_btn), GTK_ALIGN_CENTER);
            /* The button will be wired to gsr_hotkeys from the window */
            g_object_set_data(G_OBJECT(info_row), "change-button", change_btn);
            adw_action_row_add_suffix(info_row, GTK_WIDGET(change_btn));
        } else {
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(info_row),
                "Hotkeys are managed by your compositor");
            adw_action_row_set_subtitle(info_row,
                "Go to system settings to change hotkeys");
        }
        adw_preferences_group_add(self->hotkey_group, GTK_WIDGET(info_row));

        /* Hotkey labels (shown after portal succeeds) */
        AdwActionRow *start_row = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(start_row),
            "Start/Stop streaming");
        adw_action_row_set_subtitle(start_row, "");
        adw_preferences_group_add(self->hotkey_group, GTK_WIDGET(start_row));

        /* Initially hidden on Wayland */
        gtk_widget_set_visible(GTK_WIDGET(self->hotkey_info_row), FALSE);
        gtk_widget_set_visible(GTK_WIDGET(start_row), FALSE);
    }
#endif /* HAVE_WAYLAND */

#ifdef HAVE_X11
    if (ds == GSR_DISPLAY_SERVER_X11) {
        /* X11: interactive shortcut rows */
        self->x11_start_stop_row = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->x11_start_stop_row),
            "Start/Stop streaming");
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(self->x11_start_stop_row), TRUE);

        self->x11_start_stop_label = GTK_SHORTCUT_LABEL(
            gtk_shortcut_label_new(self->x11_start_stop_accel ? self->x11_start_stop_accel : ""));
        gtk_widget_set_valign(GTK_WIDGET(self->x11_start_stop_label), GTK_ALIGN_CENTER);
        adw_action_row_add_suffix(self->x11_start_stop_row,
            GTK_WIDGET(self->x11_start_stop_label));
        /* Arrow indicator */
        GtkImage *arrow1 = GTK_IMAGE(gtk_image_new_from_icon_name("go-next-symbolic"));
        gtk_widget_add_css_class(GTK_WIDGET(arrow1), "dim-label");
        adw_action_row_add_suffix(self->x11_start_stop_row, GTK_WIDGET(arrow1));

        g_signal_connect(self->x11_start_stop_row, "activated",
            G_CALLBACK(on_x11_start_stop_activated), self);
        adw_preferences_group_add(self->hotkey_group, GTK_WIDGET(self->x11_start_stop_row));
    }
#endif /* HAVE_X11 */

    (void)ds;

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(self), self->hotkey_group);
}

static void
build_service_group(GsrStreamPage *self)
{
    self->service_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(self->service_group, "Streaming Service");

    /* Service selector */
    self->service_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->service_row),
        "Service");
    GtkStringList *svc_model = gtk_string_list_new(
        (const char *const[]){ "Twitch", "YouTube", "Custom", NULL });
    adw_combo_row_set_model(self->service_row, G_LIST_MODEL(svc_model));
    adw_combo_row_set_selected(self->service_row, 0);
    g_signal_connect(self->service_row, "notify::selected",
        G_CALLBACK(on_service_changed), self);
    adw_preferences_group_add(self->service_group,
        GTK_WIDGET(self->service_row));

    /* Twitch stream key */
    self->twitch_key_row = ADW_PASSWORD_ENTRY_ROW(adw_password_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->twitch_key_row),
        "Stream key");
    adw_preferences_group_add(self->service_group,
        GTK_WIDGET(self->twitch_key_row));

    /* YouTube stream key */
    self->youtube_key_row = ADW_PASSWORD_ENTRY_ROW(adw_password_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->youtube_key_row),
        "Stream key");
    gtk_widget_set_visible(GTK_WIDGET(self->youtube_key_row), FALSE);
    adw_preferences_group_add(self->service_group,
        GTK_WIDGET(self->youtube_key_row));

    /* Custom URL */
    self->custom_url_row = ADW_PASSWORD_ENTRY_ROW(adw_password_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->custom_url_row),
        "URL");
    gtk_widget_set_visible(GTK_WIDGET(self->custom_url_row), FALSE);
    adw_preferences_group_add(self->service_group,
        GTK_WIDGET(self->custom_url_row));

    /* Container (custom service only) */
    self->container_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->container_row),
        "Container");

    GtkStringList *ct_model = gtk_string_list_new(NULL);
    const char *containers[] = {
        "mp4", "flv", "mkv", "mov", "ts", "m3u8", NULL
    };
    for (int i = 0; containers[i]; i++)
        gtk_string_list_append(ct_model, containers[i]);

    /* Add webm if VP8 or VP9 supported */
    if (self->info->supported_video_codecs.vp8 ||
        self->info->supported_video_codecs.vp9)
        gtk_string_list_append(ct_model, "webm");

    adw_combo_row_set_model(self->container_row, G_LIST_MODEL(ct_model));
    adw_combo_row_set_selected(self->container_row, 1); /* default: flv */
    gtk_widget_set_visible(GTK_WIDGET(self->container_row), FALSE);
    adw_preferences_group_add(self->service_group,
        GTK_WIDGET(self->container_row));

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(self), self->service_group);
}

static void
build_action_group(GsrStreamPage *self)
{
    self->action_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());

    /* Button box */
    GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12));
    gtk_widget_set_halign(GTK_WIDGET(box), GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(GTK_WIDGET(box), 6);
    gtk_widget_set_margin_bottom(GTK_WIDGET(box), 6);

    self->start_button = GTK_BUTTON(gtk_button_new_with_label("Start streaming"));
    gtk_widget_set_hexpand(GTK_WIDGET(self->start_button), TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(self->start_button), "suggested-action");
    g_signal_connect(self->start_button, "clicked",
        G_CALLBACK(on_start_streaming_clicked), self);
    gtk_box_append(box, GTK_WIDGET(self->start_button));

    adw_preferences_group_add(self->action_group, GTK_WIDGET(box));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(self), self->action_group);
}

static void
build_status_group(GsrStreamPage *self)
{
    self->status_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());

    self->status_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8));
    gtk_widget_set_halign(GTK_WIDGET(self->status_box), GTK_ALIGN_CENTER);
    gtk_widget_set_opacity(GTK_WIDGET(self->status_box), 0.5);

    GtkImage *icon = GTK_IMAGE(gtk_image_new_from_icon_name("media-record-symbolic"));
    self->record_icon = icon;
    gtk_box_append(self->status_box, GTK_WIDGET(icon));

    self->timer_label = GTK_LABEL(gtk_label_new("00:00:00"));
    gtk_box_append(self->status_box, GTK_WIDGET(self->timer_label));

    adw_preferences_group_add(self->status_group, GTK_WIDGET(self->status_box));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(self), self->status_group);
}

/* ── GObject lifecycle ───────────────────────────────────────────── */

static void
gsr_stream_page_finalize(GObject *object)
{
#ifdef HAVE_X11
    GsrStreamPage *self = GSR_STREAM_PAGE(object);
    g_free(self->x11_start_stop_accel);
#endif
    G_OBJECT_CLASS(gsr_stream_page_parent_class)->finalize(object);
}

static void
gsr_stream_page_init(GsrStreamPage *self)
{
    (void)self;
}

static void
gsr_stream_page_class_init(GsrStreamPageClass *klass)
{
    GObjectClass *obj_class = G_OBJECT_CLASS(klass);
    obj_class->finalize = gsr_stream_page_finalize;
}

/* ── Public API ──────────────────────────────────────────────────── */

GsrStreamPage *
gsr_stream_page_new(const GsrInfo *info)
{
    GsrStreamPage *self = g_object_new(GSR_TYPE_STREAM_PAGE, NULL);
    self->info = info;

    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(self), "Stream");
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(self),
        "network-transmit-symbolic");

    build_hotkey_group(self);
    build_service_group(self);
    build_action_group(self);
    build_status_group(self);

    /* Set initial visibility */
    update_service_visibility(self);

    return self;
}

/* ── Config apply/read ───────────────────────────────────────────── */

/* Helper: select a combo row item by matching string from model */
static void
combo_row_select_string(AdwComboRow *row, const char *value)
{
    if (!value || !value[0]) return;
    GListModel *model = adw_combo_row_get_model(row);
    guint n = g_list_model_get_n_items(model);
    for (guint i = 0; i < n; i++) {
        GtkStringObject *obj = GTK_STRING_OBJECT(g_list_model_get_item(model, i));
        const char *str = gtk_string_object_get_string(obj);
        g_object_unref(obj);
        if (g_str_equal(str, value)) {
            adw_combo_row_set_selected(row, i);
            return;
        }
    }
}

static const char *
combo_row_get_selected_string(AdwComboRow *row)
{
    GtkStringObject *obj = GTK_STRING_OBJECT(
        adw_combo_row_get_selected_item(row));
    return obj ? gtk_string_object_get_string(obj) : "";
}

/* Map config streaming service string to combo index */
static guint
service_string_to_index(const char *svc)
{
    if (!svc) return 0;
    if (g_str_equal(svc, "twitch"))  return 0;
    if (g_str_equal(svc, "youtube")) return 1;
    if (g_str_equal(svc, "custom"))  return 2;
    return 0;
}

static const char *
service_index_to_string(guint idx)
{
    switch (idx) {
    case 0:  return "twitch";
    case 1:  return "youtube";
    case 2:  return "custom";
    default: return "twitch";
    }
}

/* Map config container ID to display name */
static const char *
stream_container_id_to_display(const char *id)
{
    if (!id) return "flv";
    /* The config stores internal IDs like "matroska", but our combo shows "mkv" etc. */
    if (g_str_equal(id, "matroska")) return "mkv";
    if (g_str_equal(id, "mpegts"))   return "ts";
    if (g_str_equal(id, "hls"))      return "m3u8";
    return id; /* mp4, flv, mov, webm pass through */
}

static const char *
stream_container_display_to_id(const char *display)
{
    if (!display) return "flv";
    if (g_str_equal(display, "mkv"))  return "matroska";
    if (g_str_equal(display, "ts"))   return "mpegts";
    if (g_str_equal(display, "m3u8")) return "hls";
    return display; /* mp4, flv, mov, webm pass through */
}

void
gsr_stream_page_apply_config(GsrStreamPage *self, const GsrConfig *config)
{
    const GsrStreamingConfig *s = &config->streaming_config;

    adw_combo_row_set_selected(self->service_row,
        service_string_to_index(s->streaming_service));

    gtk_editable_set_text(GTK_EDITABLE(self->twitch_key_row),
        s->twitch_stream_key ? s->twitch_stream_key : "");
    gtk_editable_set_text(GTK_EDITABLE(self->youtube_key_row),
        s->youtube_stream_key ? s->youtube_stream_key : "");
    gtk_editable_set_text(GTK_EDITABLE(self->custom_url_row),
        s->custom_url ? s->custom_url : "");

    /* Container for custom service */
    combo_row_select_string(self->container_row,
        stream_container_id_to_display(s->custom_container));

    update_service_visibility(self);

    /* Hotkeys (X11 only) */
#ifdef HAVE_X11
    if (self->x11_start_stop_label) {
        g_free(self->x11_start_stop_accel);
        self->x11_start_stop_accel = gsr_config_hotkey_to_accel(&s->start_stop_hotkey);
        gtk_shortcut_label_set_accelerator(self->x11_start_stop_label,
            self->x11_start_stop_accel ? self->x11_start_stop_accel : "");
    }
#endif
}

void
gsr_stream_page_read_config(GsrStreamPage *self, GsrConfig *config)
{
    GsrStreamingConfig *s = &config->streaming_config;

    g_free(s->streaming_service);
    s->streaming_service = g_strdup(service_index_to_string(
        adw_combo_row_get_selected(self->service_row)));

    g_free(s->twitch_stream_key);
    s->twitch_stream_key = g_strdup(
        gtk_editable_get_text(GTK_EDITABLE(self->twitch_key_row)));

    g_free(s->youtube_stream_key);
    s->youtube_stream_key = g_strdup(
        gtk_editable_get_text(GTK_EDITABLE(self->youtube_key_row)));

    g_free(s->custom_url);
    s->custom_url = g_strdup(
        gtk_editable_get_text(GTK_EDITABLE(self->custom_url_row)));

    g_free(s->custom_container);
    s->custom_container = g_strdup(stream_container_display_to_id(
        combo_row_get_selected_string(self->container_row)));

    /* Hotkeys */
#ifdef HAVE_X11
    gsr_config_hotkey_from_accel(&s->start_stop_hotkey, self->x11_start_stop_accel);
#endif
}

/* ── Process management API ──────────────────────────────────────── */

void
gsr_stream_page_set_active(GsrStreamPage *self, gboolean active)
{
    if (active) {
        gtk_button_set_label(self->start_button, "Stop streaming");
        gtk_widget_remove_css_class(GTK_WIDGET(self->start_button), "suggested-action");
        gtk_widget_add_css_class(GTK_WIDGET(self->start_button), "destructive-action");
        gtk_widget_set_opacity(GTK_WIDGET(self->status_box), 1.0);
        gtk_widget_add_css_class(GTK_WIDGET(self->record_icon), "recording-active");
    } else {
        gtk_button_set_label(self->start_button, "Start streaming");
        gtk_widget_remove_css_class(GTK_WIDGET(self->start_button), "destructive-action");
        gtk_widget_add_css_class(GTK_WIDGET(self->start_button), "suggested-action");
        gtk_widget_set_opacity(GTK_WIDGET(self->status_box), 0.5);
        gtk_widget_remove_css_class(GTK_WIDGET(self->record_icon), "recording-active");
        gtk_label_set_text(self->timer_label, "00:00:00");

        /* Reset internal state (handles external stop via handle_child_death) */
        self->is_active = FALSE;
        if (self->timer_source_id) {
            g_source_remove(self->timer_source_id);
            self->timer_source_id = 0;
        }
    }
}

void
gsr_stream_page_update_timer(GsrStreamPage *self, const char *text)
{
    gtk_label_set_text(self->timer_label, text);
}

char *
gsr_stream_page_get_stream_url(GsrStreamPage *self)
{
    StreamService svc = get_selected_service(self);

    switch (svc) {
    case STREAM_SERVICE_TWITCH: {
        const char *key = gtk_editable_get_text(GTK_EDITABLE(self->twitch_key_row));
        return g_strdup_printf("rtmp://live.twitch.tv/app/%s", key ? key : "");
    }
    case STREAM_SERVICE_YOUTUBE: {
        const char *key = gtk_editable_get_text(GTK_EDITABLE(self->youtube_key_row));
        return g_strdup_printf("rtmp://a.rtmp.youtube.com/live2/%s", key ? key : "");
    }
    case STREAM_SERVICE_CUSTOM: {
        const char *url = gtk_editable_get_text(GTK_EDITABLE(self->custom_url_row));
        if (!url || !url[0])
            return g_strdup("");
        /* If no recognized scheme prefix, prepend rtmp:// */
        if (g_str_has_prefix(url, "rtmp://")  ||
            g_str_has_prefix(url, "rtmps://") ||
            g_str_has_prefix(url, "rtsp://")  ||
            g_str_has_prefix(url, "srt://")   ||
            g_str_has_prefix(url, "http://")  ||
            g_str_has_prefix(url, "https://") ||
            g_str_has_prefix(url, "tcp://")   ||
            g_str_has_prefix(url, "udp://"))
            return g_strdup(url);
        return g_strdup_printf("rtmp://%s", url);
    }
    }
    return g_strdup("");
}

char *
gsr_stream_page_get_container(GsrStreamPage *self)
{
    StreamService svc = get_selected_service(self);
    if (svc != STREAM_SERVICE_CUSTOM)
        return g_strdup("flv");
    return g_strdup(stream_container_display_to_id(
        combo_row_get_selected_string(self->container_row)));
}

void
gsr_stream_page_activate_start_stop(GsrStreamPage *self)
{
    g_return_if_fail(GSR_IS_STREAM_PAGE(self));
    gtk_widget_activate(GTK_WIDGET(self->start_button));
}

#ifdef HAVE_WAYLAND
void
gsr_stream_page_set_wayland_hotkeys_supported(GsrStreamPage *self,
                                               gboolean       supported)
{
    g_return_if_fail(GSR_IS_STREAM_PAGE(self));

    if (self->hotkey_not_supported_label)
        gtk_widget_set_visible(self->hotkey_not_supported_label, !supported);
    if (self->hotkey_info_row)
        gtk_widget_set_visible(self->hotkey_info_row, supported);
}
#endif /* HAVE_WAYLAND */
