#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# NickleGPT debug dump: syslog, installed library hash, imageformats listing.
OUT=/mnt/onboard/.adds/nicklegpt/debug.txt
mkdir -p /mnt/onboard/.adds/nicklegpt
{
  echo "== date"; date
  echo "== md5"; md5sum /usr/local/Kobo/imageformats/libnicklegpt.so
  echo "== imageformats"; ls -l /usr/local/Kobo/imageformats/
  echo "== syslog"; logread
} > "$OUT" 2>&1
sync
echo "Saved to .adds/nicklegpt/debug.txt"
