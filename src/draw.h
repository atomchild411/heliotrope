/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 atomchild411
 *
 * The licence's full text is in LICENSE.
 */

/*
 * Drawing Heliotrope's own parts of the window: the toolbar with its buttons
 * and address field, the status line, and the page area when frames arrive in
 * shared memory.
 */

#ifndef HELIOTROPE_DRAW_H
#define HELIOTROPE_DRAW_H

#include "heliotrope.h"

void bevel(App *app, const XRectangle *r, gboolean sunken);
gboolean button_enabled(App *app, int i);
void draw_bar(App *app);
void draw_page(App *app);
void redraw(App *app);

#endif
