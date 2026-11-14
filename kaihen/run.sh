#!/bin/bash
# Build WiiCompiled + Retro Rewind as a kaiHEN app (.nro). Clang compiles, with the shipped PGO
# profile (hblauncher/pgo), in the image kaihen/docker/Dockerfile.clang makes from the HBLauncher build
# image and the Switch 2 devkit's (github.com/ouncebrew/ounce-devkit: `docker build -t ounce-devkit .`
# in its checkout). KAIHEN_COMPILER=gcc builds with the devkit's GCC instead, without PGO.
#   kaihen/run.sh configure   configure the CMake build
#   kaihen/run.sh build       build and make the .nro (kaihen/dist/)
# KAIHEN_JOBS: parallel jobs (6 fits 8 GB of Docker memory); KAIHEN_PGO=0: no profile.
# The translated game and the patched Dawn and SDL3 sources come from the HBLauncher build volume
# (wiicompiled-hbx, read only): run hblauncher/docker/run.sh deps and translate --online first. This build's own files live in the volume
# wiicompiled-kaihen (KAIHEN_VOLUME).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
export KAIHEN_COMPILER="${KAIHEN_COMPILER:-clang}"
docker image inspect ounce-devkit >/dev/null 2>&1 || {
  echo "Build the devkit image first: docker build -t ounce-devkit . (in the ounce-devkit checkout)" >&2
  exit 1
}
if [[ "$KAIHEN_COMPILER" == clang ]]; then
  IMAGE="${KAIHEN_IMAGE:-ounce-devkit-clang}"
  if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    HBX_IMAGE="wiicompiled-hbx-build:$(shasum -a 256 "$REPO/hblauncher/docker/Dockerfile" | cut -c1-12)"
    docker image inspect "$HBX_IMAGE" >/dev/null 2>&1 || {
      echo "Build the HBLauncher image first (hblauncher/docker/run.sh builds it)" >&2
      exit 1
    }
    docker build -f "$HERE/docker/Dockerfile.clang" --build-arg HBX_IMAGE="$HBX_IMAGE" -t "$IMAGE" "$HERE/docker"
  fi
else
  IMAGE="${KAIHEN_IMAGE:-ounce-devkit}"
fi
TTY=(); [[ -t 0 ]] && TTY=(-it)
mkdir -p "$HERE/dist"
exec docker run --rm ${TTY[@]+"${TTY[@]}"} \
  -v "$REPO:/host:ro" -v "${HBX_VOLUME:-wiicompiled-hbx}:/opt/work:ro" \
  -v "${KAIHEN_VOLUME:-wiicompiled-kaihen}:/opt/kaihen" -v "$HERE/dist:/dist" -e KAIHEN_JOBS -e KAIHEN_PRODUCT -e KAIHEN_COMPILER -e KAIHEN_PGO -e KAIHEN_VERSION \
  "$IMAGE" bash /host/kaihen/build.sh "$@"
