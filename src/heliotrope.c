/*
 * Heliotrope -- a small web browser for IRIX, on WPE WebKit.
 *
 *   heliotrope [-g WIDTHxHEIGHT[+X+Y]] [-o shot.ppm] [-s seconds] [-q] [URL]
 *
 * WebKit renders into WPE's headless platform. With irix-egl-gles (EGL and
 * GLES on IRIX's own OpenGL) it copies each frame on the GPU into the page
 * area's child window; otherwise frames arrive in shared memory and are
 * copied into a 24-bit TrueColor X window. X input goes back to WebKit as
 * WPE events.
 *
 * The toolbar has Back, Forward, Reload (Stop while loading) and Home, and
 * the address field: click it (or Ctrl+L) and type; Enter goes, Escape
 * cancels. Text that is not an address searches DuckDuckGo Lite. The
 * scrollbar is WebKit's own (HELIOTROPE_OVERLAY_SCROLLBARS=1 for the kind
 * that appears only while scrolling). Alt+Left/Right go back and forward,
 * F5 reloads, Ctrl+Q quits.
 *
 * -o writes the page as a PPM image once it has loaded and -s seconds
 * (default 5) have passed; -q then quits. HELIOTROPE_HOME sets Home (default:
 * the first page).
 *
 * Blocking: third-party loads from ad and tracking servers are blocked (Peter
 * Lowe's list, https://pgl.yoyo.org/adservers/, compiled by WebKit's content
 * blocker on first use and cached). The shield button turns blocking off for
 * the current site, the JS button JavaScript; both are remembered per site in
 * ~/.config/heliotrope/sites. HELIOTROPE_BLOCKLIST names another rule list.
 *
 * Caches: WebKit's HTTP cache (it follows Cache-Control, Expires and
 * revalidation) is in ~/.cache/heliotrope, and scripts' compiled bytecode in
 * ~/.cache/heliotrope/bytecode, trimmed to HELIOTROPE_BYTECODE_CACHE_MB
 * (default 64) at start; HELIOTROPE_NO_BYTECODE_CACHE=1 turns that off.
 *
 * Memory: every site shares one Web process (HELIOTROPE_SITE_ISOLATION=1
 * gives each site its own, as WebKit does by default), caches are sized for
 * documents rather than a big browser, and WebKit is asked to shed caches
 * well before HELIOTROPE_MEMORY_LIMIT (MB per process, default 256).
 */

#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>
#include <wpe/headless/wpe-headless.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/extensions/XShm.h>
#include <dlfcn.h>

#include <glib-unix.h>
#include <glib/gstdio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/resource.h>
#include <sys/shm.h>
#include <sys/stat.h>

#ifndef BLOCKLIST_PATH
#define BLOCKLIST_PATH "/opt/pkgsrc/share/heliotrope/blocklist.json"
#endif

#define BAR_HEIGHT 30           /* toolbar */
#define BUTTON_WIDTH 28
#define BUTTON_HEIGHT 24

enum { BUTTON_BACK, BUTTON_FORWARD, BUTTON_RELOAD, BUTTON_HOME, BUTTON_JS, BUTTON_BLOCK, N_BUTTONS };

/* per-site choices, remembered in ~/.config/heliotrope/sites */
enum { SITE_NO_JS = 1, SITE_NO_BLOCKING = 2 };

typedef struct {
    Display *dpy;
    Window win;
    Visual *visual;
    int depth;
    GC gc;
    XFontStruct *font;
    Cursor arrow, hand, text;
    Cursor current_cursor;
    unsigned long bar_bg, bar_shadow, bar_light, ink, ink_disabled, field_bg, field_edit_bg,
        selection, progress, status_bg;
    int rshift, gshift, bshift; /* of the visual's masks */
    int width, height;          /* of the window */
    int x, y;                   /* where -g asked for it */
    gboolean positioned;

    /* HELIOTROPE_TIMING=1: a load timeline on stderr */
    gboolean timing;
    gint64 load_start;
    gboolean first_frame_seen;
    unsigned frames;
    Atom wm_delete, clipboard, utf8_string, selection_property;

    /* Present mode (irix-egl-gles): WebKit copies its frames on the GPU into
     * page_win, a child window over the page area; no pixels reach us. The
     * status line then needs its own window above it. */
    gboolean present;
    Window page_win, status_win;

    XImage *image;              /* the last frame, in the visual's format */
    XShmSegmentInfo shm;
    gboolean use_shm;
    int image_width, image_height;

    WebKitWebView *web_view;
    WPEView *view;
    char *home;

    /* the address field */
    gboolean editing;
    GString *edit;
    gsize cursor;               /* byte offset in edit */
    gboolean all_selected;      /* typing replaces the whole text */
    double progress_value;
    int pressed_button;         /* toolbar button under a press, or -1 */


    char *status;               /* the link under the pointer */

    /* blocking and per-site choices */
    WebKitUserContentManager *content_manager;
    WebKitUserContentFilterStore *filter_store;
    WebKitUserContentFilter *filter;    /* the compiled block list, once ready */
    gboolean filter_active;             /* added to content_manager */
    char *filter_id;
    char *pending_url;                  /* the first page, waiting for the filter */
    GHashTable *sites;                  /* host -> SITE_* flags */
    char *sites_path;

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

static int page_width(App *app)
{
    return app->width > 0 ? app->width : 1;
}

static int page_height(App *app)
{
    return app->height > BAR_HEIGHT ? app->height - BAR_HEIGHT : 1;
}

static void button_rect(int i, XRectangle *r)
{
    r->x = 4 + i * (BUTTON_WIDTH + 2);
    r->y = (BAR_HEIGHT - BUTTON_HEIGHT) / 2;
    r->width = BUTTON_WIDTH;
    r->height = BUTTON_HEIGHT;
}

static void field_rect(App *app, XRectangle *r)
{
    int x = 4 + N_BUTTONS * (BUTTON_WIDTH + 2) + 6;
    r->x = x;
    r->y = (BAR_HEIGHT - BUTTON_HEIGHT) / 2;
    r->width = app->width - x - 6 > 40 ? app->width - x - 6 : 40;
    r->height = BUTTON_HEIGHT;
}

static gboolean in_rect(const XRectangle *r, int x, int y)
{
    return x >= r->x && x < r->x + r->width && y >= r->y && y < r->y + r->height;
}

/* --- drawing -------------------------------------------------------------- */

static void bevel(App *app, const XRectangle *r, gboolean sunken)
{
    XSetForeground(app->dpy, app->gc, sunken ? app->bar_shadow : app->bar_light);
    XDrawLine(app->dpy, app->win, app->gc, r->x, r->y, r->x + r->width - 1, r->y);
    XDrawLine(app->dpy, app->win, app->gc, r->x, r->y, r->x, r->y + r->height - 1);
    XSetForeground(app->dpy, app->gc, sunken ? app->bar_light : app->bar_shadow);
    XDrawLine(app->dpy, app->win, app->gc, r->x, r->y + r->height - 1, r->x + r->width - 1, r->y + r->height - 1);
    XDrawLine(app->dpy, app->win, app->gc, r->x + r->width - 1, r->y, r->x + r->width - 1, r->y + r->height - 1);
}

static unsigned current_site_flags(App *app);

/* An environment switch is on when set to anything but "" or "0". */
static gboolean env_on(const char *name)
{
    const char *v = getenv(name);
    return v && *v && strcmp(v, "0");
}

static void draw_icon(App *app, int which, const XRectangle *r, gboolean enabled)
{
    int cx = r->x + r->width / 2, cy = r->y + r->height / 2;
    XPoint p[8];
    XSetForeground(app->dpy, app->gc, enabled ? app->ink : app->ink_disabled);
    switch (which) {
    case BUTTON_BACK:
    case BUTTON_FORWARD: {
        int d = which == BUTTON_BACK ? -1 : 1;
        p[0].x = cx + d * 7; p[0].y = cy;
        p[1].x = cx - d * 1; p[1].y = cy - 7;
        p[2].x = cx - d * 1; p[2].y = cy - 3;
        p[3].x = cx - d * 7; p[3].y = cy - 3;
        p[4].x = cx - d * 7; p[4].y = cy + 3;
        p[5].x = cx - d * 1; p[5].y = cy + 3;
        p[6].x = cx - d * 1; p[6].y = cy + 7;
        XFillPolygon(app->dpy, app->win, app->gc, p, 7, Nonconvex, CoordModeOrigin);
        break;
    }
    case BUTTON_RELOAD:
        if (webkit_web_view_is_loading(app->web_view)) {
            /* Stop: an X */
            XSetLineAttributes(app->dpy, app->gc, 3, LineSolid, CapRound, JoinRound);
            XDrawLine(app->dpy, app->win, app->gc, cx - 5, cy - 5, cx + 5, cy + 5);
            XDrawLine(app->dpy, app->win, app->gc, cx - 5, cy + 5, cx + 5, cy - 5);
            XSetLineAttributes(app->dpy, app->gc, 0, LineSolid, CapButt, JoinMiter);
        } else {
            XSetLineAttributes(app->dpy, app->gc, 2, LineSolid, CapButt, JoinMiter);
            XDrawArc(app->dpy, app->win, app->gc, cx - 7, cy - 7, 14, 14, 100 * 64, 280 * 64);
            XSetLineAttributes(app->dpy, app->gc, 0, LineSolid, CapButt, JoinMiter);
            p[0].x = cx - 1; p[0].y = cy - 11;
            p[1].x = cx + 5; p[1].y = cy - 7;
            p[2].x = cx - 1; p[2].y = cy - 3;
            XFillPolygon(app->dpy, app->win, app->gc, p, 3, Convex, CoordModeOrigin);
        }
        break;
    case BUTTON_HOME:
        p[0].x = cx; p[0].y = cy - 8;
        p[1].x = cx + 8; p[1].y = cy - 1;
        p[2].x = cx + 5; p[2].y = cy - 1;
        p[3].x = cx + 5; p[3].y = cy + 7;
        p[4].x = cx - 5; p[4].y = cy + 7;
        p[5].x = cx - 5; p[5].y = cy - 1;
        p[6].x = cx - 8; p[6].y = cy - 1;
        XFillPolygon(app->dpy, app->win, app->gc, p, 7, Nonconvex, CoordModeOrigin);
        XSetForeground(app->dpy, app->gc, app->bar_bg);
        XFillRectangle(app->dpy, app->win, app->gc, cx - 2, cy + 2, 4, 5);
        break;
    case BUTTON_JS: {
        gboolean on = !(current_site_flags(app) & SITE_NO_JS);
        XSetForeground(app->dpy, app->gc, on ? app->ink : app->ink_disabled);
        if (app->font)
            XDrawString(app->dpy, app->win, app->gc, cx - XTextWidth(app->font, "JS", 2) / 2,
                cy + (app->font->ascent - app->font->descent) / 2, "JS", 2);
        if (!on) {
            XSetForeground(app->dpy, app->gc, app->ink);
            XDrawLine(app->dpy, app->win, app->gc, cx - 8, cy + 7, cx + 8, cy - 7);
        }
        break;
    }
    case BUTTON_BLOCK: {
        gboolean on = app->filter && !(current_site_flags(app) & SITE_NO_BLOCKING);
        p[0].x = cx - 7; p[0].y = cy - 7;
        p[1].x = cx + 7; p[1].y = cy - 7;
        p[2].x = cx + 7; p[2].y = cy;
        p[3].x = cx; p[3].y = cy + 8;
        p[4].x = cx - 7; p[4].y = cy;
        p[5] = p[0];
        if (on) {
            XSetForeground(app->dpy, app->gc, app->progress);
            XFillPolygon(app->dpy, app->win, app->gc, p, 5, Convex, CoordModeOrigin);
        } else {
            XSetForeground(app->dpy, app->gc, app->ink_disabled);
            XDrawLines(app->dpy, app->win, app->gc, p, 6, CoordModeOrigin);
            XDrawLine(app->dpy, app->win, app->gc, cx - 8, cy + 7, cx + 8, cy - 7);
        }
        break;
    }
    }
}

/* the SITE_* flags of the page being shown */
static unsigned current_site_flags(App *app)
{
    const char *uri = webkit_web_view_get_uri(app->web_view);
    GUri *parsed = uri ? g_uri_parse(uri, G_URI_FLAGS_NONE, NULL) : NULL;
    unsigned flags = 0;
    if (parsed) {
        const char *host = g_uri_get_host(parsed);
        if (host)
            flags = GPOINTER_TO_UINT(g_hash_table_lookup(app->sites, host));
        g_uri_unref(parsed);
    }
    return flags;
}

static gboolean button_enabled(App *app, int i)
{
    switch (i) {
    case BUTTON_BACK:
        return webkit_web_view_can_go_back(app->web_view);
    case BUTTON_FORWARD:
        return webkit_web_view_can_go_forward(app->web_view);
    default:
        return TRUE;
    }
}

static void draw_field(App *app)
{
    XRectangle r;
    const char *text;
    int len, start = 0, avail, tx, ty;
    field_rect(app, &r);

    XSetForeground(app->dpy, app->gc, app->editing ? app->field_edit_bg : app->field_bg);
    XFillRectangle(app->dpy, app->win, app->gc, r.x, r.y, r.width, r.height);
    bevel(app, &r, TRUE);
    if (!app->editing && app->progress_value > 0 && app->progress_value < 1) {
        XSetForeground(app->dpy, app->gc, app->progress);
        XFillRectangle(app->dpy, app->win, app->gc, r.x + 1, r.y + r.height - 4,
            (unsigned)((r.width - 2) * app->progress_value), 3);
    }

    if (app->editing)
        text = app->edit->str;
    else {
        const char *u = webkit_web_view_get_uri(app->web_view);
        text = u ? u : "";
    }
    len = (int)strlen(text);
    avail = r.width - 12;
    /* keep the cursor in view: drop leading characters while it is past the end */
    if (app->editing && app->font) {
        while (start < (int)app->cursor && XTextWidth(app->font, text + start, (int)app->cursor - start) > avail)
            start++;
    }
    tx = r.x + 6;
    ty = r.y + (r.height + (app->font ? app->font->ascent - app->font->descent : 8)) / 2;

    XSetClipRectangles(app->dpy, app->gc, 0, 0, &r, 1, Unsorted);
    if (app->editing && app->all_selected && len) {
        XSetForeground(app->dpy, app->gc, app->selection);
        XFillRectangle(app->dpy, app->win, app->gc, tx - 1, r.y + 3,
            (unsigned)(app->font ? XTextWidth(app->font, text + start, len - start) + 2 : 0), r.height - 6);
    }
    XSetForeground(app->dpy, app->gc, app->ink);
    XDrawString(app->dpy, app->win, app->gc, tx, ty, text + start, len - start);
    if (app->editing && !app->all_selected) {
        int cx = tx + (app->font ? XTextWidth(app->font, text + start, (int)app->cursor - start) : 0);
        XFillRectangle(app->dpy, app->win, app->gc, cx, r.y + 4, 1, r.height - 8);
    }
    XSetClipMask(app->dpy, app->gc, None);
}

static void draw_bar(App *app)
{
    int i;
    XSetForeground(app->dpy, app->gc, app->bar_bg);
    XFillRectangle(app->dpy, app->win, app->gc, 0, 0, app->width, BAR_HEIGHT - 1);
    XSetForeground(app->dpy, app->gc, app->bar_shadow);
    XDrawLine(app->dpy, app->win, app->gc, 0, BAR_HEIGHT - 1, app->width, BAR_HEIGHT - 1);
    for (i = 0; i < N_BUTTONS; i++) {
        XRectangle r;
        gboolean enabled = button_enabled(app, i);
        button_rect(i, &r);
        if (enabled)
            bevel(app, &r, app->pressed_button == i);
        draw_icon(app, i, &r, enabled);
    }
    draw_field(app);
}

static void draw_status(App *app)
{
    int w, y;
    if (!app->status || !*app->status || !app->font) {
        if (app->present)
            XUnmapWindow(app->dpy, app->status_win);
        return;
    }
    if (app->present) {
        int h = app->font->ascent + app->font->descent + 6;
        w = XTextWidth(app->font, app->status, (int)strlen(app->status)) + 10;
        if (w > page_width(app))
            w = page_width(app);
        XMoveResizeWindow(app->dpy, app->status_win, 0, app->height - h, w, h);
        XMapRaised(app->dpy, app->status_win);
        XSetForeground(app->dpy, app->gc, app->status_bg);
        XFillRectangle(app->dpy, app->status_win, app->gc, 0, 0, w, h);
        XSetForeground(app->dpy, app->gc, app->ink);
        XDrawString(app->dpy, app->status_win, app->gc, 5, h - app->font->descent - 3,
            app->status, (int)strlen(app->status));
        return;
    }
    w = XTextWidth(app->font, app->status, (int)strlen(app->status)) + 10;
    if (w > page_width(app))
        w = page_width(app);
    y = app->height - (app->font->ascent + app->font->descent + 6);
    XSetForeground(app->dpy, app->gc, app->status_bg);
    XFillRectangle(app->dpy, app->win, app->gc, 0, y, w, app->height - y);
    XSetForeground(app->dpy, app->gc, app->ink);
    XDrawString(app->dpy, app->win, app->gc, 5, app->height - app->font->descent - 3,
        app->status, (int)strlen(app->status));
}

static void draw_page(App *app)
{
    if (app->present || !app->image) {
        draw_status(app);
        return;
    }
    if (app->use_shm)
        XShmPutImage(app->dpy, app->win, app->gc, app->image, 0, 0, 0, BAR_HEIGHT,
            app->image_width, app->image_height, False);
    else
        XPutImage(app->dpy, app->win, app->gc, app->image, 0, 0, 0, BAR_HEIGHT,
            app->image_width, app->image_height);
    draw_status(app);
}

static void redraw(App *app)
{
    draw_bar(app);
    draw_page(app);
    XFlush(app->dpy);
}

/* --- frames from WebKit ----------------------------------------------------- */

static int shm_failed;

static int shm_error_handler(Display *dpy, XErrorEvent *e)
{
    (void)dpy;
    (void)e;
    shm_failed = 1;
    return 0;
}

static void free_image(App *app)
{
    if (!app->image)
        return;
    if (app->use_shm) {
        XShmDetach(app->dpy, &app->shm);
        XSync(app->dpy, False);
        XDestroyImage(app->image);
        shmdt(app->shm.shmaddr);
    } else
        XDestroyImage(app->image);
    app->image = NULL;
}

static void make_image(App *app, int w, int h)
{
    free_image(app);
    app->image_width = w;
    app->image_height = h;
    if (app->use_shm) {
        app->image = XShmCreateImage(app->dpy, app->visual, app->depth, ZPixmap, NULL, &app->shm, w, h);
        if (app->image) {
            app->shm.shmid = shmget(IPC_PRIVATE, (size_t)app->image->bytes_per_line * h, IPC_CREAT | 0600);
            app->shm.shmaddr = app->shm.shmid >= 0 ? shmat(app->shm.shmid, NULL, 0) : (char *)-1;
            if (app->shm.shmaddr != (char *)-1) {
                XErrorHandler old;
                app->image->data = app->shm.shmaddr;
                app->shm.readOnly = False;
                shm_failed = 0;
                old = XSetErrorHandler(shm_error_handler);
                XShmAttach(app->dpy, &app->shm);
                XSync(app->dpy, False);
                XSetErrorHandler(old);
                /* IRIX cannot attach a segment already marked for removal: only now. */
                shmctl(app->shm.shmid, IPC_RMID, NULL);
                if (!shm_failed)
                    return;
                shmdt(app->shm.shmaddr);
            } else if (app->shm.shmid >= 0)
                shmctl(app->shm.shmid, IPC_RMID, NULL);
            app->image->data = NULL;
            XDestroyImage(app->image);
            app->image = NULL;
        }
        g_printerr("heliotrope: X shared memory unavailable, using XPutImage\n");
        app->use_shm = FALSE;
    }
    app->image = XCreateImage(app->dpy, app->visual, app->depth, ZPixmap, 0,
        malloc((size_t)w * h * 4), w, h, 32, 0);
}

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

static void timing_mark(App *app, const char *what)
{
    if (app->timing && app->load_start)
        g_printerr("heliotrope: +%6.1f s  %s (%u frames)\n", (g_get_monotonic_time() - app->load_start) / 1e6, what, app->frames);
}

static void buffer_rendered(WPEView *view, WPEBuffer *buffer, App *app)
{
    gint64 t0 = g_get_monotonic_time();
    app->frames++;
    if (!app->first_frame_seen && app->load_start) {
        app->first_frame_seen = TRUE;
        timing_mark(app, "first frame");
    } else if (app->timing && app->load_start)
        timing_mark(app, "frame");
    if (app->present) {
        /* WebKit has already copied the frame into page_win. */
        if (app->timing)
            g_printerr("heliotrope:          frame presented by WebKit\n");
        return;
    }
    GError *error = NULL;
    /* transfer none: the pixels belong to the buffer (valid until it is released). */
    GBytes *bytes = wpe_buffer_import_to_pixels(buffer, &error);
    int w = wpe_buffer_get_width(buffer), h = wpe_buffer_get_height(buffer);
    const guint8 *src;
    gsize size;
    guint stride;
    int x, y;
    gboolean direct;
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

    if (!app->image || app->image_width != w || app->image_height != h)
        make_image(app, w, h);
    if (app->shot_path && (app->last_width != w || app->last_height != h)) {
        g_free(app->last_rgb);
        app->last_rgb = g_malloc((size_t)w * h * 3);
        app->last_width = w;
        app->last_height = h;
    }

    /* ARGB8888 as DRM defines it: a little-endian 32-bit word, so B, G, R, A
     * in memory whatever the host's byte order. Alpha is premultiplied, and
     * the page is opaque: drop it. A 32-bit image in the host's byte order is
     * written directly; anything else goes through XPutPixel. */
    direct = app->image->bits_per_pixel == 32
        && app->image->byte_order == (G_BYTE_ORDER == G_BIG_ENDIAN ? MSBFirst : LSBFirst);
    for (y = 0; y < h; y++) {
        const guint8 *p = src + (gsize)y * stride;
        guint32 *out = (guint32 *)(app->image->data + (gsize)y * app->image->bytes_per_line);
        guint8 *rgb = app->last_rgb ? app->last_rgb + (gsize)y * w * 3 : NULL;
        for (x = 0; x < w; x++, p += 4) {
            guint32 pixel = ((guint32)p[2] << app->rshift) | ((guint32)p[1] << app->gshift)
                | ((guint32)p[0] << app->bshift);
            if (direct)
                out[x] = pixel;
            else
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
    if (app->timing)
        g_printerr("heliotrope:          frame %dx%d copied to X in %.0f ms\n", w, h, (g_get_monotonic_time() - t0) / 1e3);
}

/* --- page state --------------------------------------------------------------- */

/* The page as shown, read back from the window (present mode, for -o). */
static void grab_page(App *app)
{
    int w = page_width(app), h = page_height(app), x, y;
    XImage *img = XGetImage(app->dpy, app->page_win, 0, 0, w, h, AllPlanes, ZPixmap);
    unsigned long rm = app->visual->red_mask, gm = app->visual->green_mask, bm = app->visual->blue_mask;
    guint8 *rgb;

    if (!img)
        return;
    g_free(app->last_rgb);
    app->last_rgb = rgb = g_malloc((size_t)w * h * 3);
    app->last_width = w;
    app->last_height = h;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            unsigned long p = XGetPixel(img, x, y);
            *rgb++ = (guint8)((p & rm) >> app->rshift);
            *rgb++ = (guint8)((p & gm) >> app->gshift);
            *rgb++ = (guint8)((p & bm) >> app->bshift);
        }
    XDestroyImage(img);
}

static gboolean take_shot(gpointer data)
{
    App *app = data;
    if (app->present)
        grab_page(app);
    if (app->last_rgb)
        write_ppm(app);
    app->shot_done = TRUE;
    if (app->quit_after_shot)
        g_main_loop_quit(app->loop);
    return G_SOURCE_REMOVE;
}

static void apply_site_choices(App *app);

static void load_changed(WebKitWebView *web_view, WebKitLoadEvent event, App *app)
{
    (void)web_view;
    if (event == WEBKIT_LOAD_STARTED) {
        app->load_start = g_get_monotonic_time();
        app->first_frame_seen = FALSE;
        app->frames = 0;
        timing_mark(app, "load started");
    } else if (event == WEBKIT_LOAD_COMMITTED) {
        app->first_frame_seen = FALSE; /* frames of the old page don't count */
        timing_mark(app, "committed (first bytes of the page)");
    } else if (event == WEBKIT_LOAD_FINISHED)
        timing_mark(app, "load finished");
    if (event == WEBKIT_LOAD_STARTED || event == WEBKIT_LOAD_REDIRECTED)
        apply_site_choices(app);
    if (event == WEBKIT_LOAD_FINISHED) {
        g_printerr("heliotrope: loaded %s\n", webkit_web_view_get_uri(app->web_view));
        if (app->shot_path && !app->shot_done)
            g_timeout_add_seconds(app->shot_delay, take_shot, app);
    }
    draw_bar(app);
    XFlush(app->dpy);
}

static gboolean load_failed(WebKitWebView *web_view, WebKitLoadEvent event, char *uri, GError *error, App *app)
{
    (void)web_view;
    (void)event;
    (void)app;
    g_printerr("heliotrope: failed to load %s: %s\n", uri, error->message);
    return FALSE; /* WebKit shows its error page */
}

static void title_changed(GObject *object, GParamSpec *pspec, App *app)
{
    const char *title = webkit_web_view_get_title(app->web_view);
    char *name = g_strdup_printf("Heliotrope%s%s", title && *title ? " - " : "", title ? title : "");
    /* WM_NAME is Latin-1 (STRING), which is what 4Dwm shows; titles are UTF-8. */
    char *latin1 = g_convert_with_fallback(name, -1, "ISO-8859-1", "UTF-8", "?", NULL, NULL, NULL);
    (void)object;
    (void)pspec;
    XStoreName(app->dpy, app->win, latin1 ? latin1 : name);
    g_free(latin1);
    g_free(name);
}

static void state_changed(GObject *object, GParamSpec *pspec, App *app)
{
    (void)object;
    (void)pspec;
    app->progress_value = webkit_web_view_get_estimated_load_progress(app->web_view);
    draw_bar(app);
    XFlush(app->dpy);
}

/* The Web process died (a crash, or killed for memory): reload once; if the page
 * kills it again within a minute, show why instead of looping. */
static void web_process_terminated(WebKitWebView *web_view, WebKitWebProcessTerminationReason reason, App *app)
{
    static gint64 last;
    gint64 now = g_get_monotonic_time();
    const char *why = reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "it used too much memory"
        : reason == WEBKIT_WEB_PROCESS_CRASHED ? "it crashed" : "it was terminated";
    (void)app;
    g_printerr("heliotrope: the Web process ended (%s)\n", why);
    if (last && now - last < 60 * G_USEC_PER_SEC) {
        char *html = g_strdup_printf("<body style='font-family:sans-serif;margin:3em'><h2>This page stopped</h2>"
            "<p>The page's process ended because %s, twice within a minute.</p></body>", why);
        webkit_web_view_load_alternate_html(web_view, html, webkit_web_view_get_uri(web_view), NULL);
        g_free(html);
    } else
        webkit_web_view_reload(web_view);
    last = now;
}

static void set_cursor(App *app, Cursor c)
{
    if (c != app->current_cursor) {
        XDefineCursor(app->dpy, app->win, c);
        app->current_cursor = c;
    }
}

static void mouse_target_changed(WebKitWebView *web_view, WebKitHitTestResult *hit, guint modifiers, App *app)
{
    const char *link = webkit_hit_test_result_context_is_link(hit) ? webkit_hit_test_result_get_link_uri(hit) : NULL;
    (void)web_view;
    (void)modifiers;
    if (g_strcmp0(link, app->status)) {
        g_free(app->status);
        app->status = g_strdup(link);
        draw_page(app);
    }
    set_cursor(app, link ? app->hand : webkit_hit_test_result_context_is_editable(hit) ? app->text : app->arrow);
    XFlush(app->dpy);
}

/* An address, or else a search: text with a space, or with no dot, colon or slash. */
static void load_address(App *app, const char *text)
{
    char *uri;
    while (*text == ' ')
        text++;
    if (!*text)
        return;
    if (strstr(text, "://") || g_str_has_prefix(text, "about:") || g_str_has_prefix(text, "data:"))
        uri = g_strdup(text);
    else if (strchr(text, ' ') || !strpbrk(text, ".:/")) {
        char *q = g_uri_escape_string(text, NULL, TRUE);
        uri = g_strconcat("https://lite.duckduckgo.com/lite/?q=", q, NULL);
        g_free(q);
    } else
        uri = g_strconcat("https://", text, NULL);
    webkit_web_view_load_uri(app->web_view, uri);
    g_free(uri);
}

/* --- the address field ------------------------------------------------------ */

static void start_editing(App *app)
{
    const char *u = webkit_web_view_get_uri(app->web_view);
    app->editing = TRUE;
    g_string_assign(app->edit, u ? u : "");
    app->cursor = app->edit->len;
    app->all_selected = TRUE;
    draw_bar(app);
}

static void stop_editing(App *app)
{
    app->editing = FALSE;
    app->all_selected = FALSE;
    draw_bar(app);
}

static void insert_text(App *app, const char *text, gsize len)
{
    gsize i;
    if (app->all_selected) {
        g_string_truncate(app->edit, 0);
        app->cursor = 0;
        app->all_selected = FALSE;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 32 || c == 127)
            continue; /* no newlines or controls from a paste */
        g_string_insert_c(app->edit, (gssize)app->cursor, (char)c);
        app->cursor++;
    }
}

static void paste(App *app, Atom selection)
{
    XConvertSelection(app->dpy, selection, XA_STRING, app->selection_property, app->win, CurrentTime);
}

static void selection_arrived(App *app, XSelectionEvent *e)
{
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    if (e->property == None || !app->editing)
        return;
    if (XGetWindowProperty(app->dpy, app->win, e->property, 0, 4096, True, AnyPropertyType,
            &type, &format, &n, &after, &data) == Success && data && format == 8) {
        insert_text(app, (const char *)data, n);
        draw_bar(app);
    }
    if (data)
        XFree(data);
}

/* the byte offset in the field text nearest to window x */
static gsize offset_at(App *app, int x)
{
    XRectangle r;
    gsize i;
    field_rect(app, &r);
    if (!app->font)
        return app->edit->len;
    for (i = 0; i < app->edit->len; i++) {
        int w = XTextWidth(app->font, app->edit->str, (int)i);
        int cw = XTextWidth(app->font, app->edit->str + i, 1);
        if (r.x + 6 + w + cw / 2 > x)
            return i;
    }
    return app->edit->len;
}

static gboolean handle_field_key(App *app, KeySym sym, const char *text, unsigned int state)
{
    gboolean control = (state & ControlMask) != 0;
    switch (sym) {
    case XK_Return:
    case XK_KP_Enter:
        stop_editing(app);
        load_address(app, app->edit->str);
        return TRUE;
    case XK_Escape:
        stop_editing(app);
        return TRUE;
    case XK_Left:
        if (app->all_selected)
            app->cursor = 0;
        else if (app->cursor)
            app->cursor--;
        app->all_selected = FALSE;
        break;
    case XK_Right:
        if (!app->all_selected && app->cursor < app->edit->len)
            app->cursor++;
        else if (app->all_selected)
            app->cursor = app->edit->len;
        app->all_selected = FALSE;
        break;
    case XK_Home:
        app->cursor = 0;
        app->all_selected = FALSE;
        break;
    case XK_End:
        app->cursor = app->edit->len;
        app->all_selected = FALSE;
        break;
    case XK_BackSpace:
        if (app->all_selected) {
            g_string_truncate(app->edit, 0);
            app->cursor = 0;
            app->all_selected = FALSE;
        } else if (app->cursor) {
            g_string_erase(app->edit, (gssize)app->cursor - 1, 1);
            app->cursor--;
        }
        break;
    case XK_Delete:
        if (app->all_selected) {
            g_string_truncate(app->edit, 0);
            app->cursor = 0;
            app->all_selected = FALSE;
        } else if (app->cursor < app->edit->len)
            g_string_erase(app->edit, (gssize)app->cursor, 1);
        break;
    default:
        if (control) {
            if (sym == XK_a || sym == XK_A)
                app->all_selected = TRUE;
            else if (sym == XK_u || sym == XK_U) {
                g_string_truncate(app->edit, 0);
                app->cursor = 0;
                app->all_selected = FALSE;
            } else if (sym == XK_k || sym == XK_K)
                g_string_truncate(app->edit, app->cursor);
            else if (sym == XK_v || sym == XK_V)
                paste(app, app->clipboard);
            else if (sym == XK_l || sym == XK_L)
                app->all_selected = TRUE;
        } else if (text[0])
            insert_text(app, text, strlen(text));
        break;
    }
    draw_bar(app);
    return TRUE;
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

/* --- per-site choices and the block list ------------------------------------ */

static void load_sites(App *app)
{
    char *text = NULL, **lines;
    int i;
    app->sites = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    app->sites_path = g_build_filename(g_get_user_config_dir(), "heliotrope", "sites", NULL);
    if (!g_file_get_contents(app->sites_path, &text, NULL, NULL))
        return;
    lines = g_strsplit(text, "\n", -1);
    for (i = 0; lines[i]; i++) {
        char **w = g_strsplit_set(g_strstrip(lines[i]), " \t", -1);
        unsigned flags = 0;
        int j;
        if (!w[0] || !*w[0] || w[0][0] == '#') {
            g_strfreev(w);
            continue;
        }
        for (j = 1; w[j]; j++) {
            if (!strcmp(w[j], "nojs"))
                flags |= SITE_NO_JS;
            else if (!strcmp(w[j], "noblock"))
                flags |= SITE_NO_BLOCKING;
        }
        if (flags)
            g_hash_table_insert(app->sites, g_strdup(w[0]), GUINT_TO_POINTER(flags));
        g_strfreev(w);
    }
    g_strfreev(lines);
    g_free(text);
}

static void save_sites(App *app)
{
    GString *out = g_string_new("# heliotrope per-site choices: host nojs|noblock\n");
    GHashTableIter it;
    gpointer key, value;
    char *dir = g_path_get_dirname(app->sites_path);
    g_hash_table_iter_init(&it, app->sites);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        unsigned flags = GPOINTER_TO_UINT(value);
        g_string_append_printf(out, "%s%s%s\n", (char *)key, flags & SITE_NO_JS ? " nojs" : "",
            flags & SITE_NO_BLOCKING ? " noblock" : "");
    }
    g_mkdir_with_parents(dir, 0755);
    g_file_set_contents(app->sites_path, out->str, (gssize)out->len, NULL);
    g_free(dir);
    g_string_free(out, TRUE);
}

/* Apply the current site's choices before its document and resources load. */
static void apply_site_choices(App *app)
{
    unsigned flags = current_site_flags(app);
    gboolean block = app->filter && !(flags & SITE_NO_BLOCKING);
    webkit_settings_set_enable_javascript(webkit_web_view_get_settings(app->web_view), !(flags & SITE_NO_JS));
    if (block && !app->filter_active)
        webkit_user_content_manager_add_filter(app->content_manager, app->filter);
    else if (!block && app->filter_active)
        webkit_user_content_manager_remove_filter(app->content_manager, app->filter);
    app->filter_active = block;
}

static void toggle_site(App *app, unsigned flag)
{
    const char *uri = webkit_web_view_get_uri(app->web_view);
    GUri *parsed = uri ? g_uri_parse(uri, G_URI_FLAGS_NONE, NULL) : NULL;
    const char *host = parsed ? g_uri_get_host(parsed) : NULL;
    if (host && *host) {
        unsigned flags = GPOINTER_TO_UINT(g_hash_table_lookup(app->sites, host)) ^ flag;
        if (flags)
            g_hash_table_insert(app->sites, g_strdup(host), GUINT_TO_POINTER(flags));
        else
            g_hash_table_remove(app->sites, host);
        save_sites(app);
        apply_site_choices(app);
        webkit_web_view_reload(app->web_view);
    }
    if (parsed)
        g_uri_unref(parsed);
}

static void load_pending_page(App *app)
{
    if (app->pending_url) {
        load_address(app, app->pending_url);
        g_clear_pointer(&app->pending_url, g_free);
    }
}

static void filter_ready(App *app, WebKitUserContentFilter *filter, gint64 started)
{
    app->filter = filter;
    g_printerr("heliotrope: block list ready (%.1f s)\n", (g_get_monotonic_time() - started) / 1e6);
    g_clear_pointer(&app->status, g_free);
    load_pending_page(app);
    draw_bar(app);
}

typedef struct { App *app; gint64 started; } FilterJob;

static void filter_saved(GObject *store, GAsyncResult *result, gpointer data)
{
    FilterJob *job = data;
    GError *error = NULL;
    WebKitUserContentFilter *filter = webkit_user_content_filter_store_save_finish(WEBKIT_USER_CONTENT_FILTER_STORE(store), result, &error);
    if (filter)
        filter_ready(job->app, filter, job->started);
    else {
        g_printerr("heliotrope: cannot compile the block list: %s\n", error->message);
        g_error_free(error);
        g_clear_pointer(&job->app->status, g_free);
        load_pending_page(job->app);
    }
    g_free(job);
}

static void filter_loaded(GObject *store, GAsyncResult *result, gpointer data)
{
    FilterJob *job = data;
    App *app = job->app;
    WebKitUserContentFilter *filter = webkit_user_content_filter_store_load_finish(WEBKIT_USER_CONTENT_FILTER_STORE(store), result, NULL);
    const char *path = getenv("HELIOTROPE_BLOCKLIST") ? getenv("HELIOTROPE_BLOCKLIST") : BLOCKLIST_PATH;
    char *json = NULL;
    gsize length;
    if (filter) {
        filter_ready(app, filter, job->started);
        g_free(job);
        return;
    }
    /* Not compiled yet (or the list changed): compile it now, once. */
    if (!g_file_get_contents(path, &json, &length, NULL)) {
        g_clear_pointer(&app->status, g_free);
        load_pending_page(app);
        g_free(job);
        return;
    }
    g_free(app->status);
    app->status = g_strdup("Preparing the block list (first run only)...");
    draw_page(app);
    XFlush(app->dpy);
    webkit_user_content_filter_store_save(app->filter_store, app->filter_id, g_bytes_new_take(json, length), NULL, filter_saved, job);
}

/* The compiled list is stored under an identifier made from the rule file's
 * size and time, so a new list is compiled once and the old one replaced. */
static void start_block_list(App *app)
{
    const char *path = getenv("HELIOTROPE_BLOCKLIST") ? getenv("HELIOTROPE_BLOCKLIST") : BLOCKLIST_PATH;
    GStatBuf st;
    FilterJob *job;
    char *dir;
    if (!*path || g_stat(path, &st) != 0) {
        load_pending_page(app);
        return;
    }
    dir = g_build_filename(g_get_user_cache_dir(), "heliotrope", "content-filters", NULL);
    app->filter_store = webkit_user_content_filter_store_new(dir);
    g_free(dir);
    app->filter_id = g_strdup_printf("blocklist-%ld-%ld", (long)st.st_size, (long)st.st_mtime);
    job = g_new0(FilterJob, 1);
    job->app = app;
    job->started = g_get_monotonic_time();
    webkit_user_content_filter_store_load(app->filter_store, app->filter_id, NULL, filter_loaded, job);
}

static void press_button(App *app, int i)
{
    switch (i) {
    case BUTTON_BACK:
        webkit_web_view_go_back(app->web_view);
        break;
    case BUTTON_FORWARD:
        webkit_web_view_go_forward(app->web_view);
        break;
    case BUTTON_RELOAD:
        if (webkit_web_view_is_loading(app->web_view))
            webkit_web_view_stop_loading(app->web_view);
        else
            webkit_web_view_reload(app->web_view);
        break;
    case BUTTON_HOME:
        load_address(app, app->home);
        break;
    case BUTTON_JS:
        toggle_site(app, SITE_NO_JS);
        break;
    case BUTTON_BLOCK:
        toggle_site(app, SITE_NO_BLOCKING);
        break;
    }
}

/* keys the browser handles itself, whatever has the focus */
static gboolean handle_browser_key(App *app, KeySym sym, unsigned int state)
{
    if ((state & ControlMask) && (sym == XK_l || sym == XK_L) && !app->editing) {
        start_editing(app);
        return TRUE;
    }
    if ((state & ControlMask) && (sym == XK_q || sym == XK_Q)) {
        g_main_loop_quit(app->loop);
        return TRUE;
    }
    if ((state & ControlMask) && (sym == XK_r || sym == XK_R)) {
        webkit_web_view_reload(app->web_view);
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
    if ((state & Mod1Mask) && sym == XK_Home) {
        load_address(app, app->home);
        return TRUE;
    }
    if (sym == XK_F5) {
        webkit_web_view_reload(app->web_view);
        return TRUE;
    }
    return FALSE;
}

static void handle_toolbar_button(App *app, XButtonEvent *b)
{
    XRectangle r;
    int i;
    if (b->button == Button1) {
        for (i = 0; i < N_BUTTONS; i++) {
            button_rect(i, &r);
            if (!in_rect(&r, b->x, b->y))
                continue;
            if (b->type == ButtonPress && button_enabled(app, i)) {
                app->pressed_button = i;
                draw_bar(app);
            } else if (b->type == ButtonRelease && app->pressed_button == i) {
                app->pressed_button = -1;
                press_button(app, i);
                draw_bar(app);
            }
            return;
        }
    }
    if (b->type == ButtonRelease && app->pressed_button >= 0) {
        app->pressed_button = -1;
        draw_bar(app);
    }
    field_rect(app, &r);
    if (b->type == ButtonPress && in_rect(&r, b->x, b->y)) {
        if (b->button == Button2) {
            if (!app->editing)
                start_editing(app);
            app->all_selected = FALSE;
            app->cursor = offset_at(app, b->x);
            paste(app, XA_PRIMARY);
        } else if (b->button == Button1) {
            if (!app->editing)
                start_editing(app);
            else {
                app->cursor = offset_at(app, b->x);
                app->all_selected = FALSE;
            }
        }
        draw_bar(app);
    }
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
            if (app->present)
                XResizeWindow(app->dpy, app->page_win, page_width(app), page_height(app));
            wpe_toplevel_resize(wpe_view_get_toplevel(app->view), page_width(app), page_height(app));
            redraw(app);
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
        if (b->y < BAR_HEIGHT) {
            handle_toolbar_button(app, b);
            break;
        }
        if (app->editing && ev->type == ButtonPress)
            stop_editing(app);
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
        XEvent latest = *ev;
        XMotionEvent *m = &latest.xmotion;
        /* only the latest position matters */
        while (XCheckTypedWindowEvent(app->dpy, app->win, MotionNotify, &latest))
            ;
        if (m->y < BAR_HEIGHT) {
            XRectangle r;
            field_rect(app, &r);
            set_cursor(app, in_rect(&r, m->x, m->y) ? app->text : app->arrow);
            break;
        }
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
        if (ev->type == KeyPress && handle_browser_key(app, sym, k->state))
            break;
        if (app->editing) {
            if (ev->type == KeyPress)
                handle_field_key(app, sym, text, k->state);
            break;
        }
        /* X keysyms are what WPE (XKB) keyvals are. */
        send(app, wpe_event_keyboard_new(ev->type == KeyPress ? WPE_EVENT_KEYBOARD_KEY_DOWN : WPE_EVENT_KEYBOARD_KEY_UP,
            app->view, WPE_INPUT_SOURCE_KEYBOARD, (guint32)k->time, modifiers_of(k->state),
            k->keycode, (guint)sym));
        break;
    }
    case SelectionNotify:
        selection_arrived(app, &ev->xselection);
        break;
    case ClientMessage:
        if ((Atom)ev->xclient.data.l[0] == app->wm_delete)
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

/* IRIX starts processes with 200 descriptors. A page like GitHub loads ~140
 * resources at once, and WebKit passes each chunk of their data between its
 * processes as a shared-memory file descriptor: at 200 the Web process ran
 * out and the page never finished loading. Raise the soft limit to the hard
 * one; the Web and network processes inherit it. */
static void raise_descriptor_limit(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
}

static unsigned long color(App *app, int r, int g, int b)
{
    return ((unsigned long)r << app->rshift) | ((unsigned long)g << app->gshift) | ((unsigned long)b << app->bshift);
}

static gboolean open_window(App *app)
{
    XVisualInfo vinfo;
    XSetWindowAttributes attrs;
    int screen, major, minor;
    Bool pixmaps;

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
    app->use_shm = XShmQueryVersion(app->dpy, &major, &minor, &pixmaps) && !getenv("HELIOTROPE_NO_SHM");

    attrs.colormap = XCreateColormap(app->dpy, RootWindow(app->dpy, screen), app->visual, AllocNone);
    attrs.background_pixel = 0;
    attrs.border_pixel = 0;
    attrs.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask | KeyReleaseMask
        | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | FocusChangeMask;
    app->win = XCreateWindow(app->dpy, RootWindow(app->dpy, screen), app->x, app->y,
        app->width, app->height, 0, app->depth, InputOutput, app->visual,
        CWColormap | CWBackPixel | CWBorderPixel | CWEventMask, &attrs);
    XStoreName(app->dpy, app->win, "Heliotrope");
    if (app->positioned) {
        /* -g WxH+X+Y: a position the user chose, which the window manager keeps */
        XSizeHints hints;
        memset(&hints, 0, sizeof hints);
        hints.flags = USPosition | USSize;
        hints.x = app->x;
        hints.y = app->y;
        hints.width = app->width;
        hints.height = app->height;
        XSetWMNormalHints(app->dpy, app->win, &hints);
    }
    app->wm_delete = XInternAtom(app->dpy, "WM_DELETE_WINDOW", False);
    app->clipboard = XInternAtom(app->dpy, "CLIPBOARD", False);
    app->selection_property = XInternAtom(app->dpy, "HELIOTROPE_SELECTION", False);
    XSetWMProtocols(app->dpy, app->win, &app->wm_delete, 1);

    app->gc = XCreateGC(app->dpy, app->win, 0, NULL);
    app->font = XLoadQueryFont(app->dpy, "-*-helvetica-medium-r-normal--12-*-*-*-*-*-iso8859-1");
    if (!app->font)
        app->font = XLoadQueryFont(app->dpy, "fixed");
    if (app->font)
        XSetFont(app->dpy, app->gc, app->font->fid);
    app->arrow = XCreateFontCursor(app->dpy, XC_left_ptr);
    app->hand = XCreateFontCursor(app->dpy, XC_hand2);
    app->text = XCreateFontCursor(app->dpy, XC_xterm);
    app->current_cursor = None;

    app->bar_bg = color(app, 0xdd, 0xd6, 0xe8);
    app->bar_light = color(app, 0xf6, 0xf2, 0xfb);
    app->bar_shadow = color(app, 0x8f, 0x86, 0x9c);
    app->ink = color(app, 0x20, 0x10, 0x30);
    app->ink_disabled = color(app, 0xa8, 0xa0, 0xb4);
    app->field_bg = color(app, 0xf3, 0xf0, 0xf7);
    app->field_edit_bg = color(app, 0xff, 0xff, 0xff);
    app->selection = color(app, 0xc9, 0xb3, 0xe6);
    app->progress = color(app, 0x8a, 0x5c, 0xc0);
    app->status_bg = color(app, 0xee, 0xe8, 0xf4);
    set_cursor(app, app->arrow);
    if (app->present) {
        /* No event mask: input still reaches app->win. No background: only
         * WebKit draws here. */
        XSetWindowAttributes pa;
        pa.colormap = attrs.colormap;
        pa.border_pixel = 0;
        pa.background_pixmap = None;
        app->page_win = XCreateWindow(app->dpy, app->win, 0, BAR_HEIGHT, page_width(app), page_height(app), 0,
            app->depth, InputOutput, app->visual, CWColormap | CWBorderPixel | CWBackPixmap, &pa);
        XMapWindow(app->dpy, app->page_win);
        app->status_win = XCreateWindow(app->dpy, app->win, 0, 0, 1, 1, 0, app->depth, InputOutput, app->visual,
            CWColormap | CWBorderPixel, &pa);
    }
    XMapWindow(app->dpy, app->win);
    XFlush(app->dpy);
    return TRUE;
}

/* Can the web process put frames straight into a window? It can when its
 * EGL (the one libepoxy will open) is irix-egl-gles, which offers
 * eglPresentToWindowIRIX. HELIOTROPE_NO_PRESENT=1 keeps the shared-memory
 * copy. */
static gboolean present_available(void)
{
    const char *path = getenv("EPOXY_EGL_LIB");
    void *lib, *(*get_proc)(const char *);
    gboolean yes = FALSE;

    if (env_on("HELIOTROPE_NO_PRESENT"))
        return FALSE;
    lib = dlopen(path && *path ? path : "/opt/pkgsrc/lib/libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!lib)
        return FALSE;
    get_proc = (void *(*)(const char *))dlsym(lib, "eglGetProcAddress");
    yes = get_proc && get_proc("eglPresentToWindowIRIX");
    dlclose(lib);
    return yes;
}

/* --- compiled JavaScript kept on disk ---------------------------------------- */

typedef struct {
    char *path;
    goffset size;
    time_t used;
} CacheFile;

static int by_use(const void *a, const void *b)
{
    const CacheFile *x = a, *y = b;
    return x->used < y->used ? -1 : x->used > y->used;
}

/* WebKit (patch 0011) keeps scripts' bytecode in WPE_BYTECODE_CACHE_DIR, one
 * file per script version; trim it to HELIOTROPE_BYTECODE_CACHE_MB (default
 * 64), the files used longest ago first. HELIOTROPE_NO_BYTECODE_CACHE=1: none. */
static void setup_bytecode_cache(void)
{
    const char *mb = getenv("HELIOTROPE_BYTECODE_CACHE_MB");
    goffset limit = (goffset)(mb && atoi(mb) > 0 ? atoi(mb) : 64) << 20, total = 0;
    char *dir;
    GDir *d;
    const char *name;
    GArray *files;
    guint i;

    if (env_on("HELIOTROPE_NO_BYTECODE_CACHE"))
        return;
    dir = g_build_filename(g_get_user_cache_dir(), "heliotrope", "bytecode", NULL);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_free(dir);
        return;
    }
    g_setenv("WPE_BYTECODE_CACHE_DIR", dir, TRUE);
    files = g_array_new(FALSE, FALSE, sizeof(CacheFile));
    if ((d = g_dir_open(dir, 0, NULL))) {
        while ((name = g_dir_read_name(d))) {
            CacheFile f;
            struct stat st;
            f.path = g_build_filename(dir, name, NULL);
            if (stat(f.path, &st) != 0 || !S_ISREG(st.st_mode)) {
                g_free(f.path);
                continue;
            }
            f.size = st.st_size;
            f.used = st.st_atime > st.st_mtime ? st.st_atime : st.st_mtime;
            total += f.size;
            g_array_append_val(files, f);
        }
        g_dir_close(d);
    }
    g_array_sort(files, by_use);
    for (i = 0; i < files->len; i++) {
        CacheFile *f = &g_array_index(files, CacheFile, i);
        if (total > limit && g_unlink(f->path) == 0)
            total -= f->size;
        g_free(f->path);
    }
    g_array_free(files, TRUE);
    g_free(dir);
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
    app.pressed_button = -1;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-g") && i + 1 < argc)
            app.positioned = sscanf(argv[++i], "%dx%d+%d+%d", &app.width, &app.height, &app.x, &app.y) == 4;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc)
            app.shot_path = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc)
            app.shot_delay = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-q"))
            app.quit_after_shot = TRUE;
        else if (argv[i][0] == '-') {
            g_printerr("usage: heliotrope [-g WIDTHxHEIGHT[+X+Y]] [-o shot.ppm] [-s seconds] [-q] [URL]\n");
            return 2;
        } else
            url = argv[i];
    }
    app.home = g_strdup(getenv("HELIOTROPE_HOME") ? getenv("HELIOTROPE_HOME") : url);
    app.timing = env_on("HELIOTROPE_TIMING");
    raise_descriptor_limit();
    /* WebKit's caches (HTTP, HSTS) then live under ~/.cache/heliotrope */
    g_set_prgname("heliotrope");
    setup_bytecode_cache();

    app.edit = g_string_new(NULL);
    app.present = present_available();
    if (!open_window(&app))
        return 1;
    if (app.present) {
        char id[32];
        /* the web processes inherit it (WebKit patch 0010) */
        g_snprintf(id, sizeof id, "0x%lx", (unsigned long)app.page_win);
        g_setenv("WPE_PRESENT_X11_WINDOW", id, TRUE);
        if (app.timing)
            g_printerr("heliotrope: frames presented into window %s\n", id);
    }

    display = wpe_display_headless_new();
    {
        /* Every repaint costs a full software-composited frame here (seconds on
         * an R10000): no caret blinking (a static page then stops repainting),
         * and ask pages for reduced motion. HELIOTROPE_ANIMATIONS=1 undoes both. */
        WPESettings *wpe_settings = wpe_display_get_settings(display);
        if (!env_on("HELIOTROPE_ANIMATIONS")) {
            wpe_settings_set_uint32(wpe_settings, WPE_SETTING_CURSOR_BLINK_TIME, 0, WPE_SETTINGS_SOURCE_APPLICATION, NULL);
            wpe_settings_set_boolean(wpe_settings, WPE_SETTING_DISABLE_ANIMATIONS, TRUE, WPE_SETTINGS_SOURCE_APPLICATION, NULL);
        }
        /* WebKit's own scrollbar, always shown (the overlay kind appears only
         * while scrolling); HELIOTROPE_OVERLAY_SCROLLBARS=1 for that. */
        if (!env_on("HELIOTROPE_OVERLAY_SCROLLBARS"))
            wpe_settings_set_boolean(wpe_settings, WPE_SETTING_OVERLAY_SCROLLBARS, FALSE, WPE_SETTINGS_SOURCE_APPLICATION, NULL);
    }
    {
        const char *limit = getenv("HELIOTROPE_MEMORY_LIMIT");
        guint mb = limit && atoi(limit) > 0 ? (guint)atoi(limit) : 256;
        WebKitMemoryPressureSettings *pressure = webkit_memory_pressure_settings_new();
        WebKitWebContext *context;
        app.content_manager = webkit_user_content_manager_new();
        webkit_memory_pressure_settings_set_memory_limit(pressure, mb);
        webkit_memory_pressure_settings_set_conservative_threshold(pressure, 0.33);
        webkit_memory_pressure_settings_set_strict_threshold(pressure, 0.5);
        webkit_network_session_set_memory_pressure_settings(pressure);
        context = WEBKIT_WEB_CONTEXT(g_object_new(WEBKIT_TYPE_WEB_CONTEXT,
            "memory-pressure-settings", pressure,
            "process-swap-on-cross-site-navigation-enabled", env_on("HELIOTROPE_SITE_ISOLATION"),
            NULL));
        webkit_memory_pressure_settings_free(pressure);
        webkit_web_context_set_cache_model(context, WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER);
        app.web_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display,
            "web-context", context, "user-content-manager", app.content_manager, NULL));
        g_object_unref(context);
    }
    g_object_unref(display);
    /* WebKit features to switch on: redraw only what changed (damage tracking,
     * off by default in WPE; a full frame costs seconds in software GL, a
     * keystroke's worth a fraction of one; HELIOTROPE_NO_DAMAGE=1 turns it off),
     * plus any named in HELIOTROPE_FEATURES=Name,Name for experiments. */
    {
        WebKitSettings *settings = webkit_web_view_get_settings(app.web_view);
        WebKitFeatureList *all = webkit_settings_get_all_features();
        char *list = g_strjoin(",", env_on("HELIOTROPE_NO_DAMAGE") ? "" : "PropagateDamagingInformation,UseDamagingInformationForCompositing",
            getenv("HELIOTROPE_FEATURES") ? getenv("HELIOTROPE_FEATURES") : "", NULL);
        char **names = g_strsplit(list, ",", -1);
        gsize n = webkit_feature_list_get_length(all), k;
        int j;
        for (j = 0; names[j]; j++) {
            gboolean found = FALSE;
            if (!*names[j])
                continue;
            for (k = 0; k < n; k++) {
                WebKitFeature *feature = webkit_feature_list_get(all, k);
                if (!g_strcmp0(webkit_feature_get_identifier(feature), names[j])) {
                    webkit_settings_set_feature_enabled(settings, feature, TRUE);
                    found = TRUE;
                }
            }
            if (!found || app.timing)
                g_printerr("heliotrope: feature %s %s\n", names[j], found ? "on" : "not found");
        }
        g_strfreev(names);
        g_free(list);
        webkit_feature_list_unref(all);
    }
    /* HELIOTROPE_CONSOLE=1: the page's console messages (including what the
     * content blocker stopped) go to stdout, for testing. */
    if (env_on("HELIOTROPE_CONSOLE"))
        webkit_settings_set_enable_write_console_messages_to_stdout(webkit_web_view_get_settings(app.web_view), TRUE);
    app.view = webkit_web_view_get_wpe_view(app.web_view);
    wpe_toplevel_resize(wpe_view_get_toplevel(app.view), page_width(&app), page_height(&app));
    g_signal_connect(app.view, "buffer-rendered", G_CALLBACK(buffer_rendered), &app);
    g_signal_connect(app.web_view, "load-changed", G_CALLBACK(load_changed), &app);
    g_signal_connect(app.web_view, "load-failed", G_CALLBACK(load_failed), &app);
    g_signal_connect(app.web_view, "notify::title", G_CALLBACK(title_changed), &app);
    g_signal_connect(app.web_view, "notify::estimated-load-progress", G_CALLBACK(state_changed), &app);
    g_signal_connect(app.web_view, "notify::is-loading", G_CALLBACK(state_changed), &app);
    g_signal_connect(app.web_view, "notify::uri", G_CALLBACK(state_changed), &app);
    g_signal_connect(app.web_view, "mouse-target-changed", G_CALLBACK(mouse_target_changed), &app);
    g_signal_connect(app.web_view, "web-process-terminated", G_CALLBACK(web_process_terminated), &app);
    wpe_view_focus_in(app.view);

    g_unix_fd_add(ConnectionNumber(app.dpy), G_IO_IN, x_ready, &app);
    load_sites(&app);
    app.pending_url = g_strdup(url);
    start_block_list(&app);

    app.loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(app.loop);
    return 0;
}
