#!/bin/bash
# Runs in the ounce-devkit-clang or ounce-devkit image (kaihen/run.sh). /host: the repository (read only); /opt/work: the
# HBLauncher build volume (read only: the translated game and the patched Dawn and SDL3 sources);
# /opt/kaihen: this build's volume; /dist: kaihen/dist on the host.
set -euo pipefail
source "$DEVKITPRO/switch2vars.sh" >/dev/null
PRODUCT="${KAIHEN_PRODUCT:-RetroRewind}"
COMPILER="${KAIHEN_COMPILER:-clang}"
SRC=/opt/kaihen/src
OUT="/opt/kaihen/build-$COMPILER"
JOBS="${KAIHEN_JOBS:-$(nproc)}"

sync_tree() {
  mkdir -p "$SRC"
  rsync -a --delete --exclude .git --exclude generated --exclude build --exclude "build-*" \
    --exclude Assets --exclude PulsarPacks --exclude hblauncher/deps --exclude hblauncher/build \
    --exclude hblauncher/dist --exclude hblauncher/fsroot --exclude hblauncher/fsroot-wiicompiled \
    --exclude kaihen/dist --exclude "translator/*/*/bin" --exclude "translator/*/*/obj" /host/ "$SRC/"
  ln -sfn /opt/work/src/generated "$SRC/generated"
  # The same aurora patch series as the HBLauncher build (hblauncher/build.py apply_aurora_patch).
  python3 - "$SRC" <<'EOF'
import re, subprocess, sys
from pathlib import Path
src = Path(sys.argv[1])
build_py = (src / "hblauncher/build.py").read_text()
body = build_py[build_py.index("def apply_aurora_patch"):build_py.index("def link_flags")]
series = [src / "hblauncher/patches" / p for p in
          ["aurora-hbx-switch2.diff"] + re.findall(r'"(aurora-hbx-[a-z0-9-]+\.diff)"', body)]
series.append(src / "kaihen/patches/aurora-kaihen.diff")
for patch in series:
    subprocess.run(["patch", "-p1", "-s", "-N", "-d", str(src / "aurora-main"), "-i", str(patch)], check=True)
print(f"aurora: {len(series)} patches")
EOF
}

# Dawn and SDL3: the HBLauncher build's patched copies, plus this port's patches. Dawn's own Switch
# path (__SWITCH__) presents through VK_NN_vi_surface; its patch is abseil on newlib. SDL3's adds a
# Switch platform section.
deps() {
  mkdir -p /opt/kaihen/deps
  for dep in dawn sdl3; do
    rsync -a --delete --exclude .git "/opt/work/deps/$dep/" "/opt/kaihen/deps/$dep/"
    patch -p1 -s -N -d "/opt/kaihen/deps/$dep" -i "/host/kaihen/patches/$dep-kaihen.diff"
  done
}

# The shipped profile for this product (hblauncher/pgo/<product>.profdata.xz), unpacked under a name
# with its content hash: Clang doesn't track the profile as a dependency, so a new one changes the
# compile command. Clang only.
pgo_options() {
  local name
  case "$PRODUCT" in RetroRewind) name=retro-rewind ;; *) name=wiicompiled ;; esac
  local packed="$SRC/hblauncher/pgo/$name.profdata.xz"
  if [[ "$COMPILER" != clang || "${KAIHEN_PGO:-1}" == 0 || ! -f "$packed" ]]; then
    echo "-DMKW_PGO="
    return
  fi
  mkdir -p /opt/kaihen/pgo
  local hash
  hash=$(xz -dc "$packed" | sha256sum | cut -c1-12)
  local profile="/opt/kaihen/pgo/$name-$hash.profdata"
  [[ -f "$profile" ]] || xz -dc "$packed" > "$profile"
  echo "-DMKW_PGO=use -DMKW_PGO_PROFILE=$profile"
}

configure() {
  sync_tree
  deps
  local pgo clang=OFF
  read -ra pgo <<< "$(pgo_options)"
  [[ "$COMPILER" == clang ]] && clang=ON
  local off=(DAWN_BUILD_PROTOBUF TINT_BUILD_IR_BINARY DAWN_USE_X11 DAWN_USE_WAYLAND DAWN_ENABLE_OPENGLES
    DAWN_ENABLE_DESKTOP_GL TINT_BUILD_GLSL_WRITER TINT_BUILD_HLSL_WRITER TINT_BUILD_MSL_WRITER
    TINT_BUILD_GLSL_VALIDATOR DAWN_USE_GLFW DAWN_SUPPORTS_GLFW_FOR_WINDOWING GLFW_BUILD_WAYLAND GLFW_BUILD_X11
    SDL_X11 SDL_WAYLAND SDL_KMSDRM SDL_OPENGL SDL_OPENGLES SDL_ALSA SDL_PULSEAUDIO SDL_PIPEWIRE SDL_JACK SDL_SNDIO
    SDL_DBUS SDL_IBUS SDL_LIBUDEV SDL_HIDAPI SDL_CAMERA SDL_TESTS SDL_EXAMPLES)
  local options=()
  for o in "${off[@]}"; do options+=("-D$o=OFF"); done
  cmake -S "$SRC/runtime" -B "$OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$SRC/kaihen/cmake/kaihen.cmake" \
    -UCMAKE_C_FLAGS -UCMAKE_CXX_FLAGS -UCMAKE_ASM_FLAGS -UCMAKE_EXE_LINKER_FLAGS \
    -DKAIHEN_CLANG=$clang "${pgo[@]}" \
    -DHBX_SWITCH2=ON -DMKW_KAIHEN=ON -DMKW_GUEST_FLAT_CHECKED_ONLY=ON -DMKW_AARCH64_CPU=cortex-a78c \
    -DAURORA_DAWN_PROVIDER=vendor -DAURORA_DAWN_LINKAGE=static -DFETCHCONTENT_SOURCE_DIR_DAWN=/opt/kaihen/deps/dawn \
    -DAURORA_SDL3_PROVIDER=vendor -DAURORA_SDL3_LINKAGE=static -DFETCHCONTENT_SOURCE_DIR_SDL=/opt/kaihen/deps/sdl3 \
    -DHBX_SDK_INCLUDE=/opt/work/deps/hblauncher/sdk/include \
    -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_OFFSCREEN=ON "${options[@]}" "$@"
}

build() {
  sync_tree
  cmake --build "$OUT" --target "$PRODUCT" --parallel "$JOBS" "$@"
  local elf="$OUT/$PRODUCT.elf"
  # hbmenu's title, author and version, and the icon: kaihen/icon-<product>.jpg (256x256, kept out of
  # git: it is the game's art) when it exists.
  local title="Mario Kart Wii" icon="/host/kaihen/icon-$PRODUCT.jpg"
  [[ "$PRODUCT" == RetroRewind ]] && title="Retro Rewind"
  [[ -f "$icon" ]] || icon="$DEVKITPRO/libounce/default_icon.jpg"
  nacptool --create "$title" "6rimson" "${KAIHEN_VERSION:-0.3.0}" "$OUT/$PRODUCT.nacp"
  elf2nro "$elf" "/dist/$PRODUCT.nro" --nacp="$OUT/$PRODUCT.nacp" --icon="$icon"
  ls -la "/dist/$PRODUCT.nro"
}

action="${1:?usage: build.sh configure|build}"
shift
case "$action" in
  configure) configure "$@" ;;
  build) build "$@" ;;
  shell) sync_tree; bash ;;
  *) echo "unknown action $action" >&2; exit 2 ;;
esac
