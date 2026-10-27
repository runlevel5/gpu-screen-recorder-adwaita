#include "global_shortcuts.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <gio/gio.h>
#include <sys/random.h>

static bool generate_random_characters(char *buffer, int buffer_size, const char *alphabet, size_t alphabet_size) {
    if(getrandom(buffer, buffer_size, 0) < buffer_size) {
        g_warning("generate_random_characters: failed to get random bytes: %s", strerror(errno));
        return false;
    }

    for(int i = 0; i < buffer_size; ++i) {
        unsigned char c = *(unsigned char*)&buffer[i];
        buffer[i] = alphabet[c % alphabet_size];
    }

    return true;
}

static void gsr_dbus_portal_get_unique_handle_token(gsr_global_shortcuts *self, char *buffer, int size) {
    snprintf(buffer, size, "gpu_screen_recorder_adwaita_handle_%s_%u", self->random_str, self->handle_counter++);
}

/* Assumes shortcuts is an array */
static void handle_shortcuts_data(GVariant *shortcuts, gsr_shortcut_callback callback, void *userdata) {
    for(guint i = 0; i < g_variant_n_children(shortcuts); ++i) {
        gchar *shortcut_id = NULL;
        GVariant *shortcut_values = NULL;
        g_variant_get_child(shortcuts, i, "(s@a{sv})", &shortcut_id, &shortcut_values);

        if(!shortcut_id || !shortcut_values) {
            g_free(shortcut_id);
            if(shortcut_values) g_variant_unref(shortcut_values);
            continue;
        }

        gchar *trigger_description = NULL;
        g_variant_lookup(shortcut_values, "trigger_description", "s", &trigger_description);

        gsr_shortcut shortcut;
        shortcut.id = shortcut_id;
        shortcut.trigger_description = trigger_description ? trigger_description : "";
        callback(shortcut, userdata);

        g_free(shortcut_id);
        g_free(trigger_description);
        g_variant_unref(shortcut_values);
    }
}

/* Portal methods answer asynchronously with a Response signal on a Request
 * object. Its path is derived from the handle_token we pass in:
 *     /org/freedesktop/portal/desktop/request/<sender>/<token>
 * so subscribe there *before* making the call, as the portal docs advise.
 * Subscribing afterwards (or creating a proxy for the returned handle) can
 * miss a fast Response and leave the request pending forever.
 *
 * Pending requests are tracked so deinit can close the ones still open and
 * no late Response calls back into a freed owner. */

typedef enum {
    REQUEST_CREATE_SESSION,
    REQUEST_SHORTCUTS,      /* ListShortcuts and BindShortcuts reply alike */
} request_kind;

typedef struct {
    gsr_global_shortcuts *self;
    request_kind kind;
    char *path;             /* Request object path we are subscribed on */
    guint subscription_id;
    union {
        gsr_init_callback init;
        gsr_shortcut_callback shortcuts;
    } callback;
    void *userdata;
} pending_request;

static void on_request_response(GDBusConnection *connection, const gchar *sender_name, const gchar *object_path, const gchar *interface_name, const gchar *signal_name, GVariant *parameters, gpointer userdata);

static void pending_request_free(pending_request *req) {
    if(req->subscription_id)
        g_dbus_connection_signal_unsubscribe(req->self->gdbus_con, req->subscription_id);
    free(req->path);
    free(req);
}

static bool request_subscribe(pending_request *req, const char *path) {
    free(req->path);
    req->path = strdup(path);
    if(!req->path)
        return false;
    req->subscription_id = g_dbus_connection_signal_subscribe(req->self->gdbus_con, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.Request", "Response", path, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_request_response, req, NULL);
    return req->subscription_id != 0;
}

/* Subscribe for the Response of the request |handle_token| will create.
 * The caller fills in the callback and then makes the portal call. */
static pending_request *request_begin(gsr_global_shortcuts *self, request_kind kind, const char *handle_token, void *userdata) {
    /* The sender part is our unique name without the ':' and with '.'
     * replaced by '_', e.g. ":1.42" -> "1_42" */
    const char *unique_name = g_dbus_connection_get_unique_name(self->gdbus_con);
    if(!unique_name || unique_name[0] != ':')
        return NULL;

    char sender[128];
    snprintf(sender, sizeof(sender), "%s", unique_name + 1);
    for(char *c = sender; *c; ++c) {
        if(*c == '.')
            *c = '_';
    }

    char path[512];
    snprintf(path, sizeof(path), "/org/freedesktop/portal/desktop/request/%s/%s", sender, handle_token);

    pending_request *req = calloc(1, sizeof(pending_request));
    if(!req)
        return NULL;
    req->self = self;
    req->kind = kind;
    req->userdata = userdata;
    if(!request_subscribe(req, path)) {
        pending_request_free(req);
        return NULL;
    }

    self->pending_requests = g_slist_prepend(self->pending_requests, req);
    return req;
}

static void request_abort(pending_request *req) {
    req->self->pending_requests = g_slist_remove(req->self->pending_requests, req);
    pending_request_free(req);
}

/* Call with the portal method's return value (or NULL on failure). Old
 * portals (xdg-desktop-portal < 0.9) ignore handle_token and return a
 * different handle; move the subscription there, accepting the small race. */
static bool request_started(pending_request *req, GVariant *ret) {
    if(!ret) {
        request_abort(req);
        return false;
    }

    const gchar *handle = NULL;
    g_variant_get(ret, "(&o)", &handle);
    if(handle && g_strcmp0(handle, req->path) != 0) {
        g_dbus_connection_signal_unsubscribe(req->self->gdbus_con, req->subscription_id);
        req->subscription_id = 0;
        if(!request_subscribe(req, handle)) {
            g_variant_unref(ret);
            request_abort(req);
            return false;
        }
    }
    g_variant_unref(ret);
    return true;
}

static void handle_create_session_response(gsr_global_shortcuts *self, guint32 response, GVariant *results, gsr_init_callback callback, void *userdata) {
    bool success = false;
    if(response == 0 && results) {
        gchar *session_handle = NULL;
        if(g_variant_lookup(results, "session_handle", "s", &session_handle) && session_handle) {
            self->session_handle = strdup(session_handle);
            self->session_created = true;
            success = true;
            g_free(session_handle);
        }
    }
    callback(success, userdata);
}

static void handle_shortcuts_response(guint32 response, GVariant *results, gsr_shortcut_callback callback, void *userdata) {
    if(response != 0 || !results)
        return;

    GVariant *shortcuts = g_variant_lookup_value(results, "shortcuts", G_VARIANT_TYPE("a(sa{sv})"));
    if(!shortcuts)
        return;

    handle_shortcuts_data(shortcuts, callback, userdata);
    g_variant_unref(shortcuts);
}

static void on_request_response(GDBusConnection *connection, const gchar *sender_name, const gchar *object_path, const gchar *interface_name, const gchar *signal_name, GVariant *parameters, gpointer userdata) {
    (void)connection;
    (void)sender_name;
    (void)object_path;
    (void)interface_name;
    (void)signal_name;
    pending_request *req = userdata;

    /* Finish the request before calling out: the callback may tear down
     * the owner, so the callback must be the last thing touched. */
    gsr_global_shortcuts *self = req->self;
    const request_kind kind = req->kind;
    const gsr_init_callback init_callback = req->callback.init;
    const gsr_shortcut_callback shortcuts_callback = req->callback.shortcuts;
    void *callback_userdata = req->userdata;
    request_abort(req);

    guint32 response = 0;
    GVariant *results = NULL;
    g_variant_get(parameters, "(u@a{sv})", &response, &results);

    switch(kind) {
        case REQUEST_CREATE_SESSION:
            handle_create_session_response(self, response, results, init_callback, callback_userdata);
            break;
        case REQUEST_SHORTCUTS:
            handle_shortcuts_response(response, results, shortcuts_callback, callback_userdata);
            break;
    }

    if(results) g_variant_unref(results);
}

typedef struct {
    gsr_global_shortcuts *self;
    gsr_deactivated_callback deactivated_callback;
    gsr_shortcut_callback shortcut_changed_callback;
    void *userdata;
} signal_userdata;

static void signal_callback(GDBusConnection *connection,
                            const gchar     *sender_name,
                            const gchar     *object_path,
                            const gchar     *interface_name,
                            const gchar     *signal_name,
                            GVariant        *parameters,
                            gpointer        userdata)
{
    (void)connection;
    (void)sender_name;
    (void)object_path;
    (void)interface_name;
    (void)signal_name;
    (void)parameters;
    signal_userdata *cu = userdata;
    
    /* Button released */
    if(g_strcmp0(signal_name, "Deactivated") == 0) {
        gchar *session_handle = NULL;
        gchar *shortcut_id = NULL;
        guint64 timestamp = 0;
        GVariant *options = NULL;
        g_variant_get(parameters, "(ost@a{sv})", &session_handle, &shortcut_id, &timestamp, &options);

        if(session_handle && shortcut_id && g_strcmp0(session_handle, cu->self->session_handle) == 0)
            cu->deactivated_callback(shortcut_id, cu->userdata);

        g_free(session_handle);
        g_free(shortcut_id);
        if(options) g_variant_unref(options);
    } else if(g_strcmp0(signal_name, "ShortcutsChanged") == 0) {
        gchar *session_handle = NULL;
        GVariant *shortcuts = NULL;
        g_variant_get(parameters, "(o@a(sa{sv}))", &session_handle, &shortcuts);

        if(session_handle && shortcuts && g_strcmp0(session_handle, cu->self->session_handle) == 0)
            handle_shortcuts_data(shortcuts, cu->shortcut_changed_callback, cu->userdata);

        g_free(session_handle);
        if(shortcuts) g_variant_unref(shortcuts);
    }
}

static bool gsr_global_shortcuts_create_session(gsr_global_shortcuts *self, gsr_init_callback callback, void *userdata) {
    char handle_token[128];
    gsr_dbus_portal_get_unique_handle_token(self, handle_token, sizeof(handle_token));

    pending_request *req = request_begin(self, REQUEST_CREATE_SESSION, handle_token, userdata);
    if(!req)
        return false;
    req->callback.init = callback;

    char session_handle_token[128];
    snprintf(session_handle_token, sizeof(session_handle_token), "gpu_screen_recorder_adwaita_%s", self->random_str);

    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(handle_token));
    g_variant_builder_add(&builder, "{sv}", "session_handle_token", g_variant_new_string(session_handle_token));
    GVariant *aa = g_variant_builder_end(&builder);

    GVariant *ret = g_dbus_connection_call_sync(self->gdbus_con, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.portal.GlobalShortcuts", "CreateSession", g_variant_new_tuple(&aa, 1), NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, NULL, NULL);
    return request_started(req, ret);
}

bool gsr_global_shortcuts_init(gsr_global_shortcuts *self, gsr_init_callback callback, void *userdata) {
    memset(self, 0, sizeof(*self));

    self->random_str[DBUS_RANDOM_STR_SIZE] = '\0';
    if(!generate_random_characters(self->random_str, DBUS_RANDOM_STR_SIZE, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", 62)) {
        g_warning("gsr_global_shortcuts_init: failed to generate random string");
        return false;
    }

    self->gdbus_con = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if(!self->gdbus_con) {
        g_warning("gsr_global_shortcuts_init: g_bus_get_sync failed");
        return false;
    }

    if(!gsr_global_shortcuts_create_session(self, callback, userdata)) {
        gsr_global_shortcuts_deinit(self);
        return false;
    }

    return true;
}

void gsr_global_shortcuts_deinit(gsr_global_shortcuts *self) {
    if(self->activated_signal_id && self->gdbus_con)
        g_dbus_connection_signal_unsubscribe(self->gdbus_con, self->activated_signal_id);
    self->activated_signal_id = 0;

    /* Tell the portal to drop requests we no longer care about, so e.g. a
     * BindShortcuts dialog doesn't stay open after the window is gone */
    while(self->pending_requests) {
        pending_request *req = self->pending_requests->data;
        g_dbus_connection_call(self->gdbus_con, "org.freedesktop.portal.Desktop", req->path, "org.freedesktop.portal.Request", "Close", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
        request_abort(req);
    }
    if(self->gdbus_con)
        g_dbus_connection_flush_sync(self->gdbus_con, NULL, NULL);
    g_clear_object(&self->gdbus_con);

    g_clear_pointer(&self->session_handle, free);
}

bool gsr_global_shortcuts_list_shortcuts(gsr_global_shortcuts *self, gsr_shortcut_callback callback, void *userdata) {
    if(!self->session_created)
        return false;

    char handle_token[128];
    gsr_dbus_portal_get_unique_handle_token(self, handle_token, sizeof(handle_token));

    pending_request *req = request_begin(self, REQUEST_SHORTCUTS, handle_token, userdata);
    if(!req)
        return false;
    req->callback.shortcuts = callback;

    GVariant *session_handle_obj = g_variant_new_object_path(self->session_handle);

    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(handle_token));
    GVariant *aa = g_variant_builder_end(&builder);

    GVariant *args[2] = { session_handle_obj, aa };

    GVariant *ret = g_dbus_connection_call_sync(self->gdbus_con, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.portal.GlobalShortcuts", "ListShortcuts", g_variant_new_tuple(args, 2), NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, NULL, NULL);
    return request_started(req, ret);
}

bool gsr_global_shortcuts_bind_shortcuts(gsr_global_shortcuts *self, const gsr_bind_shortcut *shortcuts, int num_shortcuts, gsr_shortcut_callback callback, void *userdata) {
    if(!self->session_created)
        return false;

    char handle_token[128];
    gsr_dbus_portal_get_unique_handle_token(self, handle_token, sizeof(handle_token));

    pending_request *req = request_begin(self, REQUEST_SHORTCUTS, handle_token, userdata);
    if(!req)
        return false;
    req->callback.shortcuts = callback;

    GVariant *session_handle_obj = g_variant_new_object_path(self->session_handle);

    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a(sa{sv})"));

    for(int i = 0; i < num_shortcuts; ++i) {
        GVariantBuilder shortcuts_builder;
        g_variant_builder_init(&shortcuts_builder, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&shortcuts_builder, "{sv}", "description", g_variant_new_string(shortcuts[i].description));
        g_variant_builder_add(&shortcuts_builder, "{sv}", "preferred_trigger", g_variant_new_string(shortcuts[i].shortcut.trigger_description));
        GVariant *shortcuts_data = g_variant_builder_end(&shortcuts_builder);
        GVariant *ss_l[2] = { g_variant_new_string(shortcuts[i].shortcut.id), shortcuts_data };
        g_variant_builder_add_value(&builder, g_variant_new_tuple(ss_l, 2));
    }
    GVariant *aa = g_variant_builder_end(&builder);

    GVariantBuilder builder_zzz;
    g_variant_builder_init(&builder_zzz, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&builder_zzz, "{sv}", "handle_token", g_variant_new_string(handle_token));
    GVariant *bb = g_variant_builder_end(&builder_zzz);

    GVariant *parent_window = g_variant_new_string("");
    GVariant *args[4] = { session_handle_obj, aa, parent_window, bb };

    GVariant *ret = g_dbus_connection_call_sync(self->gdbus_con, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.portal.GlobalShortcuts", "BindShortcuts", g_variant_new_tuple(args, 4), NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL, NULL);
    return request_started(req, ret);
}

bool gsr_global_shortcuts_subscribe_activated_signal(gsr_global_shortcuts *self, gsr_deactivated_callback deactivated_callback, gsr_shortcut_callback shortcut_changed_callback, void *userdata) {
    if(!self->session_created)
        return false;

    signal_userdata *cu = malloc(sizeof(signal_userdata));
    if(!cu)
        return false;
    cu->self = self;
    cu->deactivated_callback = deactivated_callback;
    cu->shortcut_changed_callback = shortcut_changed_callback;
    cu->userdata = userdata;
    if(self->activated_signal_id)
        g_dbus_connection_signal_unsubscribe(self->gdbus_con, self->activated_signal_id);
    self->activated_signal_id = g_dbus_connection_signal_subscribe(self->gdbus_con, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.GlobalShortcuts", NULL, "/org/freedesktop/portal/desktop", NULL, G_DBUS_SIGNAL_FLAGS_NONE, signal_callback, cu, free);
    return true;
}
