/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 atomchild411
 *
 * The licence's full text is in LICENSE.
 */

/*
 * Per-site choices (JavaScript off for a site), remembered in
 * ~/.config/heliotrope/sites.
 */

#ifndef HELIOTROPE_SITES_H
#define HELIOTROPE_SITES_H

#include "heliotrope.h"

unsigned current_site_flags(App *app);
void load_sites(App *app);
void apply_site_choices(App *app, WebKitWebView *web_view);
void toggle_site(App *app, unsigned flag);

#endif
