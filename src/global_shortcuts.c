#include "global_shortcuts.h"
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/random.h>
#include <gio/gio.h>

/* TODO: Remove G_DBUS_CALL_FLAGS_NO_AUTO_START and G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START? also in gpu screen recorder equivalent */
/* TODO: More error handling and clean up resources after done */
/* TODO: Use GArray instead of GVariant where possible */

static bool generate_random_characters(char *buffer, int buffer_size, const char *alphabet, size_t alphabet_size) {
    /* TODO: Use other functions on other platforms than linux */
    if(getrandom(buffer, buffer_size, 0) < buffer_size) {
        fprintf(stderr, "gsr error: generate_random_characters: failed to get random bytes, error: %s\n", strerror(errno));
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

        if(!shortcut_id || !shortcut_values)
            continue;

        // gchar *description = NULL;
        // g_variant_lookup(shortcut_values, "description", "s", &description);

        gchar *trigger_description = NULL;
        g_variant_lookup(shortcut_values, "trigger_description", "s", &trigger_description);

        gsr_shortcut shortcut;
        shortcut.id = shortcut_id;
        shortcut.trigger_description = trigger_description ? trigger_description : "";
        callback(shortcut, userdata);
    }
}

typedef struct {
    gsr_global_shortcuts *self;
    gsr_shortcut_callback callback;
    void *userdata;
} signal_list_bind_userdata;

static void dbus_signal_list_bind(GDBusProxy *proxy, gchar *sender_name, gchar *signal_name, GVariant *parameters, signal_list_bind_userdata *userdata) {
    (void)proxy;
    (void)sender_name;
    if(g_strcmp0(signal_name, "Response") != 0)
        goto done;

    guint32 response = 0;
    GVariant *results = NULL;
    g_variant_get(parameters, "(u@a{sv})", &response, &results);

    if(response != 0 || !results)
        goto done;

    GVariant *shortcuts = g_variant_lookup_value(results, "shortcuts", G_VARIANT_TYPE("a(sa{sv})"));
    if(!shortcuts)
        goto done;

    handle_shortcuts_data(shortcuts, userdata->callback, userdata->userdata);

    done:
    free(userdata);
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
    if(strcmp(signal_name, "Deactivated") == 0) {
        gchar *session_handle = NULL;
        gchar *shortcut_id = NULL;
        gchar *timestamp = NULL;
        GVariant *options = NULL;
        g_variant_get(parameters, "(ost@a{sv})", &session_handle, &shortcut_id, &timestamp, &options);

        if(session_handle && shortcut_id && strcmp(session_handle, cu->self->session_handle) == 0)
            cu->deactivated_callback(shortcut_id, cu->userdata);
    } else if(strcmp(signal_name, "ShortcutsChanged") == 0) {
        gchar *session_handle = NULL;
        GVariant *shortcuts = NULL;
        g_variant_get(parameters, "(o@a(sa{sv}))", &session_handle, &shortcuts);

        if(session_handle && shortcuts && strcmp(session_handle, cu->self->session_handle) == 0)
            handle_shortcuts_data(shortcuts, cu->shortcut_changed_callback, cu->userdata);
    }
}

typedef struct {
    gsr_global_shortcuts *self;
    gsr_init_callback callback;
    void *userdata;
} signal_create_session_userdata;

static void dbus_signal_create_session(GDBusProxy *proxy, gchar *sender_name, gchar *signal_name, GVariant *parameters, signal_create_session_userdata *cu) {
    (void)proxy;
    (void)sender_name;
    if(g_strcmp0(signal_name, "Response") != 0)
        goto done;

    guint32 response = 0;
    GVariant *results = NULL;
    g_variant_get(parameters, "(u@a{sv})", &response, &results);

    if(response != 0 || !results) {
        cu->callback(false, cu->userdata);
        goto done;
    }

    gchar *session_handle = NULL;
    if(g_variant_lookup(results, "session_handle", "s", &session_handle) && session_handle) {
        cu->self->session_handle = strdup(session_handle);
        cu->self->session_created = true;
        cu->callback(true, cu->userdata);
    }

    done:
    free(cu);
}

static bool gsr_global_shortcuts_create_session(gsr_global_shortcuts *self, gsr_init_callback callback, void *userdata) {
    char handle_token[64];
    gsr_dbus_portal_get_unique_handle_token(self, handle_token, sizeof(handle_token));

    char session_handle_token[64];
    snprintf(session_handle_token, sizeof(session_handle_token), "gpu_screen_recorder_adwaita");
    
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(handle_token));
    g_variant_builder_add(&builder, "{sv}", "session_handle_token", g_variant_new_string(session_handle_token));
    GVariant *aa = g_variant_builder_end(&builder);

    GVariant *ret = g_dbus_connection_call_sync(self->gdbus_con, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.portal.GlobalShortcuts", "CreateSession", g_variant_new_tuple(&aa, 1), NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, NULL, NULL);

    if(ret) {
        const gchar *val = NULL;
        g_variant_get(ret, "(&o)", &val);
        if(!val)
            return false;
        //g_variant_unref(ret);

        GDBusProxy *proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, NULL, "org.freedesktop.portal.Desktop", val, "org.freedesktop.portal.Request", NULL, NULL);
        if(!proxy)
            return false;
        //g_object_unref(proxy);

        signal_create_session_userdata *cu = malloc(sizeof(signal_create_session_userdata));
        cu->self = self;
        cu->callback = callback;
        cu->userdata = userdata;
        g_signal_connect(proxy, "g-signal", G_CALLBACK(dbus_signal_create_session), cu);
        return true;
    } else {
        return false;
    }
}

bool gsr_global_shortcuts_init(gsr_global_shortcuts *self, gsr_init_callback callback, void *userdata) {
    memset(self, 0, sizeof(*self));

    self->random_str[DBUS_RANDOM_STR_SIZE] = '\0';
    if(!generate_random_characters(self->random_str, DBUS_RANDOM_STR_SIZE, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", 62)) {
        fprintf(stderr, "gsr error: gsr_global_shortcuts_init: failed to generate random string\n");
        return false;
    }

    self->gdbus_con = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if(!self->gdbus_con) {
        fprintf(stderr, "gsr error: gsr_global_shortcuts_init: g_bus_get_sync failed\n");
        return false;
    }

    if(!gsr_global_shortcuts_create_session(self, callback, userdata)) {
        gsr_global_shortcuts_deinit(self);
        return false;
    }

    return true;
}

void gsr_global_shortcuts_deinit(gsr_global_shortcuts *self) {
    if(self->gdbus_con) {
        /* TODO: Re-add this. Right now it causes errors as the connection is already closed, but checking if it's already closed here has no effect */
        //g_dbus_connection_close(self->gdbus_con, NULL, NULL, NULL);
        self->gdbus_con = NULL;
    }

    if(self->session_handle) {
        free(self->session_handle);
        self->session_handle = NULL;
    }
}

bool gsr_global_shortcuts_list_shortcuts(gsr_global_shortcuts *self, gsr_shortcut_callback callback, void *userdata) {
    if(!self->session_created)
        return false;

    char handle_token[64];
    gsr_dbus_portal_get_unique_handle_token(self, handle_token, sizeof(handle_token));

    GVariant *session_handle_obj = g_variant_new_object_path(self->session_handle);

    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(handle_token));
    GVariant *aa = g_variant_builder_end(&builder);

    GVariant *args[2] = { session_handle_obj, aa };

    GVariant *ret = g_dbus_connection_call_sync(self->gdbus_con, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.portal.GlobalShortcuts", "ListShortcuts", g_variant_new_tuple(args, 2), NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, NULL, NULL);
    if(ret) {
        const gchar *val = NULL;
        g_variant_get(ret, "(&o)", &val);
        if(!val)
            return false;

        GDBusProxy *proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, NULL, "org.freedesktop.portal.Desktop", val, "org.freedesktop.portal.Request", NULL, NULL);
        if(!proxy)
            return false;
        //g_object_unref(proxy);

        signal_list_bind_userdata *cu = malloc(sizeof(signal_list_bind_userdata));
        cu->self = self;
        cu->callback = callback;
        cu->userdata = userdata;
        g_signal_connect(proxy, "g-signal", G_CALLBACK(dbus_signal_list_bind), cu);
        return true;
    } else {
        return false;
    }
}

bool gsr_global_shortcuts_bind_shortcuts(gsr_global_shortcuts *self, const gsr_bind_shortcut *shortcuts, int num_shortcuts, gsr_shortcut_callback callback, void *userdata) {
    if(!self->session_created)
        return false;

    char handle_token[64];
    gsr_dbus_portal_get_unique_handle_token(self, handle_token, sizeof(handle_token));

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
    if(ret) {
        const gchar *val = NULL;
        g_variant_get(ret, "(&o)", &val);
        if(!val)
            return false;

        GDBusProxy *proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, NULL, "org.freedesktop.portal.Desktop", val, "org.freedesktop.portal.Request", NULL, NULL);
        if(!proxy)
            return false;
        //g_object_unref(proxy);

        signal_list_bind_userdata *cu = malloc(sizeof(signal_list_bind_userdata));
        cu->self = self;
        cu->callback = callback;
        cu->userdata = userdata;
        g_signal_connect(proxy, "g-signal", G_CALLBACK(dbus_signal_list_bind), cu);
        return true;
    } else {
        return false;
    }
}

bool gsr_global_shortcuts_subscribe_activated_signal(gsr_global_shortcuts *self, gsr_deactivated_callback deactivated_callback, gsr_shortcut_callback shortcut_changed_callback, void *userdata) {
    if(!self->session_created)
        return false;

    signal_userdata *cu = malloc(sizeof(signal_userdata));
    cu->self = self;
    cu->deactivated_callback = deactivated_callback;
    cu->shortcut_changed_callback = shortcut_changed_callback;
    cu->userdata = userdata;
    g_dbus_connection_signal_subscribe(self->gdbus_con, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.GlobalShortcuts", NULL, "/org/freedesktop/portal/desktop", NULL, G_DBUS_SIGNAL_FLAGS_NONE, signal_callback, cu, free);
    return true;
}
