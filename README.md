# kiln

Minimal launcher for the TrimUI Brick. One static aarch64 binary: draws straight to
`/dev/fb0` (double-buffered by panning), reads the `TRIMUI Player1` evdev device, browses
ROMs and runs the stock launch scripts. Runs on the stock kernel; no device libraries needed.

- **Games**: every `/mnt/SDCARD/Emus/*/config.json` with ROMs in its `rompath`, filtered by
  `extlist`. Subfolders browse; a ROM runs `Emus/<sys>/<launch> "<rom path>"`.
- **Search** (Y): on-screen keyboard, live results. In a system's list it filters that
  list; on the Games screen it searches every system (indexed on first use, in well under a
  second). Each typed word must start a word in the title.
- **Emulator picker** (X on a system or game): choose from the config's `launchlist`. The
  choice is written to that config's `"launch"`, the field the stock UI also uses, via a
  temp file and rename. Only that value changes.
- **Ports**: `/mnt/SDCARD/Ports/*/config.json`, same format as Apps: the stock Ports
  folder, where PortMaster also puts each port it installs (see PortMaster below).
- **Apps**: `/mnt/SDCARD/Apps/*/config.json` (`label`, `launch`).
- **System info**, return to the stock UI, reboot, power off.

Keys: A open/play, B back, X emulator, Y search, Menu to top, d-pad left/right page,
L/R jump to the previous/next letter, SELECT+START back to the stock UI.
Keyboard: A types, B deletes, X space, START or Y done.

Apps run in their own process group; if kiln gets SIGTERM mid-game it stops the game
first. (Killing kiln while an emulator still had the framebuffer once wedged the sunxi fb
driver until reboot.) Sleep and wake are fine, both idle and mid-game.

## Dev loop

Needs Docker (arm64 Alpine toolchain), Python 3 with Pillow, and `adb`.

```
./dev.sh build     # build/kiln
./dev.sh run       # push to /tmp/kiln, freeze stock UI, start kiln
./dev.sh shot      # screenshot -> build/shot.png
./dev.sh log       # kiln stderr (every input event is logged)
./dev.sh restore   # kill kiln, stock UI comes back
./dev.sh press down down a   # inject buttons, for testing without touching the device
```

`run` stops the stock `runtrimui.sh` loop with SIGSTOP and kills MainUI. `restore`, or
SELECT+START / "Return to TrimUI" in kiln, sends SIGCONT so the loop starts MainUI
again. `run` also touches `/tmp/stay_awake` and `/tmp/stay_alive` (stock keep-awake flags)
so the device doesn't sleep mid-session; `restore` removes them. While they exist, the
power button only blanks the screen, so remove them before testing real sleep. Nothing is written to
flash; `/tmp/kiln` is cleared on reboot.

`press` sends one button every 60 ms. A bigger burst overflows the evdev client buffer
while kiln is rendering and the kernel drops the rest.

The font is rasterized at build time from `$FONT` (default: Fira Code, OFL) into
`build/font.h`.

## Autostart

```
./dev.sh install     # kiln starts at every boot
./dev.sh uninstall   # back to stock from the next boot
```

`install` copies `kiln` and `boot/boot.sh` to `/mnt/SDCARD/kiln/` and `boot/kiln.sh` to
`/mnt/SDCARD/System/starts/`, which the stock `runtrimui.sh` runs at boot. Nothing outside
the SD card changes; deleting those files (or removing the card) brings back stock.

The hook only starts `boot.sh` in the background, so the stock boot still sets up USB/ADB,
Wi-Fi, audio, keymon and inputd. `boot.sh` waits for MainUI, freezes the stock loop, kills
MainUI and runs `kiln --boot`. Whenever kiln exits (Return to TrimUI, SELECT+START, a
crash) it resumes the loop and the stock UI comes back until the next boot.

Hold **B** while booting to skip kiln for that boot. The log is `/tmp/kiln/kiln.log`
(`./dev.sh log`). `./dev.sh run` still works with autostart installed.

## PortMaster

```
./dev.sh portmaster   # PortMaster, plus the bash, python3 and libraries it needs
```

Then **Apps > PortMaster** browses and installs ports (needs Wi-Fi; join a network from
the stock UI's settings). Installed ports show up under **Ports**. Tested with Cave Story
(Evo).

Stock firmware 1.1.1 is missing what PortMaster and its ports assume, and PortMaster calls
stock TrimUI unsupported. `portmaster` fills the gaps in `/mnt/SDCARD/kiln`:

| Path | Why |
|---|---|
| `bin/bash` | static bash 5.2; PortMaster's launcher and every port script are bash |
| `python/`, `bin/python3` | python-build-standalone 3.12 for PortMaster's GUI (FAT32 has no symlinks, so a wrapper) |
| `bin/busybox`, `bin/tr` | busybox 1.36 `tr`; PortMaster's device check needs `tr '[:upper:]'`, which stock busybox 1.27 lacks, so the Brick was taken for a Smart Pro with sticks and ports ignored the d-pad |
| `lib/` | libpng16 (stock has 12), zlib 1.2.11 (stock 1.2.8), SDL2_mixer 2.8 (stock lacks `Mix_OpenAudioDevice`); built by `tools/compat-libs.sh` |

kiln adds `kiln/bin` to PATH and `kiln/lib` ahead of the stock libraries, and runs with
bash, only for Apps and Ports. Emulators get exactly the stock environment. The one change
outside the SD card is a `/bin/bash` symlink on the rootfs overlay (PortMaster itself also
writes `/roms/ports/PortMaster/control.txt` there).

PortMaster is preset to its "Ports tab" mode. Its default "Roms section" mode never copies
the port script on TrimUI (`os.path.samefile()` raises on the missing target). `Data/ports`
is created before PortMaster's first run, or its migration step moves the stock `Ports/`
apps into it.

Known gaps:
- **Screen size.** Stock TrimUI SDL doesn't scale a window smaller than 1024x768. Ports
  that pick their size from the screen width can come up small in a corner. Cave Story
  picked 640x480; it is set to 1024x768 now (Options > Graphics in the game).
- **Unsupported on stock.** Other ports may want more libraries. Check the port's
  `log.txt` in `/mnt/SDCARD/Data/ports/<port>/` for "cannot open shared object file" or
  "undefined symbol".

To remove: delete `Apps/PortMaster`, `Data/ports`, `Ports/portmaster-*`, `kiln/bin`,
`kiln/python`, `kiln/lib` and `/bin/bash`.

## Layout

- `src/gfx.c` framebuffer, blending, rounded rects, text
- `src/input.c` evdev reading, button mapping, d-pad repeat
- `src/library.c` emulator configs, ROM folder scanning
- `src/util.c` file and JSON helpers
- `src/main.c` screen stack, rendering, launching, power actions
- `tools/mkfont.py` TTF to C glyph atlas
- `boot/kiln.sh` boot hook, `boot/boot.sh` boot supervisor
- `tools/compat-libs.sh` libraries ports need that stock lacks (runs in ubuntu:20.04)

## Buttons (Brick, via trimui_inputd)

| Button | evdev |
|---|---|
| A / B / X / Y | BTN_EAST / BTN_SOUTH / BTN_WEST / BTN_NORTH |
| D-pad | ABS_HAT0X / ABS_HAT0Y |
| L / R | BTN_TL / BTN_TR |
| Select / Start / Menu | BTN_SELECT / BTN_START / BTN_MODE |
