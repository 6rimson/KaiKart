# KaiKart: Mario Kart Wii and Retro Rewind on Switch 2

KaiKart is an experimental port of [WiiCompiled](https://github.com/patchzyy/Wiicompiled) for the
Nintendo Switch 2. It plays Retro Rewind and plain Mario Kart Wii as homebrew, started from hbmenu
under kurumuz's [kaiHEN](https://github.com/ouncebrew/kaihen). It's built with ouncebrew's
[Switch 2 devkit](https://github.com/ouncebrew/ounce-devkit) and draws through its Vulkan driver,
OunceVK. No PC is needed: KaiKart sets everything up on the console from your own disc image.

> [!NOTE]
> **Made with AI assistance.** Most of this port's code was written with Claude Code. I tested
> every build on my own console and decided what went in.

**What's in a release:** the KaiKart launcher and the two game builds (`.nro` files), which hold
WiiCompiled's translation of Mario Kart Wii's code. There are no game files, textures, music or
disc images. You need your own Mario Kart Wii PAL disc (`RMCP01`). Retro Rewind is downloaded
from its own server, which is its official update source.

The older version, for HBLauncher and SmileBASIC, is on the
[`switch2-hblauncher`](https://github.com/6rimson/KaiKart/tree/switch2-hblauncher)
branch.

## Play

**You need:**

- **A Switch 2 that can run kaiHEN** and reach hbmenu. See
  [kaiHEN's README](https://github.com/ouncebrew/kaihen) for how to start it.
- **Your own Mario Kart Wii PAL disc image** (`RMCP01`), unmodified, as an `.iso` or `.wbfs` file.
  A `.wbfs` that's split into `.wbfs`, `.wbf1`, `.wbf2` and so on works too.
- **About 6 GB free on the microSD card**, plus room for the disc image itself (up to 4.4 GB)
  until the first start is done.
- **An internet connection** on the console for the first start.

**Steps:**

1. Download `KaiKart-<version>.nro` from the
   [latest release](https://github.com/6rimson/KaiKart/releases/latest) and put it in
   the microSD card's `switch` folder.
2. Make a folder named `wiicompiled` at the root of the card, and put your disc image in it.
3. Put the card in the console, start kaiHEN, open hbmenu, and choose **KaiKart**.
4. Choose **Retro Rewind** or **Mario Kart Wii**.

The first start of each game sets it up. It downloads the game build, sets up your disc, and, for
Retro Rewind, downloads Retro Rewind (about 2 GB). It takes about 10 minutes, mostly for the card.
Keep the console on and the card in. After that, games start straight away, and you can delete the
disc image to free space.

The first launch then spends about 15-20 seconds on the title screen setting up its saved shaders.
Wait there until the shader message is gone before racing.

### Coming from v0.3

KaiKart uses the `wiicompiled` folder you already have, with your saves and settings. Put
`KaiKart-<version>.nro` in `switch`, and delete the old `switch/RetroRewind.nro` and
`switch/WiiCompiled.nro`: they're no longer updated.

### Your saves

Saves stay on the card when you update. To start from saves you already have, put them there
before the first start:

- **Mario Kart Wii:** a Dolphin or WiiCompiled NAND folder, as `wiicompiled/UserData/NAND`.
- **Retro Rewind:** your `rksys.dat`, as
  `wiicompiled/RetroRewind/riivolution/save/RetroWFC/RMCP/rksys.dat`.

## Updates

Open KaiKart and choose **Check for updates**. It looks for a newer KaiKart release on GitHub,
and offers to download it: first the new game builds, then the launcher itself.

**Retro Rewind updates** come in two kinds:

- **Updates that change Retro Rewind's code** need a new KaiKart release, because the game's code
  is translated ahead of time. Until there is one, KaiKart keeps playing the version it supports,
  and "Check for updates" says a newer Retro Rewind is out.
- **Each KaiKart release** installs the Retro Rewind version it supports the next time you start
  Retro Rewind, with Retro Rewind's own small update files when it can. Files you added to the
  `RetroRewind6` folder, such as saved ghosts, are kept.

## Controls and multiplayer

Up to four players can race in split screen. Each player can use a Pro Controller, a pair of
Joy-Cons, or a single Joy-Con held sideways.

**For local multiplayer, set up the controllers first**, in the console's HOME Menu under
Controllers → Change Grip/Order. Hold each single Joy-Con sideways and press SL + SR on it. The
order on that screen is the player order in the game.

Mario Kart Wii is played here with a GameCube controller's buttons, and each Switch 2 controller
stands in for one. The port menu uses the GameCube names too, so they're in the first column:

| GameCube button | Action | Pro Controller, Joy-Con pair | Right Joy-Con, sideways | Left Joy-Con, sideways |
|---|---|---|---|---|
| A | Accelerate, OK | A | X | Right arrow |
| B | Brake, back | B | A | Down arrow |
| X | Look back (same as Z) | X | – | – |
| Y | Not used in races | Y | – | – |
| Z | Look back | R | B | Left arrow |
| L | Use item | ZL | SL | SL |
| R | Drift, hop | ZR | SR | SR |
| Start | Pause | + | + | − |
| D-pad | Tricks | D-pad | Y, with the stick for the direction | Up arrow, with the stick for the direction |
| Stick | Steer | Left stick | Stick | Stick |

A single Joy-Con has no spare buttons for X and Y, so it looks back with Z. Each player can change
their buttons in the [port menu](#the-port-menu).

**Split screen and frame rate:** for races with two or more players, set the frame rate to 30 FPS
in Retro Rewind's settings. At 60 FPS, split screen asks more than the console has and runs at
about 30-38 FPS, unevenly.

## The port menu

Open it during the game with **L + R + right stick press**, or **SL + SR + stick press** on a
single Joy-Con. Move with the D-pad or the stick, change a setting with left and right, press A to
choose and B to close. Everything here is saved.

| Setting | Default | Notes |
|---|---|---|
| Resolution | 1.0× | Change it in menus, not during a race. |
| Show FPS | Off | |
| Volume | 100% | |
| CPU clock | 1702 MHz | The system's own rate is 1100.8 MHz. |
| Memory clock | System | 2133 MHz handheld. Raised rates apply only on a charger. |
| GPU clock | System | 561 MHz handheld. Raised rates help at 2× and 3× resolution; only on a charger. |
| Controls for | Player 1 | Whose buttons the rows below show, with their controller. |
| Buttons | See [Controls](#controls-and-multiplayer) | Press A on a row, then the new button. Pressing nothing for 10 seconds keeps the old one. |
| Reset buttons to default | | For the player shown. |
| Quit to HOME Menu | | Press A twice. Holding **+ and −** together also quits. |

Below the clocks, the menu shows whether the console is on a charger.

## Known issues

- **One game per kaiHEN session.** Starting a second game in the same session crashes, so each
  game quits to the HOME Menu, which closes the app, instead of returning to hbmenu. To play
  again, hold R while starting a game to open hbmenu. If a second game is started anyway, it says
  so and quits instead of crashing.
- **Raised memory and GPU clocks need a charger.** On battery, the console halves the CPU's real
  speed when memory or the GPU run above their handheld rates. So KaiKart runs them as you chose
  only while plugged in, and at the handheld rates on battery. The CPU keeps its rate either way.
- **Each launch spends about 15-20 seconds setting up its saved shaders** on the title screen. New
  places also stutter the first time while their shaders build. They're saved after that.
- **PAL discs only** (`RMCP01`) for now. US and Japanese discs need their own game builds.
- **Modified discs don't work.** The game's code is translated from the original disc, so KaiKart
  checks that the disc's code is unchanged. Mods that change files on the disc, such as texture
  packs, aren't supported yet.
- **A thin black line can show below the race minimap** at some resolutions (1× and 2×, not 1.5×).
  It's an upstream WiiCompiled bug,
  [patchzyy/Wiicompiled#397](https://github.com/patchzyy/Wiicompiled/issues/397).
- **Change the resolution outside races.** Changing it during a race can break the graphics until
  you restart the game.

If something goes wrong, `wiicompiled/UserData/Logs/` on the card has the logs: `kaikart.log` from
the launcher, and `kaihen.log` from the last game launch.

## Building from source

You don't need this to play. It's for working on KaiKart, or for building it yourself instead of
downloading a release. Expect an hour or two for the first build; most of it runs on its own.

### What you need

- **An Apple Silicon Mac.** The scripts should also work on aarch64 Linux, but I haven't tried.
- [Docker Desktop](https://www.docker.com/products/docker-desktop/) with its memory set to at
  least 8 GB (Settings → Resources), Git and Python 3 (both come with `xcode-select --install`),
  and about 40 GB of free space.
- **Your own Mario Kart Wii PAL disc (`RMCP01`)**, and for Retro Rewind its latest
  `RetroRewind6` folder.

### Get this repository

```sh
mkdir -p ~/Switch2 && cd ~/Switch2
git clone --recursive --branch switch2-kaihen https://github.com/6rimson/KaiKart.git
cd KaiKart
```

`--recursive` also fetches aurora, the graphics library WiiCompiled includes. Run every command
below from this folder.

### The easy way

Run `kaihen/easy-build.sh` in Terminal (or drag the file into a Terminal window and press Return).
It asks for:

- Retro Rewind or plain Mario Kart Wii;
- your disc (drag the file into the window);
- your `RetroRewind6` folder;
- your microSD card, once the build is done.

It extracts and checks the disc, downloads the Retro WFC payload, builds the game and KaiKart, and
copies everything to the card. If something fails, or after an update, run it again: it skips
what's already done. The build's log is in `build/build-log.txt`.

The steps below do the same by hand.

### 1. Put your files in place

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

### 2. Build

```sh
kaihen/run.sh setup
```

The first time, this downloads and builds what it needs: the Switch 2 devkit (from GitHub), the
build tools image, and Dawn and SDL3. Then it translates your game, builds it, and builds the
launcher. **You should see** `kaihen/dist/RetroRewind.nro` and `kaihen/dist/KaiKart.nro` at the
end.

`setup` is five steps you can also run one at a time: `sources`, `translate`, `configure`, `build`
and `launcher`. If your Mac runs out of memory partway, run `KAIHEN_JOBS=4 kaihen/run.sh build`.

For plain Mario Kart Wii, which needs only the disc, put `KAIHEN_PRODUCT=WiiCompiled` in front of
each command, for example `KAIHEN_PRODUCT=WiiCompiled kaihen/run.sh setup`.

### 3. Put it on the microSD card

Put the card in the Mac. macOS shows it under `/Volumes/` with the card's name (the name Finder
shows; `ls /Volumes` lists them). With your card's name in place of `SWITCH`:

```sh
kaihen/run.sh sd /Volumes/SWITCH
```

This copies KaiKart to `switch/`, the game build to `wiicompiled/apps/`, and your extracted disc
and Retro Rewind folder to `wiicompiled/`. The first time takes a while. To start from saves you
already have, add `--nand /path/to/NAND` or `--rr-save /path/to/rksys.dat` after the card.

It never overwrites your files: folders and settings already on the card are left as they are,
and only the `.nro` files, `dsp_coef.bin` and `cacert.pem` are replaced. An interrupted copy is
redone the next time you run it.

### Updating a build

```sh
git pull --recurse-submodules
kaihen/run.sh build
kaihen/run.sh launcher
kaihen/run.sh sd /Volumes/SWITCH
```

`--recurse-submodules` also moves aurora to the version this one expects. If the release notes
say a step such as `translate` is needed, run it before `build`.

After a Retro Rewind update that changes its code, replace the `RetroRewind6` folder in
`PulsarPacks/completed/RetroRewind/`, run `kaihen/run.sh translate` before `build`, and add
`--replace-rr` to the `sd` command.

### Release builds

`kaihen/run.sh release` builds both games and the launcher with the repository's icons, and
writes `kaihen/dist/release/` with `KaiKart-<version>.nro`, `RetroRewind-<version>.nro`,
`WiiCompiled-<version>.nro` and `SHA256SUMS`. The launcher's updates read these names from the
GitHub release.

### Settings for testing

These go in `wiicompiled/settings.env` on the card, with the clocks the port menu saves:

- `MKW_PERF_STATS=1` logs FPS and frame timing to `wiicompiled/UserData/Logs/kaihen.log`.
- `KAIHEN_PROFILE=1` samples the game during races into `UserData/Logs/profile.bin`, for speed
  work.

## How it's built

- **The games:** WiiCompiled's translated game and runtime, compiled with Clang for the devkit's
  newlib and libounce, linked by its GCC, with the shipped PGO profile (`kaihen/pgo/`).
- **The launcher** (`kaihen/launcher/`): a small libnx app drawn in software, which sets up the
  games (`kaihen/shared/`) and starts them through kaiHEN's loader.
- **`kaihen/platform/`:** the layer between the game and the console: startup, the screen,
  threads on their own cores, clocks, the power state and quitting.
- **`kaihen/patches/`:** one patch each for Dawn, SDL3 (with audio and controller drivers on
  libnx) and aurora. `kaihen/dependencies.json` pins the devkit, Dawn and SDL3.
- **The scripts:** `kaihen/easy-build.sh` asks the questions and calls `kaihen/run.sh`, which
  starts Docker and runs `kaihen/docker/build.sh` inside it for each step.
- **Docker volumes:** `wiicompiled-kaihen-work` (sources and the translated game) and
  `wiicompiled-kaihen` (the build). `KAIHEN_WORK` and `KAIHEN_VOLUME` pick others.

## Credits

- **[WiiCompiled](https://github.com/patchzyy/Wiicompiled)** by patchzyy and contributors: the
  recompilation this is built on.
- **[kaiHEN](https://github.com/ouncebrew/kaihen)** by kurumuz: the Switch 2 homebrew environment
  and hbmenu this runs in.
- **[ounce-devkit](https://github.com/ouncebrew/ounce-devkit)**, libounce and OunceVK by
  ouncebrew: the Switch 2 toolchain, system library and Vulkan driver.
- **[DriftDroid](https://github.com/dorPXP/driftdroid)** by dorPXP: the disc image importer
  (`kaihen/shared/wii_disc_extractor.cpp`, GPLv3), and its Switch 1 port's fixes and ideas.
- **[KartPadNX](https://github.com/KawaiiBunga/KartPadNX)** by KawaiiBunga: a Switch 1 port whose
  ideas (such as PGO) helped throughout.
- **RavensClawX's [Twilight Princess](https://github.com/ravensclawx/TwilightPrincess-HBLauncher)
  port**: parts of the Dawn and aurora patches, and the layout of the SDL drivers (MIT,
  `kaihen/patches/LICENSE.tp-integration`).
- **[Retro Rewind](https://wiki.tockdom.com/wiki/Retro_Rewind)** by ZPL and team, and
  **[Retro WFC](https://rwfc.net)**.
- **[aurora](https://github.com/encounter/aurora)** by encounter,
  **[Dawn](https://dawn.googlesource.com/dawn)** by Google, **[SDL](https://github.com/libsdl-org/SDL)**,
  [mbed TLS](https://github.com/Mbed-TLS/mbedtls), stb_truetype and libjpeg-turbo.

WiiCompiled is GPL-3.0 (see `../LICENSE`), and so is this port. See
[THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md). "Nintendo Switch 2" and "Mario Kart Wii" are
trademarks of Nintendo; this project is not affiliated with or endorsed by Nintendo.
