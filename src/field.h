/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 atomchild411
 *
 * The licence's full text is in LICENSE.
 */

/*
 * The address field: editing it, and its prompts (an address, a file to save
 * to, text to find).
 */

#ifndef HELIOTROPE_FIELD_H
#define HELIOTROPE_FIELD_H

#include "heliotrope.h"

void start_prompt(App *app, int prompt, const char *text);
void start_editing(App *app);
void stop_editing(App *app);
void field_paste(App *app, Atom selection);
void selection_arrived(App *app, XSelectionEvent *e);
gsize offset_at(App *app, int x);
gboolean handle_field_key(App *app, KeySym sym, const char *text, unsigned int state);

#endif
