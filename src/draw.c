/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 atomchild411
 *
 * The licence's full text is in LICENSE.
 */

/*
 * Drawing Heliotrope's own parts of the window with Xlib: bevelled toolbar
 * buttons and their icons, the address field (with its cursor, selection and
 * load progress), the status line, and the last frame in the page area when
 * WebKit's frames come to us (not in present mode).
 */

#include "heliotrope.h"
#include "draw.h"
#include "menus.h"
#include "sites.h"
#include "tabs.h"
#include "window.h"

void bevel(App *app, const XRectangle *r, gboolean sunken)
{
    XSetForeground(app->dpy, app->gc, sunken ? app->bar_shadow : app->bar_light);
    XDrawLine(app->dpy, app->win, app->gc, r->x, r->y, r->x + r->width - 1, r->y);
    XDrawLine(app->dpy, app->win, app->gc, r->x, r->y, r->x, r->y + r->height - 1);
    XSetForeground(app->dpy, app->gc, sunken ? app->bar_light : app->bar_shadow);
    XDrawLine(app->dpy, app->win, app->gc, r->x, r->y + r->height - 1, r->x + r->width - 1, r->y + r->height - 1);
    XDrawLine(app->dpy, app->win, app->gc, r->x + r->width - 1, r->y, r->x + r->width - 1, r->y + r->height - 1);
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
    }
}

gboolean button_enabled(App *app, int i)
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

void draw_bar(App *app)
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

void draw_page(App *app)
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

void redraw(App *app)
{
    draw_menubar(app);
    draw_bar(app);
    draw_tabs(app);
    draw_page(app);
    XFlush(app->dpy);
}
