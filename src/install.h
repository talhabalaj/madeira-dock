/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 125hz
 * Madeira Converter Exception: see LICENSE-EXCEPTION.md */
#ifndef MADEIRA_DOCK_INSTALL_H
#define MADEIRA_DOCK_INSTALL_H
#include "session.h"
#include "validation.h"
int sh_install(HMODULE module, void *engine, const struct sh_api *api,
               const struct sh_observer *o, int32_t pipe, int32_t user,
               uint32_t appid, const struct dock_client_layout *layout);
#ifdef _WIN64
struct sh_install_job {
    uint32_t app, request;
    void *manager;
    char manifest[4096];
    bool verifying, observed_update;
    uint64_t began;
    struct sh_install_activity activity;
};
int sh_install_begin(HMODULE module, void *engine, const struct sh_observer *o,
                     int32_t pipe, int32_t user, uint32_t app,
                     const struct dock_client_layout *layout, struct sh_install_job *job);
/* -1 pending; 0 complete; positive numbered error. Does not pump or sleep. */
void sh_install_observe(struct sh_install_job *job);
int sh_install_poll(struct sh_install_job *job, const struct sh_observer *o,
                    bool playing, bool user_paused);
bool sh_install_placement(const struct sh_install_job *job, int32_t placement,
                          HMODULE module, const struct dock_client_layout *layout);
#endif
#endif
