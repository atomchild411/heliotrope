/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 atomchild411
 *
 * The licence's full text is in LICENSE.
 */

/*
 * Menus: the menu bar (File, Edit, View, Go, Help), the popup each opens, and
 * the page's context menu for a link, an image or the page, all drawn in an
 * override-redirect window. do_action() carries out every menu choice; the
 * keyboard shortcuts in events.c use it too.
 */

#include "heliotrope.h"
#include "clipboard.h"
#include "draw.h"
#include "field.h"
#include "menus.h"
#include "page.h"
#include "sites.h"
#include "tabs.h"

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

int menu_at(App *app, int x)
{
    int i;
    for (i = 0; i < N_MENUS; i++)
        if (x >= menu_title_x(app, i) - 6 && x < menu_title_x(app, i) + menu_title_width(app, i))
            return i;
    return -1;
}

void draw_menubar(App *app)
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

void open_bar_menu(App *app, int menu, Time time)
{
    Window child;
    int rx, ry;
    XTranslateCoordinates(app->dpy, app->win, RootWindow(app->dpy, DefaultScreen(app->dpy)),
        menu_title_x(app, menu) - 6, MENU_HEIGHT - 1, &rx, &ry, &child);
    open_popup(app, menu, rx, ry, time);
}

gboolean context_menu(WebKitWebView *web_view, WebKitContextMenu *menu, WebKitHitTestResult *hit, Tab *tab)
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

void do_action(App *app, int action)
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
gboolean handle_popup_event(App *app, XEvent *ev)
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
