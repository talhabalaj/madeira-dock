/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 125hz
 * Madeira Converter Exception: see LICENSE-EXCEPTION.md */
#ifndef MADEIRA_DOCK_SERVE_H
#define MADEIRA_DOCK_SERVE_H
#include "session.h"
int sh_serve(HMODULE module, void *engine, void *client_user,
             const struct sh_api *api, const struct sh_observer *o,
             int32_t pipe, int32_t user, uint64_t steamid,
             const struct dock_client_layout *layout);
#endif
