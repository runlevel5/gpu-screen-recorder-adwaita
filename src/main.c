#include <adwaita.h>
#include "gsr-window.h"

/* ── About dialog ────────────────────────────────────────────────── */

static void
on_about_action(GSimpleAction *action,
                GVariant      *parameter,
                gpointer       user_data)
{
    (void)action;
    (void)parameter;

    GtkApplication *app = GTK_APPLICATION(user_data);
    GtkWindow *win = gtk_application_get_active_window(app);

    AdwAboutDialog *about = ADW_ABOUT_DIALOG(adw_about_dialog_new());
    adw_about_dialog_set_application_name(about, "GPU Screen Recorder");
    adw_about_dialog_set_version(about, GSR_VERSION);
    adw_about_dialog_set_developer_name(about, "dec05eba");
    adw_about_dialog_set_application_icon(about,
        "com.dec05eba.gpu_screen_recorder");
    adw_about_dialog_set_license_type(about, GTK_LICENSE_GPL_3_0);
    adw_about_dialog_set_website(about,
        "https://github.com/runlevel5/gpu-screen-recorder-adwaita");
    adw_about_dialog_set_issue_url(about,
        "https://github.com/runlevel5/gpu-screen-recorder-adwaita/issues");

    const char *developers[] = { "dec05eba", NULL };
    adw_about_dialog_set_developers(about, developers);

    adw_about_dialog_add_credit_section(about, "Adwaita Port",
        (const char *[]){ "Trung Lê", NULL });

    adw_dialog_present(ADW_DIALOG(about), GTK_WIDGET(win));
}

/* ── Keyboard shortcuts dialog ───────────────────────────────────── */

static void
on_shortcuts_action(GSimpleAction *action,
                    GVariant      *parameter,
                    gpointer       user_data)
{
    (void)action;
    (void)parameter;

    GtkApplication *app = GTK_APPLICATION(user_data);
    GtkWindow *win = gtk_application_get_active_window(app);

    AdwShortcutsDialog *dialog = ADW_SHORTCUTS_DIALOG(
        adw_shortcuts_dialog_new());

    /* ── Stream section ─── */
    AdwShortcutsSection *stream_sec = adw_shortcuts_section_new("Stream");
    adw_shortcuts_section_add(stream_sec,
        adw_shortcuts_item_new("Start / Stop streaming", "<Alt>1"));
    adw_shortcuts_dialog_add(dialog, stream_sec);

    /* ── Record section ─── */
    AdwShortcutsSection *record_sec = adw_shortcuts_section_new("Record");
    adw_shortcuts_section_add(record_sec,
        adw_shortcuts_item_new("Start / Stop recording", "<Alt>1"));
    adw_shortcuts_section_add(record_sec,
        adw_shortcuts_item_new("Pause / Unpause recording", "<Alt>2"));
    adw_shortcuts_dialog_add(dialog, record_sec);

    /* ── Replay section ─── */
    AdwShortcutsSection *replay_sec = adw_shortcuts_section_new("Replay");
    adw_shortcuts_section_add(replay_sec,
        adw_shortcuts_item_new("Start / Stop replay", "<Alt>1"));
    adw_shortcuts_section_add(replay_sec,
        adw_shortcuts_item_new("Save replay", "<Alt>2"));
    adw_shortcuts_dialog_add(dialog, replay_sec);

    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(win));
}

/* ── Application activate ────────────────────────────────────────── */

static void
load_custom_css(void)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider,
        ".recording-active { color: @error_color; }\n"
        ".recording-paused { color: @warning_color; }\n");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static void
on_activate(GtkApplication *app, gpointer user_data)
{
    (void)user_data;

    static gboolean css_loaded = FALSE;
    if (!css_loaded) {
        load_custom_css();
        css_loaded = TRUE;
    }

    GtkWindow *win = gtk_application_get_active_window(app);
    if (win == NULL) {
        win = GTK_WINDOW(gsr_window_new(ADW_APPLICATION(app)));
    }
    gtk_window_present(win);
}

/* ── Main ────────────────────────────────────────────────────────── */

int
main(int argc, char *argv[])
{
    AdwApplication *app = adw_application_new(
        "com.dec05eba.gpu_screen_recorder",
        G_APPLICATION_DEFAULT_FLAGS);

    static const GActionEntry app_actions[] = {
        { .name = "shortcuts", .activate = on_shortcuts_action },
        { .name = "about", .activate = on_about_action },
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app),
        app_actions, G_N_ELEMENTS(app_actions), app);

    gtk_application_set_accels_for_action(GTK_APPLICATION(app),
        "app.shortcuts",
        (const char *[]){ "<Ctrl>question", NULL });

    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
