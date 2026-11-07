#!/usr/bin/env bash
# Dev loop for kiln on a TrimUI Brick over adb.
#   ./dev.sh build      compile build/kiln (static aarch64 musl, via Docker)
#   ./dev.sh run        push to /tmp/kiln, freeze the stock UI, start kiln
#   ./dev.sh stop       kill kiln (screen keeps its last frame)
#   ./dev.sh restore    kill kiln and hand the screen back to the stock UI
#   ./dev.sh log        print kiln's stderr log from the device
#   ./dev.sh shot [f]   save the visible screen as PNG (default build/shot.png)
#   ./dev.sh press ...  inject buttons: a b x y l r up down left right select start menu
#   ./dev.sh install    autostart kiln from the SD card at boot
#   ./dev.sh uninstall  remove the autostart files from the SD card
#   ./dev.sh portmaster install PortMaster, plus the bash and python3 it needs
set -euo pipefail
cd "$(dirname "$0")"

FONT="${FONT:-$HOME/Library/Fonts/FiraCodeiScript-Regular.ttf}"
IMAGE=kiln-build
DEV=/tmp/kiln

PM_RELEASE=2026.11.02-0433
PY_RELEASE=20261026
PY_ASSET=cpython-3.12.16+$PY_RELEASE-aarch64-unknown-linux-gnu-install_only_stripped.tar.gz
PY_SHA256=bb4b6392670c5d3f93876996fcaa19858a4a413a0c7720144c18b0d6f1f0db2d
BASH_VER=5.2.37
BASH_SHA256=9599b22ecd1d5787ad7d3b7bf0c59f312b3396d1e281175dd1f8a4014da621ff
MIXER_SHA256=cb760211b056bfe44f4a1e180cc7cb201137e4d1572f2002cc1be728efd22660

build() {
	mkdir -p build
	docker image inspect "$IMAGE" >/dev/null 2>&1 || docker build --platform linux/arm64 -t "$IMAGE" .
	python3 tools/mkfont.py "$FONT" build/font.h font_big:32 font_mid:26 font_sm:19
	docker run --rm --platform linux/arm64 -v "$PWD":/src -w /src "$IMAGE" \
		cc -std=gnu11 -O2 -Wall -Wextra -static -Ibuild -o build/kiln src/*.c -lm
	echo "built build/kiln ($(wc -c <build/kiln | tr -d ' ') bytes)"
}

# runtrimui.sh respawns MainUI in a loop: freeze the loop, then stop MainUI.
# pidof matches the process name, so it never hits the adb shell running this.
# MainUI ignores SIGTERM; the stock script also falls back to kill -9.
stop_stock() {
	adb shell 'P=$(pidof runtrimui.sh); [ -n "$P" ] && kill -STOP $P
		killall MainUI 2>/dev/null
		for i in 1 2 3 4 5 6; do pidof MainUI >/dev/null || break; sleep 0.5; done
		killall -9 MainUI 2>/dev/null; sleep 0.3; true'
}

# SIGTERM lets kiln stop a running game first; anything still alive after 3 s gets SIGKILL.
KILL_KILN='killall kiln 2>/dev/null
	for i in 1 2 3 4 5 6 7 8 9 10; do pidof kiln >/dev/null || break; sleep 0.3; done
	killall -9 kiln 2>/dev/null'

run() {
	[ -f build/kiln ] || build
	adb shell "mkdir -p $DEV; $KILL_KILN; true"
	adb push build/kiln "$DEV/kiln" >/dev/null
	stop_stock
	# Stock preload.sh clears these keep-awake flags; set them so adb doesn't drop mid-session.
	adb shell 'touch /tmp/stay_awake /tmp/stay_alive'
	# busybox here has no setsid/nohup; start-stop-daemon -b detaches from the adb session.
	# The pidfile makes it check only its own pid instead of every /bin/sh.
	adb shell "chmod +x $DEV/kiln && rm -f $DEV/pid $DEV/kiln.log &&
		start-stop-daemon -S -b -m -p $DEV/pid -x /bin/sh -- -c 'cd $DEV && exec ./kiln >kiln.log 2>&1'"
	sleep 1
	adb shell "pidof kiln >/dev/null && echo 'kiln running' || { echo 'kiln died:'; cat $DEV/kiln.log; }"
}

stop() {
	adb shell "$KILL_KILN; true"
}

restore() {
	adb shell "$KILL_KILN; rm -f /tmp/stay_awake /tmp/stay_alive"'
		P=$(pidof runtrimui.sh); [ -n "$P" ] && kill -CONT $P; true'
}

# Autostart from the SD card: kiln and its supervisor in /mnt/SDCARD/kiln, plus the
# hook the stock runtrimui.sh runs at boot. Nothing outside the SD card is touched.
install() {
	[ -f build/kiln ] || build
	adb shell 'mkdir -p /mnt/SDCARD/kiln /mnt/SDCARD/System/starts'
	adb push build/kiln /mnt/SDCARD/kiln/kiln >/dev/null
	adb push boot/boot.sh /mnt/SDCARD/kiln/boot.sh >/dev/null
	adb push boot/kiln.sh /mnt/SDCARD/System/starts/kiln.sh >/dev/null
	adb shell sync
	echo "installed; kiln starts on next boot (hold B while booting to skip it)"
}

uninstall() {
	adb shell 'rm -f /mnt/SDCARD/System/starts/kiln.sh
		rmdir /mnt/SDCARD/System/starts /mnt/SDCARD/System 2>/dev/null
		rm -f /mnt/SDCARD/kiln/kiln /mnt/SDCARD/kiln/boot.sh
		rmdir /mnt/SDCARD/kiln 2>/dev/null; sync; true'
	echo "uninstalled; stock UI from next boot"
}

# PortMaster's TrimUI build, made to run on stock firmware 1.1.1, which lacks what it and
# its ports assume. All of it goes in /mnt/SDCARD/kiln, which kiln puts on PATH and
# LD_LIBRARY_PATH for apps and ports only:
#   bin/bash     static; PortMaster's launcher and every port script are bash
#   python/      python-build-standalone, for PortMaster's GUI (bin/python3 wraps it)
#   bin/busybox  static 1.36, for bin/tr: PortMaster's device check lowercases with
#                tr '[:upper:]', which stock busybox 1.27 can't do, so the Brick was taken
#                for a Smart Pro with analog sticks and ports ignored the d-pad
#   lib/         libpng16, zlib, SDL2_mixer (tools/compat-libs.sh)
# PortMaster is set to its "Ports tab" mode, which gives each port a stock-style
# Ports/portmaster-<name>/config.json that kiln's Ports screen lists. (Its default "Roms
# section" mode never copies the script on TrimUI: os.path.samefile() raises on the
# not-yet-existing target.) The only change outside the SD card is the /bin/bash symlink
# on the rootfs overlay.
portmaster() {
	local dl=build/pm stage=build/pm/stage py=build/pm/stage/kiln/python
	mkdir -p "$dl"
	docker image inspect "$IMAGE" >/dev/null 2>&1 || docker build --platform linux/arm64 -t "$IMAGE" .
	if [ ! -f "$dl/bash" ]; then
		docker run --rm --platform linux/arm64 -v "$PWD/$dl":/w -w /w "$IMAGE" sh -ec "
			wget -q https://ftp.gnu.org/gnu/bash/bash-$BASH_VER.tar.gz
			echo '$BASH_SHA256  bash-$BASH_VER.tar.gz' | sha256sum -c -
			tar xzf bash-$BASH_VER.tar.gz && cd bash-$BASH_VER
			./configure --enable-static-link --without-bash-malloc --disable-nls >/dev/null
			make -j8 >/dev/null && strip bash && cp bash /w/bash"
	fi
	[ -s "$dl/busybox" ] || docker run --rm --platform linux/arm64 "$IMAGE" \
		sh -c 'apk add -q busybox-static >/dev/null && cat /bin/busybox.static' >"$dl/busybox"
	[ -f "$dl/lib/libSDL2_mixer-2.0.so.0" ] || docker run --rm --platform linux/arm64 \
		-v "$PWD/tools":/tools:ro -v "$PWD/$dl/lib":/out ubuntu:20.04 \
		sh /tools/compat-libs.sh /out "$MIXER_SHA256"
	[ -f "$dl/$PY_ASSET" ] || curl -fL -o "$dl/$PY_ASSET" \
		"https://github.com/astral-sh/python-build-standalone/releases/download/$PY_RELEASE/$PY_ASSET"
	echo "$PY_SHA256  $dl/$PY_ASSET" | shasum -a 256 -c -
	if [ ! -f "$dl/trimui.portmaster.zip" ]; then
		local url=https://github.com/PortsMaster/PortMaster-GUI/releases/download/$PM_RELEASE/trimui.portmaster.zip
		curl -fL -o "$dl/trimui.portmaster.zip" "$url"
		[ "$(md5 -q "$dl/trimui.portmaster.zip")" = "$(curl -fsL "$url.md5" | cut -d' ' -f1)" ]
	fi

	rm -rf "$stage" && mkdir -p "$stage/kiln/bin"
	cp "$dl/bash" "$dl/busybox" "$stage/kiln/bin/"
	cp -R "$dl/lib" "$stage/kiln/lib"
	printf '#!/bin/sh\nexec /mnt/SDCARD/kiln/bin/busybox tr "$@"\n' >"$stage/kiln/bin/tr"
	tar -xzf "$dl/$PY_ASSET" -C "$stage/kiln"
	# FAT32 has no symlinks: python3.12 is self-contained, so drop the links and the parts
	# PortMaster never uses, and reach it through a wrapper on kiln's PATH.
	(cd "$py" && rm -rf share include bin/[!p]* bin/pip* bin/pydoc* bin/python bin/python3 bin/python3*-config \
		lib/pkgconfig lib/libpython3* lib/libtcl* lib/libtk* lib/tcl* lib/tk* lib/itcl* lib/thread* \
		lib/python3.12/{test,idlelib,tkinter,turtledemo,ensurepip,lib2to3,config-3.12-aarch64-linux-gnu} \
		lib/python3.12/site-packages/pip* lib/python3.12/lib-dynload/_tkinter*)
	[ -z "$(find "$py" -type l)" ]
	printf '#!/bin/sh\nexec /mnt/SDCARD/kiln/python/bin/python3.12 "$@"\n' >"$stage/kiln/bin/python3"
	unzip -q "$dl/trimui.portmaster.zip" -d "$stage"
	# PortMaster's DEFAULT_CONFIG plus the port mode.
	mkdir -p "$stage/Apps/PortMaster/PortMaster/config"
	printf '{"version": 2, "first-run": true, "ports_info_checked": null, "porters_checked": null, "show_experimental": false, "trimui-port-mode": "ports"}\n' \
		>"$stage/Apps/PortMaster/PortMaster/config/config.json"
	COPYFILE_DISABLE=1 tar -C "$stage" -czf "$dl/sd.tgz" kiln Apps

	adb push "$dl/sd.tgz" /mnt/SDCARD/kiln-pm.tgz
	# PortMaster updates itself and rewrites its own files on first run, so an existing
	# Apps/PortMaster is left alone. Data/ports must exist before that first run: otherwise
	# its migration step takes the stock Ports folder for an old PortMaster one and moves
	# the stock apps out of it.
	adb shell 'cd /mnt/SDCARD && tar xzf kiln-pm.tgz kiln &&
		{ [ -d Apps/PortMaster ] || tar xzf kiln-pm.tgz Apps; } && rm kiln-pm.tgz &&
		mkdir -p Data/ports Ports &&
		{ [ -e /bin/bash ] || ln -s /mnt/SDCARD/kiln/bin/bash /bin/bash; } && sync &&
		/bin/bash -c "echo bash \$BASH_VERSION" && /mnt/SDCARD/kiln/bin/python3 -V'
	echo "PortMaster installed: kiln > Apps > PortMaster (needs Wi-Fi), ports appear under kiln > Ports"
}

log() {
	adb shell "cat $DEV/kiln.log"
}

# Inject button presses into the TRIMUI Player1 evdev node, e.g. ./dev.sh press down down a
press() {
	mkdir -p build
	python3 - "$@" >build/press.bin <<'EOF'
import struct, sys
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
KEYS = {"a": 305, "b": 304, "x": 308, "y": 307, "l": 310, "r": 311,
        "select": 314, "start": 315, "menu": 316}
HATS = {"up": (17, -1), "down": (17, 1), "left": (16, -1), "right": (16, 1)}
out = sys.stdout.buffer
ev = lambda t, c, v: out.write(struct.pack("<qqHHi", 0, 0, t, c, v))
for name in sys.argv[1:]:
    if name in KEYS:
        ev(EV_KEY, KEYS[name], 1); ev(EV_SYN, 0, 0); ev(EV_KEY, KEYS[name], 0); ev(EV_SYN, 0, 0)
    elif name in HATS:
        code, val = HATS[name]
        ev(EV_ABS, code, val); ev(EV_SYN, 0, 0); ev(EV_ABS, code, 0); ev(EV_SYN, 0, 0)
    else:
        sys.exit(f"unknown button {name}")
EOF
	adb push build/press.bin /tmp/press.bin >/dev/null
	# One button (4 events, 96 bytes) at a time: a single burst overflows the evdev
	# client buffer while kiln is busy rendering, and the kernel drops the rest.
	adb shell 'for e in /sys/class/input/event*; do
			grep -q "TRIMUI Player1" $e/device/name && dev=/dev/input/${e##*/}; done
		n=$(($(wc -c </tmp/press.bin) / 96)); i=0
		while [ $i -lt $n ]; do
			dd if=/tmp/press.bin bs=96 skip=$i count=1 2>/dev/null >$dev; sleep 0.06; i=$((i + 1))
		done'
}

shot() {
	local out="${1:-build/shot.png}"
	mkdir -p build
	adb shell 'Y=$(cut -d, -f2 /sys/class/graphics/fb0/pan); dd if=/dev/fb0 of=/tmp/shot.raw bs=4096 skip=$Y count=768 2>/dev/null'
	adb pull /tmp/shot.raw build/shot.raw >/dev/null
	adb shell rm /tmp/shot.raw
	python3 - build/shot.raw "$out" <<'EOF'
import sys
from PIL import Image
raw = open(sys.argv[1], "rb").read()
Image.frombuffer("RGBA", (1024, 768), raw, "raw", "BGRA", 0, 1).convert("RGB").save(sys.argv[2])
EOF
	echo "$out"
}

case "${1:-}" in
build | run | stop | restore | log | shot | press | install | uninstall | portmaster) "$@" ;;
*) sed -n '2,12p' "$0" && exit 1 ;;
esac
