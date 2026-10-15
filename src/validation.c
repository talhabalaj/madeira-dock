/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 125hz
 * Madeira Converter Exception: see LICENSE-EXCEPTION.md
 * ml1820: validation shared by the Windows host and native sanitizer tests.
 */
#include "validation.h"
#include <string.h>
#include <stdio.h>

bool sh_parse_launch_option(const char *text, uint32_t *option)
{
    if (!text || !*text || !option) return false;
    uint32_t value = 0;
    for (unsigned i = 0; text[i]; ++i) {
        if (i >= 10 || text[i] < '0' || text[i] > '9') return false;
        unsigned digit = (unsigned)(text[i] - '0');
        if (value > ((uint32_t)INT32_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *option = value;
    return true;
}

bool sh_launch_option_missing(const void *payload, size_t size, uint32_t option)
{
    if (!payload || size != 524) return false;
    const char *detail = (const char *)payload + 12;
    const char *end = memchr(detail, 0, 512);
    if (!end) return false;
    char expected[80];
    int length = snprintf(expected, sizeof(expected), "entry for launch option %u not found", (unsigned)option);
    return length > 0 && (size_t)length < sizeof(expected) && strstr(detail, expected) != NULL;
}
bool sh_subscription_list_contains(const uint32_t *apps, int32_t count,
                                   size_t capacity, uint32_t requested)
{
    if (!apps || count <= 0 || (size_t)count >= capacity || !requested || requested == UINT32_MAX)
        return false;
    for (int32_t i = 0; i < count; ++i) if (apps[i] == requested) return true;
    return false;
}

bool sh_decode_launch_result(const void *payload, size_t size, uint64_t expected_game,
                             int32_t *error)
{
    uint64_t returned_game;
    if (!payload || !error || size != 524) return false;
    memcpy(&returned_game, payload, sizeof(returned_game));
    if (returned_game != expected_game) return false;
    memcpy(error, (const char *)payload+8, sizeof(*error));
    return true;
}

/* ml1970: Valve's client refuses a launch while the app, or an app owning one
 * of its shared depots, still needs content: 17 dependency not ready,
 * 19 update required, 20 still busy. Desktop Steam lets its client finish
 * that update and then starts the game; the host keeps Valve's client alive
 * and asks again. Every other result, including licence and connection
 * failures, still fails closed.
 */
bool sh_launch_error_waits_for_content(int32_t error)
{
    return error == 17 || error == 19 || error == 20;
}

/* ml2011: 22 invalid app config and 23 invalid depot config also come back when
 * Valve's client has not yet received the app's configuration from Steam after
 * signing in (a freshly started client begins with an empty app-info cache).
 * The host asks again for a bounded time; a genuinely broken configuration
 * still fails closed once the bound passes.
 */
bool sh_launch_error_waits_for_config(int32_t error)
{
    return error == 22 || error == 23;
}

/* ml2015: 35, another session is playing on this account. */
bool sh_launch_error_waits_for_session(int32_t error)
{
    return error == 35;
}

/* 10 s, 20 s, then every 30 s: the client schedules its update after the
 * first refusal, so later requests only confirm that it has finished.
 */
uint32_t sh_launch_retry_delay_ms(unsigned attempt)
{
    return attempt == 0 ? 10000 : attempt == 1 ? 20000 : 30000;
}

/* ml1990: Valve's client customizes CustomExecutable files per user. Its
 * RequestCustomBinaries returns an EResult and writes the number of per-file
 * jobs it started; each job then posts its own EResult for this AppID. Only
 * k_EResultOK (1) from every started job counts as success. Busy (10) means
 * the app is currently updating or running and may be asked again. Any
 * other result, including OK with zero jobs, fails closed: without prepared
 * binaries the protected executable cannot start.
 */
static bool decode_ceg(const void *payload, size_t size, size_t expected,
                       uint32_t appid, int32_t *result)
{
    uint32_t returned;
    if (!payload || !result || size != expected || !appid) return false;
    memcpy(&returned, (const char *)payload+4, sizeof(returned));
    if (returned != appid) return false;
    memcpy(result, payload, sizeof(*result));
    return true;
}

bool sh_decode_ceg_job(const void *payload, size_t size, uint32_t appid, int32_t *result)
{
    return decode_ceg(payload, size, 8, appid, result);
}

bool sh_decode_ceg_reply(const void *payload, size_t size, uint32_t appid, int32_t *result)
{
    return decode_ceg(payload, size, 12, appid, result);
}

enum sh_ceg_step sh_ceg_request_step(int32_t result, uint32_t jobs, struct sh_ceg_progress *progress)
{
    if (!progress) return SH_CEG_FAIL;
    if (result == 10) return SH_CEG_RETRY;
    if (result != 1 || !jobs || jobs > SH_CEG_MAX_JOBS) {
        progress->failure = result == 1 ? 0 : result;
        return SH_CEG_FAIL;
    }
    *progress = (struct sh_ceg_progress){jobs, 0, 0};
    return SH_CEG_WAIT;
}

enum sh_ceg_step sh_ceg_record_job(struct sh_ceg_progress *progress, int32_t result)
{
    if (!progress || !progress->expected || progress->finished >= progress->expected)
        return SH_CEG_FAIL;
    if (result != 1) {
        progress->failure = result;
        return SH_CEG_FAIL;
    }
    return ++progress->finished == progress->expected ? SH_CEG_DONE : SH_CEG_WAIT;
}

/* ml2000: Wine's sechost reports a missing \pipe\svcctl listener as
 * RPC_S_SERVER_UNAVAILABLE (1722). Only that means "no service manager": any
 * other failure (for example access denied) is not fixed by starting one.
 * RPC_S_SERVER_TOO_BUSY (1723) can occur while a new manager is starting.
 */
bool sh_scm_error_means_absent(uint32_t error)
{
    return error == 1722;
}

bool sh_scm_error_retryable(uint32_t error)
{
    return error == 1722 || error == 1723;
}

int32_t sh_ceg_scm_result(bool manager_reachable, bool service_registered)
{
    if (!manager_reachable) return SH_CEG_SCM_UNAVAILABLE;
    return service_registered ? 0 : SH_CEG_SERVICE_UNREGISTERED;
}

uint32_t sh_remaining_ms(uint64_t now, uint64_t begin, uint64_t bound)
{
    uint64_t elapsed = now >= begin ? now - begin : 0;
    if (elapsed >= bound) return 0;
    uint64_t left = bound - elapsed;
    return left > UINT32_MAX - 1 ? UINT32_MAX - 1 : (uint32_t)left;
}

uint32_t sh_scm_poll_slice_ms(uint32_t left)
{
    return left < SH_SCM_POLL_MS ? left : SH_SCM_POLL_MS;
}

/* A service counts as stopped only when the manager reports it stopped and
 * its process is gone; a process that outlives the bound is ended by the host.
 */
enum sh_service_stop sh_service_stop_outcome(bool was_running, bool stopped_by_manager,
                                             bool process_gone, bool ended_by_host)
{
    if (!was_running) return SH_SERVICE_NOT_RUNNING;
    if (stopped_by_manager && process_gone) return SH_SERVICE_STOPPED;
    if (ended_by_host) return SH_SERVICE_ENDED_AFTER_BOUND;
    return SH_SERVICE_STOP_FAILED;
}

/* Never re-run the installer over an existing registration or when the
 * service manager answered with an unexpected error (fail closed as -5).
 */
bool sh_should_install_service(bool registered, bool missing, bool install_enabled)
{
    return install_enabled && !registered && missing;
}

int32_t sh_service_install_report(bool file_present, bool finished, uint32_t exit_code)
{
    if (!file_present) return SH_SERVICE_INSTALL_MISSING;
    if (!finished) return SH_SERVICE_INSTALL_TIMEOUT;
    return (int32_t)exit_code;
}

/* ml2014: the install batch's service manager. Never start a second manager:
 * only a definite "no listener" (1722) on the first open spawns one.
 */
bool sh_install_scm_should_spawn(bool enabled, uint32_t first_error)
{
    return enabled && sh_scm_error_means_absent(first_error);
}

enum sh_install_scm sh_install_scm_outcome(bool enabled, uint32_t first_error,
                                           bool spawned, bool reachable)
{
    if (!enabled) return SH_INSTALL_SCM_OFF;
    if (!first_error) return SH_INSTALL_SCM_ALREADY;
    if (!reachable) return SH_INSTALL_SCM_FAILED;
    return spawned ? SH_INSTALL_SCM_STARTED : SH_INSTALL_SCM_ALREADY;
}

const char *sh_install_scm_word(int32_t outcome)
{
    switch (outcome) {
    case SH_INSTALL_SCM_OFF: return "off";
    case SH_INSTALL_SCM_ALREADY: return "already";
    case SH_INSTALL_SCM_STARTED: return "started";
    default: return "failed";
    }
}

/* ACF is Valve's output, but a partial/ambiguous file must never end a download.
 * Read quoted tokens and braces; only AppState's direct numeric fields count. */
static int acf_token(const char **cursor, const char *end, const char **word, size_t *length)
{
    const char *p = *cursor;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    if (p == end) { *cursor = p; return 0; }
    if (*p == '{' || *p == '}') { *cursor = p + 1; return *p == '{' ? 2 : 3; }
    if (*p++ != '"') return -1;
    *word = p;
    while (p < end && *p != '"') {
        if ((unsigned char)*p < 32) return -1;
        if (*p == '\\' && ++p == end) return -1;
        ++p;
    }
    if (p == end) return -1;
    *length = (size_t)(p - *word); *cursor = p + 1;
    return 1;
}

static bool acf_number(const char *word, size_t size, uint32_t *number)
{
    uint32_t value = 0;
    if (!size || size > 10) return false;
    for (size_t i = 0; i < size; ++i) {
        if (word[i] < '0' || word[i] > '9') return false;
        unsigned digit = (unsigned)(word[i] - '0');
        if (value > (UINT32_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *number = value;
    return true;
}

bool sh_install_manifest_ready(const char *text, size_t size, uint32_t appid, uint32_t *state)
{
    if (!text || !state || !appid || size > 1024 * 1024) return false;
    const char *p = text, *end = text + size, *word = NULL;
    size_t length = 0;
    if (acf_token(&p, end, &word, &length) != 1 || length != 8 || memcmp(word, "AppState", 8) ||
        acf_token(&p, end, &word, &length) != 2) return false;
    unsigned depth = 1;
    bool have_app = false, have_state = false;
    uint32_t actual = 0, flags = 0;
    while (depth) {
        int key = acf_token(&p, end, &word, &length);
        if (key == 3) { --depth; continue; }
        if (key != 1) return false;
        bool is_app = depth == 1 && length == 5 && !memcmp(word, "appid", 5);
        bool is_state = depth == 1 && length == 10 && !memcmp(word, "StateFlags", 10);
        int value = acf_token(&p, end, &word, &length);
        if (value == 2) {
            if (is_app || is_state || ++depth > 32) return false;
        } else if (value == 1) {
            if (is_app && (have_app || !acf_number(word, length, &actual))) return false;
            if (is_state && (have_state || !acf_number(word, length, &flags))) return false;
            have_app |= is_app; have_state |= is_state;
        } else return false;
    }
    if (acf_token(&p, end, &word, &length) != 0 || !have_app || !have_state || actual != appid)
        return false;
    *state = flags;
    return flags == 4; /* No pending update, missing/corrupt files or active install. */
}

bool sh_install_decode_progress(const void *data, size_t size, struct sh_install_progress *out)
{
    if (!data || !out || size != 244) return false;
    memcpy(&out->state, (const char *)data + 4, 4);
    memcpy(&out->total, (const char *)data + 60, 8);
    memcpy(&out->completed, (const char *)data + 68, 8);
    return out->completed <= out->total;
}

int32_t sh_install_kib(uint64_t bytes)
{
    uint64_t kib = bytes / 1024;
    return kib > INT32_MAX ? INT32_MAX : (int32_t)kib;
}
