#!/bin/bash
# Runs in the ounce-devkit-clang (or ounce-devkit) image, from kaihen/run.sh. /host: the repository
# (read only except for sources and translate); /opt/work: the work volume (Dawn and SDL3 in deps/,
# the translated game in src/); /opt/kaihen: the build volume; /dist: kaihen/dist on the host.
set -euo pipefail
PRODUCT="${KAIHEN_PRODUCT:-RetroRewind}"
COMPILER="${KAIHEN_COMPILER:-clang}"
JOBS="${KAIHEN_JOBS:-$(nproc)}"
PATCHES=/host/kaihen/patches
# Each product has its own copy of the tree and build folder: they link different translations.
SUFFIX=""
[[ "$PRODUCT" == RetroRewind ]] || SUFFIX="-$PRODUCT"
SRC="/opt/kaihen/src$SUFFIX"
OUT="/opt/kaihen/build-$COMPILER$SUFFIX"
WORK_SRC=/opt/work/src

pin() {
  python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]][sys.argv[3]])" \
    /host/kaihen/dependencies.json "$1" "$2"
}

# The repository without build output and game files.
copy_repo() {
  mkdir -p "$1"
  rsync -a --delete --exclude .git --exclude generated --exclude build --exclude "build-*" \
    --exclude Assets --exclude PulsarPacks --exclude kaihen/dist --exclude kaihen/deps \
    --exclude kaihen/platform/kaihen_rr_pin.h \
    --exclude "translator/*/*/bin" --exclude "translator/*/*/obj" /host/ "$1/"
}

# Dawn and SDL3 at their pins with this port's patch, in the work volume. A checkout that is
# already there is put back to the pin first, so a changed patch applies cleanly.
sources() {
  local dep dir commit
  mkdir -p /opt/work/deps
  for dep in dawn sdl3; do
    dir="/opt/work/deps/$dep"
    commit="$(pin "$dep" commit)"
    if [[ ! -d "$dir/.git" ]]; then
      git clone -q --no-checkout --filter=blob:none "$(pin "$dep" repository).git" "$dir"
    fi
    git -C "$dir" checkout -q --force --detach "$commit"
    git -C "$dir" clean -fdq
    git -C "$dir" apply "$PATCHES/$(pin "$dep" patch)"
    echo "$dep at ${commit:0:12}, patched"
  done
}

# The game, translated in the work volume. Your files are read from the repository: Assets/ (the
# disc's main.dol and StaticR.rel), PulsarPacks/ (Retro Rewind) and build/retro-wfc (the Retro WFC
# payload). The translator writes absolute paths, so the translation stays at /opt/work/src.
translate() {
  copy_repo "$WORK_SRC"
  mkdir -p "$WORK_SRC/build"
  ln -sfn /host/Assets "$WORK_SRC/Assets"
  ln -sfn /host/PulsarPacks "$WORK_SRC/PulsarPacks"
  ln -sfn /host/build/retro-wfc "$WORK_SRC/build/retro-wfc"
  cd "$WORK_SRC"
  local translator=translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll
  [[ -f "$translator" ]] || dotnet build translator/src/Translator.Cli/Translator.Cli.csproj -c Release
  run_translator() { dotnet "$translator" "$@"; }
  local rr=PulsarPacks/completed/RetroRewind/RetroRewind6 wfc=build/retro-wfc/binary/payload.RMCPD00.bin
  local inputs=(Assets/main.dol Assets/StaticR.rel)
  [[ "$PRODUCT" == RetroRewind ]] && inputs+=("$rr/Binaries/Code.pul" "$wfc")
  for input in "${inputs[@]}"; do
    [[ -f "$input" ]] || { echo "Missing $input: see kaihen/README.md, step 2" >&2; exit 1; }
  done
  local jobs project=(--project projects/mkwii/recomp.yml)
  jobs="$(nproc)"
  mkdir -p generated/functions generated/build_shards build/base
  run_translator translate-recursive 0x800060A4 "${project[@]}" --outdir generated/functions \
    --output-metadata generated/base_translation_output.json \
    --production-source-bundle generated/base_translation_sources.bin \
    --no-function-files --prune-stale --threads "$jobs"
  run_translator emit-base-manifest "${project[@]}" --out build/base --functions-dir generated/functions \
    --translation-output-metadata generated/base_translation_output.json --region P
  local shards=(emit-build-shards "${project[@]}" --base-metadata generated/base_translation_output.json
    --base-functions-dir generated/functions --native-source-dir runtime/src --out generated/build_shards)
  if [[ "$PRODUCT" != RetroRewind ]]; then
    run_translator generate-data-init "${project[@]}"
    run_translator "${shards[@]}"
    return
  fi
  run_translator validate-retro-wfc-payload --directory build/retro-wfc --game-id RMCP01
  mkdir -p build/mods/retro_rewind_full_cpp
  run_translator translate-mod "${project[@]}" --profile retro-rewind --base-manifest build/base/mkwii_base_manifest.json \
    --base-translation-output-metadata generated/base_translation_output.json \
    --code-pul "$WORK_SRC/$rr/Binaries/Code.pul" --mod-root "$WORK_SRC/$rr" --mod-name "Retro Rewind" --region P \
    --out build/mods/retro_rewind_full_cpp --prefer-cached-inputs --emit-cpp --threads "$jobs" \
    --retro-wfc-payload "$WORK_SRC/$wfc"
  run_translator generate-data-init "${project[@]}"
  run_translator "${shards[@]}" --resolved-profile build/mods/retro_rewind_full_cpp/resolved_dispatch_profile.json \
    --retro-cpp-dir build/mods/retro_rewind_full_cpp/cpp
}

# This build's tree: the repository, the translation from the work volume, and aurora with this
# port's patch.
sync_tree() {
  [[ -d "$WORK_SRC/generated" ]] || {
    echo "No translated game in the work volume: run kaihen/run.sh translate first" >&2
    exit 1
  }
  copy_repo "$SRC"
  ln -sfn "$WORK_SRC/generated" "$SRC/generated"
  rr_pin
  patch -p1 -s -N -d "$SRC/aurora-main" -i "$PATCHES/$(pin aurora patch)"
}

# The Retro Rewind release this build is for (version and Code.pul's SHA-256), in
# kaihen/platform/kaihen_rr_pin.h: at its first launch the app downloads that release from Retro
# Rewind's server when the card doesn't have it (kaihen_setup.cpp). Written only when it changes.
rr_pin() {
  local pin="$SRC/kaihen/platform/kaihen_rr_pin.h" rr=/host/PulsarPacks/completed/RetroRewind/RetroRewind6 text
  if [[ "$PRODUCT" == RetroRewind ]]; then
    [[ -f "$rr/Binaries/Code.pul" && -f "$rr/version.txt" ]] || {
      echo "Missing $rr: see kaihen/README.md, step 2" >&2
      exit 1
    }
    text="#define KAIHEN_RR_VERSION \"$(tr -d '[:space:]' < "$rr/version.txt")\"
#define KAIHEN_RR_CODE_SHA256 \"$(sha256sum "$rr/Binaries/Code.pul" | cut -c1-64 | tr a-f A-F)\""
  else
    text="/* plain Mario Kart Wii: no Retro Rewind */"
  fi
  [[ -f "$pin" && "$(cat "$pin")" == "$text" ]] || printf '%s\n' "$text" > "$pin"
}

# Dawn and SDL3 from the work volume (already patched by sources).
deps() {
  mkdir -p /opt/kaihen/deps
  for dep in dawn sdl3; do
    [[ -d "/opt/work/deps/$dep" ]] || {
      echo "No $dep in the work volume: run kaihen/run.sh sources first" >&2
      exit 1
    }
    rsync -a --delete --exclude .git "/opt/work/deps/$dep/" "/opt/kaihen/deps/$dep/"
  done
}

# The shipped profile for this product (kaihen/pgo/<product>.profdata.xz), unpacked under a name
# with its content hash: Clang doesn't track the profile as a dependency, so a new one changes the
# compile command. Clang only.
pgo_options() {
  local name
  case "$PRODUCT" in RetroRewind) name=retro-rewind ;; *) name=wiicompiled ;; esac
  local packed="$SRC/kaihen/pgo/$name.profdata.xz"
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
  source "$DEVKITPRO/switch2vars.sh" >/dev/null
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
    -UCMAKE_C_FLAGS -UCMAKE_CXX_FLAGS -UCMAKE_ASM_FLAGS -UCMAKE_EXE_LINKER_FLAGS -UHBX_SDK_INCLUDE \
    -DKAIHEN_CLANG=$clang "${pgo[@]}" \
    -DHBX_SWITCH2=ON -DMKW_KAIHEN=ON -DMKW_GUEST_FLAT_CHECKED_ONLY=ON -DMKW_AARCH64_CPU=cortex-a78c \
    -DAURORA_DAWN_PROVIDER=vendor -DAURORA_DAWN_LINKAGE=static -DFETCHCONTENT_SOURCE_DIR_DAWN=/opt/kaihen/deps/dawn \
    -DAURORA_SDL3_PROVIDER=vendor -DAURORA_SDL3_LINKAGE=static -DFETCHCONTENT_SOURCE_DIR_SDL=/opt/kaihen/deps/sdl3 \
    -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_OFFSCREEN=ON "${options[@]}" "$@"
}

build() {
  source "$DEVKITPRO/switch2vars.sh" >/dev/null
  sync_tree
  cmake --build "$OUT" --target "$PRODUCT" --parallel "$JOBS" "$@"
  local elf="$OUT/$PRODUCT.elf"
  # hbmenu's title, author and version, and the icon: your own kaihen/icon-<product>.jpg (256x256,
  # kept out of git) when it exists, else kaihen/icons/<product>.jpg, WiiCompiled's own app icon.
  local title="WiiCompiled" icon="/host/kaihen/icon-$PRODUCT.jpg"
  [[ "$PRODUCT" == RetroRewind ]] && title="Retro Rewind"
  [[ -f "$icon" && -z "${KAIHEN_RELEASE:-}" ]] || icon="/host/kaihen/icons/$PRODUCT.jpg"
  nacptool --create "$title" "6rimson" "${KAIHEN_VERSION:-0.3.1}" "$OUT/$PRODUCT.nacp"
  # The wiicompiled folder's own small files, without any game files: in the app (romfs), which
  # copies what a card lacks at its first launch (kaihen_platform.c, card_setup), and in
  # kaihen/dist/card, the card's layout to copy by hand (kaihen/README.md).
  local romfs="$OUT/romfs"
  rm -rf "$romfs"
  mkdir -p "$romfs"
  rsync -a --exclude .DS_Store /host/kaihen/card/wiicompiled "$romfs/"
  cp "$SRC/runtime/assets/dsp/dsp_coef.bin" "$SRC/runtime/assets/certs/cacert.pem" "$romfs/wiicompiled/"
  cp -R "$SRC/runtime/assets/wii" "$romfs/wiicompiled/wii_bootstrap"
  elf2nro "$elf" "/dist/$PRODUCT.nro" --nacp="$OUT/$PRODUCT.nacp" --icon="$icon" --romfsdir="$romfs"
  ls -la "/dist/$PRODUCT.nro"
  local card=/dist/card
  mkdir -p "$card/switch"
  rm -rf "$card/wiicompiled/wii_bootstrap"
  cp -R "$romfs/wiicompiled" "$card/"
  ln -f "/dist/$PRODUCT.nro" "$card/switch/$PRODUCT.nro" 2>/dev/null || cp "/dist/$PRODUCT.nro" "$card/switch/"
}

action="${1:?usage: build.sh sources|translate|configure|build|shell}"
shift
case "$action" in
  sources) sources ;;
  translate) translate ;;
  configure) configure "$@" ;;
  build) build "$@" ;;
  shell) sync_tree; bash ;;
  *) echo "unknown action $action" >&2; exit 2 ;;
esac
