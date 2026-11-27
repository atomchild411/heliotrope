/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 atomchild411
 *
 * The licence's full text is in LICENSE.
 */

/*
 * X events: every event on the display is read here and goes to the open
 * menu, the address field, the toolbar, the tab strip or the page (as WPE
 * events). The browser's keyboard shortcuts are here too.
 */

#include "heliotrope.h"
#include "clipboard.h"
#include "draw.h"
#include "events.h"
#include "field.h"
#include "menus.h"
#include "page.h"
#include "sites.h"
#include "tabs.h"
#include "window.h"

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
            field_paste(app, XA_PRIMARY);
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

gboolean x_ready(gint fd, GIOCondition condition, gpointer data)
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
