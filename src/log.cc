// SPDX-License-Identifier: GPL-3.0-or-later

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <NickelHook.h>

#include "log.h"

static const char *DIR_PATH = "/mnt/onboard/.adds/nickelfootnote";
static const char *LOG_PATH = "/mnt/onboard/.adds/nickelfootnote/log.txt";
static const double STARTUP_GRACE_S = 30;
static const int MAX_FEATURES = 8;

static double init_time = -1;

static double monotonic() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

double nfn_uptime() {
    if (init_time < 0)
        init_time = monotonic();
    return monotonic() - init_time;
}

bool nfn_starting() {
    return nfn_uptime() < STARTUP_GRACE_S;
}

static void write_synced(const char *path, int flags, const char *buf, size_t len) {
    int fd = open(path, flags | O_WRONLY | O_CREAT, 0644);
    if (fd < 0)
        return;
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        buf += n;
        len -= n;
    }
    fsync(fd);
    close(fd);
}

void nfn_log(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    nh_log("%s", msg);

    char line[600];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int n = snprintf(line, sizeof(line), "%02d:%02d:%02d +%.1fs %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, nfn_uptime(), msg);
    if (n > 0) {
        mkdir(DIR_PATH, 0755);
        write_synced(LOG_PATH, O_APPEND, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
    }
}

// --- guards --------------------------------------------------------------------

static struct {
    const char *name;
    bool enabled;
} features[MAX_FEATURES];

static void guard_path(char *buf, size_t sz, const char *prefix, const char *feature, const char *suffix) {
    snprintf(buf, sz, "%s/%s%s%s", DIR_PATH, prefix, feature, suffix);
}

void nfn_guard_init(const char *const *names) {
    mkdir(DIR_PATH, 0755);
    for (int i = 0; names[i] && i < MAX_FEATURES; i++) {
        char guard[256], crashed[256];
        guard_path(guard, sizeof(guard), "guard-", names[i], "");
        guard_path(crashed, sizeof(crashed), "crashed-", names[i], ".txt");
        features[i].name = names[i];
        features[i].enabled = true;
        if (!access(guard, F_OK)) {
            rename(guard, crashed);
            sync();
            nfn_log("guard: %s was interrupted last time (see crashed-%s.txt), turning it off", names[i], names[i]);
        }
        if (!access(crashed, F_OK)) {
            features[i].enabled = false;
            nfn_log("guard: %s is off; delete crashed-%s.txt to turn it back on", names[i], names[i]);
        }
    }
}

bool nfn_enabled(const char *feature) {
    for (int i = 0; i < MAX_FEATURES && features[i].name; i++)
        if (!strcmp(features[i].name, feature))
            return features[i].enabled;
    return false;
}

void nfn_guard_enter(const char *feature, const char *step) {
    char guard[256], text[256];
    guard_path(guard, sizeof(guard), "guard-", feature, "");
    int n = snprintf(text, sizeof(text), "step: %s\nuptime: %.1fs\n", step, nfn_uptime());
    if (n > 0)
        write_synced(guard, O_TRUNC, text, (size_t)n);
}

void nfn_guard_leave(const char *feature) {
    char guard[256];
    guard_path(guard, sizeof(guard), "guard-", feature, "");
    unlink(guard);
}
