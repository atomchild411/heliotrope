/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 atomchild411
 *
 * The licence's full text is in LICENSE.
 */

/*
 * X events: the event loop's source, and turning X input into WebKit events,
 * toolbar presses and keyboard shortcuts.
 */

#ifndef HELIOTROPE_EVENTS_H
#define HELIOTROPE_EVENTS_H

#include "heliotrope.h"

gboolean x_ready(gint fd, GIOCondition condition, gpointer data);

#endif
