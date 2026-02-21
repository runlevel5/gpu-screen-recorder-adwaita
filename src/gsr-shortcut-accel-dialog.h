#pragma once

/*
 * gsr-shortcut-accel-dialog.h — Key capture dialog for hotkey assignment.
 *
 * Ptyxis-style AdwDialog with GtkEventControllerKey in capture phase.
 * Opens in immediate capture mode: user presses key combo, then confirms.
 *
 * Escape = cancel, Backspace = clear/disable shortcut.
 */

#include <adwaita.h>

G_BEGIN_DECLS

#define GSR_TYPE_SHORTCUT_ACCEL_DIALOG (gsr_shortcut_accel_dialog_get_type())
G_DECLARE_FINAL_TYPE(GsrShortcutAccelDialog, gsr_shortcut_accel_dialog,
                     GSR, SHORTCUT_ACCEL_DIALOG, AdwDialog)

/**
 * Create a new shortcut accel dialog.
 * @shortcut_title: Human-readable name like "Start/Stop streaming"
 * @current_accel:  Current accelerator string (e.g. "<Alt>1") or NULL
 */
GsrShortcutAccelDialog *gsr_shortcut_accel_dialog_new(const char *shortcut_title,
                                                       const char *current_accel);

/**
 * Get the accelerator string that was captured.
 * Returns NULL if the shortcut was cleared (Backspace).
 * Returns a borrowed string, valid until the dialog is destroyed.
 */
const char *gsr_shortcut_accel_dialog_get_accelerator(GsrShortcutAccelDialog *self);

G_END_DECLS
