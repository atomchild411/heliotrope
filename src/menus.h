/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 atomchild411
 *
 * The licence's full text is in LICENSE.
 */

/*
 * Menus: the menu bar, the popup menus and the page's context menu, and every
 * action they (and the keyboard shortcuts) can take.
 */

#ifndef HELIOTROPE_MENUS_H
#define HELIOTROPE_MENUS_H

#include "heliotrope.h"

enum {
    ACT_NONE, ACT_NEW_TAB, ACT_NEW_WINDOW, ACT_OPEN_LOCATION, ACT_SAVE_PAGE, ACT_CLOSE_TAB, ACT_EXIT,
    ACT_COPY, ACT_PASTE, ACT_SELECT_ALL, ACT_FIND, ACT_FIND_AGAIN,
    ACT_RELOAD, ACT_STOP, ACT_ZOOM_IN, ACT_ZOOM_OUT, ACT_ZOOM_RESET, ACT_TOGGLE_JS,
    ACT_BACK, ACT_FORWARD, ACT_HOME, ACT_PREV_TAB, ACT_NEXT_TAB, ACT_ABOUT,
    ACT_LINK_OPEN, ACT_LINK_TAB, ACT_LINK_WINDOW, ACT_LINK_SAVE, ACT_LINK_COPY,
    ACT_IMAGE_TAB, ACT_IMAGE_SAVE, ACT_IMAGE_COPY
};

int menu_at(App *app, int x);
void draw_menubar(App *app);
void open_bar_menu(App *app, int menu, Time time);
gboolean context_menu(WebKitWebView *web_view, WebKitContextMenu *menu, WebKitHitTestResult *hit, Tab *tab);
void do_action(App *app, int action);
gboolean handle_popup_event(App *app, XEvent *ev);

#endif
