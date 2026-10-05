#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# One-off recovery after Nickel crashed during startup: NickelHook leaves the
# plugins that were starting at the time renamed to *.so.failsafe (disabled).
# Renames every other plugin back and turns NickleGPT off (renamed to
# .disabled), since it is the likely cause. Install into .adds/nm/ only when
# needed, and remove it afterwards.
D=/usr/local/Kobo/imageformats
OUT=/mnt/onboard/.adds/nicklegpt/restore.txt

# NickelHook renames every plugin to .failsafe for a few seconds at each
# boot; running this then would delete a plugin that is just starting up.
up=$(cut -d. -f1 /proc/uptime)
if [ "$up" -lt 60 ]; then
  echo "Too soon after boot (${up}s). Wait a minute and run it again."
  exit 0
fi

mkdir -p /mnt/onboard/.adds/nicklegpt
{
  echo "== before"; ls -l "$D"
  for f in "$D"/*.so.failsafe; do
    [ -f "$f" ] || continue
    lib=${f%.failsafe}
    name=${lib##*/}
    if [ "$name" = "libnicklegpt.so" ]; then
      if [ -e "$lib" ]; then
        rm -f "$f" && echo "removed a stale NickleGPT copy"
      else
        mv "$f" "$lib.disabled" && echo "NickleGPT disabled (renamed to .disabled)"
      fi
    elif [ -e "$lib" ]; then
      echo "skipped $name (both $name and $name.failsafe exist)"
    else
      mv "$f" "$lib" && echo "restored $name"
    fi
  done
  sync
  echo "== after"; ls -l "$D"
} > "$OUT" 2>&1
sync
grep -E "restored|skipped|removed|disabled" "$OUT" || echo "Nothing to restore."
echo "Now restart the Kobo."
