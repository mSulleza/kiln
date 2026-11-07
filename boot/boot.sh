#!/bin/sh
# kiln boot supervisor. Lets the stock boot finish (USB, Wi-Fi, audio, keymon, inputd),
# takes the screen the moment MainUI starts, and hands it back whenever kiln exits:
# "Return to TrimUI", hold-B-at-boot, or a crash all land on the stock UI.
KILN=/mnt/SDCARD/kiln

[ -x $KILN/kiln ] || exit 0

# Wait up to 60 s for the stock loop to start MainUI.
i=0
until pidof MainUI >/dev/null; do
	i=$((i + 1))
	[ $i -gt 600 ] && exit 0
	sleep 0.1
done

P=$(pidof runtrimui.sh)
[ -n "$P" ] || exit 0
kill -STOP $P          # the loop would respawn MainUI
killall -9 MainUI      # MainUI ignores SIGTERM

cd $KILN
./kiln --boot >/tmp/kiln/kiln.log 2>&1
echo "boot: kiln exited with $?" >>/tmp/kiln/kiln.log

kill -CONT $P          # stock loop resumes and starts MainUI
