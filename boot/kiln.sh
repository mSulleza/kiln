#!/bin/sh
# Boot hook, installed as /mnt/SDCARD/System/starts/kiln.sh. The stock runtrimui.sh runs
# every script there before its launcher loop and waits for each, so only start the
# supervisor in the background and return; the rest of the stock boot carries on.
[ -x /mnt/SDCARD/kiln/boot.sh ] || exit 0
mkdir -p /tmp/kiln
start-stop-daemon -S -b -m -p /tmp/kiln/boot.pid -x /bin/sh -- /mnt/SDCARD/kiln/boot.sh
