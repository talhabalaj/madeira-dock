/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 125hz
 * Madeira Converter Exception: see LICENSE-EXCEPTION.md */
#ifndef MADEIRA_STEAM_HOST_LAUNCH_H
#define MADEIRA_STEAM_HOST_LAUNCH_H
#include "session.h"
int sh_launch(HMODULE module, void *engine, void *client_user,
              const struct sh_api *api, const struct sh_observer *o,
              int32_t pipe, int32_t user, uint64_t steamid, uint32_t appid,
              const struct dock_client_layout *layout);
#ifdef _WIN64
struct sh_launch_job;
int sh_launch_begin(HMODULE module, void *engine, void *client_user,
                    const struct sh_observer *o, int32_t pipe, int32_t user,
                    uint64_t steamid, uint32_t appid, uint32_t option,
                    const wchar_t *expected, bool ceg,
                    const struct dock_client_layout *layout, struct sh_launch_job **out);
void sh_launch_callback(struct sh_launch_job *job, const struct sh_callback *cb);
int sh_launch_poll(struct sh_launch_job *job);
bool sh_launch_running(const struct sh_launch_job *job);
int sh_launch_close(struct sh_launch_job *job);
#endif
#endif
