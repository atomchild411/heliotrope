/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 atomchild411
 *
 * The licence's full text is in LICENSE.
 */

/*
 * Heliotrope's shared types: the window's layout constants, a tab (Tab), a
 * menu line (MenuItem) and the browser's whole state (App), which every
 * module receives as its first argument. Each module's functions are declared
 * in its own header (draw.h, tabs.h, ...).
 *
 * The modules: main.c (start-up), window.c (the X window and its geometry),
 * draw.c (the toolbar, address field and status line), frames.c (WebKit's
 * frames into the window), page.c (a page's loads, titles, status messages
 * and find), field.c (the address field and its prompts), events.c (X events
 * and keys), tabs.c, menus.c (the menu bar, popups and every menu action),
 * sites.c (per-site choices), clipboard.c, downloads.c and bytecode.c
 * (compiled JavaScript kept on disk).
 */

#ifndef HELIOTROPE_H
#define HELIOTROPE_H

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
#include <unistd.h>
#include <sys/stat.h>

#define MENU_HEIGHT 22          /* the menu bar */
#define BAR_HEIGHT 30           /* toolbar */
#define TAB_HEIGHT 24           /* the tab strip, shown when there are two tabs or more */
#define BUTTON_WIDTH 28
#define BUTTON_HEIGHT 24

enum { BUTTON_BACK, BUTTON_FORWARD, BUTTON_RELOAD, BUTTON_HOME, BUTTON_JS, N_BUTTONS };

/* per-site choices, remembered in ~/.config/heliotrope/sites */
enum { SITE_NO_JS = 1 };

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

    /* per-site choices */
    WebKitUserContentManager *content_manager;
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

/* An environment switch is on when set to anything but "" or "0". */
static inline gboolean env_on(const char *name)
{
    const char *v = getenv(name);
    return v && *v && strcmp(v, "0");
}

#endif
