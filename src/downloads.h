/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 atomchild411
 *
 * The licence's full text is in LICENSE.
 */

/*
 * Downloads: where files go (~/Downloads), their progress, and which
 * responses become downloads.
 */

#ifndef HELIOTROPE_DOWNLOADS_H
#define HELIOTROPE_DOWNLOADS_H

#include "heliotrope.h"

void download_started(WebKitNetworkSession *session, WebKitDownload *download, App *app);
void destination_chosen(App *app, const char *path);
gboolean decide_policy(WebKitWebView *web_view, WebKitPolicyDecision *decision, WebKitPolicyDecisionType type, Tab *tab);

#endif
