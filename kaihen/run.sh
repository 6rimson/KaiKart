#!/bin/bash
# WiiCompiled + Retro Rewind as a kaiHEN app (.nro), built in Docker. Guide: kaihen/README.md.
#   kaihen/run.sh setup        everything below in order: sources, translate, configure, build
#   kaihen/run.sh sources      fetch Dawn and SDL3 at their pins (kaihen/dependencies.json), patched
#   kaihen/run.sh translate    translate your game (and Retro Rewind, with Retro WFC)
#   kaihen/run.sh configure    configure the CMake build
#   kaihen/run.sh build        build and make the .nro (kaihen/dist/)
#   kaihen/run.sh launcher     build the KaiKart launcher (kaihen/dist/KaiKart.nro), after a build
#   kaihen/run.sh sd /Volumes/CARD [--nand DIR] [--rr-save FILE]
#                              put the .nro and the game's files on a microSD card (kaihen/sd.py)
#   kaihen/run.sh release      build the launcher and both games for a release: kaihen/dist/release/
#                              (KaiKart-, RetroRewind-, WiiCompiled-<version>.nro) with SHA256SUMS
# KAIHEN_PRODUCT: RetroRewind (default) or WiiCompiled (plain Mario Kart Wii).
# KAIHEN_JOBS: parallel jobs (6 fits 8 GB of Docker memory). KAIHEN_PGO=0: no profile.
# KAIHEN_COMPILER=gcc: the devkit's GCC instead of Clang, without PGO.
# Docker images, built the first time they are needed: ounce-devkit (the Switch 2 devkit, from its
# repository at the pinned commit), the tools image (kaihen/docker/Dockerfile.tools: Clang, .NET for
# the translator) and ounce-devkit-clang (the two together, kaihen/docker/Dockerfile.clang).
# Volumes: KAIHEN_WORK holds the sources and the translated game (default wiicompiled-kaihen-work,
# or wiicompiled-kaihen-work-mkw for plain Mario Kart Wii); KAIHEN_VOLUME holds the build (default
# wiicompiled-kaihen).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
export KAIHEN_COMPILER="${KAIHEN_COMPILER:-clang}"
export KAIHEN_PRODUCT="${KAIHEN_PRODUCT:-RetroRewind}"
case "$KAIHEN_PRODUCT" in
  RetroRewind) WORK="${KAIHEN_WORK:-wiicompiled-kaihen-work}" ;;
  WiiCompiled) WORK="${KAIHEN_WORK:-wiicompiled-kaihen-work-mkw}" ;;
  *) echo "KAIHEN_PRODUCT is RetroRewind or WiiCompiled" >&2; exit 2 ;;
esac
action="${1:?usage: kaihen/run.sh setup|sources|translate|configure|build|launcher|sd|release|shell}"

if [[ "$action" == sd ]]; then
  shift
  exec python3 "$HERE/sd.py" "$@"
fi
# Both apps as released: the card's template folder as committed (it goes into the apps), the icons
# in kaihen/icons (never a local icon-*.jpg), and both work volumes already translated.
if [[ "$action" == release ]]; then
  if [[ -n "$(git -C "$REPO" status --porcelain --ignored -- kaihen/card | grep -v '\.DS_Store$')" ]]; then
    echo "kaihen/card has local changes or extra files; they would go into the apps. Commit or remove them first." >&2
    exit 1
  fi
  version="${KAIHEN_VERSION:-$(sed -n 's/^VERSION="\${KAIHEN_VERSION:-\([0-9.]*\)}"$/\1/p' "$HERE/build.sh")}"
  out="$HERE/dist/release"
  rm -rf "$out"
  mkdir -p "$out"
  for product in RetroRewind WiiCompiled; do
    echo "== $product"
    KAIHEN_PRODUCT=$product KAIHEN_RELEASE=1 KAIHEN_VERSION="$version" "$0" build
    cp "$HERE/dist/$product.nro" "$out/$product-$version.nro"
    if [[ "$product" == RetroRewind ]]; then
      echo "== KaiKart"
      KAIHEN_RELEASE=1 KAIHEN_VERSION="$version" "$0" launcher
      cp "$HERE/dist/KaiKart.nro" "$out/KaiKart-$version.nro"
    fi
  done
  (cd "$out" && shasum -a 256 ./*.nro | sed 's# \./# #' > SHA256SUMS && cat SHA256SUMS)
  exit 0
fi
if [[ "$action" == setup ]]; then
  shift
  for step in sources translate configure build; do
    echo "== kaihen/run.sh $step"
    "$0" "$step" "$@"
  done
  exit 0
fi

pin() {
  python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]][sys.argv[3]])" \
    "$HERE/dependencies.json" "$1" "$2"
}

# The Switch 2 devkit image, from its repository at the tested commit.
if ! docker image inspect ounce-devkit >/dev/null 2>&1; then
  checkout="$HERE/deps/ounce-devkit"
  [[ -d "$checkout/.git" ]] || git clone -q "$(pin ounce-devkit repository).git" "$checkout"
  git -C "$checkout" checkout -q --detach "$(pin ounce-devkit commit)"
  docker build -t ounce-devkit "$checkout"
fi
# The tools image with the devkit: it translates, and builds with Clang.
CLANG_IMAGE="${KAIHEN_CLANG_IMAGE:-ounce-devkit-clang}"
if ! docker image inspect "$CLANG_IMAGE" >/dev/null 2>&1; then
  TOOLS_IMAGE="wiicompiled-kaihen-tools:$(shasum -a 256 "$HERE/docker/Dockerfile.tools" | cut -c1-12)"
  docker image inspect "$TOOLS_IMAGE" >/dev/null 2>&1 ||
    docker build -f "$HERE/docker/Dockerfile.tools" -t "$TOOLS_IMAGE" "$HERE/docker"
  docker build -f "$HERE/docker/Dockerfile.clang" --build-arg TOOLS_IMAGE="$TOOLS_IMAGE" -t "$CLANG_IMAGE" "$HERE/docker"
fi
IMAGE="$CLANG_IMAGE"
if [[ "$KAIHEN_COMPILER" == gcc && "$action" =~ ^(configure|build|shell)$ ]]; then
  IMAGE=ounce-devkit
fi

TTY=(); [[ -t 0 ]] && TTY=(-it)
mkdir -p "$HERE/dist"
# sources and translate write the work volume (and translate reads your game files through the
# repository); configure and build only read it.
HOST=ro WORK_MODE=ro
case "$action" in sources|translate) HOST=rw WORK_MODE=rw ;; esac
exec docker run --rm ${TTY[@]+"${TTY[@]}"} \
  -v "$REPO:/host:$HOST" -v "$WORK:/opt/work:$WORK_MODE" \
  -v "${KAIHEN_VOLUME:-wiicompiled-kaihen}:/opt/kaihen" -v "$HERE/dist:/dist" \
  -e KAIHEN_JOBS -e KAIHEN_PRODUCT -e KAIHEN_COMPILER -e KAIHEN_PGO -e KAIHEN_VERSION -e KAIHEN_RELEASE -e KAIHEN_UPDATE_REPO \
  "$IMAGE" bash /host/kaihen/build.sh "$@"
