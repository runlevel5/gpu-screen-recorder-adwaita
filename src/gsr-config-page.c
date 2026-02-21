#include "gsr-config-page.h"
#ifdef HAVE_X11
#include "gsr-x11-window-picker.h"
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  GsrConfigPage — "Config" tab
 *
 *  Groups:  Capture Target · Audio · Video · Notifications
 *  All widgets are built programmatically from GsrInfo data.
 * ═══════════════════════════════════════════════════════════════════ */

struct _GsrConfigPage {
    AdwPreferencesPage parent_instance;

    const GsrInfo *info;   /* borrowed, must outlive page */

    /* ── Capture Target group ─── */
    AdwPreferencesGroup *capture_group;
    AdwComboRow         *record_area_row;
    GtkStringList       *record_area_model;
    /* parallel array of IDs for the record_area_model */
    char               **record_area_ids;
    int                  n_record_area_ids;

    AdwSwitchRow        *change_resolution_row;
    AdwSpinRow          *video_width_row;
    AdwSpinRow          *video_height_row;
    AdwSpinRow          *area_width_row;
    AdwSpinRow          *area_height_row;
    AdwSwitchRow        *restore_portal_row;

#ifdef HAVE_X11
    /* X11 window picker */
    AdwActionRow        *select_window_row;
    unsigned long        selected_window_id;   /* X11 Window, 0 = none */
    char                *selected_window_name; /* owned, or NULL */
    GsrX11WindowPicker  *active_picker;        /* non-NULL during pick */
#endif

    /* ── Audio group ─── */
    AdwPreferencesGroup *audio_group;
    GtkListBox          *audio_rows_box;
    GtkButton           *add_device_btn;
    GtkButton           *add_app_btn;
    GtkButton           *add_custom_app_btn;
    AdwSwitchRow        *split_audio_row;
    AdwSwitchRow        *app_audio_inverted_row;
    AdwComboRow         *audio_codec_row;

    /* ── Video group ─── */
    AdwPreferencesGroup *video_group;
    AdwComboRow         *quality_row;
    AdwSpinRow          *bitrate_row;
    AdwComboRow         *video_codec_row;
    GtkStringList       *video_codec_model;
    char               **video_codec_ids;
    int                  n_video_codec_ids;
    AdwComboRow         *color_range_row;
    AdwSpinRow          *fps_row;
    AdwComboRow         *framerate_mode_row;
    AdwSwitchRow        *overclock_row;
    AdwSwitchRow        *record_cursor_row;

    /* ── Notifications group ─── */
    AdwPreferencesGroup *notifications_group;
    AdwSwitchRow        *notify_started_row;
    AdwSwitchRow        *notify_stopped_row;
    AdwSwitchRow        *notify_saved_row;
};

G_DEFINE_FINAL_TYPE(GsrConfigPage, gsr_config_page, ADW_TYPE_PREFERENCES_PAGE)

/* ── Helpers ─────────────────────────────────────────────────────── */

static void
ids_array_append(char ***ids, int *n, int *cap, const char *id)
{
    if (*n >= *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *ids = g_realloc_n(*ids, (gsize)(*cap + 1), sizeof(char *));
    }
    (*ids)[*n] = g_strdup(id);
    (*n)++;
    (*ids)[*n] = NULL;
}

/* ── Capture Target ──────────────────────────────────────────────── */

static void
on_record_area_changed(GObject *obj, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    GsrConfigPage *self = GSR_CONFIG_PAGE(user_data);
    guint idx = adw_combo_row_get_selected(ADW_COMBO_ROW(obj));
    const char *id = (idx < (guint)self->n_record_area_ids)
                   ? self->record_area_ids[idx] : "";

    gboolean is_focused = g_str_equal(id, "focused");
    gboolean is_portal  = g_str_equal(id, "portal");

    /* area size rows only for "focused" */
    gtk_widget_set_visible(GTK_WIDGET(self->area_width_row), is_focused);
    gtk_widget_set_visible(GTK_WIDGET(self->area_height_row), is_focused);

    /* change-resolution + video size rows hidden for "focused" */
    gtk_widget_set_visible(GTK_WIDGET(self->change_resolution_row), !is_focused);

    gboolean show_vid_res = !is_focused &&
        adw_switch_row_get_active(self->change_resolution_row);
    gtk_widget_set_visible(GTK_WIDGET(self->video_width_row), show_vid_res);
    gtk_widget_set_visible(GTK_WIDGET(self->video_height_row), show_vid_res);

    /* portal session restore */
    gtk_widget_set_visible(GTK_WIDGET(self->restore_portal_row), is_portal);

#ifdef HAVE_X11
    /* "Select window..." row */
    gboolean is_window = g_str_equal(id, "window");
    gtk_widget_set_visible(GTK_WIDGET(self->select_window_row), is_window);
#endif
}

static void
on_change_resolution_toggled(GObject *obj, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    GsrConfigPage *self = GSR_CONFIG_PAGE(user_data);
    gboolean active = adw_switch_row_get_active(ADW_SWITCH_ROW(obj));

    /* Also check that we're not in "focused" mode */
    guint idx = adw_combo_row_get_selected(self->record_area_row);
    const char *id = (idx < (guint)self->n_record_area_ids)
                   ? self->record_area_ids[idx] : "";
    gboolean show = active && !g_str_equal(id, "focused");

    gtk_widget_set_visible(GTK_WIDGET(self->video_width_row), show);
    gtk_widget_set_visible(GTK_WIDGET(self->video_height_row), show);
}

/* ── Window picker callback & handler ────────────────────────────── */

#ifdef HAVE_X11
static void
on_window_picked(const GsrX11WindowPickResult *result, void *userdata)
{
    GsrConfigPage *self = GSR_CONFIG_PAGE(userdata);
    self->active_picker = NULL; /* picker self-destructs after callback */

    if (result->window == None) {
        /* Cancelled — keep previous selection (if any) */
        return;
    }

    /* Store the selected window */
    g_free(self->selected_window_name);
    self->selected_window_id = (unsigned long)result->window;
    self->selected_window_name = g_strdup(result->name);

    /* Update the row subtitle */
    char *subtitle = g_strdup_printf("%s (0x%lx)",
        result->name ? result->name : "(no name)",
        (unsigned long)result->window);
    adw_action_row_set_subtitle(self->select_window_row, subtitle);
    g_free(subtitle);
}

static void
on_select_window_activated(AdwActionRow *row G_GNUC_UNUSED, gpointer user_data)
{
    GsrConfigPage *self = GSR_CONFIG_PAGE(user_data);

    /* Only allow on X11 */
    if (self->info->system_info.display_server != GSR_DISPLAY_SERVER_X11)
        return;

    /* Cancel any existing picker */
    if (self->active_picker) {
        gsr_x11_window_picker_free(self->active_picker);
        self->active_picker = NULL;
    }

    self->active_picker = gsr_x11_window_picker_new(on_window_picked, self);
    if (!self->active_picker) {
        adw_action_row_set_subtitle(self->select_window_row,
            "Failed to grab pointer");
    }
}
#endif /* HAVE_X11 */

static void
build_capture_group(GsrConfigPage *self)
{
    self->capture_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(self->capture_group, "Capture Target");

    /* Record area combo */
    self->record_area_model = gtk_string_list_new(NULL);
    self->record_area_ids = NULL;
    self->n_record_area_ids = 0;
    int cap = 0;

    const GsrInfo *info = self->info;

#ifdef HAVE_X11
    /* "Window" — X11 only */
    if (info->system_info.display_server == GSR_DISPLAY_SERVER_X11) {
        gtk_string_list_append(self->record_area_model, "Window");
        ids_array_append(&self->record_area_ids, &self->n_record_area_ids, &cap, "window");
    }

    /* "Follow focused window" — X11 only */
    if (info->system_info.display_server == GSR_DISPLAY_SERVER_X11) {
        gtk_string_list_append(self->record_area_model, "Focused window");
        ids_array_append(&self->record_area_ids, &self->n_record_area_ids, &cap, "focused");
    }
#endif

    /* Monitors */
    int first_monitor_idx = self->n_record_area_ids;
    for (int i = 0; i < info->supported_capture_options.n_monitors; i++) {
        const GsrMonitor *m = &info->supported_capture_options.monitors[i];
        char *label;
        if (m->width > 0 && m->height > 0)
            label = g_strdup_printf("Monitor %s (%dx%d)", m->name, m->width, m->height);
        else
            label = g_strdup_printf("Monitor %s", m->name);
        gtk_string_list_append(self->record_area_model, label);
        ids_array_append(&self->record_area_ids, &self->n_record_area_ids, &cap, m->name);
        g_free(label);
    }

    /* Desktop portal — only on Wayland with portal support */
    if (info->system_info.display_server == GSR_DISPLAY_SERVER_WAYLAND
        && info->supported_capture_options.portal) {
        gtk_string_list_append(self->record_area_model, "Desktop portal (no HDR)");
        ids_array_append(&self->record_area_ids, &self->n_record_area_ids, &cap, "portal");
    }

    self->record_area_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->record_area_row), "Record area");

    /* Default selection: first monitor if available, else first entry */
    guint default_idx = (info->supported_capture_options.n_monitors > 0)
        ? (guint)first_monitor_idx : 0;
    adw_combo_row_set_selected(self->record_area_row, default_idx);

    adw_combo_row_set_model(self->record_area_row,
        G_LIST_MODEL(self->record_area_model));
    adw_combo_row_set_selected(self->record_area_row, default_idx);

    g_signal_connect(self->record_area_row, "notify::selected",
        G_CALLBACK(on_record_area_changed), self);

    adw_preferences_group_add(self->capture_group, GTK_WIDGET(self->record_area_row));

    /* "Select window..." row (X11 only, shown when record area = "window") */
#ifdef HAVE_X11
    self->select_window_row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->select_window_row),
        "Select window...");
    adw_action_row_set_subtitle(self->select_window_row, "Click to pick a window");
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(self->select_window_row), TRUE);
    GtkImage *pick_icon = GTK_IMAGE(gtk_image_new_from_icon_name("find-location-symbolic"));
    adw_action_row_add_suffix(self->select_window_row, GTK_WIDGET(pick_icon));
    g_signal_connect(self->select_window_row, "activated",
        G_CALLBACK(on_select_window_activated), self);
    gtk_widget_set_visible(GTK_WIDGET(self->select_window_row), FALSE);
    self->selected_window_id = 0;
    self->selected_window_name = NULL;
    self->active_picker = NULL;
    adw_preferences_group_add(self->capture_group, GTK_WIDGET(self->select_window_row));
#endif /* HAVE_X11 */

    /* Change video resolution */
    self->change_resolution_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->change_resolution_row),
        "Change video resolution");
    adw_switch_row_set_active(self->change_resolution_row, FALSE);
    g_signal_connect(self->change_resolution_row, "notify::active",
        G_CALLBACK(on_change_resolution_toggled), self);
    adw_preferences_group_add(self->capture_group, GTK_WIDGET(self->change_resolution_row));

    /* Video resolution W×H */
    self->video_width_row = ADW_SPIN_ROW(adw_spin_row_new_with_range(5, 10000, 1));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->video_width_row), "Video width");
    adw_spin_row_set_value(self->video_width_row, 1920);
    gtk_widget_set_visible(GTK_WIDGET(self->video_width_row), FALSE);
    adw_preferences_group_add(self->capture_group, GTK_WIDGET(self->video_width_row));

    self->video_height_row = ADW_SPIN_ROW(adw_spin_row_new_with_range(5, 10000, 1));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->video_height_row), "Video height");
    adw_spin_row_set_value(self->video_height_row, 1080);
    gtk_widget_set_visible(GTK_WIDGET(self->video_height_row), FALSE);
    adw_preferences_group_add(self->capture_group, GTK_WIDGET(self->video_height_row));

    /* Area size W×H (focused mode) */
    self->area_width_row = ADW_SPIN_ROW(adw_spin_row_new_with_range(5, 10000, 1));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->area_width_row), "Area width");
    adw_spin_row_set_value(self->area_width_row, 1920);
    gtk_widget_set_visible(GTK_WIDGET(self->area_width_row), FALSE);
    adw_preferences_group_add(self->capture_group, GTK_WIDGET(self->area_width_row));

    self->area_height_row = ADW_SPIN_ROW(adw_spin_row_new_with_range(5, 10000, 1));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->area_height_row), "Area height");
    adw_spin_row_set_value(self->area_height_row, 1080);
    gtk_widget_set_visible(GTK_WIDGET(self->area_height_row), FALSE);
    adw_preferences_group_add(self->capture_group, GTK_WIDGET(self->area_height_row));

    /* Restore portal session */
    self->restore_portal_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->restore_portal_row),
        "Restore portal session");
    adw_switch_row_set_active(self->restore_portal_row, TRUE);
    gtk_widget_set_visible(GTK_WIDGET(self->restore_portal_row), FALSE);
    adw_preferences_group_add(self->capture_group, GTK_WIDGET(self->restore_portal_row));

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(self), self->capture_group);
}

/* ── Audio group ─────────────────────────────────────────────────── */

static void on_add_audio_device_clicked(GtkButton *btn, gpointer user_data);
static void on_add_app_audio_clicked(GtkButton *btn, gpointer user_data);
static void on_add_custom_app_clicked(GtkButton *btn, gpointer user_data);

static void
build_audio_group(GsrConfigPage *self)
{
    self->audio_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(self->audio_group, "Audio");

    /* Button row for adding audio tracks */
    GtkBox *btn_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6));
    gtk_widget_set_margin_top(GTK_WIDGET(btn_box), 4);
    gtk_widget_set_margin_bottom(GTK_WIDGET(btn_box), 4);

    self->add_device_btn = GTK_BUTTON(gtk_button_new());
    AdwButtonContent *dev_bc = ADW_BUTTON_CONTENT(adw_button_content_new());
    adw_button_content_set_icon_name(dev_bc, "list-add-symbolic");
    adw_button_content_set_label(dev_bc, "Audio device");
    gtk_button_set_child(self->add_device_btn, GTK_WIDGET(dev_bc));
    gtk_widget_add_css_class(GTK_WIDGET(self->add_device_btn), "flat");
    g_signal_connect(self->add_device_btn, "clicked",
        G_CALLBACK(on_add_audio_device_clicked), self);
    gtk_box_append(btn_box, GTK_WIDGET(self->add_device_btn));

    self->add_app_btn = GTK_BUTTON(gtk_button_new());
    AdwButtonContent *app_bc = ADW_BUTTON_CONTENT(adw_button_content_new());
    adw_button_content_set_icon_name(app_bc, "list-add-symbolic");
    adw_button_content_set_label(app_bc, "App audio");
    gtk_button_set_child(self->add_app_btn, GTK_WIDGET(app_bc));
    gtk_widget_add_css_class(GTK_WIDGET(self->add_app_btn), "flat");
    g_signal_connect(self->add_app_btn, "clicked",
        G_CALLBACK(on_add_app_audio_clicked), self);
    gtk_box_append(btn_box, GTK_WIDGET(self->add_app_btn));

    self->add_custom_app_btn = GTK_BUTTON(gtk_button_new());
    AdwButtonContent *custom_bc = ADW_BUTTON_CONTENT(adw_button_content_new());
    adw_button_content_set_icon_name(custom_bc, "list-add-symbolic");
    adw_button_content_set_label(custom_bc, "Custom app");
    gtk_button_set_child(self->add_custom_app_btn, GTK_WIDGET(custom_bc));
    gtk_widget_add_css_class(GTK_WIDGET(self->add_custom_app_btn), "flat");
    g_signal_connect(self->add_custom_app_btn, "clicked",
        G_CALLBACK(on_add_custom_app_clicked), self);
    gtk_box_append(btn_box, GTK_WIDGET(self->add_custom_app_btn));

    if (!self->info->system_info.supports_app_audio) {
        gtk_widget_set_visible(GTK_WIDGET(self->add_app_btn), FALSE);
        gtk_widget_set_visible(GTK_WIDGET(self->add_custom_app_btn), FALSE);
    }

    adw_preferences_group_add(self->audio_group, GTK_WIDGET(btn_box));

    /* Container for dynamic audio rows — boxed-list gives the card look */
    self->audio_rows_box = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->audio_rows_box, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->audio_rows_box), "boxed-list");
    gtk_widget_set_visible(GTK_WIDGET(self->audio_rows_box), FALSE);
    g_object_set_data(G_OBJECT(self->audio_rows_box), "config-page", self);
    adw_preferences_group_add(self->audio_group, GTK_WIDGET(self->audio_rows_box));

    /* Split audio */
    self->split_audio_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->split_audio_row),
        "Split audio tracks");
    adw_switch_row_set_active(self->split_audio_row, FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->split_audio_row), FALSE); /* advanced only */
    adw_preferences_group_add(self->audio_group, GTK_WIDGET(self->split_audio_row));

    /* Record app audio inverted */
    self->app_audio_inverted_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->app_audio_inverted_row),
        "Record all apps except selected");
    adw_switch_row_set_active(self->app_audio_inverted_row, FALSE);
    if (!self->info->system_info.supports_app_audio)
        gtk_widget_set_visible(GTK_WIDGET(self->app_audio_inverted_row), FALSE);
    adw_preferences_group_add(self->audio_group, GTK_WIDGET(self->app_audio_inverted_row));

    /* Audio codec */
    self->audio_codec_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->audio_codec_row), "Audio codec");
    GtkStringList *ac_model = gtk_string_list_new(
        (const char *const[]){ "Opus (Recommended)", "AAC", NULL });
    adw_combo_row_set_model(self->audio_codec_row, G_LIST_MODEL(ac_model));
    adw_combo_row_set_selected(self->audio_codec_row, 0);
    gtk_widget_set_visible(GTK_WIDGET(self->audio_codec_row), FALSE); /* advanced only */
    adw_preferences_group_add(self->audio_group, GTK_WIDGET(self->audio_codec_row));

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(self), self->audio_group);
}

/* ── Audio row management ─────────────────────────────────────────── */

static void
update_audio_rows_visibility(GsrConfigPage *self)
{
    gboolean has_children =
        gtk_widget_get_first_child(GTK_WIDGET(self->audio_rows_box)) != NULL;
    gtk_widget_set_visible(GTK_WIDGET(self->audio_rows_box), has_children);
}

static void
on_remove_audio_row(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GtkWidget *row = GTK_WIDGET(user_data);
    GtkWidget *parent = gtk_widget_get_parent(row);
    if (parent && GTK_IS_LIST_BOX(parent)) {
        GsrConfigPage *self = g_object_get_data(G_OBJECT(parent), "config-page");
        gtk_list_box_remove(GTK_LIST_BOX(parent), row);
        if (self)
            update_audio_rows_visibility(self);
    }
}

static GtkWidget *
create_audio_row(const char *track_type, const char *title_text,
                 const char *const *items, int n_items,
                 const char *preselect, GsrConfigPage *self)
{
    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title_text);

    g_object_set_data_full(G_OBJECT(row), "audio-track-type",
        g_strdup(track_type), g_free);

    if (g_str_equal(track_type, "app-custom")) {
        /* Free-form text entry */
        GtkEntry *entry = GTK_ENTRY(gtk_entry_new());
        gtk_widget_set_hexpand(GTK_WIDGET(entry), TRUE);
        gtk_widget_set_valign(GTK_WIDGET(entry), GTK_ALIGN_CENTER);
        if (preselect)
            gtk_editable_set_text(GTK_EDITABLE(entry), preselect);
        g_object_set_data(G_OBJECT(row), "input-widget", entry);
        adw_action_row_add_suffix(row, GTK_WIDGET(entry));
    } else {
        /* Dropdown from list */
        GtkStringList *model = gtk_string_list_new(NULL);
        int selected_idx = 0;
        for (int i = 0; i < n_items; i++) {
            gtk_string_list_append(model, items[i]);
            if (preselect && g_str_equal(items[i], preselect))
                selected_idx = i;
        }
        GtkDropDown *dd = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(model), NULL));
        gtk_widget_set_valign(GTK_WIDGET(dd), GTK_ALIGN_CENTER);
        if (n_items > 0)
            gtk_drop_down_set_selected(dd, (guint)selected_idx);
        g_object_set_data(G_OBJECT(row), "input-widget", dd);
        g_object_set_data_full(G_OBJECT(row), "string-list",
            g_object_ref(model), g_object_unref);
        adw_action_row_add_suffix(row, GTK_WIDGET(dd));
    }

    /* Remove button with × icon */
    GtkButton *rm = GTK_BUTTON(gtk_button_new_from_icon_name("window-close-symbolic"));
    gtk_widget_add_css_class(GTK_WIDGET(rm), "flat");
    gtk_widget_add_css_class(GTK_WIDGET(rm), "circular");
    gtk_widget_set_valign(GTK_WIDGET(rm), GTK_ALIGN_CENTER);
    g_signal_connect(rm, "clicked", G_CALLBACK(on_remove_audio_row), row);
    adw_action_row_add_suffix(row, GTK_WIDGET(rm));

    (void)self;
    return GTK_WIDGET(row);
}

static void
on_add_audio_device_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GsrConfigPage *self = GSR_CONFIG_PAGE(user_data);
    int n_devs = 0;
    GsrAudioDevice *devs = gsr_audio_devices_get(&n_devs);

    const char **labels = g_new0(const char *, n_devs + 1);
    for (int i = 0; i < n_devs; i++)
        labels[i] = devs[i].description;

    GtkWidget *row = create_audio_row("device", "Device", labels, n_devs,
                                      NULL, self);
    gtk_list_box_append(self->audio_rows_box, row);
    update_audio_rows_visibility(self);

    /* Store device names as data on the row for later retrieval */
    char **names = g_new0(char *, n_devs + 1);
    for (int i = 0; i < n_devs; i++)
        names[i] = g_strdup(devs[i].name);
    g_object_set_data_full(G_OBJECT(row), "device-names",
        names, (GDestroyNotify)g_strfreev);

    g_free(labels);
    gsr_audio_devices_free(devs, n_devs);
}

static void
on_add_app_audio_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GsrConfigPage *self = GSR_CONFIG_PAGE(user_data);
    int n_apps = 0;
    char **apps = gsr_application_audio_get(&n_apps);

    GtkWidget *row = create_audio_row("app", "Application",
                                      (const char *const *)apps, n_apps,
                                      NULL, self);
    gtk_list_box_append(self->audio_rows_box, row);
    update_audio_rows_visibility(self);
    gsr_application_audio_free(apps, n_apps);
}

static void
on_add_custom_app_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GsrConfigPage *self = GSR_CONFIG_PAGE(user_data);
    GtkWidget *row = create_audio_row("app-custom", "Application",
                                      NULL, 0, "", self);
    gtk_list_box_append(self->audio_rows_box, row);
    update_audio_rows_visibility(self);
}

/* ── Video group ─────────────────────────────────────────────────── */

static void
on_quality_changed(GObject *obj, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    GsrConfigPage *self = GSR_CONFIG_PAGE(user_data);
    guint idx = adw_combo_row_get_selected(ADW_COMBO_ROW(obj));
    /* index 0 = "Constant bitrate" (custom) */
    gtk_widget_set_visible(GTK_WIDGET(self->bitrate_row), idx == 0);
}

static void
build_video_group(GsrConfigPage *self)
{
    self->video_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(self->video_group, "Video");

    const GsrInfo *info = self->info;

    /* Quality */
    self->quality_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->quality_row), "Video quality");
    GtkStringList *q_model = gtk_string_list_new((const char *const[]){
        "Constant bitrate",
        "Medium", "High",
        "Very High",
        "Ultra", NULL });
    adw_combo_row_set_model(self->quality_row, G_LIST_MODEL(q_model));
    adw_combo_row_set_selected(self->quality_row, 0);
    g_signal_connect(self->quality_row, "notify::selected",
        G_CALLBACK(on_quality_changed), self);
    adw_preferences_group_add(self->video_group, GTK_WIDGET(self->quality_row));

    /* Bitrate */
    self->bitrate_row = ADW_SPIN_ROW(adw_spin_row_new_with_range(1, 500000, 1));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->bitrate_row), "Video bitrate (kbps)");
    adw_spin_row_set_value(self->bitrate_row, 15000);
    adw_preferences_group_add(self->video_group, GTK_WIDGET(self->bitrate_row));

    /* Video codec */
    self->video_codec_model = gtk_string_list_new(NULL);
    self->video_codec_ids = NULL;
    self->n_video_codec_ids = 0;
    int vc_cap = 0;

    struct { const char *id; const char *label_ok; const char *label_na; } codecs[] = {
        { "auto",          "Auto", NULL },
        { "h264",          "H.264",
                           "H.264 (N/A)" },
        { "hevc",          "HEVC",
                           "HEVC (N/A)" },
        { "hevc_10bit",    "HEVC 10-bit",
                           "HEVC 10-bit (N/A)" },
        { "hevc_hdr",
            info->system_info.display_server == GSR_DISPLAY_SERVER_WAYLAND
                ? "HEVC HDR" : "HEVC HDR (X11 N/A)",
            "HEVC HDR (N/A)" },
        { "av1",           "AV1",
                           "AV1 (N/A)" },
        { "av1_10bit",     "AV1 10-bit",
                           "AV1 10-bit (N/A)" },
        { "av1_hdr",
            info->system_info.display_server == GSR_DISPLAY_SERVER_WAYLAND
                ? "AV1 HDR" : "AV1 HDR (X11 N/A)",
            "AV1 HDR (N/A)" },
        { "vp8",           "VP8",  "VP8 (N/A)" },
        { "vp9",           "VP9",  "VP9 (N/A)" },
        { "h264_software", "H.264 Software (slow)",
                           "H.264 Software (N/A)" },
    };

    for (size_t i = 0; i < G_N_ELEMENTS(codecs); i++) {
        bool ok = gsr_info_is_codec_supported(info, codecs[i].id);
        const char *label = (ok || !codecs[i].label_na) ? codecs[i].label_ok : codecs[i].label_na;
        gtk_string_list_append(self->video_codec_model, label);
        ids_array_append(&self->video_codec_ids, &self->n_video_codec_ids, &vc_cap, codecs[i].id);
    }

    self->video_codec_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->video_codec_row), "Video codec");
    adw_combo_row_set_model(self->video_codec_row, G_LIST_MODEL(self->video_codec_model));
    adw_combo_row_set_selected(self->video_codec_row, 0);
    gtk_widget_set_visible(GTK_WIDGET(self->video_codec_row), FALSE); /* advanced only */
    adw_preferences_group_add(self->video_group, GTK_WIDGET(self->video_codec_row));

    /* Color range */
    self->color_range_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->color_range_row), "Color range");
    GtkStringList *cr_model = gtk_string_list_new(
        (const char *const[]){ "Limited", "Full", NULL });
    adw_combo_row_set_model(self->color_range_row, G_LIST_MODEL(cr_model));
    adw_combo_row_set_selected(self->color_range_row, 0);
    gtk_widget_set_visible(GTK_WIDGET(self->color_range_row), FALSE); /* advanced only */
    adw_preferences_group_add(self->video_group, GTK_WIDGET(self->color_range_row));

    /* FPS */
    self->fps_row = ADW_SPIN_ROW(adw_spin_row_new_with_range(1, 500, 1));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->fps_row), "Frame rate");
    adw_spin_row_set_value(self->fps_row, 60);
    adw_preferences_group_add(self->video_group, GTK_WIDGET(self->fps_row));

    /* Framerate mode */
    self->framerate_mode_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->framerate_mode_row), "Frame rate mode");
    GtkStringList *fm_model = gtk_string_list_new(
        (const char *const[]){ "Auto (Recommended)", "Constant", "Variable", NULL });
    adw_combo_row_set_model(self->framerate_mode_row, G_LIST_MODEL(fm_model));
    adw_combo_row_set_selected(self->framerate_mode_row, 0);
    gtk_widget_set_visible(GTK_WIDGET(self->framerate_mode_row), FALSE); /* advanced only */
    adw_preferences_group_add(self->video_group, GTK_WIDGET(self->framerate_mode_row));

    /* Overclock (NVIDIA + X11 only) */
    self->overclock_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->overclock_row),
        "Overclock memory transfer rate");
    adw_switch_row_set_active(self->overclock_row, FALSE);
    /* Only visible in advanced mode AND nvidia AND not wayland */
    gtk_widget_set_visible(GTK_WIDGET(self->overclock_row), FALSE);
    adw_preferences_group_add(self->video_group, GTK_WIDGET(self->overclock_row));

    /* Record cursor */
    self->record_cursor_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->record_cursor_row), "Record cursor");
    adw_switch_row_set_active(self->record_cursor_row, TRUE);
    adw_preferences_group_add(self->video_group, GTK_WIDGET(self->record_cursor_row));

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(self), self->video_group);
}

/* ── Notifications group ─────────────────────────────────────────── */

static void
build_notifications_group(GsrConfigPage *self)
{
    self->notifications_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(self->notifications_group, "Notifications");
    gtk_widget_set_visible(GTK_WIDGET(self->notifications_group), FALSE); /* advanced only */

    self->notify_started_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->notify_started_row),
        "Show started notification");
    adw_switch_row_set_active(self->notify_started_row, FALSE);
    adw_preferences_group_add(self->notifications_group, GTK_WIDGET(self->notify_started_row));

    self->notify_stopped_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->notify_stopped_row),
        "Show stopped notification");
    adw_switch_row_set_active(self->notify_stopped_row, FALSE);
    adw_preferences_group_add(self->notifications_group, GTK_WIDGET(self->notify_stopped_row));

    self->notify_saved_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->notify_saved_row),
        "Show video saved notification");
    adw_switch_row_set_active(self->notify_saved_row, TRUE);
    adw_preferences_group_add(self->notifications_group, GTK_WIDGET(self->notify_saved_row));

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(self), self->notifications_group);
}

/* ── GObject lifecycle ───────────────────────────────────────────── */

static void
gsr_config_page_finalize(GObject *object)
{
    GsrConfigPage *self = GSR_CONFIG_PAGE(object);

    g_strfreev(self->record_area_ids);
    g_strfreev(self->video_codec_ids);

    /* Window picker cleanup */
#ifdef HAVE_X11
    if (self->active_picker) {
        gsr_x11_window_picker_free(self->active_picker);
        self->active_picker = NULL;
    }
    g_free(self->selected_window_name);
#endif

    G_OBJECT_CLASS(gsr_config_page_parent_class)->finalize(object);
}

static void
gsr_config_page_init(GsrConfigPage *self)
{
    (void)self;
    /* Real init happens in gsr_config_page_new after info is set */
}

static void
gsr_config_page_class_init(GsrConfigPageClass *klass)
{
    GObjectClass *obj_class = G_OBJECT_CLASS(klass);
    obj_class->finalize = gsr_config_page_finalize;
}

/* ── Public API ──────────────────────────────────────────────────── */

GsrConfigPage *
gsr_config_page_new(const GsrInfo *info)
{
    GsrConfigPage *self = g_object_new(GSR_TYPE_CONFIG_PAGE, NULL);
    self->info = info;

    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(self), "Config");
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(self),
        "preferences-system-symbolic");

    build_capture_group(self);
    build_audio_group(self);
    build_video_group(self);
    build_notifications_group(self);

    /* Trigger initial visibility */
    on_record_area_changed(G_OBJECT(self->record_area_row), NULL, self);

    return self;
}

void
gsr_config_page_set_advanced(GsrConfigPage *self, gboolean advanced)
{
    /* Audio: advanced-only widgets */
    gtk_widget_set_visible(GTK_WIDGET(self->split_audio_row), advanced);
    gtk_widget_set_visible(GTK_WIDGET(self->audio_codec_row), advanced);

    /* Video: advanced-only widgets */
    gtk_widget_set_visible(GTK_WIDGET(self->video_codec_row), advanced);
    gtk_widget_set_visible(GTK_WIDGET(self->color_range_row), advanced);
    gtk_widget_set_visible(GTK_WIDGET(self->framerate_mode_row), advanced);

    /* Overclock: advanced + NVIDIA + not Wayland */
    gboolean show_oc = advanced
        && self->info->gpu_info.vendor == GSR_GPU_VENDOR_NVIDIA
        && self->info->system_info.display_server != GSR_DISPLAY_SERVER_WAYLAND;
    gtk_widget_set_visible(GTK_WIDGET(self->overclock_row), show_oc);

    /* Notifications: advanced only */
    gtk_widget_set_visible(GTK_WIDGET(self->notifications_group), advanced);
}

/* ── Config apply/read ───────────────────────────────────────────── */

/* Helper: find index in a parallel ID array matching a string, or -1 */
static int
find_id_index(char **ids, int n, const char *id)
{
    if (!id || !id[0])
        return -1;
    for (int i = 0; i < n; i++) {
        if (g_str_equal(ids[i], id))
            return i;
    }
    return -1;
}

/* Map config quality string to combo index */
static guint
quality_string_to_index(const char *q)
{
    if (!q) return 3; /* very_high */
    if (g_str_equal(q, "custom"))    return 0;
    if (g_str_equal(q, "medium"))    return 1;
    if (g_str_equal(q, "high"))      return 2;
    if (g_str_equal(q, "very_high")) return 3;
    if (g_str_equal(q, "ultra"))     return 4;
    return 3;
}

static const char *
quality_index_to_string(guint idx)
{
    switch (idx) {
    case 0:  return "custom";
    case 1:  return "medium";
    case 2:  return "high";
    case 3:  return "very_high";
    case 4:  return "ultra";
    default: return "very_high";
    }
}

/* Map config audio_codec string to combo index */
static guint
audio_codec_string_to_index(const char *ac)
{
    if (ac && g_str_equal(ac, "aac")) return 1;
    return 0; /* opus */
}

static const char *
audio_codec_index_to_string(guint idx)
{
    return (idx == 1) ? "aac" : "opus";
}

/* Map config color_range string to combo index */
static guint
color_range_string_to_index(const char *cr)
{
    if (cr && g_str_equal(cr, "full")) return 1;
    return 0; /* limited */
}

static const char *
color_range_index_to_string(guint idx)
{
    return (idx == 1) ? "full" : "limited";
}

/* Map config framerate_mode string to combo index */
static guint
framerate_mode_string_to_index(const char *fm)
{
    if (!fm) return 0;
    if (g_str_equal(fm, "auto")) return 0;
    if (g_str_equal(fm, "cfr"))  return 1;
    if (g_str_equal(fm, "vfr"))  return 2;
    return 0;
}

static const char *
framerate_mode_index_to_string(guint idx)
{
    switch (idx) {
    case 0:  return "auto";
    case 1:  return "cfr";
    case 2:  return "vfr";
    default: return "auto";
    }
}

void
gsr_config_page_apply_config(GsrConfigPage *self, const GsrConfig *config)
{
    const GsrMainConfig *m = &config->main_config;

    /* ── Capture Target ── */

    /* Record area: find matching ID in our combo model */
    int ra_idx = find_id_index(self->record_area_ids, self->n_record_area_ids,
                               m->record_area_option);
    if (ra_idx >= 0)
        adw_combo_row_set_selected(self->record_area_row, (guint)ra_idx);

    /* Resolution */
    adw_switch_row_set_active(self->change_resolution_row, m->change_video_resolution);
    if (m->video_width > 0)
        adw_spin_row_set_value(self->video_width_row, m->video_width);
    if (m->video_height > 0)
        adw_spin_row_set_value(self->video_height_row, m->video_height);
    if (m->record_area_width > 0)
        adw_spin_row_set_value(self->area_width_row, m->record_area_width);
    if (m->record_area_height > 0)
        adw_spin_row_set_value(self->area_height_row, m->record_area_height);

    /* Portal session */
    adw_switch_row_set_active(self->restore_portal_row, m->restore_portal_session);

    /* ── Audio ── */

    /* Clear existing audio rows */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->audio_rows_box))) != NULL)
        gtk_list_box_remove(self->audio_rows_box, child);

    /* Populate from config audio_input array */
    for (int i = 0; i < m->n_audio_input; i++) {
        const char *input = m->audio_input[i];
        if (!input) continue;

        if (g_str_has_prefix(input, "app:")) {
            const char *app_name = input + 4;
            if (!self->info->system_info.supports_app_audio)
                continue;

            /* Try to find in app list; if not found, create custom row */
            int n_apps = 0;
            char **apps = gsr_application_audio_get(&n_apps);

            gboolean found = FALSE;
            for (int j = 0; j < n_apps; j++) {
                if (g_ascii_strcasecmp(apps[j], app_name) == 0) {
                    found = TRUE;
                    break;
                }
            }

            if (found) {
                GtkWidget *row = create_audio_row("app", "Application",
                    (const char *const *)apps, n_apps, app_name, self);
                gtk_list_box_append(self->audio_rows_box, row);
            } else {
                GtkWidget *row = create_audio_row("app-custom", "Application",
                    NULL, 0, app_name, self);
                gtk_list_box_append(self->audio_rows_box, row);
            }
            gsr_application_audio_free(apps, n_apps);

        } else {
            /* "device:xxx" or bare legacy name */
            const char *desc = input;
            if (g_str_has_prefix(input, "device:"))
                desc = input + 7;

            int n_devs = 0;
            GsrAudioDevice *devs = gsr_audio_devices_get(&n_devs);

            const char **labels = g_new0(const char *, n_devs + 1);
            for (int j = 0; j < n_devs; j++)
                labels[j] = devs[j].description;

            GtkWidget *row = create_audio_row("device", "Device",
                labels, n_devs, desc, self);
            gtk_list_box_append(self->audio_rows_box, row);

            /* Attach device names */
            char **names = g_new0(char *, n_devs + 1);
            for (int j = 0; j < n_devs; j++)
                names[j] = g_strdup(devs[j].name);
            g_object_set_data_full(G_OBJECT(row), "device-names",
                names, (GDestroyNotify)g_strfreev);

            g_free(labels);
            gsr_audio_devices_free(devs, n_devs);
        }
    }

    update_audio_rows_visibility(self);

    /* Split audio (inverted from merge_audio_tracks) */
    adw_switch_row_set_active(self->split_audio_row, !m->merge_audio_tracks);
    adw_switch_row_set_active(self->app_audio_inverted_row, m->record_app_audio_inverted);

    /* Audio codec */
    adw_combo_row_set_selected(self->audio_codec_row,
        audio_codec_string_to_index(m->audio_codec));

    /* ── Video ── */
    adw_combo_row_set_selected(self->quality_row,
        quality_string_to_index(m->quality));
    if (m->video_bitrate > 0)
        adw_spin_row_set_value(self->bitrate_row, m->video_bitrate);

    /* Video codec: set "auto" first as fallback, then try actual value */
    int vc_auto = find_id_index(self->video_codec_ids, self->n_video_codec_ids, "auto");
    if (vc_auto >= 0)
        adw_combo_row_set_selected(self->video_codec_row, (guint)vc_auto);
    int vc_idx = find_id_index(self->video_codec_ids, self->n_video_codec_ids, m->codec);
    if (vc_idx >= 0)
        adw_combo_row_set_selected(self->video_codec_row, (guint)vc_idx);

    /* Color range */
    adw_combo_row_set_selected(self->color_range_row,
        color_range_string_to_index(m->color_range));

    /* FPS */
    if (m->fps > 0)
        adw_spin_row_set_value(self->fps_row, m->fps);

    /* Framerate mode */
    adw_combo_row_set_selected(self->framerate_mode_row,
        framerate_mode_string_to_index(m->framerate_mode));

    /* Overclock */
    adw_switch_row_set_active(self->overclock_row, m->overclock);

    /* Record cursor */
    adw_switch_row_set_active(self->record_cursor_row, m->record_cursor);

    /* ── Notifications ── */
    adw_switch_row_set_active(self->notify_started_row,
        m->show_recording_started_notifications);
    adw_switch_row_set_active(self->notify_stopped_row,
        m->show_recording_stopped_notifications);
    adw_switch_row_set_active(self->notify_saved_row,
        m->show_recording_saved_notifications);

    /* Trigger visibility callbacks */
    on_record_area_changed(G_OBJECT(self->record_area_row), NULL, self);
    on_quality_changed(G_OBJECT(self->quality_row), NULL, self);
}

void
gsr_config_page_read_config(GsrConfigPage *self, GsrConfig *config)
{
    GsrMainConfig *m = &config->main_config;

    /* ── Capture Target ── */
    guint ra_idx = adw_combo_row_get_selected(self->record_area_row);
    g_free(m->record_area_option);
    m->record_area_option = (ra_idx < (guint)self->n_record_area_ids)
        ? g_strdup(self->record_area_ids[ra_idx])
        : g_strdup("");

    m->change_video_resolution = adw_switch_row_get_active(self->change_resolution_row);
    m->video_width = (int32_t)adw_spin_row_get_value(self->video_width_row);
    m->video_height = (int32_t)adw_spin_row_get_value(self->video_height_row);
    m->record_area_width = (int32_t)adw_spin_row_get_value(self->area_width_row);
    m->record_area_height = (int32_t)adw_spin_row_get_value(self->area_height_row);
    m->restore_portal_session = adw_switch_row_get_active(self->restore_portal_row);

    /* ── Audio ── */

    /* Clear old audio_input array */
    if (m->audio_input) {
        for (int i = 0; i < m->n_audio_input; i++)
            g_free(m->audio_input[i]);
        g_free(m->audio_input);
        m->audio_input = NULL;
        m->n_audio_input = 0;
    }

    /* Walk audio rows */
    int n_audio = 0;
    int audio_cap = 0;
    char **audio_arr = NULL;

    for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->audio_rows_box));
         child != NULL;
         child = gtk_widget_get_next_sibling(child))
    {
        const char *track_type = g_object_get_data(G_OBJECT(child), "audio-track-type");
        if (!track_type) continue;

        char *value = NULL;

        if (g_str_equal(track_type, "device")) {
            GtkWidget *input_w = g_object_get_data(G_OBJECT(child), "input-widget");
            if (GTK_IS_DROP_DOWN(input_w)) {
                GtkDropDown *dd = GTK_DROP_DOWN(input_w);
                guint sel = gtk_drop_down_get_selected(dd);
                GtkStringList *model = GTK_STRING_LIST(
                    g_object_get_data(G_OBJECT(child), "string-list"));
                if (model && sel < g_list_model_get_n_items(G_LIST_MODEL(model))) {
                    const char *desc = gtk_string_list_get_string(model, sel);
                    value = g_strdup_printf("device:%s", desc ? desc : "");
                }
            }
        } else if (g_str_equal(track_type, "app")) {
            GtkWidget *input_w = g_object_get_data(G_OBJECT(child), "input-widget");
            if (GTK_IS_DROP_DOWN(input_w)) {
                GtkDropDown *dd = GTK_DROP_DOWN(input_w);
                guint sel = gtk_drop_down_get_selected(dd);
                GtkStringList *model = GTK_STRING_LIST(
                    g_object_get_data(G_OBJECT(child), "string-list"));
                if (model && sel < g_list_model_get_n_items(G_LIST_MODEL(model))) {
                    const char *app = gtk_string_list_get_string(model, sel);
                    value = g_strdup_printf("app:%s", app ? app : "");
                }
            }
        } else if (g_str_equal(track_type, "app-custom")) {
            GtkWidget *input_w = g_object_get_data(G_OBJECT(child), "input-widget");
            if (GTK_IS_ENTRY(input_w)) {
                const char *text = gtk_editable_get_text(GTK_EDITABLE(input_w));
                value = g_strdup_printf("app:%s", text ? text : "");
            }
        }

        if (value) {
            if (n_audio >= audio_cap) {
                audio_cap = audio_cap ? audio_cap * 2 : 8;
                audio_arr = g_realloc(audio_arr, sizeof(char *) * (gsize)(audio_cap + 1));
            }
            audio_arr[n_audio] = value;
            n_audio++;
            audio_arr[n_audio] = NULL;
        }
    }

    m->audio_input = audio_arr;
    m->n_audio_input = n_audio;

    /* Split audio (inverted to merge) */
    m->merge_audio_tracks = !adw_switch_row_get_active(self->split_audio_row);
    m->record_app_audio_inverted = adw_switch_row_get_active(self->app_audio_inverted_row);

    /* Audio codec */
    g_free(m->audio_codec);
    m->audio_codec = g_strdup(audio_codec_index_to_string(
        adw_combo_row_get_selected(self->audio_codec_row)));

    /* ── Video ── */
    g_free(m->quality);
    m->quality = g_strdup(quality_index_to_string(
        adw_combo_row_get_selected(self->quality_row)));

    m->video_bitrate = (int32_t)adw_spin_row_get_value(self->bitrate_row);

    /* Video codec */
    guint vc_idx = adw_combo_row_get_selected(self->video_codec_row);
    g_free(m->codec);
    m->codec = (vc_idx < (guint)self->n_video_codec_ids)
        ? g_strdup(self->video_codec_ids[vc_idx])
        : g_strdup("auto");

    /* Color range */
    g_free(m->color_range);
    m->color_range = g_strdup(color_range_index_to_string(
        adw_combo_row_get_selected(self->color_range_row)));

    m->fps = (int32_t)adw_spin_row_get_value(self->fps_row);

    g_free(m->framerate_mode);
    m->framerate_mode = g_strdup(framerate_mode_index_to_string(
        adw_combo_row_get_selected(self->framerate_mode_row)));

    m->overclock = adw_switch_row_get_active(self->overclock_row);
    m->record_cursor = adw_switch_row_get_active(self->record_cursor_row);

    /* ── Notifications ── */
    m->show_recording_started_notifications =
        adw_switch_row_get_active(self->notify_started_row);
    m->show_recording_stopped_notifications =
        adw_switch_row_get_active(self->notify_stopped_row);
    m->show_recording_saved_notifications =
        adw_switch_row_get_active(self->notify_saved_row);
}

/* ── Command-line helpers (Phase 5) ──────────────────────────────── */

const char *
gsr_config_page_get_record_area_id(GsrConfigPage *self)
{
    guint idx = adw_combo_row_get_selected(self->record_area_row);
    if (idx < (guint)self->n_record_area_ids)
        return self->record_area_ids[idx];
    return "";
}

const char *
gsr_config_page_get_video_codec_id(GsrConfigPage *self)
{
    guint idx = adw_combo_row_get_selected(self->video_codec_row);
    if (idx < (guint)self->n_video_codec_ids)
        return self->video_codec_ids[idx];
    return "auto";
}

gboolean
gsr_config_page_get_app_audio_inverted(GsrConfigPage *self)
{
    return adw_switch_row_get_active(self->app_audio_inverted_row);
}

/* ── Scalar getters ──────────────────────────────────────────────── */

int
gsr_config_page_get_fps(GsrConfigPage *self)
{
    return (int)adw_spin_row_get_value(self->fps_row);
}

const char *
gsr_config_page_get_quality_id(GsrConfigPage *self)
{
    return quality_index_to_string(adw_combo_row_get_selected(self->quality_row));
}

int
gsr_config_page_get_video_bitrate(GsrConfigPage *self)
{
    return (int)adw_spin_row_get_value(self->bitrate_row);
}

const char *
gsr_config_page_get_color_range_id(GsrConfigPage *self)
{
    return color_range_index_to_string(
        adw_combo_row_get_selected(self->color_range_row));
}

const char *
gsr_config_page_get_audio_codec_id(GsrConfigPage *self)
{
    return audio_codec_index_to_string(
        adw_combo_row_get_selected(self->audio_codec_row));
}

const char *
gsr_config_page_get_framerate_mode_id(GsrConfigPage *self)
{
    return framerate_mode_index_to_string(
        adw_combo_row_get_selected(self->framerate_mode_row));
}

gboolean
gsr_config_page_get_record_cursor(GsrConfigPage *self)
{
    return adw_switch_row_get_active(self->record_cursor_row);
}

gboolean
gsr_config_page_get_overclock(GsrConfigPage *self)
{
    return adw_switch_row_get_active(self->overclock_row);
}

gboolean
gsr_config_page_get_restore_portal_session(GsrConfigPage *self)
{
    return adw_switch_row_get_active(self->restore_portal_row);
}

gboolean
gsr_config_page_get_change_video_resolution(GsrConfigPage *self)
{
    return adw_switch_row_get_active(self->change_resolution_row);
}

int
gsr_config_page_get_video_width(GsrConfigPage *self)
{
    return (int)adw_spin_row_get_value(self->video_width_row);
}

int
gsr_config_page_get_video_height(GsrConfigPage *self)
{
    return (int)adw_spin_row_get_value(self->video_height_row);
}

int
gsr_config_page_get_area_width(GsrConfigPage *self)
{
    return (int)adw_spin_row_get_value(self->area_width_row);
}

int
gsr_config_page_get_area_height(GsrConfigPage *self)
{
    return (int)adw_spin_row_get_value(self->area_height_row);
}

gboolean
gsr_config_page_get_split_audio(GsrConfigPage *self)
{
    return adw_switch_row_get_active(self->split_audio_row);
}

gboolean
gsr_config_page_get_notify_started(GsrConfigPage *self)
{
    return adw_switch_row_get_active(self->notify_started_row);
}

gboolean
gsr_config_page_get_notify_stopped(GsrConfigPage *self)
{
    return adw_switch_row_get_active(self->notify_stopped_row);
}

gboolean
gsr_config_page_get_notify_saved(GsrConfigPage *self)
{
    return adw_switch_row_get_active(self->notify_saved_row);
}

GPtrArray *
gsr_config_page_build_audio_args(GsrConfigPage *self, gboolean merge_tracks)
{
    /*
     * Walk the audio_rows_box children. Each child has:
     * - "audio-track-type": "device", "app", or "app-custom"
     * - "input-widget": GtkDropDown (device/app) or GtkEntry (app-custom)
     * - "device-names": char** (only for device rows)
     *
     * For device rows: the real ID is device-names[selected_index].
     * For app rows: the display string IS the app name.
     * For app-custom: the entry text IS the app name.
     *
     * If app_audio_inverted: prefix app tracks with "app-inverse:" instead of "app:".
     *
     * Returns a GPtrArray of individual track strings.
     * If merge_tracks, all tracks are merged into one pipe-delimited string.
     */
    gboolean inverted = adw_switch_row_get_active(self->app_audio_inverted_row);

    GPtrArray *tracks = g_ptr_array_new_with_free_func(g_free);

    for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->audio_rows_box));
         child != NULL;
         child = gtk_widget_get_next_sibling(child))
    {
        const char *track_type = g_object_get_data(G_OBJECT(child), "audio-track-type");
        if (!track_type) continue;

        char *value = NULL;

        if (g_str_equal(track_type, "device")) {
            GtkWidget *input_w = g_object_get_data(G_OBJECT(child), "input-widget");
            char **device_names = g_object_get_data(G_OBJECT(child), "device-names");
            if (GTK_IS_DROP_DOWN(input_w) && device_names) {
                guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(input_w));
                /* Count device names to bounds-check */
                guint n_names = 0;
                while (device_names[n_names]) n_names++;
                if (sel < n_names)
                    value = g_strdup(device_names[sel]);
            }
        } else if (g_str_equal(track_type, "app")) {
            GtkWidget *input_w = g_object_get_data(G_OBJECT(child), "input-widget");
            if (GTK_IS_DROP_DOWN(input_w)) {
                GtkStringList *model = GTK_STRING_LIST(
                    g_object_get_data(G_OBJECT(child), "string-list"));
                guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(input_w));
                if (model && sel < g_list_model_get_n_items(G_LIST_MODEL(model))) {
                    const char *app = gtk_string_list_get_string(model, sel);
                    const char *prefix = inverted ? "app-inverse:" : "app:";
                    value = g_strdup_printf("%s%s", prefix, app ? app : "");
                }
            }
        } else if (g_str_equal(track_type, "app-custom")) {
            GtkWidget *input_w = g_object_get_data(G_OBJECT(child), "input-widget");
            if (GTK_IS_ENTRY(input_w)) {
                const char *text = gtk_editable_get_text(GTK_EDITABLE(input_w));
                const char *prefix = inverted ? "app-inverse:" : "app:";
                value = g_strdup_printf("%s%s", prefix, text ? text : "");
            }
        }

        if (value)
            g_ptr_array_add(tracks, value);
    }

    if (merge_tracks && tracks->len > 1) {
        /* Merge all into one pipe-delimited string */
        GString *merged = g_string_new(g_ptr_array_index(tracks, 0));
        for (guint i = 1; i < tracks->len; i++) {
            g_string_append_c(merged, '|');
            g_string_append(merged, g_ptr_array_index(tracks, i));
        }
        g_ptr_array_set_size(tracks, 0);
        g_ptr_array_add(tracks, g_string_free(merged, FALSE));
    }

    return tracks;
}

unsigned long
gsr_config_page_get_selected_window(GsrConfigPage *self)
{
    g_return_val_if_fail(GSR_IS_CONFIG_PAGE(self), 0);
#ifdef HAVE_X11
    return self->selected_window_id;
#else
    return 0;
#endif
}

gboolean
gsr_config_page_has_valid_window_selection(GsrConfigPage *self)
{
    g_return_val_if_fail(GSR_IS_CONFIG_PAGE(self), FALSE);

    const char *area_id = gsr_config_page_get_record_area_id(self);
    if (!g_str_equal(area_id, "window"))
        return TRUE; /* not in window mode, always valid */

#ifdef HAVE_X11
    return self->selected_window_id != 0;
#else
    return FALSE;
#endif
}
