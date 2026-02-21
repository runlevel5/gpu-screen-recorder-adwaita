#include "gsr-window.h"
#include "gsr-info.h"
#include "gsr-config.h"
#include "gsr-config-page.h"
#include "gsr-stream-page.h"
#include "gsr-record-page.h"
#include "gsr-replay-page.h"
#include "gsr-hotkeys.h"

#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

struct _GsrWindow {
    AdwApplicationWindow parent_instance;

    /* Header bar */
    AdwHeaderBar       *header_bar;
    AdwViewSwitcher    *header_switcher;
    GtkStack           *header_title_stack;
    GtkLabel           *header_title_label;
    GtkMenuButton      *menu_button;

    /* View stack & bottom switcher */
    AdwViewStack       *view_stack;
    AdwViewSwitcherBar *view_switcher_bar;

    /* Toast overlay */
    AdwToastOverlay    *toast_overlay;

    /* System info (owned, lifetime = window) */
    GsrInfo             info;

    /* Config (owned, lifetime = window) */
    GsrConfig           config;

    /* Pages */
    GsrConfigPage      *config_page;
    GsrStreamPage      *stream_page;
    GsrRecordPage      *record_page;
    GsrReplayPage      *replay_page;

    /* ── Hotkeys ─── */
    GsrHotkeys         *hotkeys;
#ifdef HAVE_WAYLAND
    gboolean            wayland_shortcuts_registered;
#endif

    /* ── Process management ─── */
    pid_t               child_pid;          /* -1 when idle */
    int                 prev_exit_status;
    GsrActiveMode       active_mode;
    char               *record_filename;    /* owned, recording only */
    guint               poll_timer_id;      /* 500ms poll timer */

    /* ── Desktop notifications ─── */
    gboolean            showing_notification;
    gboolean            is_kde;             /* KDE workaround */

    /* ── Startup error state ─── */
    GsrInfoExitStatus   info_status;        /* cached for deferred dialog */
};

G_DEFINE_FINAL_TYPE(GsrWindow, gsr_window, ADW_TYPE_APPLICATION_WINDOW)

/* ── Forward declarations ────────────────────────────────────────── */

static void handle_child_death(GsrWindow *self, int exit_status);
static void send_notification(GsrWindow *self, const char *title,
                              const char *body, GNotificationPriority priority);

/* ── Desktop notification helpers ────────────────────────────────── */

/**
 * Withdraw the desktop notification if its timeout has elapsed.
 * Called as a one-shot GLib timeout.
 */
static gboolean
on_notification_withdraw(gpointer user_data)
{
    GsrWindow *self = GSR_WINDOW(user_data);

    if (self->showing_notification) {
        GtkApplication *app = GTK_APPLICATION(
            gtk_window_get_application(GTK_WINDOW(self)));
        if (app)
            g_application_withdraw_notification(G_APPLICATION(app),
                "gpu-screen-recorder");
        self->showing_notification = FALSE;
    }

    return G_SOURCE_REMOVE; /* one-shot */
}

/**
 * Send a desktop notification AND an in-app toast.
 *
 * GNotification goes to the desktop notification daemon via GApplication;
 * AdwToast shows inside our window.  The desktop notification is auto-withdrawn
 * after 3 s (normal) or 10 s (urgent).
 *
 * KDE workaround: when a capture is active, KDE suppresses normal-priority
 * notifications because the app is "screen sharing".  We force URGENT in that
 * case so the user still sees them.
 */
static void
send_notification(GsrWindow *self, const char *title,
                  const char *body, GNotificationPriority priority)
{
    GtkApplication *app = GTK_APPLICATION(
        gtk_window_get_application(GTK_WINDOW(self)));
    if (!app)
        return;

    /* ── Desktop notification ── */
    GNotification *notif = g_notification_new(title);
    g_notification_set_body(notif, body);

    /* KDE workaround: force urgent while capturing */
    GNotificationPriority effective = priority;
    if (self->is_kde && self->child_pid > 0 &&
        effective < G_NOTIFICATION_PRIORITY_URGENT)
        effective = G_NOTIFICATION_PRIORITY_URGENT;

    g_notification_set_priority(notif, effective);

    /* Cancel any pending withdrawal timer, then send */
    if (self->showing_notification) {
        g_application_withdraw_notification(G_APPLICATION(app),
            "gpu-screen-recorder");
    }
    g_application_send_notification(G_APPLICATION(app),
        "gpu-screen-recorder", notif);
    self->showing_notification = TRUE;
    g_object_unref(notif);

    /* Auto-withdraw after timeout */
    guint timeout_ms = (effective >= G_NOTIFICATION_PRIORITY_URGENT) ? 10000 : 3000;
    g_timeout_add(timeout_ms, on_notification_withdraw, self);

    /* ── In-app toast ── */
    AdwToast *toast = adw_toast_new(body);
    adw_toast_set_timeout(toast, (effective >= G_NOTIFICATION_PRIORITY_URGENT) ? 10 : 3);
    adw_toast_overlay_add_toast(self->toast_overlay, toast);
}

/* ── Container compatibility fix ─────────────────────────────────── */

static const char *
fix_container_for_codec(const char *container, const char *codec)
{
    gboolean is_vp = g_str_equal(codec, "vp8") || g_str_equal(codec, "vp9");

    if (is_vp) {
        /* VP8/VP9 needs webm or matroska */
        if (!g_str_equal(container, "webm") && !g_str_equal(container, "matroska"))
            return "webm";
    } else {
        /* Non-VP codec in webm container → force mp4 */
        if (g_str_equal(container, "webm"))
            return "mp4";
    }
    return container;
}

/* ── Resolve codec for "auto" ────────────────────────────────────── */

static void
resolve_codec_and_encoder(GsrWindow *self, const char **out_codec,
                          gboolean *out_use_software)
{
    const char *selected = gsr_config_page_get_video_codec_id(self->config_page);
    *out_use_software = FALSE;

    if (g_str_equal(selected, "h264_software")) {
        *out_codec = "h264";
        *out_use_software = TRUE;
        return;
    }

    if (g_str_equal(selected, "auto")) {
        const char *hw = gsr_info_get_first_usable_hw_video_codec(&self->info);
        if (hw) {
            *out_codec = hw;
        } else {
            /* Fallback to h264 software */
            *out_codec = "h264";
            *out_use_software = TRUE;
        }
        return;
    }

    *out_codec = selected;
}

/* ── Build recording filename ────────────────────────────────────── */

static char *
build_record_filename(const char *dir, const char *container_display)
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char date_buf[64];
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d_%H-%M-%S", tm);
    return g_strdup_printf("%s/Video_%s.%s", dir, date_buf, container_display);
}

/* Map internal container ID to display extension for filename */
static const char *
container_id_to_extension(const char *id)
{
    if (g_str_equal(id, "matroska")) return "mkv";
    if (g_str_equal(id, "mpegts"))   return "ts";
    if (g_str_equal(id, "hls"))      return "m3u8";
    return id; /* mp4, flv, mov, webm pass through */
}

/* ── Build command-line args ─────────────────────────────────────── */

static GPtrArray *
build_command_args(GsrWindow *self, GsrActiveMode mode)
{
    GPtrArray *args = g_ptr_array_new_with_free_func(g_free);

    g_ptr_array_add(args, g_strdup("gpu-screen-recorder"));

    /* ── Record area / window ─── */
    const char *area_id = gsr_config_page_get_record_area_id(self->config_page);
    g_ptr_array_add(args, g_strdup("-w"));

    if (g_str_equal(area_id, "focused")) {
        int aw = gsr_config_page_get_area_width(self->config_page);
        int ah = gsr_config_page_get_area_height(self->config_page);
        g_ptr_array_add(args, g_strdup_printf("focused:%dx%d", aw, ah));
    } else if (g_str_equal(area_id, "portal")) {
        g_ptr_array_add(args, g_strdup("portal"));
    } else if (g_str_equal(area_id, "window")) {
        unsigned long wid = gsr_config_page_get_selected_window(self->config_page);
        if (wid == 0) {
            g_ptr_array_unref(args);
            return NULL;
        }
        g_ptr_array_add(args, g_strdup_printf("%lu", wid));
    } else {
        /* Monitor name */
        g_ptr_array_add(args, g_strdup(area_id));
    }

    /* ── Codec & encoder ─── */
    const char *codec = NULL;
    gboolean use_software = FALSE;
    resolve_codec_and_encoder(self, &codec, &use_software);

    /* ── Container (mode-specific, with compat fix) ─── */
    char *container_owned = NULL;
    const char *container = NULL;

    switch (mode) {
    case GSR_ACTIVE_MODE_STREAM:
        container_owned = gsr_stream_page_get_container(self->stream_page);
        container = container_owned;
        break;
    case GSR_ACTIVE_MODE_RECORD:
        container_owned = gsr_record_page_get_container(self->record_page);
        container = container_owned;
        break;
    case GSR_ACTIVE_MODE_REPLAY:
        container_owned = gsr_replay_page_get_container(self->replay_page);
        container = container_owned;
        break;
    default:
        container = "mp4";
        break;
    }

    container = fix_container_for_codec(container, codec);

    g_ptr_array_add(args, g_strdup("-c"));
    g_ptr_array_add(args, g_strdup(container));

    g_ptr_array_add(args, g_strdup("-k"));
    g_ptr_array_add(args, g_strdup(codec));

    /* Audio codec */
    g_ptr_array_add(args, g_strdup("-ac"));
    g_ptr_array_add(args, g_strdup(gsr_config_page_get_audio_codec_id(self->config_page)));

    /* FPS */
    g_ptr_array_add(args, g_strdup("-f"));
    g_ptr_array_add(args, g_strdup_printf("%d", gsr_config_page_get_fps(self->config_page)));

    /* Cursor */
    g_ptr_array_add(args, g_strdup("-cursor"));
    g_ptr_array_add(args, g_strdup(
        gsr_config_page_get_record_cursor(self->config_page) ? "yes" : "no"));

    /* Restore portal session */
    g_ptr_array_add(args, g_strdup("-restore-portal-session"));
    g_ptr_array_add(args, g_strdup(
        gsr_config_page_get_restore_portal_session(self->config_page) ? "yes" : "no"));

    /* Color range */
    g_ptr_array_add(args, g_strdup("-cr"));
    g_ptr_array_add(args, g_strdup(gsr_config_page_get_color_range_id(self->config_page)));

    /* Encoder */
    g_ptr_array_add(args, g_strdup("-encoder"));
    g_ptr_array_add(args, g_strdup(use_software ? "cpu" : "gpu"));

    /* ── Quality args ─── */
    const char *quality = gsr_config_page_get_quality_id(self->config_page);
    if (g_str_equal(quality, "custom")) {
        g_ptr_array_add(args, g_strdup("-bm"));
        g_ptr_array_add(args, g_strdup("cbr"));
        g_ptr_array_add(args, g_strdup("-q"));
        g_ptr_array_add(args, g_strdup_printf("%d",
            gsr_config_page_get_video_bitrate(self->config_page)));
    } else {
        g_ptr_array_add(args, g_strdup("-q"));
        g_ptr_array_add(args, g_strdup(quality));
    }

    /* ── Framerate mode ─── */
    const char *fm = gsr_config_page_get_framerate_mode_id(self->config_page);
    if (!g_str_equal(fm, "auto")) {
        g_ptr_array_add(args, g_strdup("-fm"));
        g_ptr_array_add(args, g_strdup(fm));
    }

    /* ── Resolution ─── */
    if (gsr_config_page_get_change_video_resolution(self->config_page) &&
        !g_str_equal(area_id, "focused"))
    {
        int vw = gsr_config_page_get_video_width(self->config_page);
        int vh = gsr_config_page_get_video_height(self->config_page);
        g_ptr_array_add(args, g_strdup("-s"));
        g_ptr_array_add(args, g_strdup_printf("%dx%d", vw, vh));
    }

    /* ── Overclock ─── */
    if (gsr_config_page_get_overclock(self->config_page)) {
        g_ptr_array_add(args, g_strdup("-oc"));
        g_ptr_array_add(args, g_strdup("yes"));
    }

    /* ── Audio args ─── */
    gboolean split = gsr_config_page_get_split_audio(self->config_page);
    gboolean merge = !split;
    GPtrArray *audio_tracks = gsr_config_page_build_audio_args(self->config_page, merge);
    for (guint i = 0; i < audio_tracks->len; i++) {
        g_ptr_array_add(args, g_strdup("-a"));
        g_ptr_array_add(args, g_strdup(g_ptr_array_index(audio_tracks, i)));
    }
    g_ptr_array_unref(audio_tracks);

    /* ── Mode-specific: output (-o) and extra flags ─── */
    switch (mode) {
    case GSR_ACTIVE_MODE_REPLAY: {
        int replay_time = gsr_replay_page_get_time(self->replay_page);
        g_ptr_array_add(args, g_strdup("-r"));
        g_ptr_array_add(args, g_strdup_printf("%d", replay_time));

        const char *save_dir = gsr_replay_page_get_save_dir(self->replay_page);
        g_ptr_array_add(args, g_strdup("-o"));
        g_ptr_array_add(args, g_strdup(save_dir ? save_dir : "/tmp"));
        break;
    }
    case GSR_ACTIVE_MODE_RECORD: {
        const char *save_dir = gsr_record_page_get_save_dir(self->record_page);
        const char *ext = container_id_to_extension(container);
        char *filename = build_record_filename(
            save_dir ? save_dir : "/tmp", ext);

        g_free(self->record_filename);
        self->record_filename = g_strdup(filename);

        g_ptr_array_add(args, g_strdup("-o"));
        g_ptr_array_add(args, filename); /* transfers ownership */
        break;
    }
    case GSR_ACTIVE_MODE_STREAM: {
        char *url = gsr_stream_page_get_stream_url(self->stream_page);
        g_ptr_array_add(args, g_strdup("-o"));
        g_ptr_array_add(args, url); /* transfers ownership */
        break;
    }
    default:
        break;
    }

    g_free(container_owned);

    /* NULL-terminate for execvp */
    g_ptr_array_add(args, NULL);

    return args;
}

/* ── fork/exec ───────────────────────────────────────────────────── */

static gboolean
start_child_process(GsrWindow *self, GPtrArray *args)
{
    pid_t pid = fork();
    if (pid == -1) {
        g_warning("fork() failed: %s", g_strerror(errno));
        return FALSE;
    }

    if (pid == 0) {
        /* Child process */
#ifdef __linux__
        prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
        execvp(g_ptr_array_index(args, 0), (char **)args->pdata);
        /* If execvp returns, it failed */
        _exit(127);
    }

    /* Parent */
    self->child_pid = pid;

    /* Log the command line for debugging */
    g_autofree char *cmdline = g_strjoinv(" ", (char **)args->pdata);
    g_debug("Started gpu-screen-recorder (pid=%d): %s", pid, cmdline);

    return TRUE;
}

/* ── Kill helper (matches kill_gpu_screen_recorder_get_result) ───── */

static gboolean
kill_and_wait(pid_t pid, gboolean *already_dead)
{
    if (already_dead) *already_dead = FALSE;

    if (pid <= 0) {
        if (already_dead) *already_dead = TRUE;
        return TRUE;
    }

    /* Check if already dead */
    int status = 0;
    pid_t ret = waitpid(pid, &status, WNOHANG);
    if (ret == pid) {
        /* Already exited */
        if (already_dead) *already_dead = TRUE;
        if (WIFEXITED(status))
            return WEXITSTATUS(status) == 0;
        return FALSE;
    }

    /* Still running — send SIGINT and block until exit */
    kill(pid, SIGINT);
    ret = waitpid(pid, &status, 0);
    if (ret == pid && WIFEXITED(status))
        return WEXITSTATUS(status) == 0;
    return FALSE;
}

/* ── 500ms poll timer ────────────────────────────────────────────── */

static gboolean
on_poll_timer(gpointer user_data)
{
    GsrWindow *self = GSR_WINDOW(user_data);

    if (self->child_pid <= 0)
        return G_SOURCE_CONTINUE;

    /* Check if child died */
    int status = 0;
    pid_t ret = waitpid(self->child_pid, &status, WNOHANG);
    if (ret == self->child_pid) {
        /* Child died */
        int exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        self->child_pid = -1;
        handle_child_death(self, exit_status);
    }

    return G_SOURCE_CONTINUE;
}

/* ── Handle unexpected child death ───────────────────────────────── */

static void
handle_child_death(GsrWindow *self, int exit_status)
{
    self->prev_exit_status = exit_status;
    GsrActiveMode mode = self->active_mode;

    g_debug("Child died with exit_status=%d, mode=%d", exit_status, mode);

    /* Stop the poll timer */
    if (self->poll_timer_id) {
        g_source_remove(self->poll_timer_id);
        self->poll_timer_id = 0;
    }

    /* Enter the "stopped" state on the appropriate page */
    switch (mode) {
    case GSR_ACTIVE_MODE_STREAM:
        gsr_stream_page_set_active(self->stream_page, FALSE);
        break;
    case GSR_ACTIVE_MODE_RECORD:
        gsr_record_page_set_active(self->record_page, FALSE);
        break;
    case GSR_ACTIVE_MODE_REPLAY:
        gsr_replay_page_set_active(self->replay_page, FALSE);
        break;
    default:
        break;
    }

    self->active_mode = GSR_ACTIVE_MODE_NONE;
    gsr_window_set_recording_active(self, FALSE);

    /* Show notification based on exit code */
    if (exit_status == 60) {
        /* Canceled by user — silent */
    } else if (exit_status == 0) {
        /* Success */
        if (mode == GSR_ACTIVE_MODE_RECORD && self->record_filename) {
            if (gsr_config_page_get_notify_saved(self->config_page)) {
                char *msg = g_strdup_printf("Recording saved to %s",
                    self->record_filename);
                send_notification(self, "GPU Screen Recorder", msg,
                    G_NOTIFICATION_PRIORITY_NORMAL);
                g_free(msg);
            }
        } else if (gsr_config_page_get_notify_stopped(self->config_page)) {
            const char *mode_str =
                mode == GSR_ACTIVE_MODE_STREAM ? "streaming" :
                mode == GSR_ACTIVE_MODE_REPLAY ? "replay" : "recording";
            char *msg = g_strdup_printf("Stopped %s", mode_str);
            send_notification(self, "GPU Screen Recorder", msg,
                G_NOTIFICATION_PRIORITY_NORMAL);
            g_free(msg);
        }
    } else {
        /* Error — always notify regardless of user prefs */
        char *msg = NULL;
        if (exit_status == 10)
            msg = g_strdup_printf("You need to have pkexec installed and have "
                "a polkit agent running to record your monitor");
        else if (exit_status == 50)
            msg = g_strdup_printf("Desktop portal capture failed. Either you "
                "canceled the desktop portal or your Wayland compositor "
                "doesn't support desktop portal capture or it's incorrectly "
                "setup on your system");
        else
            msg = g_strdup_printf("Failed to save video. Either your graphics "
                "card doesn't support GPU Screen Recorder with the settings "
                "you used or you don't have enough disk space. "
                "Start GPU Screen Recorder from the terminal for more info");
        send_notification(self, "GPU Screen Recorder", msg,
            G_NOTIFICATION_PRIORITY_URGENT);
        g_free(msg);
    }
}

/* ── Actions ─────────────────────────────────────────────────────── */

static void
save_config(GsrWindow *self)
{
    /* Read current widget state into config struct */
    gsr_config_page_read_config(self->config_page, &self->config);
    gsr_stream_page_read_config(self->stream_page, &self->config);
    gsr_record_page_read_config(self->record_page, &self->config);
    gsr_replay_page_read_config(self->replay_page, &self->config);

    /* Persist view-mode */
    GAction *action = g_action_map_lookup_action(G_ACTION_MAP(self), "view-mode");
    if (action) {
        GVariant *state = g_action_get_state(action);
        const char *mode = g_variant_get_string(state, NULL);
        self->config.main_config.advanced_view = g_str_equal(mode, "advanced");
        g_variant_unref(state);
    }

    gsr_config_save(&self->config);
}

static void
on_view_mode_change(GSimpleAction *action,
                    GVariant      *parameter,
                    gpointer       user_data)
{
    GsrWindow *self = GSR_WINDOW(user_data);
    const char *mode = g_variant_get_string(parameter, NULL);
    g_simple_action_set_state(action, g_variant_new_string(mode));

    gboolean advanced = g_str_equal(mode, "advanced");
    gsr_config_page_set_advanced(self->config_page, advanced);

    /* Persist the change immediately */
    save_config(self);

    g_debug("View mode changed to: %s", mode);
}

/* ── Close request — save config while widgets are still alive ──── */

static gboolean
on_close_request(GtkWindow *window, gpointer user_data G_GNUC_UNUSED)
{
    GsrWindow *self = GSR_WINDOW(window);

    /* If child is running, kill it before exiting */
    if (self->child_pid > 0) {
        g_debug("Window closing — killing child pid %d", self->child_pid);
        kill(self->child_pid, SIGINT);
        waitpid(self->child_pid, NULL, 0);
        self->child_pid = -1;
    }

    /* Stop poll timer */
    if (self->poll_timer_id) {
        g_source_remove(self->poll_timer_id);
        self->poll_timer_id = 0;
    }

    /* Free hotkeys before the window is destroyed */
    if (self->hotkeys) {
        gsr_hotkeys_free(self->hotkeys);
        self->hotkeys = NULL;
    }

    /* Withdraw any pending desktop notification */
    if (self->showing_notification) {
        GtkApplication *app = GTK_APPLICATION(
            gtk_window_get_application(GTK_WINDOW(self)));
        if (app)
            g_application_withdraw_notification(G_APPLICATION(app),
                "gpu-screen-recorder");
        self->showing_notification = FALSE;
    }

    save_config(self);
    /* Return FALSE to let the default handler proceed */
    return FALSE;
}

/* ── Hamburger menu ──────────────────────────────────────────────── */

static GMenuModel *
create_primary_menu(void)
{
    GMenu *menu = g_menu_new();

    /* View mode section (with "View" header label) */
    GMenu *view_section = g_menu_new();
    GMenuItem *simple_item = g_menu_item_new("Simple", NULL);
    g_menu_item_set_action_and_target_value(simple_item,
        "win.view-mode", g_variant_new_string("simple"));
    g_menu_append_item(view_section, simple_item);
    g_object_unref(simple_item);

    GMenuItem *advanced_item = g_menu_item_new("Advanced", NULL);
    g_menu_item_set_action_and_target_value(advanced_item,
        "win.view-mode", g_variant_new_string("advanced"));
    g_menu_append_item(view_section, advanced_item);
    g_object_unref(advanced_item);

    g_menu_append_section(menu, "View", G_MENU_MODEL(view_section));
    g_object_unref(view_section);

    /* About section */
    GMenu *about_section = g_menu_new();
    g_menu_append(about_section, "Keyboard Shortcuts", "app.shortcuts");
    g_menu_append(about_section, "About", "app.about");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(about_section));
    g_object_unref(about_section);

    return G_MENU_MODEL(menu);
}

/* ── Hotkey: page changed → regrab + register Wayland shortcuts ─── */

static void
on_visible_page_changed(GObject *object G_GNUC_UNUSED,
                        GParamSpec *pspec G_GNUC_UNUSED,
                        gpointer user_data)
{
    GsrWindow *self = GSR_WINDOW(user_data);

    /* X11: re-grab hotkeys for the now-visible page */
#ifdef HAVE_X11
    if (self->hotkeys)
        gsr_hotkeys_regrab_for_visible_page(self->hotkeys);
#endif

    /* Wayland: register shortcuts once when first visiting an action page */
#ifdef HAVE_WAYLAND
    if (self->hotkeys && !self->wayland_shortcuts_registered) {
        const char *page = adw_view_stack_get_visible_child_name(self->view_stack);
        if (page && (!g_str_equal(page, "config"))) {
            self->wayland_shortcuts_registered = TRUE;
            gsr_hotkeys_register_wayland_shortcuts_once(self->hotkeys);
        }
    }
#endif
}

/* ── Startup error dialogs (AdwAlertDialog) ──────────────────────── */

static void
on_fatal_error_response(AdwAlertDialog *dialog G_GNUC_UNUSED,
                        const char *response G_GNUC_UNUSED,
                        gpointer user_data)
{
    GsrWindow *self = GSR_WINDOW(user_data);
    GtkApplication *app = GTK_APPLICATION(
        gtk_window_get_application(GTK_WINDOW(self)));
    if (app)
        g_application_quit(G_APPLICATION(app));
}

static void
show_fatal_error(GsrWindow *self, const char *heading, const char *body)
{
    AdwAlertDialog *dlg = ADW_ALERT_DIALOG(adw_alert_dialog_new(heading, body));
    adw_alert_dialog_add_response(dlg, "ok", "OK");
    adw_alert_dialog_set_default_response(dlg, "ok");
    adw_alert_dialog_set_close_response(dlg, "ok");
    adw_alert_dialog_set_body_use_markup(dlg, TRUE);
    g_signal_connect(dlg, "response",
        G_CALLBACK(on_fatal_error_response), self);
    adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(self));
}

static gboolean
check_startup_errors_idle(gpointer user_data)
{
    GsrWindow *self = GSR_WINDOW(user_data);

    switch (self->info_status) {
    case GSR_INFO_EXIT_FAILED_TO_RUN:
        show_fatal_error(self,
            "Failed to run gpu-screen-recorder",
            "Failed to run the <tt>gpu-screen-recorder</tt> command.\n\n"
            "Make sure <tt>gpu-screen-recorder</tt> is installed and "
            "accessible in your PATH.");
        return G_SOURCE_REMOVE;

    case GSR_INFO_EXIT_OPENGL_FAILED:
        show_fatal_error(self,
            "OpenGL initialization failed",
            "Failed to get OpenGL information.\n\n"
            "Make sure your GPU drivers are properly installed. "
            "You may need to install the Vulkan or Mesa drivers for your GPU.");
        return G_SOURCE_REMOVE;

    case GSR_INFO_EXIT_NO_DRM_CARD:
        show_fatal_error(self,
            "No DRM card found",
            "Failed to find a valid DRM card for your GPU.\n\n"
            "If you are running in a VM, make sure GPU passthrough is "
            "enabled and properly configured.");
        return G_SOURCE_REMOVE;

    case GSR_INFO_EXIT_OK:
        break;
    }

    /* Check display server */
    if (self->info.system_info.display_server == GSR_DISPLAY_SERVER_UNKNOWN) {
        show_fatal_error(self,
            "No display server detected",
            "Neither X11 nor Wayland is running.\n\n"
            "GPU Screen Recorder requires either X11 or Wayland.");
        return G_SOURCE_REMOVE;
    }

    /* Check for monitors (Wayland without portal needs monitors) */
    if (self->info.supported_capture_options.n_monitors == 0 &&
        self->info.system_info.display_server == GSR_DISPLAY_SERVER_WAYLAND &&
        !self->info.supported_capture_options.portal)
    {
        show_fatal_error(self,
            "No monitors found",
            "No monitors to record were found.\n\n"
            "Make sure GPU Screen Recorder is running on the same GPU "
            "that your monitors are connected to. You can use the "
            "<tt>DRI_PRIME</tt> environment variable to choose a GPU.");
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_REMOVE;
}

/* ── GObject boilerplate ─────────────────────────────────────────── */

static void
gsr_window_init(GsrWindow *self)
{
    /* ── Init process state ─── */
    self->child_pid = -1;
    self->prev_exit_status = 0;
    self->active_mode = GSR_ACTIVE_MODE_NONE;
    self->record_filename = NULL;
    self->poll_timer_id = 0;

    /* ── Init notification state ─── */
    self->showing_notification = FALSE;
    const char *desktop = g_getenv("XDG_CURRENT_DESKTOP");
    self->is_kde = desktop && (g_strstr_len(desktop, -1, "KDE") != NULL);

    /* ── Window properties ─── */
    gtk_window_set_title(GTK_WINDOW(self), "GPU Screen Recorder");
    gtk_window_set_default_size(GTK_WINDOW(self), 580, 600);
    gtk_widget_set_size_request(GTK_WIDGET(self), 430, 300);

    /* ── Load system info ─── */
    GsrInfoExitStatus info_status = gsr_info_load(&self->info);
    if (info_status != GSR_INFO_EXIT_OK)
        g_warning("gsr_info_load returned status %d", info_status);

    /* Store for deferred error dialog */
    self->info_status = info_status;

    /* ── Load config ─── */
    gsr_config_init_defaults(&self->config);
    gsr_config_read(&self->config);

    /* ── View stack ─── */
    self->view_stack = ADW_VIEW_STACK(adw_view_stack_new());

    self->config_page = gsr_config_page_new(&self->info);
    self->stream_page = gsr_stream_page_new(&self->info);
    self->record_page = gsr_record_page_new(&self->info);
    self->replay_page = gsr_replay_page_new(&self->info);

    adw_view_stack_add_titled_with_icon(self->view_stack,
        GTK_WIDGET(self->config_page), "config", "Config", "preferences-system-symbolic");
    adw_view_stack_add_titled_with_icon(self->view_stack,
        GTK_WIDGET(self->stream_page), "stream", "Stream", "network-transmit-symbolic");
    adw_view_stack_add_titled_with_icon(self->view_stack,
        GTK_WIDGET(self->record_page), "record", "Record", "media-record-symbolic");
    adw_view_stack_add_titled_with_icon(self->view_stack,
        GTK_WIDGET(self->replay_page), "replay", "Replay", "media-playlist-repeat-symbolic");

    /* ── Header bar with view switcher / title stack ─── */
    self->header_switcher = ADW_VIEW_SWITCHER(adw_view_switcher_new());
    adw_view_switcher_set_stack(self->header_switcher, self->view_stack);
    adw_view_switcher_set_policy(self->header_switcher, ADW_VIEW_SWITCHER_POLICY_WIDE);

    self->header_title_label = GTK_LABEL(gtk_label_new("GPU Screen Recorder"));
    gtk_widget_add_css_class(GTK_WIDGET(self->header_title_label), "title");

    /* Stack to hold both the switcher (wide) and title label (narrow) */
    self->header_title_stack = GTK_STACK(gtk_stack_new());
    gtk_stack_set_transition_type(self->header_title_stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_add_named(self->header_title_stack,
        GTK_WIDGET(self->header_switcher), "switcher");
    gtk_stack_add_named(self->header_title_stack,
        GTK_WIDGET(self->header_title_label), "title");

    self->header_bar = ADW_HEADER_BAR(adw_header_bar_new());
    adw_header_bar_set_centering_policy(self->header_bar,
        ADW_CENTERING_POLICY_STRICT);
    adw_header_bar_set_title_widget(self->header_bar,
        GTK_WIDGET(self->header_title_stack));

    /* Hamburger menu button */
    self->menu_button = GTK_MENU_BUTTON(gtk_menu_button_new());
    gtk_menu_button_set_icon_name(self->menu_button, "open-menu-symbolic");
    gtk_menu_button_set_menu_model(self->menu_button, create_primary_menu());
    adw_header_bar_pack_end(self->header_bar, GTK_WIDGET(self->menu_button));

    /* ── Bottom view switcher bar (narrow mode fallback) ─── */
    self->view_switcher_bar = ADW_VIEW_SWITCHER_BAR(adw_view_switcher_bar_new());
    adw_view_switcher_bar_set_stack(self->view_switcher_bar, self->view_stack);

    /* ── Toast overlay wrapping the view stack ─── */
    self->toast_overlay = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(self->toast_overlay, GTK_WIDGET(self->view_stack));

    /* ── Layout: toolbar-view wrapping the toast overlay ─── */
    AdwToolbarView *toolbar_view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    adw_toolbar_view_add_top_bar(toolbar_view, GTK_WIDGET(self->header_bar));
    adw_toolbar_view_set_content(toolbar_view, GTK_WIDGET(self->toast_overlay));
    adw_toolbar_view_add_bottom_bar(toolbar_view, GTK_WIDGET(self->view_switcher_bar));

    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self),
        GTK_WIDGET(toolbar_view));

    /* ── Breakpoint: narrow window → show title label in header, show bottom bar ─── */
    AdwBreakpoint *bp = adw_breakpoint_new(
        adw_breakpoint_condition_parse("max-width: 550sp"));
    adw_breakpoint_add_setters(bp,
        G_OBJECT(self->header_title_stack), "visible-child-name", "title",
        G_OBJECT(self->view_switcher_bar), "reveal", TRUE,
        NULL);
    adw_application_window_add_breakpoint(ADW_APPLICATION_WINDOW(self), bp);

    /* ── Save config on close ─── */
    g_signal_connect(self, "close-request",
        G_CALLBACK(on_close_request), NULL);

    /* ── Window actions ─── */
    const char *initial_mode = self->config.main_config.advanced_view
        ? "'advanced'" : "'simple'";
    GActionEntry win_actions[] = {
        { .name = "view-mode", .activate = on_view_mode_change,
          .parameter_type = "s", .state = initial_mode },
    };
    g_action_map_add_action_entries(G_ACTION_MAP(self),
        win_actions, G_N_ELEMENTS(win_actions), self);

    /* ── Apply config to all pages ─── */
    gsr_config_page_apply_config(self->config_page, &self->config);
    gsr_stream_page_apply_config(self->stream_page, &self->config);
    gsr_record_page_apply_config(self->record_page, &self->config);
    gsr_replay_page_apply_config(self->replay_page, &self->config);

    /* Apply advanced view mode */
    gsr_config_page_set_advanced(self->config_page,
        self->config.main_config.advanced_view);

    /* ── Hotkeys ─── */
    self->hotkeys = gsr_hotkeys_new(self->info.system_info.display_server, self);
#ifdef HAVE_WAYLAND
    self->wayland_shortcuts_registered = FALSE;
#endif

    /* Re-grab hotkeys whenever the visible page changes */
    g_signal_connect(self->view_stack, "notify::visible-child-name",
        G_CALLBACK(on_visible_page_changed), self);

    /* Initial grab for the default visible page */
#ifdef HAVE_X11
    if (self->hotkeys)
        gsr_hotkeys_regrab_for_visible_page(self->hotkeys);
#endif

    /* ── Deferred startup error check ─── */
    g_idle_add(check_startup_errors_idle, self);
}

static void
gsr_window_finalize(GObject *object)
{
    GsrWindow *self = GSR_WINDOW(object);

    if (self->poll_timer_id) {
        g_source_remove(self->poll_timer_id);
        self->poll_timer_id = 0;
    }

    if (self->hotkeys) {
        gsr_hotkeys_free(self->hotkeys);
        self->hotkeys = NULL;
    }

    g_free(self->record_filename);
    gsr_config_clear(&self->config);
    gsr_info_clear(&self->info);
    G_OBJECT_CLASS(gsr_window_parent_class)->finalize(object);
}

static void
gsr_window_class_init(GsrWindowClass *klass)
{
    GObjectClass *obj_class = G_OBJECT_CLASS(klass);
    obj_class->finalize = gsr_window_finalize;
}

/* ── Public API ──────────────────────────────────────────────────── */

GsrWindow *
gsr_window_new(AdwApplication *app)
{
    return g_object_new(GSR_TYPE_WINDOW,
                        "application", app,
                        NULL);
}

void
gsr_window_set_recording_active(GsrWindow *self, gboolean active)
{
    g_return_if_fail(GSR_IS_WINDOW(self));
    gtk_widget_set_sensitive(GTK_WIDGET(self->header_switcher), !active);
    gtk_widget_set_sensitive(GTK_WIDGET(self->view_switcher_bar), !active);
}

gboolean
gsr_window_start_process(GsrWindow *self, GsrActiveMode mode)
{
    g_return_val_if_fail(GSR_IS_WINDOW(self), FALSE);
    g_return_val_if_fail(self->child_pid <= 0, FALSE);

    /* Validate window selection if in "window" mode */
    if (!gsr_config_page_has_valid_window_selection(self->config_page)) {
        send_notification(self, "GPU Screen Recorder",
            "No window selected! Please select a window first.",
            G_NOTIFICATION_PRIORITY_URGENT);
        return FALSE;
    }

    /* Build command line */
    GPtrArray *args = build_command_args(self, mode);
    if (!args) {
        send_notification(self, "GPU Screen Recorder",
            "Failed to build command (no window selected)",
            G_NOTIFICATION_PRIORITY_URGENT);
        return FALSE;
    }

    /* Fork/exec */
    gboolean ok = start_child_process(self, args);
    g_ptr_array_unref(args);

    if (!ok) {
        const char *mode_str =
            mode == GSR_ACTIVE_MODE_STREAM ? "streaming" :
            mode == GSR_ACTIVE_MODE_RECORD ? "recording" :
            mode == GSR_ACTIVE_MODE_REPLAY ? "replay" : "unknown";
        char *msg = g_strdup_printf("Failed to start %s (failed to fork)", mode_str);
        send_notification(self, "GPU Screen Recorder", msg,
            G_NOTIFICATION_PRIORITY_URGENT);
        g_free(msg);
        return FALSE;
    }

    self->active_mode = mode;

    /* Start 500ms poll timer */
    if (self->poll_timer_id == 0)
        self->poll_timer_id = g_timeout_add(500, on_poll_timer, self);

    /* Show "started" notification */
    if (gsr_config_page_get_notify_started(self->config_page)) {
        const char *mode_str =
            mode == GSR_ACTIVE_MODE_STREAM ? "streaming" :
            mode == GSR_ACTIVE_MODE_RECORD ? "recording" :
            mode == GSR_ACTIVE_MODE_REPLAY ? "replay" : "unknown";
        char *msg = g_strdup_printf("Started %s", mode_str);
        send_notification(self, "GPU Screen Recorder", msg,
            G_NOTIFICATION_PRIORITY_NORMAL);
        g_free(msg);
    }

    return TRUE;
}

gboolean
gsr_window_stop_process(GsrWindow *self, gboolean *already_dead)
{
    g_return_val_if_fail(GSR_IS_WINDOW(self), FALSE);

    if (self->child_pid <= 0) {
        if (already_dead) *already_dead = TRUE;
        return TRUE;
    }

    pid_t pid = self->child_pid;
    self->child_pid = -1;
    GsrActiveMode mode = self->active_mode;

    gboolean success = kill_and_wait(pid, already_dead);

    /* Stop poll timer */
    if (self->poll_timer_id) {
        g_source_remove(self->poll_timer_id);
        self->poll_timer_id = 0;
    }

    self->active_mode = GSR_ACTIVE_MODE_NONE;

    /* Show stop notification (respecting user prefs) */
    if (success && mode == GSR_ACTIVE_MODE_RECORD && self->record_filename) {
        if (gsr_config_page_get_notify_saved(self->config_page)) {
            char *msg = g_strdup_printf("Recording saved to %s",
                self->record_filename);
            send_notification(self, "GPU Screen Recorder", msg,
                G_NOTIFICATION_PRIORITY_NORMAL);
            g_free(msg);
        }
    } else if (gsr_config_page_get_notify_stopped(self->config_page)) {
        const char *mode_str =
            mode == GSR_ACTIVE_MODE_STREAM ? "streaming" :
            mode == GSR_ACTIVE_MODE_RECORD ? "recording" :
            mode == GSR_ACTIVE_MODE_REPLAY ? "replay" : "unknown";
        char *msg = g_strdup_printf("Stopped %s", mode_str);
        send_notification(self, "GPU Screen Recorder", msg,
            G_NOTIFICATION_PRIORITY_NORMAL);
        g_free(msg);
    }

    return success;
}

void
gsr_window_send_signal(GsrWindow *self, int sig)
{
    g_return_if_fail(GSR_IS_WINDOW(self));
    if (self->child_pid > 0)
        kill(self->child_pid, sig);
}

void
gsr_window_notify_replay_saved(GsrWindow *self)
{
    g_return_if_fail(GSR_IS_WINDOW(self));
    if (gsr_config_page_get_notify_saved(self->config_page))
        send_notification(self, "GPU Screen Recorder", "Saved replay",
            G_NOTIFICATION_PRIORITY_NORMAL);
}

void
gsr_window_show_toast(GsrWindow *self, const char *message)
{
    g_return_if_fail(GSR_IS_WINDOW(self));
    AdwToast *toast = adw_toast_new(message);
    adw_toast_set_timeout(toast, 3);
    adw_toast_overlay_add_toast(self->toast_overlay, toast);
}

gboolean
gsr_window_is_process_running(GsrWindow *self)
{
    g_return_val_if_fail(GSR_IS_WINDOW(self), FALSE);
    return self->child_pid > 0;
}

GsrActiveMode
gsr_window_get_active_mode(GsrWindow *self)
{
    g_return_val_if_fail(GSR_IS_WINDOW(self), GSR_ACTIVE_MODE_NONE);
    return self->active_mode;
}

/* ── Hotkey dispatch (called from gsr-hotkeys) ───────────────────── */

const char *
gsr_window_get_visible_page_name(GsrWindow *self)
{
    g_return_val_if_fail(GSR_IS_WINDOW(self), NULL);
    return adw_view_stack_get_visible_child_name(self->view_stack);
}

void
gsr_window_hotkey_start_stop(GsrWindow *self)
{
    g_return_if_fail(GSR_IS_WINDOW(self));

    const char *page = adw_view_stack_get_visible_child_name(self->view_stack);
    if (!page)
        return;

    if (g_str_equal(page, "stream"))
        gsr_stream_page_activate_start_stop(self->stream_page);
    else if (g_str_equal(page, "record"))
        gsr_record_page_activate_start_stop(self->record_page);
    else if (g_str_equal(page, "replay"))
        gsr_replay_page_activate_start_stop(self->replay_page);
}

void
gsr_window_hotkey_pause_unpause(GsrWindow *self)
{
    g_return_if_fail(GSR_IS_WINDOW(self));
    gsr_record_page_activate_pause(self->record_page);
}

void
gsr_window_hotkey_save_replay(GsrWindow *self)
{
    g_return_if_fail(GSR_IS_WINDOW(self));
    gsr_replay_page_activate_save(self->replay_page);
}

#ifdef HAVE_WAYLAND
void
gsr_window_on_wayland_hotkeys_init(GsrWindow *self, gboolean success)
{
    g_return_if_fail(GSR_IS_WINDOW(self));

    gsr_stream_page_set_wayland_hotkeys_supported(self->stream_page, success);
    gsr_record_page_set_wayland_hotkeys_supported(self->record_page, success);
    gsr_replay_page_set_wayland_hotkeys_supported(self->replay_page, success);
}
#endif /* HAVE_WAYLAND */

void
gsr_window_on_hotkey_changed(GsrWindow *self)
{
    g_return_if_fail(GSR_IS_WINDOW(self));

    /* Save config so the new hotkey bindings are persisted */
    save_config(self);

    /* Re-grab X11 hotkeys with the updated bindings */
#ifdef HAVE_X11
    if (self->hotkeys)
        gsr_hotkeys_regrab_for_visible_page(self->hotkeys);
#endif
}

const GsrConfig *
gsr_window_get_config(GsrWindow *self)
{
    g_return_val_if_fail(GSR_IS_WINDOW(self), NULL);
    return &self->config;
}
