/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 atomchild411
 *
 * The licence's full text is in LICENSE.
 */

/*
 * A page's life: loading addresses, WebKit's load, title and state signals,
 * the status line's messages, -T tours and find in page.
 */

#ifndef HELIOTROPE_PAGE_H
#define HELIOTROPE_PAGE_H

#include "heliotrope.h"

double wall_now(void);
gboolean tour_next(gpointer data);
void load_changed(WebKitWebView *web_view, WebKitLoadEvent event, Tab *tab);
gboolean load_failed(WebKitWebView *web_view, WebKitLoadEvent event, char *uri, GError *error, Tab *tab);
void show_title(App *app);
void title_changed(GObject *object, GParamSpec *pspec, Tab *tab);
void state_changed(GObject *object, GParamSpec *pspec, Tab *tab);
void web_process_terminated(WebKitWebView *web_view, WebKitWebProcessTerminationReason reason, Tab *tab);
void mouse_target_changed(WebKitWebView *web_view, WebKitHitTestResult *hit, guint modifiers, Tab *tab);
void load_address(App *app, const char *text);
void show_message(App *app, const char *text, int seconds);
void find_text(App *app, const char *text);

#endif
