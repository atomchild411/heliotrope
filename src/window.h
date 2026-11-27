/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 atomchild411
 *
 * The licence's full text is in LICENSE.
 */

/*
 * The X window: opening it, its colours and cursors, and where the page area,
 * toolbar buttons and address field lie in it.
 */

#ifndef HELIOTROPE_WINDOW_H
#define HELIOTROPE_WINDOW_H

#include "heliotrope.h"

int page_top(App *app);
int page_width(App *app);
int page_height(App *app);
void button_rect(int i, XRectangle *r);
void field_rect(App *app, XRectangle *r);
gboolean in_rect(const XRectangle *r, int x, int y);
void set_cursor(App *app, Cursor c);
gboolean open_window(App *app);

#endif
