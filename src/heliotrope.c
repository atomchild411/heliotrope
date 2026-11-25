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
 * The menu bar has File (New Tab, New Window, Open Location, Save Page As,
 * Close Tab, Exit), Edit (Copy, Paste, Select All, Find), View (Reload,
 * zoom, JavaScript and blocking for the site), Go and Help. The toolbar has
 * Back, Forward, Reload (Stop while loading) and Home, and the address
 * field: click it (or Ctrl+L) and type; Enter goes, Escape cancels. Text
 * that is not an address searches DuckDuckGo Lite. The field also asks for
 * a file name when something is saved ("Save as:", in ~/Downloads) and for
 * text to find ("Find:"). The scrollbar is WebKit's own
 * (HELIOTROPE_OVERLAY_SCROLLBARS=1 for the kind that appears only while
 * scrolling).
 *
 * Tabs: a strip under the toolbar once there are two; they share one Web
 * process and only the one shown renders. The right button gives a link's
 * menu (open here, in a new tab or a new window, save, copy its address),
 * an image's, or the page's; the middle button opens a link in a new tab; a
 * page's new windows open as tabs. New Window starts another heliotrope.
 * Files the browser cannot show are saved. Copy and paste go through the X
 * clipboard (CLIPBOARD and PRIMARY).
 *
 * Keys: Ctrl+T/W new and close tab, Ctrl+PgUp/PgDn (Ctrl+Tab) between tabs,
 * Ctrl+N new window, Ctrl+S save the page, Ctrl+F/G find, Ctrl++/-/0 zoom,
 * Alt+Left/Right back and forward, F5 reload, Escape stop, Ctrl+Q quit.
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
#include <unicode/utrans.h>
#include <unicode/ustring.h>
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
#define BLOCKLIST_PATH "/usr/pkg/share/heliotrope/blocklist.json"
#endif

#define MENU_HEIGHT 22          /* the menu bar */
#define BAR_HEIGHT 30           /* toolbar */
#define TAB_HEIGHT 24           /* the tab strip, shown when there are two tabs or more */
#define BUTTON_WIDTH 28
#define BUTTON_HEIGHT 24

enum { BUTTON_BACK, BUTTON_FORWARD, BUTTON_RELOAD, BUTTON_HOME, BUTTON_JS, BUTTON_BLOCK, N_BUTTONS };

/* per-site choices, remembered in ~/.config/heliotrope/sites */
enum { SITE_NO_JS = 1, SITE_NO_BLOCKING = 2 };

typedef struct App App;

/* A tab: a page of its own in the window. Tabs are related views, so they
 * share one Web process (memory is short on an SGI), and only the one shown
 * is mapped, so the others do not render. */
typedef struct {
    App *app;
    WebKitWebView *web_view;
    WPEView *view;
    char *title;                /* Latin-1, for the tab strip */
} Tab;

/* A menu's line: label NULL is a separator. */
typedef struct {
    const char *label;
    const char *accel;
    int action;
    gboolean enabled;
} MenuItem;
#define MAX_MENU_ITEMS 24

/* what the field is asking for */
enum { PROMPT_ADDRESS, PROMPT_SAVE, PROMPT_FIND };

struct App {
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

    /* -T FILE: a tour, one address a line, each shown -s seconds once
     * loaded (or after a minute and a half if it never finishes), in one
     * session, then quit; each step on stderr with the wall-clock time. */
    char **tour;
    int tour_n, tour_i;
    guint tour_timer;
    gboolean tour_loaded;       /* this step's page has finished */
    guint8 *last_rgb;           /* the last frame as RGB, for -o */
    int last_width, last_height;

    /* tabs: app->tab is the one shown, and web_view and view above are its */
    GPtrArray *tabs;
    Tab *tab;
    WPEDisplay *display;
    const char *argv0;          /* to start another window */

    /* the one menu open: a menu of the bar (popup_menu >= 0) or the page's
     * context menu (-1), in an override-redirect window */
    Window popup;
    int popup_menu;
    int popup_hover;
    int n_popup_items;
    MenuItem popup_items[MAX_MENU_ITEMS];
    int popup_width, popup_height, popup_line;
    Time popup_time;            /* when it opened: the release of the press that opened it is not a choice */
    gboolean popup_armed;
    int pointer_x, pointer_y;   /* the pointer on the screen at the last button press in the page */
    char *ctx_link, *ctx_image; /* what the context menu was opened on */
    gboolean ctx_selection, ctx_editable;

    /* the field's prompts: an address, a file to save to, text to find */
    int prompt;
    WebKitDownload *prompt_download;
    GPtrArray *waiting_downloads; /* downloads waiting for the prompt */
    char *find_text;

    /* a message in the status line when no link is under the pointer */
    char *message;
    guint message_timer;

    /* the X clipboard: what we hold (CLIPBOARD and PRIMARY), and a paste
     * into the page waiting for another client's */
    char *clip_text;
    Atom targets, paste_property, utf8;
    gint64 wpe_clip_count;
    gboolean paste_pending;
    guint repaint_timer;
};

static void draw_tabs(App *app);
static void draw_menubar(App *app);
static void start_prompt(App *app, int prompt, const char *text);
static void destination_chosen(App *app, const char *path);
static void find_text(App *app, const char *text);
static void show_message(App *app, const char *text, int seconds);

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

static int page_top(App *app)
{
    return MENU_HEIGHT + BAR_HEIGHT + (app->tabs && app->tabs->len > 1 ? TAB_HEIGHT : 0);
}

static int page_width(App *app)
{
    return app->width > 0 ? app->width : 1;
}

static int page_height(App *app)
{
    return app->height > page_top(app) ? app->height - page_top(app) : 1;
}

static void button_rect(int i, XRectangle *r)
{
    r->x = 4 + i * (BUTTON_WIDTH + 2);
    r->y = MENU_HEIGHT + (BAR_HEIGHT - BUTTON_HEIGHT) / 2;
    r->width = BUTTON_WIDTH;
    r->height = BUTTON_HEIGHT;
}

static void field_rect(App *app, XRectangle *r)
{
    int x = 4 + N_BUTTONS * (BUTTON_WIDTH + 2) + 6;
    r->x = x;
    r->y = MENU_HEIGHT + (BAR_HEIGHT - BUTTON_HEIGHT) / 2;
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

/* the SITE_* flags of an address's site */
static unsigned site_flags_of(App *app, const char *uri)
{
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

/* the SITE_* flags of the page being shown */
static unsigned current_site_flags(App *app)
{
    return site_flags_of(app, webkit_web_view_get_uri(app->web_view));
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
    if (app->editing && app->prompt != PROMPT_ADDRESS && app->font) {
        const char *label = app->prompt == PROMPT_SAVE ? "Save as:" : "Find:";
        int lw = XTextWidth(app->font, label, (int)strlen(label)) + 8;
        XSetForeground(app->dpy, app->gc, app->progress);
        XDrawString(app->dpy, app->win, app->gc, tx, ty, label, (int)strlen(label));
        tx += lw;
        avail -= lw;
        if (avail < 20)
            avail = 20;
    }

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
    XFillRectangle(app->dpy, app->win, app->gc, 0, MENU_HEIGHT, app->width, BAR_HEIGHT - 1);
    XSetForeground(app->dpy, app->gc, app->bar_shadow);
    XDrawLine(app->dpy, app->win, app->gc, 0, MENU_HEIGHT + BAR_HEIGHT - 1, app->width, MENU_HEIGHT + BAR_HEIGHT - 1);
    for (i = 0; i < N_BUTTONS; i++) {
        XRectangle r;
        gboolean enabled = button_enabled(app, i);
        button_rect(i, &r);
        if (enabled)
            bevel(app, &r, app->pressed_button == i);
        draw_icon(app, i, &r, enabled);
    }
    draw_field(app);
    /* often drawn from WebKit's callbacks, outside the X event loop's flush */
    XFlush(app->dpy);
}

static void draw_status(App *app)
{
    int w, y;
    /* the link under the pointer, else a message (a download's progress) */
    const char *text = app->status && *app->status ? app->status : app->message;
    if (!text || !*text || !app->font) {
        if (app->present)
            XUnmapWindow(app->dpy, app->status_win);
        return;
    }
    if (app->present) {
        int h = app->font->ascent + app->font->descent + 6;
        w = XTextWidth(app->font, text, (int)strlen(text)) + 10;
        if (w > page_width(app))
            w = page_width(app);
        XMoveResizeWindow(app->dpy, app->status_win, 0, app->height - h, w, h);
        XMapRaised(app->dpy, app->status_win);
        XSetForeground(app->dpy, app->gc, app->status_bg);
        XFillRectangle(app->dpy, app->status_win, app->gc, 0, 0, w, h);
        XSetForeground(app->dpy, app->gc, app->ink);
        XDrawString(app->dpy, app->status_win, app->gc, 5, h - app->font->descent - 3,
            text, (int)strlen(text));
        return;
    }
    w = XTextWidth(app->font, text, (int)strlen(text)) + 10;
    if (w > page_width(app))
        w = page_width(app);
    y = app->height - (app->font->ascent + app->font->descent + 6);
    XSetForeground(app->dpy, app->gc, app->status_bg);
    XFillRectangle(app->dpy, app->win, app->gc, 0, y, w, app->height - y);
    XSetForeground(app->dpy, app->gc, app->ink);
    XDrawString(app->dpy, app->win, app->gc, 5, app->height - app->font->descent - 3,
        text, (int)strlen(text));
}

static void draw_page(App *app)
{
    if (app->present || !app->image) {
        draw_status(app);
        return;
    }
    if (app->use_shm)
        XShmPutImage(app->dpy, app->win, app->gc, app->image, 0, 0, 0, page_top(app),
            app->image_width, app->image_height, False);
    else
        XPutImage(app->dpy, app->win, app->gc, app->image, 0, 0, 0, page_top(app),
            app->image_width, app->image_height);
    draw_status(app);
}

static void redraw(App *app)
{
    draw_menubar(app);
    draw_bar(app);
    draw_tabs(app);
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

static void buffer_rendered(WPEView *view, WPEBuffer *buffer, Tab *tab)
{
    App *app = tab->app;
    gint64 t0 = g_get_monotonic_time();
    if (tab != app->tab)
        return;
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

static void apply_site_choices(App *app, WebKitWebView *web_view);
static void load_address(App *app, const char *text);

static double wall_now(void) { return g_get_real_time() / 1e6; }

/* The tour's next page, or the end of it. */
static gboolean tour_next(gpointer data)
{
    App *app = data;
    app->tour_timer = 0;
    g_printerr("heliotrope: tour %d/%d leave %.3f %s\n", app->tour_i + 1, app->tour_n, wall_now(),
        webkit_web_view_get_uri(app->web_view));
    if (++app->tour_i >= app->tour_n) {
        g_printerr("heliotrope: tour end %.3f\n", wall_now());
        g_main_loop_quit(app->loop);
        return G_SOURCE_REMOVE;
    }
    g_printerr("heliotrope: tour %d/%d go %.3f %s\n", app->tour_i + 1, app->tour_n, wall_now(), app->tour[app->tour_i]);
    app->tour_loaded = FALSE;
    app->tour_timer = g_timeout_add_seconds(90, tour_next, app);
    load_address(app, app->tour[app->tour_i]);
    return G_SOURCE_REMOVE;
}

static void load_changed(WebKitWebView *web_view, WebKitLoadEvent event, Tab *tab)
{
    App *app = tab->app;
    if (event == WEBKIT_LOAD_STARTED || event == WEBKIT_LOAD_REDIRECTED)
        apply_site_choices(app, web_view);
    if (tab != app->tab) {
        draw_tabs(app);
        XFlush(app->dpy);
        return;
    }
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
    if (event == WEBKIT_LOAD_FINISHED) {
        g_printerr("heliotrope: loaded %s\n", webkit_web_view_get_uri(app->web_view));
        if (app->shot_path && !app->shot_done)
            g_timeout_add_seconds(app->shot_delay, take_shot, app);
        /* On a tour: the page stays -s seconds from its first finish. */
        if (app->tour && !app->tour_loaded) {
            app->tour_loaded = TRUE;
            if (app->tour_timer)
                g_source_remove(app->tour_timer);
            g_printerr("heliotrope: tour %d/%d loaded %.3f in %.1f s\n", app->tour_i + 1, app->tour_n, wall_now(),
                (g_get_monotonic_time() - app->load_start) / 1e6);
            app->tour_timer = g_timeout_add_seconds(app->shot_delay, tour_next, app);
        }
    }
    draw_bar(app);
    draw_tabs(app);
    XFlush(app->dpy);
}

static gboolean load_failed(WebKitWebView *web_view, WebKitLoadEvent event, char *uri, GError *error, Tab *tab)
{
    (void)web_view;
    (void)event;
    (void)tab;
    g_printerr("heliotrope: failed to load %s: %s\n", uri, error->message);
    return FALSE; /* WebKit shows its error page */
}

/* A UTF-8 title as Latin-1, which is what 4Dwm can show in WM_NAME (in its C
 * locale a COMPOUND_TEXT title comes out garbled and cut short): other
 * scripts transliterated (Cyrillic as BGN's: Википедия -> Vikipediya; others
 * ICU's Any-Latin: Ελληνικά -> Ellēniká, かな -> kana), letters Latin-1 lacks
 * folded (ē -> e, Ł -> L), and
 * Han left as "?" (its Latin is Mandarin's, wrong on a Japanese page). */
static char *latin1_title(const char *utf8)
{
    static UTransliterator *tr;
    static gboolean tried;
    glong n = 0;
    gunichar2 *u16;
    char *text = NULL, *latin1;
    const char *c;

    /* Most titles are Latin-1 already: no ICU then. Making the
     * transliterator costs a few percent of a page load's CPU time. */
    for (c = utf8; *c; c = g_utf8_next_char(c))
        if (g_utf8_get_char(c) > 0xff)
            break;
    if (!*c)
        return g_convert(utf8, -1, "ISO-8859-1", "UTF-8", NULL, NULL, NULL);

    if (!tried) {
        /* Punctuation first (an em dash), then Cyrillic as BGN's, the other
         * scripts but Han, and what is left outside Latin-1. As one rule set:
         * with BGN's step leading, the steps after it took no effect. */
        static const char rules[] =
            "::[^\\u0000-\\u00ff] Latin-ASCII; ::[:Cyrillic:] Russian-Latin/BGN; "
            "::[:^Han:] Any-Latin; ::[^\\u0000-\\u00ff] Latin-ASCII;";
        UErrorCode err = U_ZERO_ERROR;
        UParseError where;
        UChar id[32], text_rules[sizeof rules];
        u_uastrcpy(id, "Heliotrope-Title");
        u_uastrcpy(text_rules, rules);
        tr = utrans_openU(id, -1, UTRANS_FORWARD, text_rules, -1, &where, &err);
        if (U_FAILURE(err))
            tr = NULL;
        tried = TRUE;
    }
    if (tr && (u16 = g_utf8_to_utf16(utf8, -1, NULL, &n, NULL)) != NULL) {
        /* Room for the Latin, which may be longer (one kana, two letters). */
        int32_t len = (int32_t)n, cap = len * 4 + 16, limit = len;
        UChar *buf = g_new(UChar, cap);
        UErrorCode err = U_ZERO_ERROR;
        memcpy(buf, u16, (size_t)len * sizeof(UChar));
        utrans_transUChars(tr, buf, &len, cap, 0, &limit, &err);
        if (U_SUCCESS(err))
            text = g_utf16_to_utf8((const gunichar2 *)buf, len, NULL, NULL, NULL);
        g_free(buf);
        g_free(u16);
    }
    latin1 = g_convert_with_fallback(text ? text : utf8, -1, "ISO-8859-1", "UTF-8", "?", NULL, NULL, NULL);
    g_free(text);
    return latin1;
}

static void show_title(App *app)
{
    const char *title = webkit_web_view_get_title(app->web_view);
    char *name = g_strdup_printf("Heliotrope%s%s", title && *title ? " - " : "", title ? title : "");
    char *latin1 = latin1_title(name);
    /* WM_NAME and WM_ICON_NAME as 4Dwm can show them (Latin-1 STRING), and
     * the title itself as _NET_WM_NAME for window managers that take UTF-8. */
    XStoreName(app->dpy, app->win, latin1 ? latin1 : name);
    XSetIconName(app->dpy, app->win, latin1 ? latin1 : name);
    XChangeProperty(app->dpy, app->win, XInternAtom(app->dpy, "_NET_WM_NAME", False),
                    XInternAtom(app->dpy, "UTF8_STRING", False), 8, PropModeReplace,
                    (const unsigned char *)name, (int)strlen(name));
    g_free(latin1);
    g_free(name);
}

static void title_changed(GObject *object, GParamSpec *pspec, Tab *tab)
{
    const char *title = webkit_web_view_get_title(tab->web_view);
    (void)object;
    (void)pspec;
    g_free(tab->title);
    tab->title = title && *title ? latin1_title(title) : NULL;
    if (tab == tab->app->tab)
        show_title(tab->app);
    draw_tabs(tab->app);
    XFlush(tab->app->dpy);
}

static void state_changed(GObject *object, GParamSpec *pspec, Tab *tab)
{
    App *app = tab->app;
    (void)object;
    (void)pspec;
    if (tab != app->tab)
        return;
    app->progress_value = webkit_web_view_get_estimated_load_progress(app->web_view);
    draw_bar(app);
    XFlush(app->dpy);
}

/* The Web process died (a crash, or killed for memory): reload once; if the page
 * kills it again within a minute, show why instead of looping. */
static void web_process_terminated(WebKitWebView *web_view, WebKitWebProcessTerminationReason reason, Tab *tab)
{
    App *app = tab->app;
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

static void mouse_target_changed(WebKitWebView *web_view, WebKitHitTestResult *hit, guint modifiers, Tab *tab)
{
    App *app = tab->app;
    const char *link = webkit_hit_test_result_context_is_link(hit) ? webkit_hit_test_result_get_link_uri(hit) : NULL;
    (void)web_view;
    (void)modifiers;
    if (tab != app->tab)
        return;
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

/* The field asks for something: an address (as typed or the page's), a
 * file name for a download, or text to find in the page. */
static void start_prompt(App *app, int prompt, const char *text)
{
    app->editing = TRUE;
    app->prompt = prompt;
    g_string_assign(app->edit, text ? text : "");
    app->cursor = app->edit->len;
    app->all_selected = FALSE;
    draw_bar(app);
}

static void start_editing(App *app)
{
    start_prompt(app, PROMPT_ADDRESS, webkit_web_view_get_uri(app->web_view));
    app->all_selected = TRUE;
    draw_bar(app);
}

static void stop_editing(App *app)
{
    int prompt = app->prompt;
    app->editing = FALSE;
    app->all_selected = FALSE;
    app->prompt = PROMPT_ADDRESS;
    /* a download's prompt left unanswered: not saved */
    if (prompt == PROMPT_SAVE && app->prompt_download)
        destination_chosen(app, NULL);
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
    int left;
    field_rect(app, &r);
    if (!app->font)
        return app->edit->len;
    left = r.x + 6;
    if (app->prompt != PROMPT_ADDRESS) {
        const char *label = app->prompt == PROMPT_SAVE ? "Save as:" : "Find:";
        left += XTextWidth(app->font, label, (int)strlen(label)) + 8;
    }
    for (i = 0; i < app->edit->len; i++) {
        int w = XTextWidth(app->font, app->edit->str, (int)i);
        int cw = XTextWidth(app->font, app->edit->str + i, 1);
        if (left + w + cw / 2 > x)
            return i;
    }
    return app->edit->len;
}

static gboolean handle_field_key(App *app, KeySym sym, const char *text, unsigned int state)
{
    gboolean control = (state & ControlMask) != 0;
    switch (sym) {
    case XK_Return:
    case XK_KP_Enter: {
        char *text = g_strdup(app->edit->str);
        int prompt = app->prompt;
        app->editing = FALSE;
        app->all_selected = FALSE;
        app->prompt = PROMPT_ADDRESS;
        draw_bar(app);
        if (prompt == PROMPT_SAVE)
            destination_chosen(app, text);
        else if (prompt == PROMPT_FIND) {
            if (*text)
                find_text(app, text);
        } else
            load_address(app, text);
        g_free(text);
        return TRUE;
    }
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

/* Apply a page's site's choices before its document and resources load. The
 * block list is the tabs' one content manager's: the latest page decides. */
static void apply_site_choices(App *app, WebKitWebView *web_view)
{
    unsigned flags = site_flags_of(app, webkit_web_view_get_uri(web_view));
    gboolean block = app->filter && !(flags & SITE_NO_BLOCKING);
    webkit_settings_set_enable_javascript(webkit_web_view_get_settings(web_view), !(flags & SITE_NO_JS));
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
        apply_site_choices(app, app->web_view);
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

/* --- tabs ------------------------------------------------------------------- */

static void buffer_rendered(WPEView *view, WPEBuffer *buffer, Tab *tab);
static void load_changed(WebKitWebView *web_view, WebKitLoadEvent event, Tab *tab);
static gboolean load_failed(WebKitWebView *web_view, WebKitLoadEvent event, char *uri, GError *error, Tab *tab);
static void title_changed(GObject *object, GParamSpec *pspec, Tab *tab);
static void state_changed(GObject *object, GParamSpec *pspec, Tab *tab);
static void mouse_target_changed(WebKitWebView *web_view, WebKitHitTestResult *hit, guint modifiers, Tab *tab);
static void web_process_terminated(WebKitWebView *web_view, WebKitWebProcessTerminationReason reason, Tab *tab);
static gboolean context_menu(WebKitWebView *web_view, WebKitContextMenu *menu, WebKitHitTestResult *hit, Tab *tab);
static gboolean decide_policy(WebKitWebView *web_view, WebKitPolicyDecision *decision, WebKitPolicyDecisionType type, Tab *tab);
static WebKitWebView *create_web_view(WebKitWebView *web_view, WebKitNavigationAction *action, Tab *tab);
static void close_requested(WebKitWebView *web_view, Tab *tab);
static void show_message(App *app, const char *text, int seconds);

/* WebKit features each view gets: redraw only what changed (damage tracking,
 * off by default in WPE; a full frame costs seconds in software GL, a
 * keystroke's worth a fraction of one; HELIOTROPE_NO_DAMAGE=1 turns it off),
 * plus any named in HELIOTROPE_FEATURES=Name,Name for experiments. */
static void apply_features(App *app, WebKitWebView *web_view)
{
    WebKitSettings *settings = webkit_web_view_get_settings(web_view);
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
        if ((!found || app->timing) && app->tabs->len == 0)
            g_printerr("heliotrope: feature %s %s\n", names[j], found ? "on" : "not found");
    }
    g_strfreev(names);
    g_free(list);
    webkit_feature_list_unref(all);
    /* HELIOTROPE_CONSOLE=1: the page's console messages (including what the
     * content blocker stopped) go to stdout, for testing. */
    if (env_on("HELIOTROPE_CONSOLE"))
        webkit_settings_set_enable_write_console_messages_to_stdout(settings, TRUE);
}

/* The page area moved (the tab strip came or went) or changed size. */
static void layout_page(App *app)
{
    guint i;
    if (app->present)
        XMoveResizeWindow(app->dpy, app->page_win, 0, page_top(app), page_width(app), page_height(app));
    for (i = 0; i < app->tabs->len; i++) {
        Tab *t = g_ptr_array_index(app->tabs, i);
        wpe_toplevel_resize(wpe_view_get_toplevel(t->view), page_width(app), page_height(app));
    }
}

/* A tab for web_view (made by the caller, or by WebKit for a page that opens
 * a window), not shown yet. */
static Tab *add_tab(App *app, WebKitWebView *web_view)
{
    Tab *tab = g_new0(Tab, 1);
    gboolean strip = app->tabs->len == 1;
    tab->app = app;
    tab->web_view = web_view;
    apply_features(app, web_view);
    tab->view = webkit_web_view_get_wpe_view(web_view);
    wpe_toplevel_resize(wpe_view_get_toplevel(tab->view), page_width(app), page_height(app));
    g_signal_connect(tab->view, "buffer-rendered", G_CALLBACK(buffer_rendered), tab);
    g_signal_connect(web_view, "load-changed", G_CALLBACK(load_changed), tab);
    g_signal_connect(web_view, "load-failed", G_CALLBACK(load_failed), tab);
    g_signal_connect(web_view, "notify::title", G_CALLBACK(title_changed), tab);
    g_signal_connect(web_view, "notify::estimated-load-progress", G_CALLBACK(state_changed), tab);
    g_signal_connect(web_view, "notify::is-loading", G_CALLBACK(state_changed), tab);
    g_signal_connect(web_view, "notify::uri", G_CALLBACK(state_changed), tab);
    g_signal_connect(web_view, "mouse-target-changed", G_CALLBACK(mouse_target_changed), tab);
    g_signal_connect(web_view, "web-process-terminated", G_CALLBACK(web_process_terminated), tab);
    g_signal_connect(web_view, "context-menu", G_CALLBACK(context_menu), tab);
    g_signal_connect(web_view, "decide-policy", G_CALLBACK(decide_policy), tab);
    g_signal_connect(web_view, "create", G_CALLBACK(create_web_view), tab);
    g_signal_connect(web_view, "close", G_CALLBACK(close_requested), tab);
    g_ptr_array_add(app->tabs, tab);
    if (app->tab)
        wpe_view_unmap(tab->view);
    if (strip)
        layout_page(app);
    return tab;
}

/* A new view sharing the first one's Web process and settings. */
static WebKitWebView *related_view(App *app, WebKitWebView *related)
{
    (void)app; /* the display, context and content manager are related's */
    return WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "related-view", related, NULL));
}

/* A tab shown again draws nothing by itself (its page has not changed):
 * a size one row short, then the right one again, gets a whole frame. */
static gboolean restore_size(gpointer data)
{
    App *app = data;
    app->repaint_timer = 0;
    wpe_toplevel_resize(wpe_view_get_toplevel(app->view), page_width(app), page_height(app));
    return G_SOURCE_REMOVE;
}

static void force_frame(App *app)
{
    int h = page_height(app);
    wpe_toplevel_resize(wpe_view_get_toplevel(app->view), page_width(app), h > 1 ? h - 1 : h + 1);
    if (app->repaint_timer)
        g_source_remove(app->repaint_timer);
    app->repaint_timer = g_timeout_add(40, restore_size, app);
}

static void select_tab(App *app, Tab *tab)
{
    Tab *old = app->tab;
    if (old == tab)
        return;
    if (old) {
        wpe_view_focus_out(old->view);
        wpe_view_unmap(old->view);
    }
    app->tab = tab;
    app->web_view = tab->web_view;
    app->view = tab->view;
    wpe_view_map(tab->view);
    wpe_view_focus_in(tab->view);
    if (old)
        force_frame(app);
    else
        wpe_toplevel_resize(wpe_view_get_toplevel(tab->view), page_width(app), page_height(app));
    app->progress_value = webkit_web_view_get_estimated_load_progress(tab->web_view);
    g_clear_pointer(&app->status, g_free);
    if (app->editing && app->prompt == PROMPT_ADDRESS) {
        app->editing = FALSE;
        app->all_selected = FALSE;
    }
    show_title(app);
    redraw(app);
    draw_tabs(app);
}

/* A new tab loading uri (an address or a search), shown or not. */
static Tab *open_tab(App *app, const char *uri, gboolean show)
{
    Tab *tab = add_tab(app, related_view(app, app->web_view));
    if (show)
        select_tab(app, tab);
    if (uri && *uri) {
        WebKitWebView *shown = app->web_view;
        app->web_view = tab->web_view;
        load_address(app, uri);
        app->web_view = shown;
    }
    draw_tabs(app);
    XFlush(app->dpy);
    return tab;
}

static void close_tab(App *app, Tab *tab)
{
    guint i;
    if (app->tabs->len <= 1) {
        g_main_loop_quit(app->loop);
        return;
    }
    for (i = 0; i < app->tabs->len && g_ptr_array_index(app->tabs, i) != tab; i++)
        ;
    if (i == app->tabs->len)
        return;
    if (tab == app->tab)
        select_tab(app, g_ptr_array_index(app->tabs, i + 1 < app->tabs->len ? i + 1 : i - 1));
    g_ptr_array_remove_index(app->tabs, i);
    g_signal_handlers_disconnect_by_data(tab->view, tab);
    g_signal_handlers_disconnect_by_data(tab->web_view, tab);
    g_object_unref(tab->web_view);
    g_free(tab->title);
    g_free(tab);
    if (app->tabs->len == 1)
        layout_page(app);
    redraw(app);
    draw_tabs(app);
}

static void step_tab(App *app, int d)
{
    guint i, n = app->tabs->len;
    for (i = 0; i < n && g_ptr_array_index(app->tabs, i) != app->tab; i++)
        ;
    if (n > 1)
        select_tab(app, g_ptr_array_index(app->tabs, (i + n + d) % n));
}

static void tab_rect(App *app, guint i, XRectangle *r)
{
    int n = (int)app->tabs->len, w = (app->width - 8) / (n ? n : 1);
    if (w > 220)
        w = 220;
    r->x = 4 + (int)i * w;
    r->y = MENU_HEIGHT + BAR_HEIGHT + 2;
    r->width = w - 2;
    r->height = TAB_HEIGHT - 3;
}

static void close_box(const XRectangle *r, XRectangle *box)
{
    box->width = box->height = 11;
    box->x = r->x + r->width - 16;
    box->y = r->y + (r->height - 11) / 2;
}

static void draw_tabs(App *app)
{
    int top = MENU_HEIGHT + BAR_HEIGHT;
    guint i;
    if (!app->tabs || app->tabs->len < 2)
        return;
    XSetForeground(app->dpy, app->gc, app->bar_bg);
    XFillRectangle(app->dpy, app->win, app->gc, 0, top, app->width, TAB_HEIGHT);
    XSetForeground(app->dpy, app->gc, app->bar_shadow);
    XDrawLine(app->dpy, app->win, app->gc, 0, top + TAB_HEIGHT - 1, app->width, top + TAB_HEIGHT - 1);
    for (i = 0; i < app->tabs->len; i++) {
        Tab *t = g_ptr_array_index(app->tabs, i);
        XRectangle r, box;
        const char *text = t->title ? t->title : webkit_web_view_get_uri(t->web_view);
        int len, avail;
        tab_rect(app, i, &r);
        XSetForeground(app->dpy, app->gc, t == app->tab ? app->field_edit_bg : app->field_bg);
        XFillRectangle(app->dpy, app->win, app->gc, r.x, r.y, r.width, r.height);
        bevel(app, &r, t != app->tab);
        if (webkit_web_view_is_loading(t->web_view)) {
            XSetForeground(app->dpy, app->gc, app->progress);
            XFillRectangle(app->dpy, app->win, app->gc, r.x + 1, r.y + r.height - 3,
                (unsigned)((r.width - 2) * webkit_web_view_get_estimated_load_progress(t->web_view)), 2);
        }
        if (!text)
            text = "New tab";
        len = (int)strlen(text);
        avail = r.width - 28;
        while (len > 0 && app->font && XTextWidth(app->font, text, len) > avail)
            len--;
        XSetForeground(app->dpy, app->gc, app->ink);
        if (app->font)
            XDrawString(app->dpy, app->win, app->gc, r.x + 6, r.y + (r.height + app->font->ascent - app->font->descent) / 2,
                text, len);
        close_box(&r, &box);
        XSetForeground(app->dpy, app->gc, app->ink_disabled);
        XDrawLine(app->dpy, app->win, app->gc, box.x + 2, box.y + 2, box.x + box.width - 3, box.y + box.height - 3);
        XDrawLine(app->dpy, app->win, app->gc, box.x + 2, box.y + box.height - 3, box.x + box.width - 3, box.y + 2);
    }
}

static void handle_tab_strip(App *app, XButtonEvent *b)
{
    guint i;
    if (b->type != ButtonPress)
        return;
    for (i = 0; i < app->tabs->len; i++) {
        XRectangle r, box;
        tab_rect(app, i, &r);
        if (!in_rect(&r, b->x, b->y))
            continue;
        close_box(&r, &box);
        if (b->button == Button2 || (b->button == Button1 && in_rect(&box, b->x, b->y)))
            close_tab(app, g_ptr_array_index(app->tabs, i));
        else if (b->button == Button1)
            select_tab(app, g_ptr_array_index(app->tabs, i));
        return;
    }
}

/* A page opens a window (target=_blank, window.open): a tab, shown when ready. */
static void ready_to_show(WebKitWebView *web_view, Tab *tab)
{
    (void)web_view;
    select_tab(tab->app, tab);
}

static WebKitWebView *create_web_view(WebKitWebView *web_view, WebKitNavigationAction *action, Tab *tab)
{
    Tab *t = add_tab(tab->app, related_view(tab->app, web_view));
    (void)action;
    g_signal_connect(t->web_view, "ready-to-show", G_CALLBACK(ready_to_show), t);
    draw_tabs(tab->app);
    return t->web_view;
}

static void close_requested(WebKitWebView *web_view, Tab *tab)
{
    (void)web_view;
    close_tab(tab->app, tab);
}

/* Another browser window: a process of its own (its own Web and network
 * processes as well: only when asked for). */
static void spawn_window(App *app, const char *uri)
{
    char *argv[3];
    GError *error = NULL;
    argv[0] = (char *)app->argv0;
    argv[1] = (char *)(uri && *uri ? uri : app->home);
    argv[2] = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error)) {
        show_message(app, error->message, 5);
        g_error_free(error);
    }
}

/* --- the status line's messages --------------------------------------------- */

static gboolean message_expired(gpointer data)
{
    App *app = data;
    app->message_timer = 0;
    g_clear_pointer(&app->message, g_free);
    draw_page(app);
    XFlush(app->dpy);
    return G_SOURCE_REMOVE;
}

/* seconds 0: until the next message */
static void show_message(App *app, const char *text, int seconds)
{
    g_free(app->message);
    app->message = g_strdup(text);
    if (app->message_timer)
        g_source_remove(app->message_timer);
    app->message_timer = seconds > 0 ? g_timeout_add_seconds((guint)seconds, message_expired, app) : 0;
    draw_page(app);
    XFlush(app->dpy);
}

/* --- the X clipboard ----------------------------------------------------------- */

/* Hold text as CLIPBOARD and PRIMARY, for other X programs. */
static void own_selection(App *app, const char *text)
{
    g_free(app->clip_text);
    app->clip_text = g_strdup(text);
    XSetSelectionOwner(app->dpy, app->clipboard, app->win, CurrentTime);
    XSetSelectionOwner(app->dpy, XA_PRIMARY, app->win, CurrentTime);
}

static void selection_request(App *app, XSelectionRequestEvent *r)
{
    XEvent reply;
    memset(&reply, 0, sizeof reply);
    reply.xselection.type = SelectionNotify;
    reply.xselection.display = r->display;
    reply.xselection.requestor = r->requestor;
    reply.xselection.selection = r->selection;
    reply.xselection.target = r->target;
    reply.xselection.time = r->time;
    reply.xselection.property = None;
    if (app->clip_text) {
        Atom property = r->property != None ? r->property : r->target;
        if (r->target == app->targets) {
            Atom list[3] = { app->targets, app->utf8, XA_STRING };
            XChangeProperty(app->dpy, r->requestor, property, XA_ATOM, 32, PropModeReplace, (unsigned char *)list, 3);
            reply.xselection.property = property;
        } else if (r->target == app->utf8) {
            XChangeProperty(app->dpy, r->requestor, property, app->utf8, 8, PropModeReplace,
                (unsigned char *)app->clip_text, (int)strlen(app->clip_text));
            reply.xselection.property = property;
        } else if (r->target == XA_STRING) {
            char *latin1 = g_convert_with_fallback(app->clip_text, -1, "ISO-8859-1", "UTF-8", "?", NULL, NULL, NULL);
            if (latin1) {
                XChangeProperty(app->dpy, r->requestor, property, XA_STRING, 8, PropModeReplace,
                    (unsigned char *)latin1, (int)strlen(latin1));
                reply.xselection.property = property;
                g_free(latin1);
            }
        }
    }
    XSendEvent(app->dpy, r->requestor, False, 0, &reply);
}

/* After the page copied (Ctrl+C, the Copy menu): what WebKit put on its
 * clipboard, offered to X. */
static gboolean export_clipboard(gpointer data)
{
    App *app = data;
    WPEClipboard *clipboard = wpe_display_get_clipboard(app->display);
    gint64 count = clipboard ? wpe_clipboard_get_change_count(clipboard) : 0;
    if (clipboard && count != app->wpe_clip_count) {
        gsize size = 0;
        char *text = wpe_clipboard_read_text(clipboard, "text/plain", &size);
        app->wpe_clip_count = count;
        if (text && *text)
            own_selection(app, text);
        g_free(text);
    }
    return G_SOURCE_REMOVE;
}

/* A paste into the page: another client's CLIPBOARD put on WebKit's
 * clipboard first (ours is there already). */
static void paste_into_page(App *app)
{
    if (XGetSelectionOwner(app->dpy, app->clipboard) == app->win) {
        webkit_web_view_execute_editing_command(app->web_view, WEBKIT_EDITING_COMMAND_PASTE);
        return;
    }
    app->paste_pending = TRUE;
    XConvertSelection(app->dpy, app->clipboard, app->utf8, app->paste_property, app->win, CurrentTime);
}

static void page_paste_arrived(App *app, XSelectionEvent *e)
{
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    app->paste_pending = FALSE;
    if (e->property != None && XGetWindowProperty(app->dpy, app->win, e->property, 0, 1 << 20, True, AnyPropertyType,
            &type, &format, &n, &after, &data) == Success && data && format == 8) {
        WPEClipboard *clipboard = wpe_display_get_clipboard(app->display);
        WPEClipboardContent *content = wpe_clipboard_content_new();
        char *text = type == XA_STRING ? g_convert((char *)data, (gssize)n, "UTF-8", "ISO-8859-1", NULL, NULL, NULL)
            : g_strndup((char *)data, n);
        wpe_clipboard_content_set_text(content, text ? text : "");
        wpe_clipboard_set_content(clipboard, content);
        app->wpe_clip_count = wpe_clipboard_get_change_count(clipboard);
        wpe_clipboard_content_unref(content);
        g_free(text);
    } else if (e->property == None && e->target == app->utf8) {
        /* no UTF8_STRING: ask for STRING */
        app->paste_pending = TRUE;
        XConvertSelection(app->dpy, app->clipboard, XA_STRING, app->paste_property, app->win, CurrentTime);
        return;
    }
    if (data)
        XFree(data);
    webkit_web_view_execute_editing_command(app->web_view, WEBKIT_EDITING_COMMAND_PASTE);
}

/* --- downloads ------------------------------------------------------------------ */

static void start_prompt(App *app, int prompt, const char *text);

static char *downloads_dir(void)
{
    const char *d = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    char *dir;
    if (d && g_file_test(d, G_FILE_TEST_IS_DIR))
        return g_strdup(d);
    dir = g_build_filename(g_get_home_dir(), "Downloads", NULL);
    if (g_file_test(dir, G_FILE_TEST_IS_DIR))
        return dir;
    g_free(dir);
    return g_strdup(g_get_home_dir());
}

/* dir/name, or dir/name-2.ext etc. if that is taken */
static char *free_path(const char *dir, const char *name)
{
    char *path = g_build_filename(dir, name && *name ? name : "download", NULL);
    const char *dot = strrchr(name && *name ? name : "download", '.');
    int i;
    for (i = 2; g_file_test(path, G_FILE_TEST_EXISTS) && i < 1000; i++) {
        char *base = g_strndup(name, dot && dot != name ? (gsize)(dot - name) : strlen(name));
        char *n = g_strdup_printf("%s-%d%s", base, i, dot && dot != name ? dot : "");
        g_free(path);
        path = g_build_filename(dir, n, NULL);
        g_free(n);
        g_free(base);
    }
    return path;
}

static void ask_destination(App *app, WebKitDownload *download)
{
    const char *name = g_object_get_data(G_OBJECT(download), "heliotrope-name");
    char *dir = downloads_dir(), *path = free_path(dir, name);
    app->prompt_download = download;
    start_prompt(app, PROMPT_SAVE, path);
    g_free(path);
    g_free(dir);
}

static gboolean decide_destination(WebKitDownload *download, const char *suggested, App *app)
{
    g_printerr("heliotrope: download %s: asking where (suggested %s)\n",
        webkit_uri_request_get_uri(webkit_download_get_request(download)), suggested ? suggested : "none");
    char *name = NULL;
    if (suggested && *suggested)
        name = g_path_get_basename(suggested);
    else {
        /* none suggested (a page): the address's last part, else its host, as .html */
        const char *uri = webkit_uri_request_get_uri(webkit_download_get_request(download));
        GUri *u = uri ? g_uri_parse(uri, G_URI_FLAGS_NONE, NULL) : NULL;
        const char *path = u ? g_uri_get_path(u) : NULL;
        char *base = path && *path && strcmp(path, "/") ? g_path_get_basename(path) : NULL;
        if (base && *base && strcmp(base, "/") && strcmp(base, "."))
            name = strchr(base, '.') ? g_strdup(base) : g_strconcat(base, ".html", NULL);
        else if (u && g_uri_get_host(u))
            name = g_strconcat(g_uri_get_host(u), ".html", NULL);
        g_free(base);
        if (u)
            g_uri_unref(u);
        if (!name)
            name = g_strdup("download");
    }
    g_object_set_data_full(G_OBJECT(download), "heliotrope-name", name, g_free);
    g_object_ref(download);
    if (app->prompt_download)
        g_ptr_array_add(app->waiting_downloads, download);
    else
        ask_destination(app, download);
    return TRUE; /* the destination is set when the prompt is answered */
}

static void download_progress(WebKitDownload *download, guint64 length, App *app)
{
    const char *dest = webkit_download_get_destination(download);
    char *name = dest ? g_path_get_basename(dest) : g_strdup("download");
    double p = webkit_download_get_estimated_progress(download);
    gint64 now = g_get_monotonic_time();
    gint64 *last = g_object_get_data(G_OBJECT(download), "heliotrope-shown");
    char *text;
    (void)length;
    if (last && now - *last < G_USEC_PER_SEC / 2)
        return;
    if (!last) {
        last = g_new(gint64, 1);
        g_object_set_data_full(G_OBJECT(download), "heliotrope-shown", last, g_free);
    }
    *last = now;
    text = p > 0 ? g_strdup_printf("Saving %s: %d%%", name, (int)(p * 100))
        : g_strdup_printf("Saving %s: %lu KB", name, (unsigned long)(webkit_download_get_received_data_length(download) / 1024));
    show_message(app, text, 0);
    g_free(text);
    g_free(name);
}

static void download_finished(WebKitDownload *download, App *app)
{
    const char *dest = webkit_download_get_destination(download);
    g_printerr("heliotrope: download to %s finished\n", dest ? dest : "?");
    if (!g_object_get_data(G_OBJECT(download), "heliotrope-failed") && dest) {
        char *text = g_strdup_printf("Saved %s", dest);
        show_message(app, text, 8);
        g_free(text);
    }
}

static void download_failed(WebKitDownload *download, GError *error, App *app)
{
    g_object_set_data(G_OBJECT(download), "heliotrope-failed", GINT_TO_POINTER(1));
    g_printerr("heliotrope: download failed: %s\n", error->message);
    if (!g_error_matches(error, WEBKIT_DOWNLOAD_ERROR, WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER)) {
        char *text = g_strdup_printf("Not saved: %s", error->message);
        show_message(app, text, 8);
        g_free(text);
    } else
        show_message(app, "Not saved", 3);
}

/* every download of the session: links saved, pages saved, files the
 * browser cannot show */
static void download_started(WebKitNetworkSession *session, WebKitDownload *download, App *app)
{
    (void)session;
    g_printerr("heliotrope: download %s started\n", webkit_uri_request_get_uri(webkit_download_get_request(download)));
    g_signal_connect(download, "decide-destination", G_CALLBACK(decide_destination), app);
    g_signal_connect(download, "received-data", G_CALLBACK(download_progress), app);
    g_signal_connect(download, "finished", G_CALLBACK(download_finished), app);
    g_signal_connect(download, "failed", G_CALLBACK(download_failed), app);
}

/* The prompt for a download answered: path, or NULL for no. */
static void destination_chosen(App *app, const char *path)
{
    WebKitDownload *download = app->prompt_download;
    app->prompt_download = NULL;
    if (download) {
        if (path && *path) {
            char *full = g_str_has_prefix(path, "~/") ? g_build_filename(g_get_home_dir(), path + 2, NULL) : g_strdup(path);
            webkit_download_set_allow_overwrite(download, TRUE);
            webkit_download_set_destination(download, full);
            g_free(full);
        } else {
            /* the destination callback must be answered before the download goes */
            webkit_download_set_destination(download, "");
            webkit_download_cancel(download);
        }
        g_object_unref(download);
    }
    if (app->waiting_downloads->len) {
        WebKitDownload *next = g_ptr_array_steal_index(app->waiting_downloads, 0);
        ask_destination(app, next);
    }
}

/* Files WebKit cannot show are saved; a middle click on a link opens it in a new tab. */
static gboolean decide_policy(WebKitWebView *web_view, WebKitPolicyDecision *decision, WebKitPolicyDecisionType type, Tab *tab)
{
    (void)web_view;
    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        WebKitResponsePolicyDecision *r = WEBKIT_RESPONSE_POLICY_DECISION(decision);
        if (!webkit_response_policy_decision_is_mime_type_supported(r)
            && webkit_response_policy_decision_is_main_frame_main_resource(r)) {
            webkit_policy_decision_download(decision);
            return TRUE;
        }
    } else if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        WebKitNavigationAction *a = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
        if (webkit_navigation_action_get_navigation_type(a) == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED
            && webkit_navigation_action_get_mouse_button(a) == 2) {
            const char *uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(a));
            open_tab(tab->app, uri, FALSE);
            show_message(tab->app, "Opened in a new tab", 3);
            webkit_policy_decision_ignore(decision);
            return TRUE;
        }
    }
    return FALSE;
}

/* --- find in page ------------------------------------------------------------------ */

static void find_failed(WebKitFindController *find, App *app)
{
    char *text = g_strdup_printf("Not found: %s", webkit_find_controller_get_search_text(find));
    show_message(app, text, 4);
    g_free(text);
}

static void find_text(App *app, const char *text)
{
    WebKitFindController *find = webkit_web_view_get_find_controller(app->web_view);
    if (!g_object_get_data(G_OBJECT(find), "heliotrope-find")) {
        g_object_set_data(G_OBJECT(find), "heliotrope-find", GINT_TO_POINTER(1));
        g_signal_connect(find, "failed-to-find-text", G_CALLBACK(find_failed), app);
    }
    g_free(app->find_text);
    app->find_text = g_strdup(text);
    webkit_find_controller_search(find, text, WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND, G_MAXUINT);
}

/* --- menus -------------------------------------------------------------------------- */

enum {
    ACT_NONE, ACT_NEW_TAB, ACT_NEW_WINDOW, ACT_OPEN_LOCATION, ACT_SAVE_PAGE, ACT_CLOSE_TAB, ACT_EXIT,
    ACT_COPY, ACT_PASTE, ACT_SELECT_ALL, ACT_FIND, ACT_FIND_AGAIN,
    ACT_RELOAD, ACT_STOP, ACT_ZOOM_IN, ACT_ZOOM_OUT, ACT_ZOOM_RESET, ACT_TOGGLE_JS, ACT_TOGGLE_BLOCK,
    ACT_BACK, ACT_FORWARD, ACT_HOME, ACT_PREV_TAB, ACT_NEXT_TAB, ACT_ABOUT,
    ACT_LINK_OPEN, ACT_LINK_TAB, ACT_LINK_WINDOW, ACT_LINK_SAVE, ACT_LINK_COPY,
    ACT_IMAGE_TAB, ACT_IMAGE_SAVE, ACT_IMAGE_COPY
};

static const char *const menu_titles[] = { "File", "Edit", "View", "Go", "Help" };
#define N_MENUS 5

static int menu_title_x(App *app, int i)
{
    int x = 6, k;
    for (k = 0; k < i; k++)
        x += (app->font ? XTextWidth(app->font, menu_titles[k], (int)strlen(menu_titles[k])) : 30) + 18;
    return x;
}

static int menu_title_width(App *app, int i)
{
    return (app->font ? XTextWidth(app->font, menu_titles[i], (int)strlen(menu_titles[i])) : 30) + 12;
}

static int menu_at(App *app, int x)
{
    int i;
    for (i = 0; i < N_MENUS; i++)
        if (x >= menu_title_x(app, i) - 6 && x < menu_title_x(app, i) + menu_title_width(app, i))
            return i;
    return -1;
}

static void draw_menubar(App *app)
{
    int i;
    XSetForeground(app->dpy, app->gc, app->bar_bg);
    XFillRectangle(app->dpy, app->win, app->gc, 0, 0, app->width, MENU_HEIGHT);
    XSetForeground(app->dpy, app->gc, app->bar_shadow);
    XDrawLine(app->dpy, app->win, app->gc, 0, MENU_HEIGHT - 1, app->width, MENU_HEIGHT - 1);
    for (i = 0; i < N_MENUS; i++) {
        int x = menu_title_x(app, i);
        if (app->popup_menu == i) {
            XRectangle r;
            r.x = x - 6;
            r.y = 1;
            r.width = menu_title_width(app, i);
            r.height = MENU_HEIGHT - 3;
            XSetForeground(app->dpy, app->gc, app->selection);
            XFillRectangle(app->dpy, app->win, app->gc, r.x, r.y, r.width, r.height);
            bevel(app, &r, TRUE);
        }
        XSetForeground(app->dpy, app->gc, app->ink);
        if (app->font)
            XDrawString(app->dpy, app->win, app->gc, x, (MENU_HEIGHT + app->font->ascent - app->font->descent) / 2,
                menu_titles[i], (int)strlen(menu_titles[i]));
    }
}

static int add_item(MenuItem *items, int n, const char *label, const char *accel, int action, gboolean enabled)
{
    if (n >= MAX_MENU_ITEMS)
        return n;
    items[n].label = label;
    items[n].accel = accel;
    items[n].action = action;
    items[n].enabled = enabled;
    return n + 1;
}

static int build_menu(App *app, int menu, MenuItem *it)
{
    int n = 0;
    gboolean loading = webkit_web_view_is_loading(app->web_view);
    unsigned flags = current_site_flags(app);
    switch (menu) {
    case 0:
        n = add_item(it, n, "New Tab", "Ctrl+T", ACT_NEW_TAB, TRUE);
        n = add_item(it, n, "New Window", "Ctrl+N", ACT_NEW_WINDOW, TRUE);
        n = add_item(it, n, "Open Location...", "Ctrl+L", ACT_OPEN_LOCATION, TRUE);
        n = add_item(it, n, NULL, NULL, ACT_NONE, FALSE);
        n = add_item(it, n, "Save Page As...", "Ctrl+S", ACT_SAVE_PAGE, webkit_web_view_get_uri(app->web_view) != NULL);
        n = add_item(it, n, NULL, NULL, ACT_NONE, FALSE);
        n = add_item(it, n, "Close Tab", "Ctrl+W", ACT_CLOSE_TAB, app->tabs->len > 1);
        n = add_item(it, n, "Exit", "Ctrl+Q", ACT_EXIT, TRUE);
        break;
    case 1:
        n = add_item(it, n, "Copy", "Ctrl+C", ACT_COPY, TRUE);
        n = add_item(it, n, "Paste", "Ctrl+V", ACT_PASTE, TRUE);
        n = add_item(it, n, "Select All", "Ctrl+A", ACT_SELECT_ALL, TRUE);
        n = add_item(it, n, NULL, NULL, ACT_NONE, FALSE);
        n = add_item(it, n, "Find...", "Ctrl+F", ACT_FIND, TRUE);
        n = add_item(it, n, "Find Again", "Ctrl+G", ACT_FIND_AGAIN, app->find_text != NULL);
        break;
    case 2:
        n = loading ? add_item(it, n, "Stop", "Esc", ACT_STOP, TRUE) : add_item(it, n, "Reload", "F5", ACT_RELOAD, TRUE);
        n = add_item(it, n, NULL, NULL, ACT_NONE, FALSE);
        n = add_item(it, n, "Zoom In", "Ctrl++", ACT_ZOOM_IN, TRUE);
        n = add_item(it, n, "Zoom Out", "Ctrl+-", ACT_ZOOM_OUT, TRUE);
        n = add_item(it, n, "Actual Size", "Ctrl+0", ACT_ZOOM_RESET, TRUE);
        n = add_item(it, n, NULL, NULL, ACT_NONE, FALSE);
        n = add_item(it, n, flags & SITE_NO_JS ? "JavaScript Off for This Site" : "JavaScript On for This Site", NULL, ACT_TOGGLE_JS, TRUE);
        n = add_item(it, n, !app->filter ? "Ad Blocking Unavailable" : flags & SITE_NO_BLOCKING ? "Ad Blocking Off for This Site"
            : "Ad Blocking On for This Site", NULL, ACT_TOGGLE_BLOCK, app->filter != NULL);
        break;
    case 3:
        n = add_item(it, n, "Back", "Alt+Left", ACT_BACK, webkit_web_view_can_go_back(app->web_view));
        n = add_item(it, n, "Forward", "Alt+Right", ACT_FORWARD, webkit_web_view_can_go_forward(app->web_view));
        n = add_item(it, n, "Home", "Alt+Home", ACT_HOME, TRUE);
        n = add_item(it, n, NULL, NULL, ACT_NONE, FALSE);
        n = add_item(it, n, "Previous Tab", "Ctrl+PgUp", ACT_PREV_TAB, app->tabs->len > 1);
        n = add_item(it, n, "Next Tab", "Ctrl+PgDn", ACT_NEXT_TAB, app->tabs->len > 1);
        break;
    case 4:
        n = add_item(it, n, "About Heliotrope", NULL, ACT_ABOUT, TRUE);
        break;
    default: /* the page's context menu */
        if (app->ctx_link) {
            n = add_item(it, n, "Open Link", NULL, ACT_LINK_OPEN, TRUE);
            n = add_item(it, n, "Open Link in New Tab", NULL, ACT_LINK_TAB, TRUE);
            n = add_item(it, n, "Open Link in New Window", NULL, ACT_LINK_WINDOW, TRUE);
            n = add_item(it, n, NULL, NULL, ACT_NONE, FALSE);
            n = add_item(it, n, "Save Link As...", NULL, ACT_LINK_SAVE, TRUE);
            n = add_item(it, n, "Copy Link Address", NULL, ACT_LINK_COPY, TRUE);
        }
        if (app->ctx_image) {
            if (n)
                n = add_item(it, n, NULL, NULL, ACT_NONE, FALSE);
            n = add_item(it, n, "Open Image in New Tab", NULL, ACT_IMAGE_TAB, TRUE);
            n = add_item(it, n, "Save Image As...", NULL, ACT_IMAGE_SAVE, TRUE);
            n = add_item(it, n, "Copy Image Address", NULL, ACT_IMAGE_COPY, TRUE);
        }
        if (app->ctx_selection || app->ctx_editable) {
            if (n)
                n = add_item(it, n, NULL, NULL, ACT_NONE, FALSE);
            n = add_item(it, n, "Copy", NULL, ACT_COPY, app->ctx_selection);
            if (app->ctx_editable)
                n = add_item(it, n, "Paste", NULL, ACT_PASTE, TRUE);
        }
        if (!n) {
            n = add_item(it, n, "Back", NULL, ACT_BACK, webkit_web_view_can_go_back(app->web_view));
            n = add_item(it, n, "Forward", NULL, ACT_FORWARD, webkit_web_view_can_go_forward(app->web_view));
            n = add_item(it, n, loading ? "Stop" : "Reload", NULL, loading ? ACT_STOP : ACT_RELOAD, TRUE);
            n = add_item(it, n, NULL, NULL, ACT_NONE, FALSE);
            n = add_item(it, n, "Save Page As...", NULL, ACT_SAVE_PAGE, webkit_web_view_get_uri(app->web_view) != NULL);
            n = add_item(it, n, "Select All", NULL, ACT_SELECT_ALL, TRUE);
        }
        break;
    }
    return n;
}

static int item_top(App *app, int i)
{
    int y = 3, k;
    for (k = 0; k < i; k++)
        y += app->popup_items[k].label ? app->popup_line : 7;
    return y;
}

static int item_at(App *app, int x, int y)
{
    int i;
    if (x < 0 || x >= app->popup_width)
        return -1;
    for (i = 0; i < app->n_popup_items; i++) {
        int top = item_top(app, i), h = app->popup_items[i].label ? app->popup_line : 7;
        if (y >= top && y < top + h)
            return app->popup_items[i].label && app->popup_items[i].enabled ? i : -1;
    }
    return -1;
}

static void draw_popup(App *app)
{
    XRectangle frame;
    int i;
    if (!app->popup)
        return;
    XSetForeground(app->dpy, app->gc, app->bar_bg);
    XFillRectangle(app->dpy, app->popup, app->gc, 0, 0, app->popup_width, app->popup_height);
    for (i = 0; i < app->n_popup_items; i++) {
        MenuItem *m = &app->popup_items[i];
        int top = item_top(app, i);
        if (!m->label) {
            XSetForeground(app->dpy, app->gc, app->bar_shadow);
            XDrawLine(app->dpy, app->popup, app->gc, 6, top + 3, app->popup_width - 7, top + 3);
            XSetForeground(app->dpy, app->gc, app->bar_light);
            XDrawLine(app->dpy, app->popup, app->gc, 6, top + 4, app->popup_width - 7, top + 4);
            continue;
        }
        if (i == app->popup_hover) {
            XSetForeground(app->dpy, app->gc, app->selection);
            XFillRectangle(app->dpy, app->popup, app->gc, 3, top, app->popup_width - 6, app->popup_line);
        }
        XSetForeground(app->dpy, app->gc, m->enabled ? app->ink : app->ink_disabled);
        if (app->font) {
            int base = top + (app->popup_line + app->font->ascent - app->font->descent) / 2;
            XDrawString(app->dpy, app->popup, app->gc, 14, base, m->label, (int)strlen(m->label));
            if (m->accel)
                XDrawString(app->dpy, app->popup, app->gc,
                    app->popup_width - 12 - XTextWidth(app->font, m->accel, (int)strlen(m->accel)), base,
                    m->accel, (int)strlen(m->accel));
        }
    }
    frame.x = frame.y = 0;
    frame.width = app->popup_width;
    frame.height = app->popup_height;
    /* bevel draws on app->win: the popup's own frame here */
    XSetForeground(app->dpy, app->gc, app->bar_light);
    XDrawLine(app->dpy, app->popup, app->gc, 0, 0, frame.width - 1, 0);
    XDrawLine(app->dpy, app->popup, app->gc, 0, 0, 0, frame.height - 1);
    XSetForeground(app->dpy, app->gc, app->bar_shadow);
    XDrawLine(app->dpy, app->popup, app->gc, 0, frame.height - 1, frame.width - 1, frame.height - 1);
    XDrawLine(app->dpy, app->popup, app->gc, frame.width - 1, 0, frame.width - 1, frame.height - 1);
}

static void close_popup(App *app)
{
    if (!app->popup || !app->n_popup_items)
        return;
    XUngrabPointer(app->dpy, CurrentTime);
    XUngrabKeyboard(app->dpy, CurrentTime);
    XUnmapWindow(app->dpy, app->popup);
    app->n_popup_items = 0;
    app->popup_menu = -1;
    app->popup_hover = -1;
    draw_menubar(app);
    XFlush(app->dpy);
}

/* menu >= 0: that menu of the bar, under its title; -1: the context menu at the pointer */
static void open_popup(App *app, int menu, int x, int y, Time time)
{
    int i, w = 0, sw, sh;
    XSetWindowAttributes a;
    if (app->n_popup_items)
        close_popup(app);
    app->n_popup_items = build_menu(app, menu, app->popup_items);
    if (!app->n_popup_items)
        return;
    app->popup_menu = menu;
    app->popup_hover = -1;
    app->popup_time = time;
    app->popup_armed = FALSE;
    app->popup_line = (app->font ? app->font->ascent + app->font->descent : 12) + 8;
    for (i = 0; i < app->n_popup_items; i++) {
        MenuItem *m = &app->popup_items[i];
        int lw;
        if (!m->label || !app->font)
            continue;
        lw = XTextWidth(app->font, m->label, (int)strlen(m->label)) + (m->accel ? XTextWidth(app->font, m->accel, (int)strlen(m->accel)) + 30 : 0);
        if (lw > w)
            w = lw;
    }
    app->popup_width = w + 30;
    app->popup_height = item_top(app, app->n_popup_items) + 3;
    sw = DisplayWidth(app->dpy, DefaultScreen(app->dpy));
    sh = DisplayHeight(app->dpy, DefaultScreen(app->dpy));
    if (x + app->popup_width > sw)
        x = sw - app->popup_width;
    if (y + app->popup_height > sh)
        y = y - app->popup_height > 0 ? y - app->popup_height : sh - app->popup_height;
    if (!app->popup) {
        a.override_redirect = True;
        a.background_pixel = app->bar_bg;
        a.border_pixel = 0;
        a.colormap = DefaultColormap(app->dpy, DefaultScreen(app->dpy));
        a.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask;
        {
            XWindowAttributes wa;
            XGetWindowAttributes(app->dpy, app->win, &wa);
            a.colormap = wa.colormap;
        }
        app->popup = XCreateWindow(app->dpy, RootWindow(app->dpy, DefaultScreen(app->dpy)), x, y,
            (unsigned)app->popup_width, (unsigned)app->popup_height, 0, app->depth, InputOutput, app->visual,
            CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWColormap | CWEventMask, &a);
    } else
        XMoveResizeWindow(app->dpy, app->popup, x, y, (unsigned)app->popup_width, (unsigned)app->popup_height);
    XMapRaised(app->dpy, app->popup);
    XGrabPointer(app->dpy, app->popup, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
        GrabModeAsync, GrabModeAsync, None, app->arrow, CurrentTime);
    XGrabKeyboard(app->dpy, app->popup, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    draw_menubar(app);
    draw_popup(app);
    XFlush(app->dpy);
}

static void open_bar_menu(App *app, int menu, Time time)
{
    Window child;
    int rx, ry;
    XTranslateCoordinates(app->dpy, app->win, RootWindow(app->dpy, DefaultScreen(app->dpy)),
        menu_title_x(app, menu) - 6, MENU_HEIGHT - 1, &rx, &ry, &child);
    open_popup(app, menu, rx, ry, time);
}

static gboolean context_menu(WebKitWebView *web_view, WebKitContextMenu *menu, WebKitHitTestResult *hit, Tab *tab)
{
    App *app = tab->app;
    (void)web_view;
    (void)menu;
    if (tab != app->tab)
        return TRUE;
    g_free(app->ctx_link);
    g_free(app->ctx_image);
    app->ctx_link = webkit_hit_test_result_context_is_link(hit) ? g_strdup(webkit_hit_test_result_get_link_uri(hit)) : NULL;
    app->ctx_image = webkit_hit_test_result_context_is_image(hit) ? g_strdup(webkit_hit_test_result_get_image_uri(hit)) : NULL;
    app->ctx_selection = webkit_hit_test_result_context_is_selection(hit);
    app->ctx_editable = webkit_hit_test_result_context_is_editable(hit);
    open_popup(app, -1, app->pointer_x, app->pointer_y, CurrentTime);
    return TRUE; /* ours, not WebKit's (WPE has none to show) */
}

static double zoom_step(double z, int d)
{
    static const double levels[] = { 0.5, 0.67, 0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0 };
    int i, n = (int)(sizeof levels / sizeof levels[0]);
    if (d == 0)
        return 1.0;
    if (d > 0) {
        for (i = 0; i < n; i++)
            if (levels[i] > z + 0.01)
                return levels[i];
        return levels[n - 1];
    }
    for (i = n - 1; i >= 0; i--)
        if (levels[i] < z - 0.01)
            return levels[i];
    return levels[0];
}

static void zoom(App *app, int d)
{
    double z = zoom_step(webkit_web_view_get_zoom_level(app->web_view), d);
    char text[32];
    webkit_web_view_set_zoom_level(app->web_view, z);
    g_snprintf(text, sizeof text, "Zoom %d%%", (int)(z * 100 + 0.5));
    show_message(app, text, 2);
}

static void save_uri(App *app, const char *uri)
{
    WebKitDownload *d;
    if (!uri || !*uri)
        return;
    g_printerr("heliotrope: save %s\n", uri);
    d = webkit_web_view_download_uri(app->web_view, uri);
    if (d)
        g_object_unref(d);
}

static const char about_html[] =
    "<html><head><title>About Heliotrope</title></head>"
    "<body style='font-family:sans-serif;margin:3em;max-width:40em'>"
    "<h1 style='color:#6a3d9a'>Heliotrope</h1>"
    "<p>A small web browser for IRIX, on WPE WebKit " G_STRINGIFY(WEBKIT_MAJOR_VERSION) "."
    G_STRINGIFY(WEBKIT_MINOR_VERSION) "." G_STRINGIFY(WEBKIT_MICRO_VERSION) ".</p>"
    "<p>Ctrl+T new tab, Ctrl+W close it, Ctrl+PgUp/PgDn between tabs; Ctrl+L an address; "
    "Ctrl+F find; Ctrl+S save the page; the right mouse button for a link's menu; "
    "the middle button opens a link in a new tab.</p></body></html>";

static void do_action(App *app, int action)
{
    switch (action) {
    case ACT_NEW_TAB:
        open_tab(app, NULL, TRUE);
        webkit_web_view_load_uri(app->web_view, "about:blank");
        start_prompt(app, PROMPT_ADDRESS, "");
        break;
    case ACT_NEW_WINDOW:
        spawn_window(app, NULL);
        break;
    case ACT_OPEN_LOCATION:
        start_prompt(app, PROMPT_ADDRESS, webkit_web_view_get_uri(app->web_view));
        app->all_selected = TRUE;
        draw_bar(app);
        break;
    case ACT_SAVE_PAGE:
        save_uri(app, webkit_web_view_get_uri(app->web_view));
        break;
    case ACT_CLOSE_TAB:
        close_tab(app, app->tab);
        break;
    case ACT_EXIT:
        g_main_loop_quit(app->loop);
        break;
    case ACT_COPY:
        webkit_web_view_execute_editing_command(app->web_view, WEBKIT_EDITING_COMMAND_COPY);
        g_timeout_add(200, export_clipboard, app);
        break;
    case ACT_PASTE:
        paste_into_page(app);
        break;
    case ACT_SELECT_ALL:
        webkit_web_view_execute_editing_command(app->web_view, WEBKIT_EDITING_COMMAND_SELECT_ALL);
        break;
    case ACT_FIND:
        start_prompt(app, PROMPT_FIND, app->find_text ? app->find_text : "");
        app->all_selected = app->find_text != NULL;
        draw_bar(app);
        break;
    case ACT_FIND_AGAIN:
        if (app->find_text)
            webkit_find_controller_search_next(webkit_web_view_get_find_controller(app->web_view));
        break;
    case ACT_RELOAD:
        webkit_web_view_reload(app->web_view);
        break;
    case ACT_STOP:
        webkit_web_view_stop_loading(app->web_view);
        break;
    case ACT_ZOOM_IN:
        zoom(app, 1);
        break;
    case ACT_ZOOM_OUT:
        zoom(app, -1);
        break;
    case ACT_ZOOM_RESET:
        zoom(app, 0);
        break;
    case ACT_TOGGLE_JS:
        toggle_site(app, SITE_NO_JS);
        break;
    case ACT_TOGGLE_BLOCK:
        toggle_site(app, SITE_NO_BLOCKING);
        break;
    case ACT_BACK:
        webkit_web_view_go_back(app->web_view);
        break;
    case ACT_FORWARD:
        webkit_web_view_go_forward(app->web_view);
        break;
    case ACT_HOME:
        load_address(app, app->home);
        break;
    case ACT_PREV_TAB:
        step_tab(app, -1);
        break;
    case ACT_NEXT_TAB:
        step_tab(app, 1);
        break;
    case ACT_ABOUT:
        open_tab(app, NULL, TRUE);
        webkit_web_view_load_html(app->web_view, about_html, NULL);
        break;
    case ACT_LINK_OPEN:
        if (app->ctx_link)
            webkit_web_view_load_uri(app->web_view, app->ctx_link);
        break;
    case ACT_LINK_TAB:
        if (app->ctx_link) {
            open_tab(app, app->ctx_link, FALSE);
            show_message(app, "Opened in a new tab", 3);
        }
        break;
    case ACT_LINK_WINDOW:
        spawn_window(app, app->ctx_link);
        break;
    case ACT_LINK_SAVE:
        save_uri(app, app->ctx_link);
        break;
    case ACT_LINK_COPY:
        if (app->ctx_link)
            own_selection(app, app->ctx_link);
        break;
    case ACT_IMAGE_TAB:
        if (app->ctx_image) {
            open_tab(app, app->ctx_image, FALSE);
            show_message(app, "Opened in a new tab", 3);
        }
        break;
    case ACT_IMAGE_SAVE:
        save_uri(app, app->ctx_image);
        break;
    case ACT_IMAGE_COPY:
        if (app->ctx_image)
            own_selection(app, app->ctx_image);
        break;
    }
    XFlush(app->dpy);
}

static void move_hover(App *app, int d)
{
    /* none yet: Down starts at the first line, Up at the last */
    int i = app->popup_hover >= 0 ? app->popup_hover : d > 0 ? -1 : app->n_popup_items, k;
    for (k = 0; k < app->n_popup_items; k++) {
        i = (i + d + app->n_popup_items) % app->n_popup_items;
        if (app->popup_items[i].label && app->popup_items[i].enabled) {
            app->popup_hover = i;
            draw_popup(app);
            return;
        }
    }
}

/* An event while a menu is open (the pointer and keyboard are ours). True if it was the menu's. */
static gboolean handle_popup_event(App *app, XEvent *ev)
{
    if (!app->n_popup_items)
        return FALSE;
    switch (ev->type) {
    case Expose:
        if (ev->xexpose.window != app->popup)
            return FALSE;
        if (!ev->xexpose.count)
            draw_popup(app);
        return TRUE;
    case MotionNotify: {
        XMotionEvent *m = &ev->xmotion;
        if (m->window == app->popup) {
            int i = item_at(app, m->x, m->y);
            if (i != app->popup_hover) {
                app->popup_hover = i;
                if (i >= 0)
                    app->popup_armed = TRUE;
                draw_popup(app);
            }
        } else if (m->window == app->win && app->popup_menu >= 0 && m->y < MENU_HEIGHT) {
            /* along the bar: the menu under the pointer */
            int i = menu_at(app, m->x);
            if (i >= 0 && i != app->popup_menu)
                open_bar_menu(app, i, m->time);
        }
        return TRUE;
    }
    case ButtonPress: {
        XButtonEvent *b = &ev->xbutton;
        if (b->window == app->popup && b->x >= 0 && b->y >= 0 && b->x < app->popup_width && b->y < app->popup_height) {
            app->popup_armed = TRUE;
            return TRUE;
        }
        /* outside: the menu goes (a press on another title of the bar opens that one) */
        if (b->window == app->win && b->y < MENU_HEIGHT && b->y >= 0) {
            int i = menu_at(app, b->x);
            if (i >= 0 && i != app->popup_menu) {
                open_bar_menu(app, i, b->time);
                return TRUE;
            }
        }
        close_popup(app);
        return TRUE;
    }
    case ButtonRelease: {
        XButtonEvent *b = &ev->xbutton;
        int i = b->window == app->popup ? item_at(app, b->x, b->y) : -1;
        /* the release of the press that opened it chooses only after a move onto a line */
        if (i >= 0 && (app->popup_armed || b->time - app->popup_time > 400)) {
            int action = app->popup_items[i].action;
            close_popup(app);
            do_action(app, action);
        }
        return TRUE;
    }
    case KeyPress: {
        KeySym sym = XLookupKeysym(&ev->xkey, 0);
        if (sym == XK_Escape)
            close_popup(app);
        else if (sym == XK_Down)
            move_hover(app, 1);
        else if (sym == XK_Up)
            move_hover(app, -1);
        else if ((sym == XK_Left || sym == XK_Right) && app->popup_menu >= 0)
            open_bar_menu(app, (app->popup_menu + (sym == XK_Right ? 1 : N_MENUS - 1)) % N_MENUS, ev->xkey.time);
        else if ((sym == XK_Return || sym == XK_KP_Enter) && app->popup_hover >= 0) {
            int action = app->popup_items[app->popup_hover].action;
            close_popup(app);
            do_action(app, action);
        }
        return TRUE;
    }
    case KeyRelease:
        return TRUE;
    }
    return FALSE;
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
    if (state & ControlMask) {
        int action = ACT_NONE;
        switch (sym) {
        case XK_t: case XK_T: action = ACT_NEW_TAB; break;
        case XK_n: case XK_N: action = ACT_NEW_WINDOW; break;
        case XK_w: case XK_W: action = ACT_CLOSE_TAB; break;
        case XK_s: case XK_S: action = ACT_SAVE_PAGE; break;
        case XK_f: case XK_F: action = ACT_FIND; break;
        case XK_g: case XK_G: action = ACT_FIND_AGAIN; break;
        case XK_Prior: action = ACT_PREV_TAB; break;
        case XK_Next: action = ACT_NEXT_TAB; break;
        case XK_Tab: action = state & ShiftMask ? ACT_PREV_TAB : ACT_NEXT_TAB; break;
        case XK_ISO_Left_Tab: action = ACT_PREV_TAB; break;
        case XK_plus: case XK_equal: case XK_KP_Add: action = ACT_ZOOM_IN; break;
        case XK_minus: case XK_KP_Subtract: action = ACT_ZOOM_OUT; break;
        case XK_0: case XK_KP_0: action = ACT_ZOOM_RESET; break;
        }
        if (action == ACT_CLOSE_TAB && app->tabs->len <= 1)
            return TRUE;
        if (action != ACT_NONE) {
            if (app->editing && action != ACT_FIND)
                stop_editing(app);
            do_action(app, action);
            return TRUE;
        }
    }
    if (sym == XK_Escape && !app->editing && webkit_web_view_is_loading(app->web_view)) {
        webkit_web_view_stop_loading(app->web_view);
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
    if (handle_popup_event(app, ev))
        return;
    switch (ev->type) {
    case Expose:
        if (ev->xexpose.window == app->win && !ev->xexpose.count)
            redraw(app);
        break;
    case ConfigureNotify:
        if (ev->xconfigure.window == app->win
            && (ev->xconfigure.width != app->width || ev->xconfigure.height != app->height)) {
            app->width = ev->xconfigure.width;
            app->height = ev->xconfigure.height;
            layout_page(app);
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
        int top = page_top(app);
        if (b->y < MENU_HEIGHT) {
            int i = menu_at(app, b->x);
            if (ev->type == ButtonPress && b->button == Button1 && i >= 0) {
                if (app->editing)
                    stop_editing(app);
                open_bar_menu(app, i, b->time);
            }
            break;
        }
        if (b->y < MENU_HEIGHT + BAR_HEIGHT) {
            handle_toolbar_button(app, b);
            break;
        }
        if (b->y < top) {
            handle_tab_strip(app, b);
            break;
        }
        if (ev->type == ButtonPress) {
            /* where a context menu goes, if this press brings one */
            app->pointer_x = b->x_root;
            app->pointer_y = b->y_root;
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
                modifiers_of(b->state), dx * 40, dy * 40, FALSE, FALSE, b->x, b->y - top));
            break;
        }
        send(app, wpe_event_pointer_button_new(ev->type == ButtonPress ? WPE_EVENT_POINTER_DOWN : WPE_EVENT_POINTER_UP,
            app->view, WPE_INPUT_SOURCE_MOUSE, (guint32)b->time, modifiers_of(b->state),
            b->button, b->x, b->y - top, ev->type == ButtonPress ? 1 : 0));
        break;
    }
    case MotionNotify: {
        XEvent latest = *ev;
        XMotionEvent *m = &latest.xmotion;
        /* only the latest position matters */
        while (XCheckTypedWindowEvent(app->dpy, app->win, MotionNotify, &latest))
            ;
        if (m->y < page_top(app)) {
            XRectangle r;
            field_rect(app, &r);
            set_cursor(app, in_rect(&r, m->x, m->y) ? app->text : app->arrow);
            break;
        }
        send(app, wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, app->view, WPE_INPUT_SOURCE_MOUSE,
            (guint32)m->time, modifiers_of(m->state), m->x, m->y - page_top(app), 0, 0));
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
        /* Paste: another program's clipboard first (Shift+Insert as well). */
        if (((k->state & ControlMask) && (sym == XK_v || sym == XK_V)) || ((k->state & ShiftMask) && sym == XK_Insert)) {
            if (ev->type == KeyPress)
                paste_into_page(app);
            break;
        }
        /* Copy and cut: what the page copies is offered to other programs too. */
        if (ev->type == KeyPress && (k->state & ControlMask)
            && (sym == XK_c || sym == XK_C || sym == XK_x || sym == XK_X || sym == XK_Insert))
            g_timeout_add(200, export_clipboard, app);
        /* X keysyms are what WPE (XKB) keyvals are. */
        send(app, wpe_event_keyboard_new(ev->type == KeyPress ? WPE_EVENT_KEYBOARD_KEY_DOWN : WPE_EVENT_KEYBOARD_KEY_UP,
            app->view, WPE_INPUT_SOURCE_KEYBOARD, (guint32)k->time, modifiers_of(k->state),
            k->keycode, (guint)sym));
        break;
    }
    case SelectionNotify:
        if (ev->xselection.property == app->paste_property || (ev->xselection.property == None && app->paste_pending))
            page_paste_arrived(app, &ev->xselection);
        else
            selection_arrived(app, &ev->xselection);
        break;
    case SelectionRequest:
        selection_request(app, &ev->xselectionrequest);
        break;
    case SelectionClear:
        if (ev->xselectionclear.selection == app->clipboard)
            g_clear_pointer(&app->clip_text, g_free);
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
    app->paste_property = XInternAtom(app->dpy, "HELIOTROPE_PASTE", False);
    app->targets = XInternAtom(app->dpy, "TARGETS", False);
    app->utf8 = XInternAtom(app->dpy, "UTF8_STRING", False);
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
        app->page_win = XCreateWindow(app->dpy, app->win, 0, page_top(app), page_width(app), page_height(app), 0,
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
    /* libepoxy's own choice (EPOXY_EGL_LIB) first, then pkgsrc's prefix,
     * then the soname wherever rld finds it. */
    lib = dlopen(path && *path ? path : "/usr/pkg/lib/libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!lib)
        lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
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
    app.popup_menu = -1;
    app.popup_hover = -1;
    app.argv0 = argv[0];
    app.tabs = g_ptr_array_new();
    app.waiting_downloads = g_ptr_array_new();
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-g") && i + 1 < argc)
            app.positioned = sscanf(argv[++i], "%dx%d+%d+%d", &app.width, &app.height, &app.x, &app.y) == 4;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc)
            app.shot_path = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc)
            app.shot_delay = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-q"))
            app.quit_after_shot = TRUE;
        else if (!strcmp(argv[i], "-T") && i + 1 < argc) {
            char *text = NULL, **lines;
            GPtrArray *urls = g_ptr_array_new();
            int k;
            if (!g_file_get_contents(argv[++i], &text, NULL, NULL)) {
                g_printerr("heliotrope: cannot read %s\n", argv[i]);
                return 2;
            }
            lines = g_strsplit(text, "\n", -1);
            for (k = 0; lines[k]; k++) {
                char *l = g_strstrip(lines[k]);
                if (*l && *l != '#')
                    g_ptr_array_add(urls, g_strdup(l));
            }
            g_strfreev(lines);
            g_free(text);
            app.tour_n = urls->len;
            g_ptr_array_add(urls, NULL);
            app.tour = (char **)g_ptr_array_free(urls, FALSE);
            if (app.tour_n == 0) {
                g_printerr("heliotrope: %s has no addresses\n", argv[i]);
                return 2;
            }
            url = app.tour[0];
        }
        else if (argv[i][0] == '-') {
            g_printerr("usage: heliotrope [-g WIDTHxHEIGHT[+X+Y]] [-o shot.ppm] [-s seconds] [-q] [-T tour-file] [URL]\n");
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
    /* No accessibility bus: asking the D-Bus session bus for it would
     * start a session bus (dbus-launch) when there is none, and wait up
     * to 30 seconds for an answer, before the first page process. Set
     * the variable to a bus address to have one. */
    g_setenv("WEBKIT_A11Y_BUS_ADDRESS", "", FALSE);
    /* Nor a session bus started for anything else (GLib's settings and
     * proxy modules ask for one): with no bus of the user's, an address
     * that fails at once, where GLib would autolaunch a dbus-daemon that
     * outlives the browser. */
    g_setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/heliotrope-no-session-bus", FALSE);

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
        if (env_on("HELIOTROPE_FAST_TEXT")) {
            /* WPE shapes all text with HarfBuzz unless text-rendering is
             * optimizeSpeed; this measures plain text with the fast path
             * (no kerning; fonts that ask for features are still shaped). */
            WebKitUserStyleSheet *fast = webkit_user_style_sheet_new(
                "* { text-rendering: optimizeSpeed !important; }",
                WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_STYLE_LEVEL_USER, NULL, NULL);
            webkit_user_content_manager_add_style_sheet(app.content_manager, fast);
            webkit_user_style_sheet_unref(fast);
        }
        webkit_memory_pressure_settings_set_memory_limit(pressure, mb);
        webkit_memory_pressure_settings_set_conservative_threshold(pressure, 0.33);
        webkit_memory_pressure_settings_set_strict_threshold(pressure, 0.5);
        webkit_network_session_set_memory_pressure_settings(pressure);
        context = WEBKIT_WEB_CONTEXT(g_object_new(WEBKIT_TYPE_WEB_CONTEXT,
            "memory-pressure-settings", pressure,
            "process-swap-on-cross-site-navigation-enabled", env_on("HELIOTROPE_SITE_ISOLATION"),
            NULL));
        webkit_memory_pressure_settings_free(pressure);
        /* HELIOTROPE_CACHE_MODEL=viewer: no disk cache and the smallest memory caches (for measuring
         * what the network process's cache costs) */
        webkit_web_context_set_cache_model(context, !g_strcmp0(getenv("HELIOTROPE_CACHE_MODEL"), "viewer")
            ? WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER : WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER);
        /* Test mode (HELIOTROPE_PROXY, below): no disk cache, so every run
         * loads everything through the proxy and runs compare. */
        WebKitNetworkSession *test_session = getenv("HELIOTROPE_PROXY") && *getenv("HELIOTROPE_PROXY")
            ? webkit_network_session_new_ephemeral() : NULL;
        app.web_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display,
            "web-context", context, "user-content-manager", app.content_manager,
            test_session ? "network-session" : NULL, test_session, NULL));
        g_clear_object(&test_session);
        g_object_unref(context);
    }
    app.display = display; /* kept: tabs are made with it */
    /* Test mode, for measuring without the network: every load goes through
     * HELIOTROPE_PROXY (tools/replayproxy.py, which records a site once and
     * plays it back) and TLS certificate errors are ignored so the proxy can
     * read HTTPS. Never for ordinary browsing. */
    if (getenv("HELIOTROPE_PROXY") && *getenv("HELIOTROPE_PROXY")) {
        WebKitNetworkSession *session = webkit_web_view_get_network_session(app.web_view);
        WebKitNetworkProxySettings *proxy = webkit_network_proxy_settings_new(getenv("HELIOTROPE_PROXY"), NULL);
        webkit_network_session_set_proxy_settings(session, WEBKIT_NETWORK_PROXY_MODE_CUSTOM, proxy);
        webkit_network_proxy_settings_free(proxy);
        webkit_network_session_set_tls_errors_policy(session, WEBKIT_TLS_ERRORS_POLICY_IGNORE);
        g_printerr("heliotrope: TEST MODE: all loads through %s, TLS errors ignored, no disk cache\n", getenv("HELIOTROPE_PROXY"));
    }
    select_tab(&app, add_tab(&app, app.web_view));
    g_signal_connect(webkit_web_view_get_network_session(app.web_view), "download-started", G_CALLBACK(download_started), &app);
    g_unix_fd_add(ConnectionNumber(app.dpy), G_IO_IN, x_ready, &app);
    load_sites(&app);
    app.pending_url = g_strdup(url);
    if (app.tour) {
        /* the first page waits for the block list as well */
        g_printerr("heliotrope: tour 1/%d go %.3f %s\n", app.tour_n, wall_now(), url);
        app.tour_timer = g_timeout_add_seconds(120, tour_next, &app);
    }
    start_block_list(&app);

    app.loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(app.loop);
    return 0;
}
