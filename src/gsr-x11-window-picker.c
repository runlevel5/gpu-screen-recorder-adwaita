#include "gsr-x11-window-picker.h"

#include <stdlib.h>
#include <string.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>

#include <glib.h>
#include <glib-unix.h>

/* ── Internal struct ─────────────────────────────────────────────── */

struct _GsrX11WindowPicker {
    Display                   *display;     /* own connection */
    Window                     root;
    Cursor                     crosshair;
    GsrX11WindowPickCallback   callback;
    void                      *userdata;
    GSource                   *source;
    guint                      source_id;
    bool                       finished;    /* prevents double callback */
};

/* ── X11 tree walker ─────────────────────────────────────────────── */

/**
 * Check if a window has a given atom property.
 */
static bool
window_has_atom(Display *display, Window window, Atom atom)
{
    Atom type_ret;
    int format_ret;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    int rc = XGetWindowProperty(display, window, atom,
                                0, 0, False, AnyPropertyType,
                                &type_ret, &format_ret,
                                &nitems, &bytes_after, &data);
    if (data)
        XFree(data);

    return rc == Success && type_ret != None;
}

/**
 * Walk the window tree looking for the first child with _NET_WM_STATE,
 * which is the "real" toplevel window managed by the WM.
 */
static Window
find_toplevel_window(Display *display, Window window)
{
    if (window == None)
        return None;

    Atom wm_state = XInternAtom(display, "_NET_WM_STATE", False);
    if (!wm_state)
        return None;

    if (window_has_atom(display, window, wm_state))
        return window;

    Window root, parent;
    Window *children = NULL;
    unsigned int n_children = 0;

    if (!XQueryTree(display, window, &root, &parent, &children, &n_children)
        || !children)
        return None;

    Window found = None;

    /* First pass: direct children with _NET_WM_STATE */
    for (int i = (int)n_children - 1; i >= 0; i--) {
        if (children[i] && window_has_atom(display, children[i], wm_state)) {
            found = children[i];
            goto done;
        }
    }
    /* Second pass: recurse */
    for (int i = (int)n_children - 1; i >= 0; i--) {
        if (children[i]) {
            Window w = find_toplevel_window(display, children[i]);
            if (w != None) {
                found = w;
                goto done;
            }
        }
    }

done:
    XFree(children);
    return found;
}

/**
 * Get the WM_NAME of a window.  Returns a g_malloc'd string, or NULL.
 */
static char *
get_window_name(Display *display, Window window)
{
    if (window == None)
        return NULL;

    /* Try _NET_WM_NAME first (UTF-8) */
    Atom net_wm_name = XInternAtom(display, "_NET_WM_NAME", False);
    Atom utf8_string = XInternAtom(display, "UTF8_STRING", False);

    if (net_wm_name && utf8_string) {
        Atom type_ret;
        int format_ret;
        unsigned long nitems, bytes_after;
        unsigned char *data = NULL;

        int rc = XGetWindowProperty(display, window, net_wm_name,
                                    0, 1024, False, utf8_string,
                                    &type_ret, &format_ret,
                                    &nitems, &bytes_after, &data);
        if (rc == Success && data && nitems > 0) {
            char *name = g_strdup((const char *)data);
            XFree(data);
            return name;
        }
        if (data)
            XFree(data);
    }

    /* Fallback: XGetWMName */
    XTextProperty wm_name;
    if (XGetWMName(display, window, &wm_name) && wm_name.nitems > 0) {
        char **list = NULL;
        int count = 0;
        char *result = NULL;

        if (Xutf8TextPropertyToTextList(display, &wm_name, &list, &count) >= 0
            && list && count > 0 && list[0])
        {
            result = g_strdup(list[0]);
        } else {
            /* Last resort: just copy the raw value */
            result = g_strndup((const char *)wm_name.value, wm_name.nitems);
        }

        if (list)
            XFreeStringList(list);
        if (wm_name.value)
            XFree(wm_name.value);

        return result;
    }

    return NULL;
}

/* ── Finish the pick ─────────────────────────────────────────────── */

static void
finish_pick(GsrX11WindowPicker *self, Window window, char *name)
{
    if (self->finished)
        return;
    self->finished = true;

    XUngrabPointer(self->display, CurrentTime);
    XUngrabKeyboard(self->display, CurrentTime);
    XSync(self->display, False);

    GsrX11WindowPickResult result = {
        .window = window,
        .name   = name,
    };
    self->callback(&result, self->userdata);

    /* Self-destroy after callback */
    gsr_x11_window_picker_free(self);
}

/* ── GSource dispatch — poll X events ────────────────────────────── */

static gboolean
picker_source_prepare(GSource *source G_GNUC_UNUSED, gint *timeout)
{
    *timeout = -1;
    return FALSE;
}

static gboolean
picker_source_check(GSource *source G_GNUC_UNUSED)
{
    return TRUE;
}

static gboolean
picker_source_dispatch(GSource *source G_GNUC_UNUSED,
                       GSourceFunc callback G_GNUC_UNUSED,
                       gpointer user_data)
{
    GsrX11WindowPicker *self = user_data;

    if (self->finished)
        return G_SOURCE_REMOVE;

    while (XPending(self->display)) {
        XEvent ev;
        XNextEvent(self->display, &ev);

        if (ev.type == ButtonPress) {
            Window clicked = ev.xbutton.subwindow;
            if (clicked == None)
                clicked = ev.xbutton.window;

            /* Walk tree to find the real toplevel */
            Window toplevel = find_toplevel_window(self->display, clicked);
            if (toplevel != None)
                clicked = toplevel;

            if (clicked == None || clicked == self->root) {
                /* Clicked root / empty space — cancel */
                finish_pick(self, None, NULL);
                return G_SOURCE_REMOVE;
            }

            char *name = get_window_name(self->display, clicked);
            if (!name)
                name = g_strdup("(no name)");

            finish_pick(self, clicked, name);
            return G_SOURCE_REMOVE;
        }

        if (ev.type == KeyPress) {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            if (ks == XK_Escape) {
                finish_pick(self, None, NULL);
                return G_SOURCE_REMOVE;
            }
        }
    }

    return G_SOURCE_CONTINUE;
}

static GSourceFuncs picker_source_funcs = {
    .prepare  = picker_source_prepare,
    .check    = picker_source_check,
    .dispatch = picker_source_dispatch,
    .finalize = NULL,
};

/* ── Public API ──────────────────────────────────────────────────── */

GsrX11WindowPicker *
gsr_x11_window_picker_new(GsrX11WindowPickCallback callback,
                           void *userdata)
{
    if (!callback)
        return NULL;

    /* Open our own X connection so we don't conflict with GDK */
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        g_warning("gsr_x11_window_picker: failed to open X display");
        return NULL;
    }

    GsrX11WindowPicker *self = g_new0(GsrX11WindowPicker, 1);
    self->display   = dpy;
    self->root      = DefaultRootWindow(dpy);
    self->callback  = callback;
    self->userdata  = userdata;
    self->finished  = false;

    self->crosshair = XCreateFontCursor(dpy, XC_crosshair);

    /* Grab pointer with crosshair */
    int status = XGrabPointer(dpy, self->root, False,
                              ButtonPressMask | ButtonReleaseMask,
                              GrabModeAsync, GrabModeAsync,
                              self->root, self->crosshair, CurrentTime);
    if (status != GrabSuccess) {
        g_warning("gsr_x11_window_picker: XGrabPointer failed (%d)", status);
        XFreeCursor(dpy, self->crosshair);
        XCloseDisplay(dpy);
        g_free(self);
        return NULL;
    }

    /* Also grab keyboard so we can detect Escape */
    XGrabKeyboard(dpy, self->root, False,
                  GrabModeAsync, GrabModeAsync, CurrentTime);

    XSync(dpy, False);

    /* Set up GSource to poll X events */
    int x_fd = ConnectionNumber(dpy);
    self->source = g_source_new(&picker_source_funcs, sizeof(GSource));
    g_source_set_callback(self->source, NULL, self, NULL);
    g_source_add_unix_fd(self->source, x_fd, G_IO_IN | G_IO_HUP | G_IO_ERR);
    self->source_id = g_source_attach(self->source, NULL);

    return self;
}

void
gsr_x11_window_picker_free(GsrX11WindowPicker *self)
{
    if (!self)
        return;

    if (self->source) {
        g_source_destroy(self->source);
        g_source_unref(self->source);
        self->source = NULL;
    }

    if (self->display) {
        XUngrabPointer(self->display, CurrentTime);
        XUngrabKeyboard(self->display, CurrentTime);
        if (self->crosshair)
            XFreeCursor(self->display, self->crosshair);
        XCloseDisplay(self->display);
        self->display = NULL;
    }

    g_free(self);
}
