// SPDX-License-Identifier: GPL-3.0-or-later

// A PLT hook that keeps hooks installed earlier by other plugins.
//
// NickelHook's nh_dlhook takes the original from dlsym, so when two plugins
// hook the same symbol, the later one silently bypasses the earlier one.
// NickelHardcover hooks ReadingMenuView's constructor (its reading menu
// items), and loads before NickleGPT, so the reading menu hook uses this
// instead and calls whatever was in the GOT before it.

#ifndef NICKLEGPT_CHAINHOOK_H
#define NICKLEGPT_CHAINHOOK_H

// Points the GOT entry for `sym` in `lib` at `target`. Returns the function to
// call next (another plugin's hook, or the real function), or null on error.
void *ngpt_chain_hook(const char *lib, const char *sym, void *target);

#endif
