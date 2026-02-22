#include "gsr-replay-page.h"
#ifdef HAVE_X11
#include "gsr-shortcut-accel-dialog.h"
#endif
#include "gsr-window.h"
#include <signal.h>
#include <time.h>

/* ═══════════════════════════════════════════════════════════════════
 *  GsrReplayPage — "Replay" tab
 *
 *  Groups: Output · Action · Status
 * ═══════════════════════════════════════════════════════════════════ */

struct _GsrReplayPage {
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
    AdwActionRow        *x11_save_row;
    GtkShortcutLabel    *x11_save_label;
    char                *x11_save_accel;              /* owned */
#endif

    /* ── Output group ─── */
    AdwPreferencesGroup *output_group;
    AdwActionRow        *save_dir_row;
    char                *save_directory;  /* owned */
    AdwComboRow         *container_row;
    AdwSpinRow          *replay_time_row;

    /* ── Action group ─── */
    AdwPreferencesGroup *action_group;
    GtkButton           *start_button;
    GtkButton           *save_button;

    /* ── Status group ─── */
    AdwPreferencesGroup *status_group;
    GtkBox              *status_box;
    GtkImage            *record_icon;
    GtkLabel            *timer_label;

    gboolean             is_active;
    double               start_time;
    guint                timer_source_id;
};

G_DEFINE_FINAL_TYPE(GsrReplayPage, gsr_replay_page, ADW_TYPE_PREFERENCES_PAGE)

/* ── Helpers ─────────────────────────────────────────────────────── */

static char *
get_default_videos_dir(void)
{
    const char *vdir = g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS);
    if (vdir)
        return g_strdup(vdir);
    return g_build_filename(g_get_home_dir(), "Videos", NULL);
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
    GsrReplayPage *self = GSR_REPLAY_PAGE(user_data);
    double elapsed = clock_get_monotonic_seconds() - self->start_time;
    char buf[32];
    format_timer(elapsed, buf, sizeof(buf));
    gtk_label_set_text(self->timer_label, buf);
    return G_SOURCE_CONTINUE;
}

/* ── File chooser ────────────────────────────────────────────────── */

static void
on_folder_dialog_finish(GObject      *source,
                        GAsyncResult *result,
                        gpointer      user_data)
{
    GsrReplayPage *self = GSR_REPLAY_PAGE(user_data);
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    GFile *folder = gtk_file_dialog_select_folder_finish(dialog, result, NULL);
    if (!folder) return;

    g_free(self->save_directory);
    self->save_directory = g_file_get_path(folder);
    adw_action_row_set_subtitle(self->save_dir_row, self->save_directory);
    g_object_unref(folder);
}

static void
on_save_dir_activated(AdwActionRow *row G_GNUC_UNUSED,
                      gpointer      user_data)
{
    GsrReplayPage *self = GSR_REPLAY_PAGE(user_data);
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select save directory");

    if (self->save_directory) {
        GFile *initial = g_file_new_for_path(self->save_directory);
        gtk_file_dialog_set_initial_folder(dialog, initial);
        g_object_unref(initial);
    }

    GtkWindow *win = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self)));
    gtk_file_dialog_select_folder(dialog, win, NULL,
        on_folder_dialog_finish, self);
    g_object_unref(dialog);
}

/* ── Callbacks ───────────────────────────────────────────────────── */

static void
on_start_replay_clicked(GtkButton *btn G_GNUC_UNUSED,
                        gpointer   user_data)
{
    GsrReplayPage *self = GSR_REPLAY_PAGE(user_data);

    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
    GsrWindow *window = (root && GSR_IS_WINDOW(root)) ? GSR_WINDOW(root) : NULL;

    if (self->is_active) {
        /* ── Stop ─── */
        if (window) {
            gsr_window_stop_process(window, NULL);
            gsr_window_set_recording_active(window, FALSE);
        }
        gsr_replay_page_set_active(self, FALSE);
        /* set_active(FALSE) resets is_active and stops timer */
    } else {
        /* ── Start ─── */
        if (!window) return;

        gboolean ok = gsr_window_start_process(window, GSR_ACTIVE_MODE_REPLAY);
        if (!ok) return;  /* fork failed — toast already shown by window */

        self->is_active = TRUE;
        gsr_replay_page_set_active(self, TRUE);
        gsr_window_set_recording_active(window, TRUE);

        /* Start display timer */
        self->start_time = clock_get_monotonic_seconds();
        self->timer_source_id = g_timeout_add(500, on_timer_tick, self);
    }
}

static void
on_save_replay_clicked(GtkButton *btn G_GNUC_UNUSED,
                       gpointer   user_data)
{
    GsrReplayPage *self = GSR_REPLAY_PAGE(user_data);
    if (!self->is_active) return;

    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
    GsrWindow *window = (root && GSR_IS_WINDOW(root)) ? GSR_WINDOW(root) : NULL;

    if (window) {
        gsr_window_send_signal(window, SIGUSR1);
        gsr_window_notify_replay_saved(window);
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
    GsrReplayPage *self = GSR_REPLAY_PAGE(user_data);
    const char *accel = gsr_shortcut_accel_dialog_get_accelerator(dialog);

    g_free(self->x11_start_stop_accel);
    self->x11_start_stop_accel = g_strdup(accel);

    if (self->x11_start_stop_label)
        gtk_shortcut_label_set_accelerator(self->x11_start_stop_label,
            accel ? accel : "");

    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
    if (root && GSR_IS_WINDOW(root))
        gsr_window_on_hotkey_changed(GSR_WINDOW(root));
}

static void
on_x11_start_stop_activated(AdwActionRow *row G_GNUC_UNUSED,
                             gpointer      user_data)
{
    GsrReplayPage *self = GSR_REPLAY_PAGE(user_data);
    GsrShortcutAccelDialog *dialog = gsr_shortcut_accel_dialog_new(
        "Start/Stop replay", self->x11_start_stop_accel);
    g_signal_connect(dialog, "shortcut-set",
        G_CALLBACK(on_x11_start_stop_shortcut_set), self);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(self));
}

static void
on_x11_save_shortcut_set(GsrShortcutAccelDialog *dialog,
                          gpointer                user_data)
{
    GsrReplayPage *self = GSR_REPLAY_PAGE(user_data);
    const char *accel = gsr_shortcut_accel_dialog_get_accelerator(dialog);

    g_free(self->x11_save_accel);
    self->x11_save_accel = g_strdup(accel);

    if (self->x11_save_label)
        gtk_shortcut_label_set_accelerator(self->x11_save_label,
            accel ? accel : "");

    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
    if (root && GSR_IS_WINDOW(root))
        gsr_window_on_hotkey_changed(GSR_WINDOW(root));
}

static void
on_x11_save_activated(AdwActionRow *row G_GNUC_UNUSED,
                       gpointer      user_data)
{
    GsrReplayPage *self = GSR_REPLAY_PAGE(user_data);
    GsrShortcutAccelDialog *dialog = gsr_shortcut_accel_dialog_new(
        "Save replay", self->x11_save_accel);
    g_signal_connect(dialog, "shortcut-set",
        G_CALLBACK(on_x11_save_shortcut_set), self);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(self));
}
#endif /* HAVE_X11 */

static void
build_hotkey_group(GsrReplayPage *self)
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
            "Start/Stop replay");
        adw_action_row_set_subtitle(start_row, "");
        adw_preferences_group_add(self->hotkey_group, GTK_WIDGET(start_row));

        AdwActionRow *save_row = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(save_row),
            "Save replay");
        adw_action_row_set_subtitle(save_row, "");
        adw_preferences_group_add(self->hotkey_group, GTK_WIDGET(save_row));

        /* Initially hidden on Wayland */
        gtk_widget_set_visible(GTK_WIDGET(self->hotkey_info_row), FALSE);
        gtk_widget_set_visible(GTK_WIDGET(start_row), FALSE);
        gtk_widget_set_visible(GTK_WIDGET(save_row), FALSE);
    }
#endif /* HAVE_WAYLAND */

#ifdef HAVE_X11
    if (ds == GSR_DISPLAY_SERVER_X11) {
        /* X11: interactive shortcut rows */
        self->x11_start_stop_row = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->x11_start_stop_row),
            "Start/Stop replay");
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(self->x11_start_stop_row), TRUE);

        self->x11_start_stop_label = GTK_SHORTCUT_LABEL(
            gtk_shortcut_label_new(self->x11_start_stop_accel ? self->x11_start_stop_accel : ""));
        gtk_widget_set_valign(GTK_WIDGET(self->x11_start_stop_label), GTK_ALIGN_CENTER);
        adw_action_row_add_suffix(self->x11_start_stop_row,
            GTK_WIDGET(self->x11_start_stop_label));
        GtkImage *arrow1 = GTK_IMAGE(gtk_image_new_from_icon_name("go-next-symbolic"));
        gtk_widget_add_css_class(GTK_WIDGET(arrow1), "dim-label");
        adw_action_row_add_suffix(self->x11_start_stop_row, GTK_WIDGET(arrow1));

        g_signal_connect(self->x11_start_stop_row, "activated",
            G_CALLBACK(on_x11_start_stop_activated), self);
        adw_preferences_group_add(self->hotkey_group, GTK_WIDGET(self->x11_start_stop_row));

        /* Save replay row */
        self->x11_save_row = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->x11_save_row),
            "Save replay");
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(self->x11_save_row), TRUE);

        self->x11_save_label = GTK_SHORTCUT_LABEL(
            gtk_shortcut_label_new(self->x11_save_accel ? self->x11_save_accel : ""));
        gtk_widget_set_valign(GTK_WIDGET(self->x11_save_label), GTK_ALIGN_CENTER);
        adw_action_row_add_suffix(self->x11_save_row,
            GTK_WIDGET(self->x11_save_label));
        GtkImage *arrow2 = GTK_IMAGE(gtk_image_new_from_icon_name("go-next-symbolic"));
        gtk_widget_add_css_class(GTK_WIDGET(arrow2), "dim-label");
        adw_action_row_add_suffix(self->x11_save_row, GTK_WIDGET(arrow2));

        g_signal_connect(self->x11_save_row, "activated",
            G_CALLBACK(on_x11_save_activated), self);
        adw_preferences_group_add(self->hotkey_group, GTK_WIDGET(self->x11_save_row));
    }
#endif /* HAVE_X11 */

    (void)ds;

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(self), self->hotkey_group);
}

static void
build_output_group(GsrReplayPage *self)
{
    self->output_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(self->output_group, "Output");

    /* Save directory */
    self->save_directory = get_default_videos_dir();
    self->save_dir_row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->save_dir_row),
        "Save directory");
    adw_action_row_set_subtitle(self->save_dir_row, self->save_directory);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(self->save_dir_row), TRUE);

    GtkImage *folder_icon = GTK_IMAGE(
        gtk_image_new_from_icon_name("document-open-symbolic"));
    adw_action_row_add_suffix(self->save_dir_row, GTK_WIDGET(folder_icon));

    g_signal_connect(self->save_dir_row, "activated",
        G_CALLBACK(on_save_dir_activated), self);
    adw_preferences_group_add(self->output_group,
        GTK_WIDGET(self->save_dir_row));

    /* Container format */
    self->container_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->container_row),
        "Container");

    GtkStringList *ct_model = gtk_string_list_new(NULL);
    const char *containers[] = {
        "mp4", "flv", "mkv", "mov", "ts", "m3u8", NULL
    };
    for (int i = 0; containers[i]; i++)
        gtk_string_list_append(ct_model, containers[i]);

    if (self->info->supported_video_codecs.vp8 ||
        self->info->supported_video_codecs.vp9)
        gtk_string_list_append(ct_model, "webm");

    adw_combo_row_set_model(self->container_row, G_LIST_MODEL(ct_model));
    adw_combo_row_set_selected(self->container_row, 0); /* default: mp4 */
    adw_preferences_group_add(self->output_group,
        GTK_WIDGET(self->container_row));

    /* Replay time */
    self->replay_time_row = ADW_SPIN_ROW(
        adw_spin_row_new_with_range(5, 1200, 1));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->replay_time_row),
        "Replay time (seconds)");
    adw_spin_row_set_value(self->replay_time_row, 30);
    adw_preferences_group_add(self->output_group,
        GTK_WIDGET(self->replay_time_row));

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(self), self->output_group);
}

static void
build_action_group(GsrReplayPage *self)
{
    self->action_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());

    GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12));
    gtk_widget_set_halign(GTK_WIDGET(box), GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(GTK_WIDGET(box), 6);
    gtk_widget_set_margin_bottom(GTK_WIDGET(box), 6);

    self->start_button = GTK_BUTTON(
        gtk_button_new_with_label("Start replay"));
    gtk_widget_set_hexpand(GTK_WIDGET(self->start_button), TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(self->start_button), "suggested-action");
    g_signal_connect(self->start_button, "clicked",
        G_CALLBACK(on_start_replay_clicked), self);
    gtk_box_append(box, GTK_WIDGET(self->start_button));

    self->save_button = GTK_BUTTON(
        gtk_button_new_with_label("Save replay"));
    gtk_widget_set_hexpand(GTK_WIDGET(self->save_button), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(self->save_button), FALSE);
    g_signal_connect(self->save_button, "clicked",
        G_CALLBACK(on_save_replay_clicked), self);
    gtk_box_append(box, GTK_WIDGET(self->save_button));

    adw_preferences_group_add(self->action_group, GTK_WIDGET(box));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(self), self->action_group);
}

static void
build_status_group(GsrReplayPage *self)
{
    self->status_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());

    self->status_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8));
    gtk_widget_set_halign(GTK_WIDGET(self->status_box), GTK_ALIGN_CENTER);
    gtk_widget_set_opacity(GTK_WIDGET(self->status_box), 0.5);

    self->record_icon = GTK_IMAGE(
        gtk_image_new_from_icon_name("media-record-symbolic"));
    gtk_box_append(self->status_box, GTK_WIDGET(self->record_icon));

    self->timer_label = GTK_LABEL(gtk_label_new("00:00:00"));
    gtk_box_append(self->status_box, GTK_WIDGET(self->timer_label));

    adw_preferences_group_add(self->status_group, GTK_WIDGET(self->status_box));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(self), self->status_group);
}

/* ── GObject lifecycle ───────────────────────────────────────────── */

static void
gsr_replay_page_finalize(GObject *object)
{
    GsrReplayPage *self = GSR_REPLAY_PAGE(object);

    if (self->timer_source_id) {
        g_source_remove(self->timer_source_id);
        self->timer_source_id = 0;
    }

    g_free(self->save_directory);
#ifdef HAVE_X11
    g_free(self->x11_start_stop_accel);
    g_free(self->x11_save_accel);
#endif
    G_OBJECT_CLASS(gsr_replay_page_parent_class)->finalize(object);
}

static void
gsr_replay_page_init(GsrReplayPage *self)
{
    (void)self;
}

static void
gsr_replay_page_class_init(GsrReplayPageClass *klass)
{
    GObjectClass *obj_class = G_OBJECT_CLASS(klass);
    obj_class->finalize = gsr_replay_page_finalize;
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

/* Map config container ID to display name */
static const char *
container_id_to_display(const char *id)
{
    if (!id) return "mp4";
    if (g_str_equal(id, "matroska")) return "mkv";
    if (g_str_equal(id, "mpegts"))   return "ts";
    if (g_str_equal(id, "hls"))      return "m3u8";
    return id; /* mp4, flv, mov, webm pass through */
}

static const char *
container_display_to_id(const char *display)
{
    if (!display) return "mp4";
    if (g_str_equal(display, "mkv"))  return "matroska";
    if (g_str_equal(display, "ts"))   return "mpegts";
    if (g_str_equal(display, "m3u8")) return "hls";
    return display; /* mp4, flv, mov, webm pass through */
}

void
gsr_replay_page_apply_config(GsrReplayPage *self, const GsrConfig *config)
{
    const GsrReplayConfig *rp = &config->replay_config;

    /* Save directory */
    if (rp->save_directory && rp->save_directory[0]) {
        g_free(self->save_directory);
        self->save_directory = g_strdup(rp->save_directory);
        adw_action_row_set_subtitle(self->save_dir_row, self->save_directory);
    }

    /* Container */
    combo_row_select_string(self->container_row,
        container_id_to_display(rp->container));

    /* Replay time */
    if (rp->replay_time > 0)
        adw_spin_row_set_value(self->replay_time_row, rp->replay_time);

    /* Hotkeys (X11 only) */
#ifdef HAVE_X11
    if (self->x11_start_stop_label) {
        g_free(self->x11_start_stop_accel);
        self->x11_start_stop_accel = gsr_config_hotkey_to_accel(&rp->start_stop_hotkey);
        gtk_shortcut_label_set_accelerator(self->x11_start_stop_label,
            self->x11_start_stop_accel ? self->x11_start_stop_accel : "");
    }
    if (self->x11_save_label) {
        g_free(self->x11_save_accel);
        self->x11_save_accel = gsr_config_hotkey_to_accel(&rp->save_hotkey);
        gtk_shortcut_label_set_accelerator(self->x11_save_label,
            self->x11_save_accel ? self->x11_save_accel : "");
    }
#endif
}

void
gsr_replay_page_read_config(GsrReplayPage *self, GsrConfig *config)
{
    GsrReplayConfig *rp = &config->replay_config;

    /* Save directory */
    g_free(rp->save_directory);
    rp->save_directory = g_strdup(self->save_directory ? self->save_directory : "");

    /* Container */
    g_free(rp->container);
    rp->container = g_strdup(container_display_to_id(
        combo_row_get_selected_string(self->container_row)));

    /* Replay time */
    rp->replay_time = (int32_t)adw_spin_row_get_value(self->replay_time_row);

    /* Hotkeys */
#ifdef HAVE_X11
    gsr_config_hotkey_from_accel(&rp->start_stop_hotkey, self->x11_start_stop_accel);
    gsr_config_hotkey_from_accel(&rp->save_hotkey, self->x11_save_accel);
#endif
}

/* ── Process management API ──────────────────────────────────────── */

void
gsr_replay_page_set_active(GsrReplayPage *self, gboolean active)
{
    if (active) {
        gtk_button_set_label(self->start_button, "Stop replay");
        gtk_widget_remove_css_class(GTK_WIDGET(self->start_button), "suggested-action");
        gtk_widget_add_css_class(GTK_WIDGET(self->start_button), "destructive-action");
        gtk_widget_set_sensitive(GTK_WIDGET(self->save_button), TRUE);
        gtk_widget_set_opacity(GTK_WIDGET(self->status_box), 1.0);
        gtk_widget_add_css_class(GTK_WIDGET(self->record_icon), "recording-active");
    } else {
        gtk_button_set_label(self->start_button, "Start replay");
        gtk_widget_remove_css_class(GTK_WIDGET(self->start_button), "destructive-action");
        gtk_widget_add_css_class(GTK_WIDGET(self->start_button), "suggested-action");
        gtk_widget_set_sensitive(GTK_WIDGET(self->save_button), FALSE);
        gtk_widget_set_opacity(GTK_WIDGET(self->status_box), 0.5);
        gtk_label_set_text(self->timer_label, "00:00:00");
        gtk_widget_remove_css_class(GTK_WIDGET(self->record_icon), "recording-active");

        /* Reset internal state (handles external stop via handle_child_death) */
        self->is_active = FALSE;
        if (self->timer_source_id) {
            g_source_remove(self->timer_source_id);
            self->timer_source_id = 0;
        }
    }
}

void
gsr_replay_page_update_timer(GsrReplayPage *self, const char *text)
{
    gtk_label_set_text(self->timer_label, text);
}

const char *
gsr_replay_page_get_save_dir(GsrReplayPage *self)
{
    return self->save_directory;
}

char *
gsr_replay_page_get_container(GsrReplayPage *self)
{
    return g_strdup(container_display_to_id(
        combo_row_get_selected_string(self->container_row)));
}

int
gsr_replay_page_get_time(GsrReplayPage *self)
{
    return (int)adw_spin_row_get_value(self->replay_time_row);
}

/* ── Public API ──────────────────────────────────────────────────── */

GsrReplayPage *
gsr_replay_page_new(const GsrInfo *info)
{
    GsrReplayPage *self = g_object_new(GSR_TYPE_REPLAY_PAGE, NULL);
    self->info = info;

    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(self), "Replay");
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(self),
        "media-playlist-repeat-symbolic");

    build_hotkey_group(self);
    build_output_group(self);
    build_action_group(self);
    build_status_group(self);

    return self;
}

void
gsr_replay_page_activate_start_stop(GsrReplayPage *self)
{
    g_return_if_fail(GSR_IS_REPLAY_PAGE(self));
    gtk_widget_activate(GTK_WIDGET(self->start_button));
}

void
gsr_replay_page_activate_save(GsrReplayPage *self)
{
    g_return_if_fail(GSR_IS_REPLAY_PAGE(self));
    if (!self->is_active) return;
    gtk_widget_activate(GTK_WIDGET(self->save_button));
}

#ifdef HAVE_WAYLAND
void
gsr_replay_page_set_wayland_hotkeys_supported(GsrReplayPage *self,
                                               gboolean       supported)
{
    g_return_if_fail(GSR_IS_REPLAY_PAGE(self));

    if (self->hotkey_not_supported_label)
        gtk_widget_set_visible(self->hotkey_not_supported_label, !supported);
    if (self->hotkey_info_row)
        gtk_widget_set_visible(self->hotkey_info_row, supported);
}
#endif /* HAVE_WAYLAND */
