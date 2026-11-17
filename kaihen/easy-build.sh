#!/bin/bash
# Builds KaiKart and a game from your own disc and puts them on a microSD card (Apple Silicon Mac).
# Run it in Terminal: kaihen/easy-build.sh (or drag this file into a Terminal window and press
# Return). It asks for your disc, your Retro Rewind folder and the card, then runs the steps in
# kaihen/README.md. Run it again at any time: steps already done are skipped, so it also picks up
# after a failure or an update.
set -euo pipefail
cd "$(dirname "$0")/.."
REPO="$PWD"
RUN="$REPO/kaihen/run.sh"
LOG="$REPO/build/build-log.txt"
NODTOOL="$REPO/build/tools/nodtool"
NODTOOL_URL="https://github.com/encounter/nod/releases/download/v2.0.0-alpha.10/nodtool-macos-arm64"
NODTOOL_SHA256="e23ca466999b720c55e6d29c9683fce8cc74451ba64ead2e543d50129f24528a"
MAIN_DOL_SHA256="80d18895b39c63bd80f457398bfcbb91b7d16ac116a41a88967e954080155b05"
STATICR_SHA256="16d9d146112541fefea701ecb5bc1a496f9d50e4a752fbb5b6778e7c6399f67d"
WFC_URL="https://rwfc.net/api/wfc/payload?g=RMCPD00"
DISC="$REPO/Assets/DATA"
RR="$REPO/PulsarPacks/completed/RetroRewind/RetroRewind6"
WFC="$REPO/build/retro-wfc/binary/payload.RMCPD00.bin"

bold() { printf '\n\033[1m%s\033[0m\n' "$*"; }
note() { printf '  %s\n' "$*"; }
fail() { printf '\n\033[31m%s\033[0m\n' "$*" >&2; printf '\nPress Return to close.'; read -r _ || true; exit 1; }
ask() { local answer; printf '%s ' "$1" >&2; read -r answer || exit 1; printf '%s' "$answer"; }
confirm() { case "$(ask "$1 [y/N]")" in [yY]*) return 0 ;; *) return 1 ;; esac; }
sha() { { shasum -a 256 "$1" 2>/dev/null || true; } | cut -d' ' -f1; }
# A path dragged into Terminal comes with backslashes before spaces, or in quotes.
dragged() {
  python3 -c '
import shlex, sys
try:
    parts = shlex.split(sys.argv[1])
except ValueError:
    parts = [sys.argv[1].strip()]
print(parts[0] if parts else "")' "$1"
}

printf '\033]0;KaiKart\007'
bold "KaiKart"
note "This builds KaiKart and a game from your own disc and puts them on your microSD card."
note "The first build takes an hour or two. You can stop it (close the window) and run this again."

# --- What the Mac needs -------------------------------------------------------------------------
bold "Checking your Mac"
[[ "$(uname -m)" == arm64 ]] || fail "This needs an Apple Silicon Mac."
xcode-select -p >/dev/null 2>&1 || fail "Git and Python 3 are missing. Run this in Terminal, then try again:
  xcode-select --install"
[[ -f aurora-main/CMakeLists.txt ]] || {
  note "Fetching aurora (the graphics library)..."
  git submodule update --init --recursive || fail "Couldn't fetch aurora. Check your internet connection."
}
command -v docker >/dev/null || fail "Docker Desktop is missing. Install it from
  https://www.docker.com/products/docker-desktop/
then set its memory to at least 8 GB (Settings, Resources) and run this again."
if ! docker info >/dev/null 2>&1; then
  note "Starting Docker Desktop..."
  open -a Docker || true
  for _ in $(seq 60); do docker info >/dev/null 2>&1 && break; sleep 2; done
  docker info >/dev/null 2>&1 || fail "Docker Desktop didn't start. Open it yourself, wait until it's running, then run this again."
fi
memory_gb=$(( $(docker info --format '{{.MemTotal}}') / 1000000000 ))
if (( memory_gb < 7 )); then
  fail "Docker has only ${memory_gb} GB of memory. Set it to at least 8 GB in Docker Desktop
(Settings, Resources), then run this again."
fi
# About 0.75 GB per compile job; 8 GB gives 6.
export KAIHEN_JOBS="${KAIHEN_JOBS:-$(( memory_gb * 3 / 4 ))}"
free_gb=$(df -g "$REPO" | awk 'NR==2 {print $4}')
# Only the first build needs the room; later ones reuse its Docker volumes.
if (( free_gb < 40 )) && ! docker volume inspect "${KAIHEN_VOLUME:-wiicompiled-kaihen}" >/dev/null 2>&1; then
  note "Only ${free_gb} GB free on this disk; the first build needs about 40 GB."
  confirm "Try anyway?" || exit 1
fi
note "Docker ${memory_gb} GB, ${KAIHEN_JOBS} build jobs, ${free_gb} GB free."

# --- Which game -----------------------------------------------------------------------------------
bold "Which game?"
note "1) Retro Rewind (default)"
note "2) Plain Mario Kart Wii"
case "$(ask "Choose 1 or 2:")" in
  2) export KAIHEN_PRODUCT=WiiCompiled ;;
  *) export KAIHEN_PRODUCT=RetroRewind ;;
esac
retro_rewind() { [[ "$KAIHEN_PRODUCT" == RetroRewind ]]; }
changed=0 rr_updated=0

# --- Your disc ------------------------------------------------------------------------------------
disc_ready() {
  [[ -f "$DISC/sys/fst.bin" ]] || return 1
  if [[ "$(sha "$REPO/Assets/main.dol")" != "$MAIN_DOL_SHA256" || "$(sha "$REPO/Assets/StaticR.rel")" != "$STATICR_SHA256" ]]; then
    # An extracted disc without the two copies the translator reads.
    [[ "$(sha "$DISC/sys/main.dol")" == "$MAIN_DOL_SHA256" && "$(sha "$DISC/files/rel/StaticR.rel")" == "$STATICR_SHA256" ]] || return 1
    cp "$DISC/sys/main.dol" "$DISC/files/rel/StaticR.rel" "$REPO/Assets/"
  fi
}
get_nodtool() {
  [[ "$(sha "$NODTOOL")" == "$NODTOOL_SHA256" ]] && return
  note "Downloading nodtool (extracts the disc)..."
  mkdir -p "$(dirname "$NODTOOL")"
  curl -fsSL --retry 3 "$NODTOOL_URL" -o "$NODTOOL.partial" || fail "Couldn't download nodtool."
  [[ "$(sha "$NODTOOL.partial")" == "$NODTOOL_SHA256" ]] || { rm -f "$NODTOOL.partial"; fail "nodtool's download didn't match its known checksum."; }
  chmod +x "$NODTOOL.partial" && mv "$NODTOOL.partial" "$NODTOOL"
}
bold "Your disc"
if disc_ready; then
  note "Already in place (Assets/), a clean PAL disc."
else
  note "Drag your Mario Kart Wii PAL disc (RMCP01) into this window and press Return."
  note "An .iso, .wbfs, .rvz or similar file, or a folder you already extracted."
  while :; do
    source_path="$(dragged "$(ask ">")")"
    [[ -e "$source_path" ]] && break
    note "Not found: '$source_path'. Try dragging it again."
  done
  partial="$DISC.partial"
  rm -rf "$partial"
  mkdir -p "$REPO/Assets"
  if [[ -d "$source_path" ]]; then
    [[ -f "$source_path/sys/fst.bin" ]] || fail "That folder isn't an extracted disc (it needs sys/ and files/)."
    note "Copying the folder..."
    cp -Rc "$source_path" "$partial" 2>/dev/null || cp -R "$source_path" "$partial"
  else
    get_nodtool
    note "Extracting the disc (a few minutes)..."
    "$NODTOOL" extract "$source_path" "$partial" >/dev/null || { rm -rf "$partial"; fail "nodtool couldn't extract that file."; }
  fi
  if [[ "$(sha "$partial/sys/main.dol")" != "$MAIN_DOL_SHA256" || "$(sha "$partial/files/rel/StaticR.rel")" != "$STATICR_SHA256" ]]; then
    rm -rf "$partial"
    fail "That isn't a clean, unmodified PAL disc (RMCP01). Only the PAL version works."
  fi
  if [[ -e "$DISC" ]]; then rm -rf "$DISC.previous"; mv "$DISC" "$DISC.previous"; fi
  mv "$partial" "$DISC"
  cp "$DISC/sys/main.dol" "$DISC/files/rel/StaticR.rel" "$REPO/Assets/"
  rm -rf "$DISC.previous"
  note "Done: a clean PAL disc."
  changed=1
fi

# --- Retro Rewind ---------------------------------------------------------------------------------
if retro_rewind; then
  bold "Retro Rewind"
  use_new=1
  if [[ -f "$RR/Binaries/Code.pul" ]]; then
    note "Found Retro Rewind $(head -c 32 "$RR/version.txt" 2>/dev/null || echo "(unknown version)")."
    confirm "Replace it with a newer version?" || use_new=0
  fi
  if [[ ! -f "$RR/Binaries/Code.pul" || $use_new == 1 ]]; then
    note "Drag your RetroRewind6 folder (the one with Binaries/Code.pul) into this window and press Return."
    while :; do
      source_path="$(dragged "$(ask ">")")"
      [[ -f "$source_path/RetroRewind6/Binaries/Code.pul" ]] && source_path="$source_path/RetroRewind6"
      [[ -f "$source_path/Binaries/Code.pul" ]] && break
      note "That isn't a RetroRewind6 folder (no Binaries/Code.pul). Try again."
    done
    note "Copying Retro Rewind..."
    mkdir -p "$(dirname "$RR")"
    rm -rf "$RR.partial"
    cp -Rc "$source_path" "$RR.partial" 2>/dev/null || cp -R "$source_path" "$RR.partial"
    [[ -e "$RR" ]] && rr_updated=1
    rm -rf "$RR" && mv "$RR.partial" "$RR"
    note "Done: Retro Rewind $(head -c 32 "$RR/version.txt" 2>/dev/null)."
    changed=1
  fi
  if [[ ! -f "$WFC" || $rr_updated == 1 ]]; then
    note "Downloading the Retro WFC payload (online play)..."
    mkdir -p "$(dirname "$WFC")"
    curl -fsSL --retry 3 "$WFC_URL" -o "$WFC.partial" || fail "Couldn't download the Retro WFC payload from rwfc.net."
    mv "$WFC.partial" "$WFC"
    changed=1
  fi
fi

# --- Build ----------------------------------------------------------------------------------------
NRO="$REPO/kaihen/dist/$KAIHEN_PRODUCT.nro"
LAUNCHER="$REPO/kaihen/dist/KaiKart.nro"
bold "Building"
build=1
if [[ -f "$NRO" && -f "$LAUNCHER" && $changed == 0 ]]; then
  note "Already built: kaihen/dist/$KAIHEN_PRODUCT.nro and KaiKart.nro."
  confirm "Build it again? (only needed after you update this repository)" || build=0
fi
if (( build )); then
  note "This takes a while. The full log is in build/build-log.txt."
  mkdir -p "$(dirname "$LOG")"
  printf '\n==== %s, %s\n' "$(date)" "$KAIHEN_PRODUCT" >> "$LOG"
  "$RUN" setup < /dev/null 2>&1 | tee -a "$LOG" ||
    fail "The build stopped with an error. The end of build/build-log.txt says why.
If it mentions memory (\"Killed\", \"signal 9\"), give Docker more memory and run this again."
  [[ -f "$NRO" && -f "$LAUNCHER" ]] ||
    fail "The build finished but kaihen/dist/$KAIHEN_PRODUCT.nro or KaiKart.nro is missing; see build/build-log.txt."
fi

# --- microSD card ---------------------------------------------------------------------------------
bold "Your microSD card"
while :; do
  cards=()
  for volume in /Volumes/*; do
    [[ -d "$volume" && ! -L "$volume" ]] || continue
    case "$(basename "$volume")" in "Macintosh HD"|Recovery|Docker|"Preboot"|"VM") continue ;; esac
    cards+=("$volume")
  done
  if (( ${#cards[@]} == 0 )); then
    case "$(ask "Put the card in the Mac, then press Return (or type s to skip):")" in [sS]*) cards=(); break ;; esac
    continue
  fi
  for i in "${!cards[@]}"; do note "$((i + 1))) ${cards[$i]}"; done
  choice="$(ask "Which one is the card? Type its number (r to look again, s to skip):")"
  case "$choice" in
    [sS]*) cards=(); break ;;
    [rR]*|"") continue ;;
  esac
  if [[ "$choice" =~ ^[0-9]+$ ]] && (( choice >= 1 && choice <= ${#cards[@]} )); then
    card="${cards[$((choice - 1))]}"
    break
  fi
done
if (( ${#cards[@]} == 0 )); then
  note "Skipped. Run this again when the card is in the Mac."
else
  sd_args=("$card")
  if [[ ! -d "$card/wiicompiled/UserData" ]]; then
    note "Starting fresh on this card. To bring saves you already have, drag in a NAND folder"
    note "(Dolphin or WiiCompiled)$(retro_rewind && printf ' or a Retro Rewind rksys.dat'). Press Return when done, or to skip."
    while :; do
      save="$(dragged "$(ask ">")")"
      [[ -z "$save" ]] && break
      if [[ -d "$save" ]]; then sd_args+=(--nand "$save"); note "NAND: $save"
      elif [[ -f "$save" ]] && retro_rewind; then sd_args+=(--rr-save "$save"); note "Retro Rewind save: $save"
      else note "Not found or not usable: '$save'."; fi
    done
  fi
  (( rr_updated )) && sd_args+=(--replace-rr)
  note "Copying to the card (the first time takes a while)..."
  "$RUN" sd "${sd_args[@]}" || fail "Copying to the card failed (see above)."
  bold "Done"
  note "Eject the card in Finder, put it in your Switch 2, start kaiHEN, open KaiKart from"
  note "hbmenu and choose $( retro_rewind && echo "Retro Rewind" || echo "Mario Kart Wii")."
fi
printf '\nPress Return to close.'
read -r _ || true
