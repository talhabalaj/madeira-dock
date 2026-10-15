/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 125hz
 * Madeira Converter Exception: see LICENSE-EXCEPTION.md */
#include "serve.h"
#include "launch.h"
#include "install.h"
#include "validation.h"
#include "scm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN64
#define COMMAND_CAP 64
#define JOB_CAP 64
static struct {
    CRITICAL_SECTION lock;
    struct sh_command commands[COMMAND_CAP];
    unsigned head, count;
    bool eof, invalid;
} input;
static uint32_t request_tag, app_tag;

static void emit(const char *key, int32_t value)
{
    printf("[steam-host] pufflet2 request-id=%u app-id=%u %s=%ld\n",
           (unsigned)request_tag, (unsigned)app_tag, key, (long)value);
    fflush(stdout);
}

/* This thread never calls Valve or emits user input. Bounded mailbox only. */
static DWORD WINAPI read_commands(void *unused)
{
    (void)unused;
    char line[96]; unsigned size = 0; bool overflow = false;
    HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
    for (;;) {
        char byte; DWORD count = 0;
        if (!ReadFile(handle, &byte, 1, &count, NULL) || !count) break;
        if (byte != '\n') {
            if (size < sizeof(line) - 1 && byte) line[size++] = byte;
            else overflow = true;
            continue;
        }
        line[size] = 0;
        struct sh_command command = {0};
        bool valid = !overflow && sh_parse_command(line, &command);
        EnterCriticalSection(&input.lock);
        if (!valid) input.invalid = true;
        else if (input.count == COMMAND_CAP) { input.invalid = true; input.eof = true; }
        else { input.commands[(input.head + input.count) % COMMAND_CAP] = command; ++input.count; }
        bool stop = input.eof;
        LeaveCriticalSection(&input.lock);
        size = 0; overflow = false;
        if (stop) return 0;
    }
    EnterCriticalSection(&input.lock); input.eof = true; LeaveCriticalSection(&input.lock);
    return 0;
}

/* Sidecar: MDOCKL01\n<appid>\n<option>\n<ceg 0/1>\n<UTF-8 expected folder>\n.
 * Only a private numeric request filename is opened; no input path is echoed. */
static bool launch_metadata(const struct sh_command *cmd, wchar_t *expected, bool *ceg)
{
    wchar_t directory[32768], path[32768];
    DWORD n = GetEnvironmentVariableW(L"MADEIRA_DOCK_REQUEST_DIR", directory, 32768);
    if (!n || n + 32 >= 32768) return false;
    if (swprintf(path, 32768, L"%ls\\%u.launch", directory, (unsigned)cmd->request) < 0) return false;
    HANDLE file = CreateFileW(path, GENERIC_READ | DELETE, 0, NULL, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info = {0};
    char bytes[32768]; DWORD read = 0;
    bool ok = GetFileInformationByHandle(file, &info) && info.nFileSizeHigh == 0 &&
        info.nFileSizeLow > 0 && info.nFileSizeLow < sizeof(bytes) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        ReadFile(file, bytes, info.nFileSizeLow, &read, NULL) && read == info.nFileSizeLow;
    if (!CloseHandle(file)) ok = false;
    if (!ok || memchr(bytes, 0, read)) return false;
    bytes[read] = 0;
    char *fields[5] = {bytes}; unsigned count = 1;
    for (unsigned i = 0; i < read; ++i) if (bytes[i] == '\n') {
        bytes[i] = 0;
        if (i + 1 < read) { if (count == 5) return false; fields[count++] = bytes + i + 1; }
    }
    uint32_t app = 0, option = 0, flag = 0;
    if (count != 5 || strcmp(fields[0], "MDOCKL01") ||
        !sh_parse_launch_option(fields[1], &app) || app != cmd->app ||
        !sh_parse_launch_option(fields[2], &option) || option != cmd->option ||
        !sh_parse_launch_option(fields[3], &flag) || flag > 1 ||
        strlen(fields[4]) < 3 || fields[4][1] != ':' || fields[4][2] != '\\' ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, fields[4], -1, expected, 32768)) return false;
    *ceg = flag == 1;
    return true;
}

static int entitled(void *client_user, uint32_t app)
{
    typedef int32_t (__thiscall *list_fn)(void *, uint32_t *, int32_t, bool);
    uint32_t *apps = calloc(65536, sizeof(uint32_t));
    if (!apps) return 36;
    int32_t count = ((list_fn)(*(void ***)client_user)[182])(client_user, apps, 65536, true);
    bool listed = sh_subscription_list_contains(apps, count, 65536, app);
    free(apps);
    return count < 0 || count >= 65536 ? 36 : listed ? 0 : 35;
}

int sh_serve(HMODULE module, void *engine, void *client_user,
             const struct sh_api *api, const struct sh_observer *o,
             int32_t pipe, int32_t user, uint64_t steamid,
             const struct dock_client_layout *layout)
{
    typedef bool (__thiscall *query_fn)(void *);
    /* Prepare service support before entering the shared callback pump. A
     * service failure refuses CEG launches, not ordinary installs/launches. */
    int scm = sh_ceg_scm_prepare(o, module);
    InitializeCriticalSection(&input.lock);
    HANDLE reader = CreateThread(NULL, 0, read_commands, NULL, 0, NULL);
    if (!reader) return 66;
    struct sh_observer observer = {o->now_ms, o->sleep_ms, emit};
    struct sh_install_job jobs[JOB_CAP] = {0};
    bool paused[JOB_CAP] = {0};
    struct sh_launch_job *launch = NULL;
    uint32_t launch_request = 0, launch_app = 0, last_request = 0;
    uint64_t idle_since = o->now_ms(), last_poll = 0, offline_since = 0;
    int result = 0;
    request_tag = app_tag = 0; emit("session-ready", 1);
    for (;;) {
        uint64_t now = o->now_ms();
        request_tag = launch_request; app_tag = launch_app;
        for (unsigned batch = 0; batch < 64; ++batch) {
            struct sh_callback cb = {0};
            if (!api->get_callback(pipe, &cb)) break;
            bool valid = cb.id > 0 && cb.size >= 0 && (!cb.size || cb.data);
            if (valid && launch) sh_launch_callback(launch, &cb);
            api->free_callback(pipe);
            if (!valid) { result = SH_CALLBACK_INVALID; goto done; }
        }
        bool online = api->logged_on(user, pipe) &&
            ((query_fn)(*(void ***)client_user)[4])(client_user) &&
            ((query_fn)(*(void ***)client_user)[6])(client_user);
        if (online) offline_since = 0;
        else if (!offline_since) offline_since = now;
        else if (now - offline_since >= 1000) { result = 64; goto done; }
        if (launch) {
            int step = sh_launch_poll(launch);
            if (step != -1) {
                int closed = sh_launch_close(launch); launch = NULL;
                emit("launch-host-result", closed ? closed : step);
                if (closed) { result = closed; goto done; }
            }
        }
        bool downloading = false;
        for (unsigned i = 0; i < JOB_CAP; ++i) if (jobs[i].app) {
            downloading = true;
            sh_install_observe(&jobs[i]);
            if (now - last_poll >= 1000) {
                request_tag = jobs[i].request; app_tag = jobs[i].app;
                int step = sh_install_poll(&jobs[i], &observer, sh_launch_running(launch), paused[i]);
                if (step != -1) {
                    emit("install-result", step);
                    jobs[i] = (struct sh_install_job){0};
                }
            }
        }
        if (now - last_poll >= 1000) last_poll = now;
        if (downloading || launch) idle_since = now;
        if (sh_session_idle(now, idle_since, 300000, downloading, launch != NULL)) {
            request_tag = app_tag = 0; emit("session-idle-shutdown", 1); break;
        }
        struct sh_command cmd = {0}; bool eof, invalid;
        EnterCriticalSection(&input.lock);
        if (input.count) { cmd = input.commands[input.head]; input.head = (input.head + 1) % COMMAND_CAP; --input.count; }
        eof = input.eof; invalid = input.invalid; input.invalid = false;
        LeaveCriticalSection(&input.lock);
        request_tag = cmd.request; app_tag = cmd.app;
        if (invalid) { request_tag = app_tag = 0; emit("command-result", 66); }
        if (!cmd.request) { if (eof) break; o->sleep_ms(20); continue; }
        if (cmd.request <= last_request) { emit("command-result", 66); continue; }
        last_request = cmd.request;
        if (cmd.kind == SH_CMD_SHUTDOWN) { emit("command-result", 0); break; }
        if (cmd.kind == SH_CMD_STATUS) {
            emit("session-ready", 1); emit("session-playing", sh_launch_running(launch)); emit("command-result", 0); continue;
        }
        idle_since = now;
        if (!online) { emit("command-result", 64); continue; }
        int authorization = entitled(client_user, cmd.app);
        if (authorization) { emit("command-result", authorization); continue; }
        unsigned index = 0;
        while (index < JOB_CAP && jobs[index].app != cmd.app) ++index;
        if (cmd.kind == SH_CMD_INSTALL) {
            if (index == JOB_CAP) for (index = 0; index < JOB_CAP && jobs[index].app; ++index) {}
            if (index == JOB_CAP) { emit("command-result", 67); continue; }
            if (jobs[index].app) { emit("command-result", 67); continue; }
            int started = sh_install_begin(module, engine, &observer, pipe, user, cmd.app, layout, &jobs[index]);
            if (started) jobs[index] = (struct sh_install_job){0};
            else { jobs[index].request = cmd.request; paused[index] = false; }
            emit("command-result", started);
        } else if (cmd.kind == SH_CMD_LAUNCH) {
            if (launch) { emit("command-result", 67); continue; }
            wchar_t expected[32768]; bool ceg = false;
            int started = !launch_metadata(&cmd, expected, &ceg) ? 41 : ceg && scm ? 49 :
                sh_launch_begin(module, engine, client_user, &observer, pipe, user, steamid,
                    cmd.app, cmd.option, expected, ceg, layout, &launch);
            if (!started) { launch_request = cmd.request; launch_app = cmd.app; }
            emit("command-result", started);
        } else {
            if (index == JOB_CAP) { emit("command-result", 68); continue; }
            int placement = cmd.kind == SH_CMD_RESUME ? 5 : 6;
            bool changed = sh_install_placement(&jobs[index], placement, module, layout);
            if (changed) {
                paused[index] = cmd.kind != SH_CMD_RESUME;
                if (cmd.kind == SH_CMD_CANCEL) jobs[index] = (struct sh_install_job){0};
                emit("install-user-paused", paused[index]);
            }
            emit("command-result", changed ? 0 : 68);
        }
    }
done:
    request_tag = launch_request; app_tag = launch_app;
    if (launch) { int closed = sh_launch_close(launch); if (closed && !result) result = closed; }
    request_tag = app_tag = 0; emit("session-result", result);
    CancelSynchronousIo(reader);
    WaitForSingleObject(reader, 2000);
    CloseHandle(reader);
    /* The static mailbox outlives a reader still unwinding at process exit. */
    return result;
}
#else
int sh_serve(HMODULE module, void *engine, void *client_user,
             const struct sh_api *api, const struct sh_observer *o,
             int32_t pipe, int32_t user, uint64_t steamid,
             const struct dock_client_layout *layout)
{
    (void)module; (void)engine; (void)client_user; (void)api; (void)o;
    (void)pipe; (void)user; (void)steamid; (void)layout;
    return 30;
}
#endif
