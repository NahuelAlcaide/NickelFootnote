// SPDX-License-Identifier: GPL-3.0-or-later

// Logging that survives a hang or a crash, and guards that turn a feature off
// after it crashed Nickel.
//
// Lines go to syslog (nh_log) and to /mnt/onboard/.adds/nicklegpt/log.txt,
// fsynced one by one: syslog lives in RAM and is lost on a forced power-off.

#ifndef NICKLEGPT_LOG_H
#define NICKLEGPT_LOG_H

void ngpt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Seconds since the plugin's init.
double ngpt_uptime();

// True during the first seconds after init. Hooks only pass calls through
// then: a fault in NickleGPT during Nickel's startup would also leave every
// plugin loaded before it disabled (NickelHook restores each plugin's file
// a few seconds after it loads; a crash in between leaves it renamed).
bool ngpt_starting();

// A feature (e.g. "menu") marks the step it's in with a file on disk and
// clears it when done. If Nickel dies in between, the next init finds the
// file, renames it to crashed-<feature>.txt and keeps the feature off until
// that file is deleted.
void ngpt_guard_init(const char *const *features);
bool ngpt_enabled(const char *feature);
void ngpt_guard_enter(const char *feature, const char *step);
void ngpt_guard_leave(const char *feature);

#endif
