#ifndef GLOBAL_SHORTCUTS_H
#define GLOBAL_SHORTCUTS_H

/* Global shortcuts via desktop portal */

#include <stdbool.h>
#include <gio/gio.h>

#define DBUS_RANDOM_STR_SIZE 16

typedef struct {
    const char *id;
    const char *trigger_description;
} gsr_shortcut;

typedef struct {
    const char *description;
    gsr_shortcut shortcut;
} gsr_bind_shortcut;

typedef void (*gsr_init_callback)(bool success, void *userdata);
typedef void (*gsr_shortcut_callback)(gsr_shortcut shortcut, void *userdata);
typedef void (*gsr_deactivated_callback)(const char *id, void *userdata);

typedef struct {
    GDBusConnection *gdbus_con;
    char *session_handle;
    bool session_created;
    char random_str[DBUS_RANDOM_STR_SIZE + 1];
    unsigned int handle_counter;
} gsr_global_shortcuts;

bool gsr_global_shortcuts_init(gsr_global_shortcuts *self, gsr_init_callback callback, void *userdata);
void gsr_global_shortcuts_deinit(gsr_global_shortcuts *self);

bool gsr_global_shortcuts_list_shortcuts(gsr_global_shortcuts *self, gsr_shortcut_callback callback, void *userdata);
bool gsr_global_shortcuts_bind_shortcuts(gsr_global_shortcuts *self, const gsr_bind_shortcut *shortcuts, int num_shortcuts, gsr_shortcut_callback callback, void *userdata);

bool gsr_global_shortcuts_subscribe_activated_signal(gsr_global_shortcuts *self, gsr_deactivated_callback deactivated_callback, gsr_shortcut_callback shortcut_changed_callback, void *userdata);

#endif /* GLOBAL_SHORTCUTS_H */
