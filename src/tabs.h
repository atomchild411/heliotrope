/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 atomchild411
 *
 * The licence's full text is in LICENSE.
 */

/*
 * Tabs: making WebKit views, showing one at a time, the tab strip, and new
 * windows.
 */

#ifndef HELIOTROPE_TABS_H
#define HELIOTROPE_TABS_H

#include "heliotrope.h"

void layout_page(App *app);
Tab *add_tab(App *app, WebKitWebView *web_view);
void select_tab(App *app, Tab *tab);
Tab *open_tab(App *app, const char *uri, gboolean show);
void close_tab(App *app, Tab *tab);
void step_tab(App *app, int d);
void draw_tabs(App *app);
void handle_tab_strip(App *app, XButtonEvent *b);
void spawn_window(App *app, const char *uri);

#endif
