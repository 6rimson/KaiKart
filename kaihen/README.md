# WiiCompiled + Retro Rewind for Switch 2 (kaiHEN)

An experimental port of [WiiCompiled](https://github.com/patchzyy/Wiicompiled) with Retro Rewind for
the Nintendo Switch 2. It's a homebrew app (`.nro`) you start from hbmenu under kurumuz's
[kaiHEN](https://github.com/ouncebrew/kaihen). It's built with ouncebrew's
[Switch 2 devkit](https://github.com/ouncebrew/ounce-devkit) and draws through its Vulkan driver,
OunceVK. Once kaiHEN is running, you need no PC to play.

> [!NOTE]
> **Made with AI assistance.** Most of this port's code was written with Claude Code. I tested
> every build on my own console and decided what went in.

This repository contains no game files, disc images, Retro Rewind files, saves or built apps. You
build everything yourself from your own PAL (`RMCP01`) disc and your own Retro Rewind folder. A
built `.nro` contains translated game code, so please don't share one.

The older version, for HBLauncher and SmileBASIC, is on the
[`switch2-hblauncher`](https://github.com/6rimson/WiiCompiled-Switch2/tree/switch2-hblauncher)
branch.

## Status

Tested with kaiHEN `20261114d` and `20261116` and the devkit at `f347815` (November 2026), on one
Switch 2.
Includes Retro Rewind 6.13.5; plain Mario Kart Wii also builds and runs. PAL disc only.

## What works

- Retro Rewind boots from hbmenu; menus, offline and online races (Retro WFC) and saves work.
- Offline races hold 60 FPS most of the time, with the CPU at 1702 MHz and PGO. On a charger with
  memory raised, the heaviest moments hold 60 too.
- The port menu: resolution, clocks, button mapping, and returning to hbmenu.

## Known issues

- **Raised memory and GPU clocks need a charger.** On battery, the console halves the CPU's real
  speed when memory or the GPU run above their own rates. So the port runs them as you chose only
  while plugged in, and at the system's rates on battery; the CPU keeps its rate either way.
- **Each launch spends about 15-20 seconds setting up its saved shaders** on the title screen. New
  places also stutter the first time while their shaders build; they're saved after that.
- **Opening the game a second time in one kaiHEN session crashes** ("the software needed to
  close"). Return to hbmenu works, and other apps open fine after it, but restart kaiHEN before
  opening the game again.
- **Returning to hbmenu after playing online can crash.** It happens after the game has closed,
  so your progress is already saved.
- **A thin black line can show below the race minimap** at some resolutions (1× and 2×, not 1.5×).
  It's an upstream WiiCompiled bug,
  [patchzyy/Wiicompiled#397](https://github.com/patchzyy/Wiicompiled/issues/397).
- **Change the resolution outside races.** Changing it during a race can break the graphics until
  you restart the game.
- **Retro Rewind updates need a rebuild** (`kaihen/run.sh translate` and `build`) when they change
  `Code.pul`.
- **Only built on an Apple Silicon Mac.** The scripts should also work on aarch64 Linux, but I
  haven't tried.

## Setup

Expect an hour or two for the first build; most of it runs on its own.

### 0. Get this repository

```sh
mkdir -p ~/Switch2 && cd ~/Switch2
git clone --recursive --branch switch2-kaihen https://github.com/6rimson/WiiCompiled-Switch2.git
cd WiiCompiled-Switch2
```

`--recursive` also fetches aurora, the graphics library WiiCompiled includes. Run every command
below from this folder.

### 1. What you need

- **A Switch 2 that can run kaiHEN** and reach hbmenu. See
  [kaiHEN's README](https://github.com/ouncebrew/kaihen) for how to start it.
- **A microSD card** for the console, and a way to put it in the Mac. The game's files take about
  9 GB.
- **On the Mac:** [Docker Desktop](https://www.docker.com/products/docker-desktop/) with its memory
  set to at least 8 GB (Settings → Resources), Git and Python 3 (both come with
  `xcode-select --install`), and about 40 GB of free space for the build.
- **Your own Mario Kart Wii PAL disc (`RMCP01`)** and the latest Retro Rewind.

### The easy way

Run `kaihen/easy-build.sh` in Terminal (or drag the file into a Terminal window and press Return).
It asks for:

- Retro Rewind or plain Mario Kart Wii;
- your disc (drag the file into the window);
- your `RetroRewind6` folder;
- your microSD card, once the build is done.

It does steps 2 to 4 for you: it extracts and checks the disc, downloads the Retro WFC payload,
builds, and copies everything to the card. If something fails, or after an update, run it again:
it skips what's already done. The build's log is in `build/build-log.txt`.

The steps below do the same by hand.

### 2. Put your files in place

**Your disc.** Extract it into `Assets/DATA` with [nodtool](https://github.com/encounter/nod/releases)
(`.iso`, `.wbfs`, `.rvz` and similar all work), then copy out the two files the translator reads:

```sh
curl -fsSL "https://github.com/encounter/nod/releases/download/v2.0.0-alpha.10/nodtool-macos-arm64" -o ~/Switch2/nodtool
chmod +x ~/Switch2/nodtool && xattr -d com.apple.quarantine ~/Switch2/nodtool 2>/dev/null
mkdir -p Assets/DATA && ~/Switch2/nodtool extract /path/to/RMCP01.iso Assets/DATA
cp Assets/DATA/sys/main.dol Assets/DATA/files/rel/StaticR.rel Assets/
shasum -a 256 Assets/main.dol Assets/StaticR.rel
```

**You should see** these two hashes, which mean a clean PAL disc:

- `Assets/main.dol`: `80d18895b39c63bd80f457398bfcbb91b7d16ac116a41a88967e954080155b05`
- `Assets/StaticR.rel`: `16d9d146112541fefea701ecb5bc1a496f9d50e4a752fbb5b6778e7c6399f67d`

**Retro Rewind.** Get the latest [Retro Rewind](https://wiki.tockdom.com/wiki/Retro_Rewind) release
(for example with [Wheel Wizard](https://github.com/TeamWheelWizard/WheelWizard)), and copy its
`RetroRewind6` folder, the one that contains `Binaries/Code.pul`:

```sh
mkdir -p PulsarPacks/completed/RetroRewind
cp -R /path/to/RetroRewind6 PulsarPacks/completed/RetroRewind/
```

**Online play.** Download the Retro WFC payload; the build checks its signature:

```sh
mkdir -p build/retro-wfc/binary
curl -fsSL --retry 3 "https://rwfc.net/api/wfc/payload?g=RMCPD00" -o build/retro-wfc/binary/payload.RMCPD00.bin
```

**Optional: your own hbmenu icon.** The app comes with WiiCompiled's own app icon
(`kaihen/icons/`). To use a different one, save a 256×256 JPEG as `kaihen/icon-RetroRewind.jpg` (or
`icon-WiiCompiled.jpg`). It's kept out of git.

### 3. Build

```sh
kaihen/run.sh setup
```

The first time, this downloads and builds what it needs: the Switch 2 devkit (from GitHub), the
build tools image, and Dawn and SDL3. Then it translates your game and builds it. **You should
see** `kaihen/dist/RetroRewind.nro` at the end.

`setup` is four steps you can also run one at a time: `sources`, `translate`, `configure` and
`build`. If your Mac runs out of memory partway, run `KAIHEN_JOBS=4 kaihen/run.sh build`.

### 4. Put it on the microSD card

Put the card in the Mac. macOS shows it under `/Volumes/` with the card's name (the name Finder
shows; `ls /Volumes` lists them). With your card's name in place of `SWITCH`:

```sh
kaihen/run.sh sd /Volumes/SWITCH
```

This copies the `.nro` to `switch/` and the game's files to `wiicompiled/`. The first time takes a
while: it copies your disc and Retro Rewind folder. Files already on the card, such as your saves,
are kept. To start from saves you already have, add `--nand /path/to/NAND` (a Dolphin or WiiCompiled
NAND folder) or `--rr-save /path/to/rksys.dat` (a Retro Rewind save) after the card the first time:

```sh
kaihen/run.sh sd /Volumes/SWITCH --nand /path/to/NAND --rr-save /path/to/rksys.dat
```

It never overwrites your files: folders and settings already on the card are left as they are,
and only the `.nro`, `dsp_coef.bin` and `cacert.pem` are replaced. An interrupted copy is redone
the next time you run it. It doesn't replace a Retro Rewind folder that's already there, either;
for that, see [Updating](#updating).

#### Copying by hand

You can also copy the files yourself, in Finder or any file manager. Each build leaves the card's
layout, without your game files, in `kaihen/dist/card/`:

1. Copy the `switch` and `wiicompiled` folders from `kaihen/dist/card/` to the card. On a card
   you've played on, don't replace `wiicompiled/settings.env` or `wiicompiled/UserData/Config.toml`
   when asked: they hold your settings.
2. Copy your extracted disc (`Assets/DATA`, with `files/` and `sys/` inside) to
   `wiicompiled/disc/DATA` on the card.
3. Copy your `RetroRewind6` folder to `wiicompiled/RetroRewind/RetroRewind6` on the card.

Saves you already have go in `wiicompiled/UserData/NAND/` (a NAND folder) and
`wiicompiled/RetroRewind/riivolution/save/RetroWFC/RMCP/rksys.dat` (Retro Rewind).

### 5. Play

Eject the card, put it in the console, start kaiHEN and open hbmenu, then choose **Retro Rewind**.
Wait on the title screen until the shader message is gone before racing.

### Plain Mario Kart Wii

Plain Mario Kart Wii needs only the disc. Build it with `KAIHEN_PRODUCT=WiiCompiled` in front of
each command:

```sh
KAIHEN_PRODUCT=WiiCompiled kaihen/run.sh setup
KAIHEN_PRODUCT=WiiCompiled kaihen/run.sh sd /Volumes/SWITCH
```

It shows in hbmenu as WiiCompiled and shares the card's `wiicompiled` folder (settings and saves)
with Retro Rewind.

### Updating

**A new version of this port:**

```sh
git pull --recurse-submodules
kaihen/run.sh build
kaihen/run.sh sd /Volumes/SWITCH
```

`git pull` gets the new version; `--recurse-submodules` also moves aurora (a separate repository
inside this one) to the version it expects. `build` rebuilds only what changed, and `sd` replaces
the app on the card. If the release notes say a step such as `translate` is needed, run it before
`build`.

**A new Retro Rewind version:** replace the `RetroRewind6` folder in
`PulsarPacks/completed/RetroRewind/` with the new one, then:

```sh
kaihen/run.sh translate
kaihen/run.sh build
kaihen/run.sh sd /Volumes/SWITCH --replace-rr
```

`translate` turns the new Retro Rewind code into the game's, and `--replace-rr` swaps the card's
`RetroRewind6` folder for the new one. Files on the card that the new version doesn't have, such
as ghosts you saved, are kept, and your save is outside that folder.

## The port menu

Press **L + R + right stick click** in the game (D-pad to move and change, A to rebind a button, B
to close). Everything here is saved.

| Setting | Default | Notes |
|---|---|---|
| Resolution | 1.0× | Change it in menus, not during a race. |
| Show FPS | off | |
| Volume | 100% | |
| CPU clock | 1702 MHz | System is 1100.8 MHz. |
| Memory clock | System | 2133 MHz handheld. Raised rates apply only on a charger. |
| GPU clock | System | 561 MHz handheld. Raised rates help at 2× and 3× resolution; only on a charger. |
| Buttons | A, B, X, Y as labelled | Rebind any button. |
| Return to hbmenu | | Press A twice. Holding **+ and −** together also returns. |

Below the clocks, the menu shows whether the console is on a charger. The clock settings are kept
in `wiicompiled/settings.env` on the card, with other settings read at launch:

- `MKW_PERF_STATS=1` logs FPS and frame timing to `wiicompiled/UserData/Logs/kaihen.log`.
- `KAIHEN_PROFILE=1` samples the game during races into `UserData/Logs/profile.bin`, for speed
  work.

If something goes wrong, `UserData/Logs/kaihen.log` has the last launch's log, and
`UserData/Logs/exit.txt` how far the last return to hbmenu got.

## How it's built

WiiCompiled's translated game and runtime, compiled with Clang for the devkit's newlib and
libounce and linked by its GCC, with the shipped PGO profile (`kaihen/pgo/`). `kaihen/platform/` is
the layer between the game and the console: startup, the screen, threads on their own cores,
clocks, the power state and the return to hbmenu. `kaihen/patches/` has one patch each for Dawn,
SDL3 (with audio and controller drivers on libnx) and aurora; `kaihen/dependencies.json` pins the
devkit, Dawn and SDL3. Docker volumes: `wiicompiled-kaihen-work` (sources and the translated game) and
`wiicompiled-kaihen` (the build); `KAIHEN_WORK` and `KAIHEN_VOLUME` pick others.

## Credits

- **[WiiCompiled](https://github.com/patchzyy/Wiicompiled)** by patchzyy and contributors: the
  recompilation this is built on.
- **[kaiHEN](https://github.com/ouncebrew/kaihen)** by kurumuz: the Switch 2 homebrew environment
  and hbmenu this runs in.
- **[ounce-devkit](https://github.com/ouncebrew/ounce-devkit)**, libounce and OunceVK by
  ouncebrew: the Switch 2 toolchain, system library and Vulkan driver.
- **RavensClawX's [Twilight Princess](https://github.com/ravensclawx/TwilightPrincess-HBLauncher)
  port**: parts of the Dawn and aurora patches, and the layout of the SDL drivers (MIT,
  `kaihen/patches/LICENSE.tp-integration`).
- **[DriftDroid](https://github.com/dorPXP/driftdroid)** by dorPXP and
  **[KartPadNX](https://github.com/KawaiiBunga/KartPadNX)** by KawaiiBunga: Switch 1 and Android
  ports of WiiCompiled, whose fixes and ideas (such as PGO) helped throughout.
- **[Retro Rewind](https://wiki.tockdom.com/wiki/Retro_Rewind)** by ZPL and team, and
  **[Retro WFC](https://rwfc.net)**.
- **[aurora](https://github.com/encounter/aurora)** by encounter,
  **[Dawn](https://dawn.googlesource.com/dawn)** by Google and
  **[SDL](https://github.com/libsdl-org/SDL)**.

WiiCompiled is GPL-3.0 (see `../LICENSE`), and so is this port. See
[THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md). "Nintendo Switch 2" and "Mario Kart Wii" are
trademarks of Nintendo; this project is not affiliated with or endorsed by Nintendo.
