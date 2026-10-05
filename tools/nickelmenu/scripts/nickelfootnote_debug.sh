#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# NickelFootnote debug dump: syslog, installed library hash, imageformats listing.
OUT=/mnt/onboard/.adds/nickelfootnote/debug.txt
mkdir -p /mnt/onboard/.adds/nickelfootnote
{
  echo "== date"; date
  echo "== md5"; md5sum /usr/local/Kobo/imageformats/libnickelfootnote.so
  echo "== imageformats"; ls -l /usr/local/Kobo/imageformats/
  echo "== syslog"; logread
} > "$OUT" 2>&1
sync
echo "Saved to .adds/nickelfootnote/debug.txt"
