/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 atomchild411
 *
 * The licence's full text is in LICENSE.
 */

/*
 * The X window: opening it (visual, colours, fonts, cursors, the present-mode
 * child windows), and the geometry every other module draws and hit-tests
 * with.
 */

#include "heliotrope.h"
#include "window.h"

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

int page_top(App *app)
{
    return MENU_HEIGHT + BAR_HEIGHT + (app->tabs && app->tabs->len > 1 ? TAB_HEIGHT : 0);
}

int page_width(App *app)
{
    return app->width > 0 ? app->width : 1;
}

int page_height(App *app)
{
    return app->height > page_top(app) ? app->height - page_top(app) : 1;
}

void button_rect(int i, XRectangle *r)
{
    r->x = 4 + i * (BUTTON_WIDTH + 2);
    r->y = MENU_HEIGHT + (BAR_HEIGHT - BUTTON_HEIGHT) / 2;
    r->width = BUTTON_WIDTH;
    r->height = BUTTON_HEIGHT;
}

void field_rect(App *app, XRectangle *r)
{
    int x = 4 + N_BUTTONS * (BUTTON_WIDTH + 2) + 6;
    r->x = x;
    r->y = MENU_HEIGHT + (BAR_HEIGHT - BUTTON_HEIGHT) / 2;
    r->width = app->width - x - 6 > 40 ? app->width - x - 6 : 40;
    r->height = BUTTON_HEIGHT;
}

gboolean in_rect(const XRectangle *r, int x, int y)
{
    return x >= r->x && x < r->x + r->width && y >= r->y && y < r->y + r->height;
}

void set_cursor(App *app, Cursor c)
{
    if (c != app->current_cursor) {
        XDefineCursor(app->dpy, app->win, c);
        app->current_cursor = c;
    }
}

static unsigned long color(App *app, int r, int g, int b)
{
    return ((unsigned long)r << app->rshift) | ((unsigned long)g << app->gshift) | ((unsigned long)b << app->bshift);
}

gboolean open_window(App *app)
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
        /* No event mask: input still reaches app->win. White until WebKit's
         * first frame covers it (with no background, whatever the screen
         * held there showed through while the first page loaded). */
        XSetWindowAttributes pa;
        pa.colormap = attrs.colormap;
        pa.border_pixel = 0;
        pa.background_pixmap = None;
        pa.background_pixel = WhitePixel(app->dpy, DefaultScreen(app->dpy));
        app->page_win = XCreateWindow(app->dpy, app->win, 0, page_top(app), page_width(app), page_height(app), 0,
            app->depth, InputOutput, app->visual, CWColormap | CWBorderPixel | CWBackPixel, &pa);
        XMapWindow(app->dpy, app->page_win);
        app->status_win = XCreateWindow(app->dpy, app->win, 0, 0, 1, 1, 0, app->depth, InputOutput, app->visual,
            CWColormap | CWBorderPixel, &pa);
    }
    XMapWindow(app->dpy, app->win);
    XFlush(app->dpy);
    return TRUE;
}
