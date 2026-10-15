/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 125hz
 * Madeira Converter Exception: see LICENSE-EXCEPTION.md
 * Pufflet spike: Valve installs content; Dock only requests and observes it. */
#include "install.h"
#include "validation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN64
typedef void *(__thiscall *manager_fn)(void *, int32_t, int32_t);
typedef int32_t (__thiscall *add_library_fn)(void *, const char *);
typedef int32_t (__thiscall *library_path_fn)(void *, int32_t, char *, int32_t);
typedef int32_t (__thiscall *install_fn)(void *, uint32_t, int32_t, bool);
typedef uint32_t (__thiscall *install_state_fn)(void *, uint32_t);
typedef bool (__thiscall *update_info_fn)(void *, uint32_t, void *);

static bool manifest_ready(const char *path, uint32_t appid, uint32_t *state)
{
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    char *text = malloc(1024 * 1024 + 1);
    bool ready = false;
    if (text) {
        size_t size = fread(text, 1, 1024 * 1024 + 1, file);
        if (!ferror(file)) ready = sh_install_manifest_ready(text, size, appid, state);
        free(text);
    }
    fclose(file);
    return ready;
}

int sh_install(HMODULE module, void *engine, const struct sh_api *api,
               const struct sh_observer *o, int32_t pipe, int32_t user,
               uint32_t appid, const struct dock_client_layout *layout)
{
    if (!dock_method_is((uintptr_t)module, engine, 43, layout->engine_manager)) return 30;
    void *manager = ((manager_fn)(*(void ***)engine)[43])(engine, user, pipe);
    const struct { unsigned slot; uintptr_t rva; } methods[] = {
        {0, layout->install_app}, {4, layout->install_state}, {20, layout->update_info},
        {76, layout->library_path}, {77, layout->add_library}
    };
    for (unsigned i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i) {
        if (!dock_method_is((uintptr_t)module, manager, methods[i].slot, methods[i].rva)) {
            o->event("install-method-mismatch", (int32_t)methods[i].slot);
            return 30;
        }
    }
    o->event("install-private-abi-verified", 1);
    char library[4096], canonical[4096], resolved[4096] = {0}, manifest[4096];
    DWORD size = GetEnvironmentVariableA("MADEIRA_DOCK_INSTALL_LIBRARY", library, sizeof(library));
    if (!size || size >= sizeof(library) || size < 3 || library[1] != ':' ||
        library[2] != '\\' || GetFileAttributesA(library) == INVALID_FILE_ATTRIBUTES) return 61;
    DWORD full = GetFullPathNameA(library, sizeof(canonical), canonical, NULL);
    if (!full || full >= sizeof(canonical)) return 61;
    while (full > 3 && canonical[full - 1] == '\\') canonical[--full] = 0;
    int length = snprintf(manifest, sizeof(manifest), "%s\\steamapps\\appmanifest_%u.acf",
                          canonical, (unsigned)appid);
    if (length < 0 || (size_t)length >= sizeof(manifest)) return 61;
    void **v = *(void ***)manager;
    int32_t folder = ((add_library_fn)v[77])(manager, canonical);
    o->event("install-library-result", folder);
    if (folder < 0) return 61;
    int32_t got = ((library_path_fn)v[76])(manager, folder, resolved, sizeof(resolved));
    if (got <= 0 || got >= (int32_t)sizeof(resolved) || !memchr(resolved, 0, sizeof(resolved)) ||
        _stricmp(canonical, resolved)) return 61;

    int32_t request = ((install_fn)v[0])(manager, appid, folder, false);
    o->event("install-request-result", request);
    if (request) return 62;
    uint64_t begin = o->now_ms(), changed = begin;
    uint32_t previous_state = UINT32_MAX;
    struct sh_install_progress previous = {0};
    while (o->now_ms() - begin < 6ULL * 60 * 60 * 1000) {
        for (unsigned batch = 0; batch < 64; ++batch) {
            struct sh_callback cb = {0};
            if (!api->get_callback(pipe, &cb)) break;
            bool valid = cb.id > 0 && cb.size >= 0 && (!cb.size || cb.data);
            api->free_callback(pipe);
            if (!valid) return SH_CALLBACK_INVALID;
        }
        if (!api->logged_on(user, pipe)) {
            o->event("install-online-lost", 1);
            return 64;
        }
        uint32_t state = ((install_state_fn)v[4])(manager, appid);
        if (state != previous_state) {
            o->event("install-state", (int32_t)state);
            previous_state = state; changed = o->now_ms();
        }
        unsigned char info[244] = {0};
        if (((update_info_fn)v[20])(manager, appid, info)) {
            struct sh_install_progress progress = {0};
            if (!sh_install_decode_progress(info, sizeof(info), &progress)) return 65;
            if (progress.state != previous.state || progress.completed != previous.completed ||
                progress.total != previous.total) {
                o->event("install-update-state", (int32_t)progress.state);
                o->event("install-work-done-kib", sh_install_kib(progress.completed));
                o->event("install-work-total-kib", sh_install_kib(progress.total));
                previous = progress; changed = o->now_ms();
            }
        }
        uint32_t flags = 0;
        if (state == 4 && manifest_ready(manifest, appid, &flags)) {
            o->event("install-manifest-state", (int32_t)flags);
            o->event("install-complete", 1);
            return 0;
        }
        /* shortcut: the spike reports a bounded stall rather than decoding the
         * remaining undocumented update/error fields; extend after a proven install. */
        if (o->now_ms() - changed >= 120000) {
            o->event("install-stalled", 1);
            return 63;
        }
        o->sleep_ms(1000);
    }
    o->event("install-timeout", 1);
    return 63;
}

int sh_install_begin(HMODULE module, void *engine, const struct sh_observer *o,
                     int32_t pipe, int32_t user, uint32_t appid,
                     const struct dock_client_layout *layout, struct sh_install_job *job)
{
    if (!dock_method_is((uintptr_t)module, engine, 43, layout->engine_manager)) return 30;
    void *manager = ((manager_fn)(*(void ***)engine)[43])(engine, user, pipe);
    const struct { unsigned slot; uintptr_t rva; } methods[] = {
        {0, layout->install_app}, {4, layout->install_state}, {20, layout->update_info},
        {76, layout->library_path}, {77, layout->add_library}, {40, layout->downloading_enabled}
    };
    for (unsigned i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i) {
        if (!dock_method_is((uintptr_t)module, manager, methods[i].slot, methods[i].rva)) {
            o->event("install-method-mismatch", (int32_t)methods[i].slot);
            return 30;
        }
    }
    o->event("install-private-abi-verified", 1);
    char library[4096], canonical[4096], resolved[4096] = {0}, manifest[4096];
    DWORD size = GetEnvironmentVariableA("MADEIRA_DOCK_INSTALL_LIBRARY", library, sizeof(library));
    if (!size || size >= sizeof(library) || size < 3 || library[1] != ':' ||
        library[2] != '\\' || GetFileAttributesA(library) == INVALID_FILE_ATTRIBUTES) return 61;
    DWORD full = GetFullPathNameA(library, sizeof(canonical), canonical, NULL);
    if (!full || full >= sizeof(canonical)) return 61;
    while (full > 3 && canonical[full - 1] == '\\') canonical[--full] = 0;
    int length = snprintf(manifest, sizeof(manifest), "%s\\steamapps\\appmanifest_%u.acf",
                          canonical, (unsigned)appid);
    if (length < 0 || (size_t)length >= sizeof(manifest)) return 61;
    void **v = *(void ***)manager;
    int32_t folder = ((add_library_fn)v[77])(manager, canonical);
    o->event("install-library-result", folder);
    if (folder < 0) return 61;
    int32_t got = ((library_path_fn)v[76])(manager, folder, resolved, sizeof(resolved));
    if (got <= 0 || got >= (int32_t)sizeof(resolved) || !memchr(resolved, 0, sizeof(resolved)) ||
        _stricmp(canonical, resolved)) return 61;

    uint32_t initial = ((install_state_fn)v[4])(manager, appid);
    int32_t request = ((install_fn)v[0])(manager, appid, folder, false);
    o->event("install-request-result", request);
    if (request) return 62;

    job->app = appid; job->manager = manager;
    strcpy(job->manifest, manifest);
    job->began = o->now_ms();
    if (initial == 4) {
        typedef bool (__thiscall *validate_fn)(void *, uint32_t);
        if (!dock_method_is((uintptr_t)module, manager, 27, layout->validate_app) ||
            !((validate_fn)v[27])(manager, appid)) return 69;
        job->verifying = true;
        o->event("install-verifying", 1);
    }
    return 0;
}

bool sh_install_placement(const struct sh_install_job *job, int32_t placement,
                          HMODULE module, const struct dock_client_layout *layout)
{
    typedef bool (__thiscall *placement_fn)(void *, uint32_t, int32_t);
    if (!dock_method_is((uintptr_t)module, job->manager, 56, layout->queue_placement)) return false;
    return ((placement_fn)(*(void ***)job->manager)[56])(job->manager, job->app, placement);
}

void sh_install_observe(struct sh_install_job *job)
{
    if (job->verifying && !job->observed_update) {
        void **v = *(void ***)job->manager;
        job->observed_update = ((install_state_fn)v[4])(job->manager, job->app) != 4;
    }
}

int sh_install_poll(struct sh_install_job *job, const struct sh_observer *o,
                    bool playing, bool user_paused)
{
    void **v = *(void ***)job->manager;
    uint32_t state = ((install_state_fn)v[4])(job->manager, job->app);
    if (state != 4) job->observed_update = true;
    o->event("install-state", (int32_t)state);
    o->event("install-paused", (state & 512) != 0);
    typedef bool (__thiscall *enabled_fn)(void *);
    bool enabled = ((enabled_fn)v[40])(job->manager);
    o->event("install-paused-playing", sh_downloads_paused_while_playing(playing, user_paused, enabled, state));
    unsigned char info[244] = {0};
    if (((update_info_fn)v[20])(job->manager, job->app, info)) {
        struct sh_install_progress progress = {0};
        if (!sh_install_decode_progress(info, sizeof(info), &progress)) return 65;
        o->event("install-update-state", (int32_t)progress.state);
        o->event("install-work-done-kib", sh_install_kib(progress.completed));
        o->event("install-work-total-kib", sh_install_kib(progress.total));
    }
    uint32_t flags = 0;
    if (state == 4 && (!job->verifying || job->observed_update) && manifest_ready(job->manifest, job->app, &flags)) {
        o->event("install-manifest-state", (int32_t)flags);
        o->event("install-complete", 1);
        return 0;
    }
    if (job->verifying && !job->observed_update && o->now_ms() - job->began >= 120000) return 69;
    return -1;
}
#endif
