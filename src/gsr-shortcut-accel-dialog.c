#include "gsr-shortcut-accel-dialog.h"

#include <gdk/gdk.h>

/* ═══════════════════════════════════════════════════════════════════
 *  GsrShortcutAccelDialog — Ptyxis-style key capture dialog
 *
 *  Opens in "editing" (capture) mode.  A GtkEventControllerKey in
 *  capture phase intercepts all key events.
 *
 *  - Escape (no modifiers) → cancel / close
 *  - Backspace (no modifiers) → clear shortcut, emit "shortcut-set"
 *  - Valid key combo → store, switch to display mode, user clicks "Set"
 *
 *  The dialog emits "shortcut-set" with the accelerator string (or NULL
 *  if cleared).
 * ═══════════════════════════════════════════════════════════════════ */

struct _GsrShortcutAccelDialog {
    AdwDialog  parent_instance;

    /* Title of the shortcut being edited (e.g. "Start/Stop streaming") */
    char      *shortcut_title;

    /* Current accelerator from config (for Reset) */
    char      *initial_accel;

    /* Captured accelerator */
    char      *accelerator;
    guint      keyval;
    GdkModifierType modifier;

    /* State */
    gboolean   editing;     /* TRUE = waiting for key press */

    /* Widgets */
    GtkStack  *stack;       /* "capture" / "display" pages */
    GtkLabel  *capture_label;
    GtkLabel  *capture_hint;
    GtkShortcutLabel *display_label;
    GtkButton *set_button;
    GtkButton *cancel_button;
};

G_DEFINE_FINAL_TYPE(GsrShortcutAccelDialog, gsr_shortcut_accel_dialog, ADW_TYPE_DIALOG)

/* ── Signals ─────────────────────────────────────────────────────── */

enum {
    SIGNAL_SHORTCUT_SET,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

/* ── Helpers ─────────────────────────────────────────────────────── */

static void
update_display(GsrShortcutAccelDialog *self)
{
    if (self->editing) {
        gtk_stack_set_visible_child_name(self->stack, "capture");
        gtk_widget_set_sensitive(GTK_WIDGET(self->set_button), FALSE);
    } else {
        if (self->accelerator) {
            gtk_shortcut_label_set_accelerator(self->display_label,
                self->accelerator);
        } else {
            gtk_shortcut_label_set_accelerator(self->display_label, "");
        }
        gtk_stack_set_visible_child_name(self->stack, "display");
        gtk_widget_set_sensitive(GTK_WIDGET(self->set_button), TRUE);
    }
}

static void
set_accelerator(GsrShortcutAccelDialog *self, const char *accel)
{
    g_free(self->accelerator);
    self->accelerator = g_strdup(accel);
}

/**
 * Sanitize modifier mask: only keep standard modifier bits.
 */
static GdkModifierType
sanitize_modifier_mask(GdkModifierType state)
{
    return state & gtk_accelerator_get_default_mod_mask();
}

/**
 * Check if shift should be dropped (the key is the same with or without shift).
 */
static gboolean
should_drop_shift(guint keyval_lower, guint keyval)
{
    /* If lowering the key didn't change it, shift isn't meaningful for this key */
    return keyval_lower == keyval;
}

/* ── Key event handlers ──────────────────────────────────────────── */

static gboolean
on_key_pressed(GtkEventControllerKey *controller,
               guint                  keyval,
               guint                  keycode G_GNUC_UNUSED,
               GdkModifierType        state,
               gpointer               user_data)
{
    GsrShortcutAccelDialog *self = GSR_SHORTCUT_ACCEL_DIALOG(user_data);

    if (!self->editing)
        return GDK_EVENT_PROPAGATE;

    GdkEvent *event = gtk_event_controller_get_current_event(
        GTK_EVENT_CONTROLLER(controller));

    /* Ignore pure modifier key presses */
    if (gdk_key_event_is_modifier(event))
        return GDK_EVENT_PROPAGATE;

    GdkModifierType real_mask = sanitize_modifier_mask(state);
    guint keyval_lower = gdk_keyval_to_lower(keyval);

    /* Normalize ISO_Left_Tab → Tab */
    if (keyval_lower == GDK_KEY_ISO_Left_Tab)
        keyval_lower = GDK_KEY_Tab;

    /* Put shift back if it changed the case of the key */
    if (keyval_lower != keyval)
        real_mask |= GDK_SHIFT_MASK;

    /* Avoid SysRq translation (keep Alt+Print) */
    if (keyval_lower == GDK_KEY_Sys_Req && (real_mask & GDK_ALT_MASK) != 0)
        keyval_lower = GDK_KEY_Print;

    /* Escape with no modifiers = cancel */
    if (real_mask == 0 && keyval_lower == GDK_KEY_Escape) {
        adw_dialog_close(ADW_DIALOG(self));
        return GDK_EVENT_STOP;
    }

    /* Backspace with no modifiers = clear shortcut */
    if (real_mask == 0 && keyval_lower == GDK_KEY_BackSpace) {
        set_accelerator(self, NULL);
        self->keyval = 0;
        self->modifier = 0;
        g_signal_emit(self, signals[SIGNAL_SHORTCUT_SET], 0);
        adw_dialog_close(ADW_DIALOG(self));
        return GDK_EVENT_STOP;
    }

    /* Store the captured shortcut */
    self->keyval = gdk_keyval_to_lower(keyval);
    self->modifier = sanitize_modifier_mask(state);

    /* Handle shift normalization */
    if ((state & GDK_SHIFT_MASK) != 0 && should_drop_shift(self->keyval, keyval))
        self->modifier &= ~GDK_SHIFT_MASK;
    if ((state & GDK_LOCK_MASK) == 0 && self->keyval != keyval)
        self->modifier |= GDK_SHIFT_MASK;

    /* Normalize Ctrl+ISO_Left_Tab → Ctrl+Shift+Tab */
    if (self->keyval == GDK_KEY_ISO_Left_Tab &&
        self->modifier == GDK_CONTROL_MASK)
    {
        self->keyval = GDK_KEY_Tab;
        self->modifier = GDK_CONTROL_MASK | GDK_SHIFT_MASK;
    }

    /* Build accelerator string */
    g_free(self->accelerator);
    self->accelerator = gtk_accelerator_name(self->keyval, self->modifier);

    /* Switch to display mode */
    self->editing = FALSE;
    update_display(self);
    gtk_widget_grab_focus(GTK_WIDGET(self->set_button));

    return GDK_EVENT_STOP;
}

/* ── Button callbacks ────────────────────────────────────────────── */

static void
on_set_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GsrShortcutAccelDialog *self = GSR_SHORTCUT_ACCEL_DIALOG(user_data);
    g_signal_emit(self, signals[SIGNAL_SHORTCUT_SET], 0);
    adw_dialog_close(ADW_DIALOG(self));
}

static void
on_cancel_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GsrShortcutAccelDialog *self = GSR_SHORTCUT_ACCEL_DIALOG(user_data);
    adw_dialog_close(ADW_DIALOG(self));
}

/* ── GObject lifecycle ───────────────────────────────────────────── */

static void
gsr_shortcut_accel_dialog_finalize(GObject *object)
{
    GsrShortcutAccelDialog *self = GSR_SHORTCUT_ACCEL_DIALOG(object);
    g_free(self->shortcut_title);
    g_free(self->initial_accel);
    g_free(self->accelerator);
    G_OBJECT_CLASS(gsr_shortcut_accel_dialog_parent_class)->finalize(object);
}

static void
gsr_shortcut_accel_dialog_init(GsrShortcutAccelDialog *self)
{
    (void)self;
}

static void
gsr_shortcut_accel_dialog_class_init(GsrShortcutAccelDialogClass *klass)
{
    GObjectClass *obj_class = G_OBJECT_CLASS(klass);
    obj_class->finalize = gsr_shortcut_accel_dialog_finalize;

    /**
     * GsrShortcutAccelDialog::shortcut-set:
     *
     * Emitted when the user confirms a new shortcut (Set button)
     * or clears it (Backspace).  Call get_accelerator() to retrieve
     * the result.
     */
    signals[SIGNAL_SHORTCUT_SET] = g_signal_new(
        "shortcut-set",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);
}

/* ── Build UI ────────────────────────────────────────────────────── */

static void
build_dialog_ui(GsrShortcutAccelDialog *self)
{
    adw_dialog_set_title(ADW_DIALOG(self), "Set Shortcut");
    adw_dialog_set_content_width(ADW_DIALOG(self), 400);
    adw_dialog_set_content_height(ADW_DIALOG(self), 260);

    /* ── Header bar ─── */
    AdwHeaderBar *header = ADW_HEADER_BAR(adw_header_bar_new());

    self->cancel_button = GTK_BUTTON(gtk_button_new_with_label("Cancel"));
    g_signal_connect(self->cancel_button, "clicked",
        G_CALLBACK(on_cancel_clicked), self);
    adw_header_bar_pack_start(header, GTK_WIDGET(self->cancel_button));

    self->set_button = GTK_BUTTON(gtk_button_new_with_label("Set"));
    gtk_widget_add_css_class(GTK_WIDGET(self->set_button), "suggested-action");
    gtk_widget_set_sensitive(GTK_WIDGET(self->set_button), FALSE);
    g_signal_connect(self->set_button, "clicked",
        G_CALLBACK(on_set_clicked), self);
    adw_header_bar_pack_end(header, GTK_WIDGET(self->set_button));

    /* ── Content stack ─── */
    self->stack = GTK_STACK(gtk_stack_new());
    gtk_stack_set_transition_type(self->stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);

    /* -- Capture page -- */
    GtkBox *capture_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 12));
    gtk_widget_set_halign(GTK_WIDGET(capture_box), GTK_ALIGN_CENTER);
    gtk_widget_set_valign(GTK_WIDGET(capture_box), GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(GTK_WIDGET(capture_box), 24);
    gtk_widget_set_margin_end(GTK_WIDGET(capture_box), 24);
    gtk_widget_set_margin_top(GTK_WIDGET(capture_box), 24);
    gtk_widget_set_margin_bottom(GTK_WIDGET(capture_box), 24);

    GtkImage *keyboard_icon = GTK_IMAGE(
        gtk_image_new_from_icon_name("input-keyboard-symbolic"));
    gtk_image_set_pixel_size(keyboard_icon, 64);
    gtk_widget_add_css_class(GTK_WIDGET(keyboard_icon), "dim-label");
    gtk_box_append(capture_box, GTK_WIDGET(keyboard_icon));

    char *escaped_title = g_markup_escape_text(
        self->shortcut_title ? self->shortcut_title : "Shortcut", -1);
    char *label_text = g_strdup_printf(
        "Enter new shortcut for\n<b>%s</b>", escaped_title);
    g_free(escaped_title);
    self->capture_label = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_markup(self->capture_label, label_text);
    gtk_label_set_justify(self->capture_label, GTK_JUSTIFY_CENTER);
    gtk_label_set_wrap(self->capture_label, TRUE);
    g_free(label_text);
    gtk_box_append(capture_box, GTK_WIDGET(self->capture_label));

    self->capture_hint = GTK_LABEL(gtk_label_new(
        "Press Escape to cancel or Backspace to disable the shortcut."));
    gtk_widget_add_css_class(GTK_WIDGET(self->capture_hint), "dim-label");
    gtk_label_set_wrap(self->capture_hint, TRUE);
    gtk_label_set_justify(self->capture_hint, GTK_JUSTIFY_CENTER);
    gtk_box_append(capture_box, GTK_WIDGET(self->capture_hint));

    gtk_stack_add_named(self->stack, GTK_WIDGET(capture_box), "capture");

    /* -- Display page -- */
    GtkBox *display_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 12));
    gtk_widget_set_halign(GTK_WIDGET(display_box), GTK_ALIGN_CENTER);
    gtk_widget_set_valign(GTK_WIDGET(display_box), GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(GTK_WIDGET(display_box), 24);
    gtk_widget_set_margin_end(GTK_WIDGET(display_box), 24);
    gtk_widget_set_margin_top(GTK_WIDGET(display_box), 24);
    gtk_widget_set_margin_bottom(GTK_WIDGET(display_box), 24);

    GtkLabel *display_title = GTK_LABEL(gtk_label_new(NULL));
    char *escaped_display_title = g_markup_escape_text(
        self->shortcut_title ? self->shortcut_title : "Shortcut", -1);
    char *display_text = g_strdup_printf(
        "Shortcut for <b>%s</b>", escaped_display_title);
    g_free(escaped_display_title);
    gtk_label_set_markup(display_title, display_text);
    gtk_label_set_justify(display_title, GTK_JUSTIFY_CENTER);
    gtk_label_set_wrap(display_title, TRUE);
    g_free(display_text);
    gtk_box_append(display_box, GTK_WIDGET(display_title));

    self->display_label = GTK_SHORTCUT_LABEL(gtk_shortcut_label_new(""));
    gtk_widget_set_halign(GTK_WIDGET(self->display_label), GTK_ALIGN_CENTER);
    gtk_box_append(display_box, GTK_WIDGET(self->display_label));

    gtk_stack_add_named(self->stack, GTK_WIDGET(display_box), "display");

    /* ── Key event controller (capture phase) ─── */
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctrl, "key-pressed",
        G_CALLBACK(on_key_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self), key_ctrl);

    /* ── Layout: toolbar-view ─── */
    AdwToolbarView *toolbar_view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    adw_toolbar_view_add_top_bar(toolbar_view, GTK_WIDGET(header));
    adw_toolbar_view_set_content(toolbar_view, GTK_WIDGET(self->stack));

    adw_dialog_set_child(ADW_DIALOG(self), GTK_WIDGET(toolbar_view));

    /* Start in capture mode */
    self->editing = TRUE;
    update_display(self);
}

/* ── Public API ──────────────────────────────────────────────────── */

GsrShortcutAccelDialog *
gsr_shortcut_accel_dialog_new(const char *shortcut_title,
                               const char *current_accel)
{
    GsrShortcutAccelDialog *self = g_object_new(
        GSR_TYPE_SHORTCUT_ACCEL_DIALOG, NULL);

    self->shortcut_title = g_strdup(shortcut_title);
    self->initial_accel = g_strdup(current_accel);
    self->accelerator = g_strdup(current_accel);
    self->editing = TRUE;
    self->keyval = 0;
    self->modifier = 0;

    build_dialog_ui(self);

    return self;
}

const char *
gsr_shortcut_accel_dialog_get_accelerator(GsrShortcutAccelDialog *self)
{
    g_return_val_if_fail(GSR_IS_SHORTCUT_ACCEL_DIALOG(self), NULL);
    return self->accelerator;
}
