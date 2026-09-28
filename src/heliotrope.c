/*
 * Heliotrope -- a small web browser for IRIX, on WPE WebKit.
 *
 *   heliotrope [-g WIDTHxHEIGHT] [-o shot.ppm] [-s seconds] [-q] [URL]
 *
 * WebKit renders into WPE's headless platform (EGL on Mesa's software
 * rasterizer, frames in shared memory); each frame is copied into a 24-bit
 * TrueColor X window, and X input goes back to WebKit as WPE events.
 *
 * The strip at the top shows the address and the load progress. Ctrl+L
 * edits the address (Enter loads, Escape cancels); Alt+Left/Right go back
 * and forward, F5 reloads, Ctrl+Q quits.
 *
 * -o writes the page as a PPM image once it has loaded and -s seconds
 * (default 5) have passed; -q then quits.
 */

#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>
#include <wpe/headless/wpe-headless.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <glib-unix.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BAR_HEIGHT 22

typedef struct {
    Display *dpy;
    Window win;
    Visual *visual;
    int depth;
    GC gc;
    XFontStruct *font;
    unsigned long bar_bg, bar_fg, bar_edit, progress;
    int rshift, gshift, bshift; /* of the visual's masks */
    int width, height;          /* of the window */

    XImage *image;              /* the last frame, in the visual's format */
    int image_width, image_height;

    WebKitWebView *web_view;
    WPEView *view;

    gboolean editing;
    GString *edit;
    double progress_value;

    const char *shot_path;
    int shot_delay;
    gboolean quit_after_shot;
    gboolean shot_done;
    GMainLoop *loop;
    guint8 *last_rgb;           /* the last frame as RGB, for -o */
    int last_width, last_height;
} App;

static int mask_shift(unsigned long mask)
{
    int s = 0;
    if (!mask)
        return 0;
    while (!(mask & 1)) {
        mask >>= 1;
        s++;
    }
    return s;
}

/* --- drawing -------------------------------------------------------------- */

static void draw_bar(App *app)
{
    const char *text;
    char *uri = NULL;
    XSetForeground(app->dpy, app->gc, app->editing ? app->bar_edit : app->bar_bg);
    XFillRectangle(app->dpy, app->win, app->gc, 0, 0, app->width, BAR_HEIGHT);
    if (app->progress_value > 0 && app->progress_value < 1) {
        XSetForeground(app->dpy, app->gc, app->progress);
        XFillRectangle(app->dpy, app->win, app->gc, 0, BAR_HEIGHT - 3,
            (unsigned)(app->width * app->progress_value), 3);
    }
    if (app->editing)
        text = app->edit->str;
    else {
        const char *u = webkit_web_view_get_uri(app->web_view);
        uri = g_strdup(u ? u : "");
        text = uri;
    }
    XSetForeground(app->dpy, app->gc, app->bar_fg);
    XDrawString(app->dpy, app->win, app->gc, 6, 15, text, (int)strlen(text));
    if (app->editing) {
        int x = 6 + XTextWidth(app->font, text, (int)strlen(text));
        XFillRectangle(app->dpy, app->win, app->gc, x + 1, 4, 2, 14);
    }
    g_free(uri);
}

static void draw_page(App *app)
{
    if (!app->image)
        return;
    XPutImage(app->dpy, app->win, app->gc, app->image, 0, 0, 0, BAR_HEIGHT,
        app->image_width, app->image_height);
}

static void redraw(App *app)
{
    draw_bar(app);
    draw_page(app);
    XFlush(app->dpy);
}

/* --- frames from WebKit ----------------------------------------------------- */

static void write_ppm(App *app)
{
    FILE *f = fopen(app->shot_path, "wb");
    if (!f) {
        perror(app->shot_path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", app->last_width, app->last_height);
    fwrite(app->last_rgb, 3, (size_t)app->last_width * app->last_height, f);
    fclose(f);
    g_printerr("heliotrope: wrote %s (%dx%d)\n", app->shot_path, app->last_width, app->last_height);
}

static void buffer_rendered(WPEView *view, WPEBuffer *buffer, App *app)
{
    GError *error = NULL;
    /* transfer none: the pixels belong to the buffer (valid until it is released). */
    GBytes *bytes = wpe_buffer_import_to_pixels(buffer, &error);
    int w = wpe_buffer_get_width(buffer), h = wpe_buffer_get_height(buffer);
    const guint8 *src;
    gsize size;
    guint stride;
    int x, y;
    (void)view;

    if (!bytes) {
        g_printerr("heliotrope: frame: %s\n", error ? error->message : "no pixels");
        g_clear_error(&error);
        return;
    }
    src = g_bytes_get_data(bytes, &size);
    stride = WPE_IS_BUFFER_SHM(buffer) ? wpe_buffer_shm_get_stride(WPE_BUFFER_SHM(buffer)) : (guint)w * 4;
    if ((gsize)stride * h > size)
        return;

    if (!app->image || app->image_width != w || app->image_height != h) {
        if (app->image)
            XDestroyImage(app->image);
        app->image = XCreateImage(app->dpy, app->visual, app->depth, ZPixmap, 0,
            malloc((size_t)w * h * 4), w, h, 32, 0);
        app->image_width = w;
        app->image_height = h;
    }
    if (app->shot_path && (app->last_width != w || app->last_height != h)) {
        g_free(app->last_rgb);
        app->last_rgb = g_malloc((size_t)w * h * 3);
        app->last_width = w;
        app->last_height = h;
    }

    /* ARGB8888 as DRM defines it: a little-endian 32-bit word, so B, G, R, A
     * in memory whatever the host's byte order. Alpha is premultiplied, and
     * the page is opaque: drop it. */
    for (y = 0; y < h; y++) {
        const guint8 *p = src + (gsize)y * stride;
        guint8 *rgb = app->last_rgb ? app->last_rgb + (gsize)y * w * 3 : NULL;
        for (x = 0; x < w; x++, p += 4) {
            unsigned long pixel = ((unsigned long)p[2] << app->rshift)
                | ((unsigned long)p[1] << app->gshift) | ((unsigned long)p[0] << app->bshift);
            XPutPixel(app->image, x, y, pixel);
            if (rgb) {
                *rgb++ = p[2];
                *rgb++ = p[1];
                *rgb++ = p[0];
            }
        }
    }
    draw_page(app);
    XFlush(app->dpy);
}

/* --- page state --------------------------------------------------------------- */

static gboolean take_shot(gpointer data)
{
    App *app = data;
    if (app->last_rgb)
        write_ppm(app);
    app->shot_done = TRUE;
    if (app->quit_after_shot)
        g_main_loop_quit(app->loop);
    return G_SOURCE_REMOVE;
}

static void load_changed(WebKitWebView *web_view, WebKitLoadEvent event, App *app)
{
    (void)web_view;
    if (event == WEBKIT_LOAD_FINISHED) {
        g_printerr("heliotrope: loaded %s\n", webkit_web_view_get_uri(app->web_view));
        if (app->shot_path && !app->shot_done)
            g_timeout_add_seconds(app->shot_delay, take_shot, app);
    }
}

static void load_failed(WebKitWebView *web_view, WebKitLoadEvent event, char *uri, GError *error, App *app)
{
    (void)web_view;
    (void)event;
    (void)app;
    g_printerr("heliotrope: failed to load %s: %s\n", uri, error->message);
}

static void title_changed(GObject *object, GParamSpec *pspec, App *app)
{
    const char *title = webkit_web_view_get_title(app->web_view);
    char *name = g_strdup_printf("Heliotrope%s%s", title && *title ? " - " : "", title ? title : "");
    (void)object;
    (void)pspec;
    XStoreName(app->dpy, app->win, name);
    g_free(name);
}

static void progress_changed(GObject *object, GParamSpec *pspec, App *app)
{
    (void)object;
    (void)pspec;
    app->progress_value = webkit_web_view_get_estimated_load_progress(app->web_view);
    draw_bar(app);
    XFlush(app->dpy);
}

static void load_address(App *app, const char *text)
{
    char *uri;
    if (strstr(text, "://") || g_str_has_prefix(text, "about:") || g_str_has_prefix(text, "data:"))
        uri = g_strdup(text);
    else
        uri = g_strconcat("https://", text, NULL);
    webkit_web_view_load_uri(app->web_view, uri);
    g_free(uri);
}

/* --- X input ---------------------------------------------------------------- */

static WPEModifiers modifiers_of(unsigned int state)
{
    unsigned m = 0;
    if (state & ControlMask)
        m |= WPE_MODIFIER_KEYBOARD_CONTROL;
    if (state & ShiftMask)
        m |= WPE_MODIFIER_KEYBOARD_SHIFT;
    if (state & Mod1Mask)
        m |= WPE_MODIFIER_KEYBOARD_ALT;
    if (state & LockMask)
        m |= WPE_MODIFIER_KEYBOARD_CAPS_LOCK;
    if (state & Button1Mask)
        m |= WPE_MODIFIER_POINTER_BUTTON1;
    if (state & Button2Mask)
        m |= WPE_MODIFIER_POINTER_BUTTON2;
    if (state & Button3Mask)
        m |= WPE_MODIFIER_POINTER_BUTTON3;
    return (WPEModifiers)m;
}

static void send(App *app, WPEEvent *event)
{
    wpe_view_event(app->view, event);
    wpe_event_unref(event);
}

static gboolean handle_bar_key(App *app, KeySym sym, const char *text, unsigned int state)
{
    if (!app->editing) {
        if ((state & ControlMask) && (sym == XK_l || sym == XK_L)) {
            const char *u = webkit_web_view_get_uri(app->web_view);
            app->editing = TRUE;
            g_string_assign(app->edit, u ? u : "");
            draw_bar(app);
            return TRUE;
        }
        if ((state & ControlMask) && (sym == XK_q || sym == XK_Q)) {
            g_main_loop_quit(app->loop);
            return TRUE;
        }
        if ((state & Mod1Mask) && sym == XK_Left) {
            webkit_web_view_go_back(app->web_view);
            return TRUE;
        }
        if ((state & Mod1Mask) && sym == XK_Right) {
            webkit_web_view_go_forward(app->web_view);
            return TRUE;
        }
        if (sym == XK_F5) {
            webkit_web_view_reload(app->web_view);
            return TRUE;
        }
        return FALSE;
    }
    if (sym == XK_Return || sym == XK_KP_Enter) {
        app->editing = FALSE;
        load_address(app, app->edit->str);
    } else if (sym == XK_Escape)
        app->editing = FALSE;
    else if (sym == XK_BackSpace) {
        if (app->edit->len)
            g_string_truncate(app->edit, app->edit->len - 1);
    } else if (text[0] && (unsigned char)text[0] >= 32)
        g_string_append(app->edit, text);
    draw_bar(app);
    return TRUE;
}

static void handle_x_event(App *app, XEvent *ev)
{
    switch (ev->type) {
    case Expose:
        if (!ev->xexpose.count)
            redraw(app);
        break;
    case ConfigureNotify:
        if (ev->xconfigure.width != app->width || ev->xconfigure.height != app->height) {
            app->width = ev->xconfigure.width;
            app->height = ev->xconfigure.height;
            if (app->height > BAR_HEIGHT)
                wpe_toplevel_resize(wpe_view_get_toplevel(app->view), app->width, app->height - BAR_HEIGHT);
            draw_bar(app);
        }
        break;
    case FocusIn:
        wpe_view_focus_in(app->view);
        break;
    case FocusOut:
        wpe_view_focus_out(app->view);
        break;
    case ButtonPress:
    case ButtonRelease: {
        XButtonEvent *b = &ev->xbutton;
        if (b->y < BAR_HEIGHT)
            break;
        if (b->button >= 4 && b->button <= 7) {
            double dx = 0, dy = 0;
            if (ev->type == ButtonRelease)
                break;
            if (b->button == 4)
                dy = 1;
            else if (b->button == 5)
                dy = -1;
            else if (b->button == 6)
                dx = 1;
            else
                dx = -1;
            send(app, wpe_event_scroll_new(app->view, WPE_INPUT_SOURCE_MOUSE, (guint32)b->time,
                modifiers_of(b->state), dx * 40, dy * 40, FALSE, FALSE, b->x, b->y - BAR_HEIGHT));
            break;
        }
        send(app, wpe_event_pointer_button_new(ev->type == ButtonPress ? WPE_EVENT_POINTER_DOWN : WPE_EVENT_POINTER_UP,
            app->view, WPE_INPUT_SOURCE_MOUSE, (guint32)b->time, modifiers_of(b->state),
            b->button, b->x, b->y - BAR_HEIGHT, ev->type == ButtonPress ? 1 : 0));
        break;
    }
    case MotionNotify: {
        XMotionEvent *m = &ev->xmotion;
        send(app, wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, app->view, WPE_INPUT_SOURCE_MOUSE,
            (guint32)m->time, modifiers_of(m->state), m->x, m->y - BAR_HEIGHT, 0, 0));
        break;
    }
    case KeyPress:
    case KeyRelease: {
        XKeyEvent *k = &ev->xkey;
        char text[16] = { 0 };
        KeySym sym = NoSymbol;
        XLookupString(k, text, sizeof(text) - 1, &sym, NULL);
        if (ev->type == KeyPress && handle_bar_key(app, sym, text, k->state))
            break;
        if (app->editing)
            break;
        /* X keysyms are what WPE (XKB) keyvals are. */
        send(app, wpe_event_keyboard_new(ev->type == KeyPress ? WPE_EVENT_KEYBOARD_KEY_DOWN : WPE_EVENT_KEYBOARD_KEY_UP,
            app->view, WPE_INPUT_SOURCE_KEYBOARD, (guint32)k->time, modifiers_of(k->state),
            k->keycode, (guint)sym));
        break;
    }
    case ClientMessage:
        g_main_loop_quit(app->loop);
        break;
    }
}

static gboolean x_ready(gint fd, GIOCondition condition, gpointer data)
{
    App *app = data;
    (void)fd;
    (void)condition;
    while (XPending(app->dpy)) {
        XEvent ev;
        XNextEvent(app->dpy, &ev);
        handle_x_event(app, &ev);
    }
    XFlush(app->dpy);
    return G_SOURCE_CONTINUE;
}

/* --- setup --------------------------------------------------------------------- */

static unsigned long color(App *app, int r, int g, int b)
{
    return ((unsigned long)r << app->rshift) | ((unsigned long)g << app->gshift) | ((unsigned long)b << app->bshift);
}

static gboolean open_window(App *app)
{
    XVisualInfo vinfo;
    XSetWindowAttributes attrs;
    Atom wm_delete;
    int screen;

    app->dpy = XOpenDisplay(NULL);
    if (!app->dpy) {
        g_printerr("heliotrope: cannot open display %s\n", XDisplayName(NULL));
        return FALSE;
    }
    screen = DefaultScreen(app->dpy);
    /* Xsgi's default visual is 8-bit PseudoColor: ask for 24-bit TrueColor. */
    if (!XMatchVisualInfo(app->dpy, screen, 24, TrueColor, &vinfo)) {
        g_printerr("heliotrope: no 24-bit TrueColor visual\n");
        return FALSE;
    }
    app->visual = vinfo.visual;
    app->depth = vinfo.depth;
    app->rshift = mask_shift(vinfo.red_mask);
    app->gshift = mask_shift(vinfo.green_mask);
    app->bshift = mask_shift(vinfo.blue_mask);

    attrs.colormap = XCreateColormap(app->dpy, RootWindow(app->dpy, screen), app->visual, AllocNone);
    attrs.background_pixel = 0;
    attrs.border_pixel = 0;
    attrs.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask | KeyReleaseMask
        | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | FocusChangeMask;
    app->win = XCreateWindow(app->dpy, RootWindow(app->dpy, screen), 0, 0,
        app->width, app->height, 0, app->depth, InputOutput, app->visual,
        CWColormap | CWBackPixel | CWBorderPixel | CWEventMask, &attrs);
    XStoreName(app->dpy, app->win, "Heliotrope");
    wm_delete = XInternAtom(app->dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(app->dpy, app->win, &wm_delete, 1);

    app->gc = XCreateGC(app->dpy, app->win, 0, NULL);
    app->font = XLoadQueryFont(app->dpy, "-*-helvetica-medium-r-normal--12-*-*-*-*-*-iso8859-1");
    if (!app->font)
        app->font = XLoadQueryFont(app->dpy, "fixed");
    if (app->font)
        XSetFont(app->dpy, app->gc, app->font->fid);
    app->bar_bg = color(app, 0xdd, 0xd6, 0xe8);
    app->bar_edit = color(app, 0xff, 0xff, 0xff);
    app->bar_fg = color(app, 0x20, 0x10, 0x30);
    app->progress = color(app, 0x8a, 0x5c, 0xc0);
    XMapWindow(app->dpy, app->win);
    XFlush(app->dpy);
    return TRUE;
}

int main(int argc, char **argv)
{
    App app = { 0 };
    WPEDisplay *display;
    const char *url = "https://en.wikipedia.org/wiki/IRIX";
    int i;

    app.width = 1024;
    app.height = 768;
    app.shot_delay = 5;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-g") && i + 1 < argc)
            sscanf(argv[++i], "%dx%d", &app.width, &app.height);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc)
            app.shot_path = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc)
            app.shot_delay = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-q"))
            app.quit_after_shot = TRUE;
        else if (argv[i][0] == '-') {
            g_printerr("usage: heliotrope [-g WIDTHxHEIGHT] [-o shot.ppm] [-s seconds] [-q] [URL]\n");
            return 2;
        } else
            url = argv[i];
    }

    app.edit = g_string_new(NULL);
    if (!open_window(&app))
        return 1;

    display = wpe_display_headless_new();
    app.web_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display, NULL));
    g_object_unref(display);
    app.view = webkit_web_view_get_wpe_view(app.web_view);
    wpe_toplevel_resize(wpe_view_get_toplevel(app.view), app.width, app.height - BAR_HEIGHT);
    g_signal_connect(app.view, "buffer-rendered", G_CALLBACK(buffer_rendered), &app);
    g_signal_connect(app.web_view, "load-changed", G_CALLBACK(load_changed), &app);
    g_signal_connect(app.web_view, "load-failed", G_CALLBACK(load_failed), &app);
    g_signal_connect(app.web_view, "notify::title", G_CALLBACK(title_changed), &app);
    g_signal_connect(app.web_view, "notify::estimated-load-progress", G_CALLBACK(progress_changed), &app);
    wpe_view_focus_in(app.view);

    g_unix_fd_add(ConnectionNumber(app.dpy), G_IO_IN, x_ready, &app);
    load_address(&app, url);

    app.loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(app.loop);
    return 0;
}
