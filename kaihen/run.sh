#!/bin/bash
# Build WiiCompiled + Retro Rewind as a kaiHEN app (.nro), in the Switch 2 devkit's Docker image
# (github.com/ouncebrew/ounce-devkit: `docker build -t ounce-devkit .` in its checkout).
#   kaihen/run.sh configure   configure the CMake build
#   kaihen/run.sh build       build and make the .nro (kaihen/dist/)
# The translated game and the patched Dawn and SDL3 sources come from the HBLauncher build volume
# (wiicompiled-hbx, read only): run hblauncher/docker/run.sh deps and translate --online first. This build's own files live in the volume
# wiicompiled-kaihen (KAIHEN_VOLUME).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
IMAGE="${KAIHEN_IMAGE:-ounce-devkit}"
docker image inspect "$IMAGE" >/dev/null 2>&1 || {
  echo "Build the devkit image first: docker build -t ounce-devkit . (in the ounce-devkit checkout)" >&2
  exit 1
}
TTY=(); [[ -t 0 ]] && TTY=(-it)
mkdir -p "$HERE/dist"
exec docker run --rm ${TTY[@]+"${TTY[@]}"} \
  -v "$REPO:/host:ro" -v "${HBX_VOLUME:-wiicompiled-hbx}:/opt/work:ro" \
  -v "${KAIHEN_VOLUME:-wiicompiled-kaihen}:/opt/kaihen" -v "$HERE/dist:/dist" -e KAIHEN_JOBS -e KAIHEN_PRODUCT \
  "$IMAGE" bash /host/kaihen/build.sh "$@"
