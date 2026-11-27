/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 atomchild411
 *
 * The licence's full text is in LICENSE.
 */

/*
 * The X clipboard (CLIPBOARD and PRIMARY) for the page: copying out of WebKit
 * and pasting into it.
 */

#ifndef HELIOTROPE_CLIPBOARD_H
#define HELIOTROPE_CLIPBOARD_H

#include "heliotrope.h"

void own_selection(App *app, const char *text);
void selection_request(App *app, XSelectionRequestEvent *r);
gboolean export_clipboard(gpointer data);
void paste_into_page(App *app);
void page_paste_arrived(App *app, XSelectionEvent *e);

#endif
