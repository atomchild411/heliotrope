/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 atomchild411
 *
 * The licence's full text is in LICENSE.
 */

/*
 * WebKit's frames: copying them into the window, and -o's screenshot.
 */

#ifndef HELIOTROPE_FRAMES_H
#define HELIOTROPE_FRAMES_H

#include "heliotrope.h"

void timing_mark(App *app, const char *what);
void buffer_rendered(WPEView *view, WPEBuffer *buffer, Tab *tab);
gboolean take_shot(gpointer data);

#endif
