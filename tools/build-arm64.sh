#!/usr/bin/env bash
# Cross-compile DaytonaArm (ROM-free distributable runtime) for AArch64 Linux
# handhelds (Anbernic RG34XX / Allwinner H700, RG35XX Plus/H/SP, RG40XX,
# RG CubeXX, Powkiddy RGB30, TrimUI Smart Pro) targeting glibc <= 2.28 +
# OpenGL ES 2.0 + ARM64 NEON, and assemble a PortMaster zip package.
#
# Zero copyrighted ROMs or ROM-derived game code ever enter this package.
# Users build their own cache/<romset>-aarch64.so locally on PC with
# tools/build-arm64-cache.sh.
#
# Usage:
#   tools/build-arm64.sh                    # default CPU: cortex_a53 (H700)
#   TARGET_CPU=cortex_a55 tools/build-arm64.sh
set -euo pipefail
cd "$(dirname "$0")/.."

DEPS="$PWD/deps"
EXTERN="$PWD/extern"
BDIR="$PWD/build-arm64"
OUT="$PWD/dist/DaytonaArm-PortMaster-aarch64"
ZIPOUT="$PWD/dist/DaytonaArm-PortMaster-aarch64.zip"
TARGET_CPU="${TARGET_CPU:-cortex_a53}"
TARGET_CPU="${TARGET_CPU//-/_}" # accept both cortex-a53 and cortex_a53
TARGET_TRIPLE="aarch64-linux-gnu.2.28"
JOBS="${JOBS:-$(nproc)}"

ZIG_VER="0.17.0"
ZIG_SHA="1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026"
SDL_VER="2.32.10"
SDL_SHA="5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165"

SOFTFLOAT_URL="https://github.com/ucb-bar/berkeley-softfloat-3.git"
SOFTFLOAT_REV="a0c6494cdc11865811dec815d5c0049fba9d82a8"
IMGUI_URL="https://github.com/ocornut/imgui.git"
IMGUI_REV="f1cc2ae15e53a861a874c3034aae6798fde194ab"
YMFM_URL="https://github.com/aaronsgiles/ymfm.git"
YMFM_REV="81aec25ccbb98f4873a255f7551ac4dadac59b4a"

mkdir -p "$DEPS/dl" "$EXTERN" "$BDIR/obj" "$BDIR/sf" "$BDIR/rt" "$BDIR/ui"

fetch_file() {
    local url="$1" name="$2" sha="$3"
    if [ ! -f "$DEPS/dl/$name" ]; then
        curl -fsSL --retry 3 -o "$DEPS/dl/$name.part" "$url"
        mv "$DEPS/dl/$name.part" "$DEPS/dl/$name"
    fi
    echo "$sha  $DEPS/dl/$name" | sha256sum -c --quiet || {
        echo "ERROR: SHA-256 mismatch for $DEPS/dl/$name" >&2
        exit 1
    }
}

fetch_git() {
    local url="$1" rev="$2" dest="$3" sentinel="$4"
    if [ ! -d "$dest/.git" ]; then
        git clone --quiet --no-checkout "$url" "$dest"
    fi
    if [ ! -f "$dest/$sentinel" ] || [ "$(git -C "$dest" rev-parse HEAD 2>/dev/null || true)" != "$rev" ]; then
        git -C "$dest" fetch --quiet --depth 1 origin "$rev" 2>/dev/null || git -C "$dest" fetch --quiet origin || true
        git -C "$dest" checkout --quiet --force --detach "$rev"
    fi
    [ -f "$dest/$sentinel" ] || { echo "ERROR: missing $dest/$sentinel after checkout of $rev" >&2; exit 1; }
}

# 1. Host Zig compiler (hermetic aarch64-linux-gnu.2.28 C/C++20 cross-compiler)
if [ ! -x "$DEPS/zig-linux/zig" ]; then
    echo "[1/6] Fetching Zig $ZIG_VER..."
    fetch_file "https://ziglang.org/download/$ZIG_VER/zig-x86_64-linux-$ZIG_VER.tar.xz" \
        "zig-x86_64-linux-$ZIG_VER.tar.xz" "$ZIG_SHA"
    tar -C "$DEPS" -xJf "$DEPS/dl/zig-x86_64-linux-$ZIG_VER.tar.xz"
    rm -rf "$DEPS/zig-linux"
    mv "$DEPS/zig-x86_64-linux-$ZIG_VER" "$DEPS/zig-linux"
fi
ZIG_BIN="$DEPS/zig-linux/zig"
export ZIG_GLOBAL_CACHE_DIR="$DEPS/zig-cache"
export ZIG_LOCAL_CACHE_DIR="$DEPS/zig-cache"

# 2. Pinned upstream third-party dependencies (SoftFloat 3e, ymfm, Dear ImGui)
echo "[2/6] Checking pinned dependencies (SoftFloat, ymfm, Dear ImGui)..."
fetch_git "$SOFTFLOAT_URL" "$SOFTFLOAT_REV" "$EXTERN/softfloat" "source/softfloat_state.c"
fetch_git "$YMFM_URL" "$YMFM_REV" "$EXTERN/ymfm" "src/ymfm_opn.cpp"
fetch_git "$IMGUI_URL" "$IMGUI_REV" "$EXTERN/imgui" "imgui.cpp"

# 3. Clean SDL2 2.32.10 headers (target-independent Linux LP64 + GLES 2.0)
if [ ! -d "$DEPS/sdl2-src/SDL2-$SDL_VER/include" ]; then
    echo "[3/6] Fetching SDL2 $SDL_VER headers..."
    fetch_file "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz" \
        "SDL2-$SDL_VER.tar.gz" "$SDL_SHA"
    rm -rf "$DEPS/sdl2-src" && mkdir -p "$DEPS/sdl2-src"
    tar -C "$DEPS/sdl2-src" -xzf "$DEPS/dl/SDL2-$SDL_VER.tar.gz"
fi
SDL_INC="$BDIR/inc"
rm -rf "$SDL_INC" && mkdir -p "$SDL_INC/SDL2"
cp "$DEPS/sdl2-src/SDL2-$SDL_VER/include"/*.h "$SDL_INC/SDL2/"
cat > "$SDL_INC/SDL2/SDL_config.h" <<'EOF'
#ifndef SDL_config_h_
#define SDL_config_h_
#include "SDL_platform.h"
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#define SDL_BYTEORDER 1234
#define SIZEOF_VOIDP 8
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDINT_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_MATH_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_ALLOCA_H 1
#define HAVE_MALLOC 1
#define HAVE_CALLOC 1
#define HAVE_REALLOC 1
#define HAVE_FREE 1
#define HAVE_MEMSET 1
#define HAVE_MEMCPY 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMCMP 1
#define HAVE_STRLEN 1
#define HAVE_STRCHR 1
#define HAVE_STRRCHR 1
#define HAVE_STRSTR 1
#define HAVE_STRCMP 1
#define HAVE_STRNCMP 1
#define HAVE_STRCASECMP 1
#define HAVE_STRNCASECMP 1
#define HAVE_VSNPRINTF 1
#define HAVE_SNPRINTF 1
#define SDL_VIDEO_OPENGL_ES2 1
#define SDL_VIDEO_OPENGL_EGL 1
#endif
EOF

COMMON_DEFS="-DSOFTFLOAT_FAST_INT64 -DLITTLEENDIAN=1 -DM2_DYNAMIC_ROMSET=1 \
  -DM2_FAST_GEN=1 -DM2_FAST_BUS=1 -DM2_FAST_TGP=1 -DM2_FAST_CPU=1 -DM2_FAST_GEO=1 -DM2_VITA_RENDER_OPT=1 \
  -DDAYTONA_GLES=1"

CXX_FLAGS="-target $TARGET_TRIPLE -mcpu=$TARGET_CPU -std=c++20 -O2 -fno-fast-math -ffp-contract=off \
  -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-but-set-variable -Wno-attributes \
  -DTHREAD_LOCAL=__thread $COMMON_DEFS \
  -Iplatform/arm64/src -Iplatform/arm64 -Iplatform/vita -Isrc \
  -I$EXTERN/softfloat/source/include -I$EXTERN/ymfm/src -I$EXTERN/imgui \
  -I$SDL_INC -I$SDL_INC/SDL2"

# 4. Cross-compile SoftFloat, runtime, ymfm, ImGui, and ARM64 GLES 2.0 frontend
echo "[4/6] Cross-compiling DaytonaArm runtime for $TARGET_TRIPLE ($TARGET_CPU, GLES 2.0, NEON, -j$JOBS)..."

# 4a. SoftFloat 3e (C11, 8086-SSE semantic specialization, FAST_INT64)
SF_LIST="$BDIR/softfloat_files.txt"
python3 - "$SF_LIST" <<'PYEOF'
import re, sys
txt = open("cmake/softfloat_sources.cmake").read()
files = re.findall(r'\$\{SOFTFLOAT_DIR\}/([^\s\)]+\.c)', txt)
with open(sys.argv[1], "w") as f:
    for p in files:
        f.write(f"extern/softfloat/{p}\n")
PYEOF

SF_CFLAGS="-target $TARGET_TRIPLE -mcpu=$TARGET_CPU -std=c11 -O2 -w \
  -DSOFTFLOAT_FAST_INT64 -DLITTLEENDIAN=1 -DTHREAD_LOCAL=_Thread_local \
  -DSOFTFLOAT_ROUND_ODD -DINLINE_LEVEL=5 -DSOFTFLOAT_FAST_DIV32TO16 -DSOFTFLOAT_FAST_DIV64TO32 \
  -I$EXTERN/softfloat/source/include -I$EXTERN/softfloat/build/Linux-x86_64-GCC -I$EXTERN/softfloat/source/8086-SSE"

if [ ! -f "$BDIR/libsoftfloat.a" ]; then
    xargs -P "$JOBS" -I{} sh -c '
        src="$1"; out="$2/$(basename "$src" .c).o"
        [ -f "$out" ] && [ "$out" -nt "$src" ] && exit 0
        exec "$3" cc '"$SF_CFLAGS"' -c "$src" -o "$out"
    ' _ {} "$BDIR/sf" "$ZIG_BIN" < "$SF_LIST"
    "$ZIG_BIN" ar rcs "$BDIR/libsoftfloat.a" "$BDIR/sf"/*.o
fi

# 4b. Core + Runtime + ymfm + ImGui + ARM64 Frontend (C++20)
RT_SRCS=(
    src/i960/isa.cpp src/i960/decode.cpp src/i960/reach.cpp src/i960/fp.cpp
    src/trace/trace.cpp
    src/runtime/enhance.cpp src/runtime/cpu.cpp src/runtime/tgp.cpp src/runtime/m2_tgp_board.cpp
    src/runtime/raster.cpp src/runtime/video.cpp src/runtime/m2_board.cpp src/runtime/game_loop.cpp
    src/runtime/zip.cpp src/runtime/rom_import.cpp src/runtime/archive.cpp src/runtime/m2_replay_bus.cpp
    src/runtime/lockstep.cpp src/runtime/snd_cpu.cpp src/runtime/multipcm.cpp src/runtime/sound_board.cpp
    src/runtime/native_sample_mixer.cpp src/runtime/native_sound_sequencer.cpp src/runtime/native_sound_engine.cpp
    src/runtime/comm_board.cpp
    platform/arm64/src/runtime/geo.cpp
    src/app/link_socket.cpp
    extern/ymfm/src/ymfm_opn.cpp extern/ymfm/src/ymfm_adpcm.cpp extern/ymfm/src/ymfm_ssg.cpp
)

UI_SRCS=(
    extern/imgui/imgui.cpp extern/imgui/imgui_draw.cpp
    extern/imgui/imgui_tables.cpp extern/imgui/imgui_widgets.cpp
)

APP_SRCS=(
    platform/arm64/module_bridge.cpp
    platform/arm64/gpu_gles.cpp
    platform/arm64/main_arm64.cpp
)

printf '%s\n' "${RT_SRCS[@]}" | xargs -P "$JOBS" -I{} sh -c '
    src="$1"
    tag=$(echo "$src" | tr "/." "__")
    out="$2/${tag}.o"
    extra=""
    case "$src" in
        extern/ymfm/*) extra="-w" ;;
        src/runtime/*|platform/arm64/src/runtime/*) extra="-fno-math-errno" ;;
    esac
    exec "$3" c++ '"$CXX_FLAGS"' $extra -c "$src" -o "$out"
' _ {} "$BDIR/rt" "$ZIG_BIN"
rm -f "$BDIR/libruntime.a"
"$ZIG_BIN" ar rcs "$BDIR/libruntime.a" "$BDIR/rt"/*.o

printf '%s\n' "${UI_SRCS[@]}" | xargs -P "$JOBS" -I{} sh -c '
    src="$1"
    out="$2/$(basename "$src" .cpp).o"
    [ -f "$out" ] && [ "$out" -nt "$src" ] && exit 0
    exec "$3" c++ -target '"$TARGET_TRIPLE"' -mcpu='"$TARGET_CPU"' -std=c++20 -O2 -w \
        -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS -I'"$EXTERN/imgui"' -c "$src" -o "$out"
' _ {} "$BDIR/ui" "$ZIG_BIN"
rm -f "$BDIR/libimgui.a"
"$ZIG_BIN" ar rcs "$BDIR/libimgui.a" "$BDIR/ui"/*.o

printf '%s\n' "${APP_SRCS[@]}" | xargs -P "$JOBS" -I{} sh -c '
    src="$1"
    out="$2/$(basename "$src" .cpp).o"
    exec "$3" c++ '"$CXX_FLAGS"' -c "$src" -o "$out"
' _ {} "$BDIR/obj" "$ZIG_BIN"

# 5. Build link-time-only libSDL2-2.0.so.0 stub so PortMaster supplies hardware Mali/KMSDRM SDL2
echo "[5/6] Generating SDL2 link stub and linking daytona.aarch64 (-rdynamic for .so modules)..."
python3 - "$BDIR/sdl2_stub.c" "$BDIR/obj"/*.o <<'PYEOF'
import subprocess, sys
out_c = sys.argv[1]
objs = sys.argv[2:]
syms = set()
for o in objs:
    p = subprocess.run(["nm", "-u", o], capture_output=True, text=True, check=True)
    for line in p.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-2] == "U" and parts[-1].startswith("SDL_"):
            syms.add(parts[-1])
with open(out_c, "w") as f:
    f.write("/* Link-time-only stub for PortMaster system libSDL2-2.0.so.0 */\n")
    for s in sorted(syms):
        f.write(f"void {s}(void) {{}}\n")
print(f"  Generated SDL2 stub with {len(syms)} symbols")
PYEOF

"$ZIG_BIN" cc -target "$TARGET_TRIPLE" -shared -Wl,-soname,libSDL2-2.0.so.0 \
    -o "$BDIR/libSDL2-2.0.so.0" "$BDIR/sdl2_stub.c"

# -rdynamic + --whole-archive on libruntime.a & libsoftfloat.a ensures EVERY
# runtime/i960/TGP/68k/SoftFloat symbol needed by dynamically loaded .so game
# modules is present and exported in daytona.aarch64's dynamic symbol table.
"$ZIG_BIN" c++ -target "$TARGET_TRIPLE" -mcpu="$TARGET_CPU" -O2 -fno-fast-math -ffp-contract=off \
    -rdynamic \
    -o "$BDIR/daytona.aarch64" \
    "$BDIR/obj"/*.o \
    -Wl,--whole-archive "$BDIR/libruntime.a" "$BDIR/libsoftfloat.a" -Wl,--no-whole-archive \
    "$BDIR/libimgui.a" \
    -L"$BDIR" -l:libSDL2-2.0.so.0 -lm -ldl -lpthread

# 6. Verify ELF architecture, dynamic deps, glibc <= 2.28, and exported runtime symbols
echo "[6/6] Verifying AArch64 binary and assembling ROM-free PortMaster package..."
FILE_INFO=$(file "$BDIR/daytona.aarch64")
echo "$FILE_INFO" | grep -q "ARM aarch64" || { echo "ERROR: not ARM aarch64 ($FILE_INFO)" >&2; exit 1; }
MAX_GLIBC=$(objdump -T "$BDIR/daytona.aarch64" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1 || true)
if [ -n "$MAX_GLIBC" ] && [ "$(printf '%s\n2.28\n' "$MAX_GLIBC" | sort -V | tail -1)" != "2.28" ]; then
    echo "ERROR: daytona.aarch64 requires GLIBC_$MAX_GLIBC (max allowed 2.28)" >&2
    exit 1
fi
READELF_D=$(readelf -d "$BDIR/daytona.aarch64")
if printf '%s\n' "$READELF_D" | grep -q "libstdc++"; then
    echo "ERROR: unexpected libstdc++.so dependency (must be statically linked by Zig)" >&2
    exit 1
fi
python3 - "$BDIR/daytona.aarch64" <<'PYEOF'
import subprocess, sys
bin_path = sys.argv[1]
syms = set()
for line in subprocess.check_output(["nm", "-D", "--defined-only", bin_path], text=True).splitlines():
    parts = line.split()
    if len(parts) >= 3:
        syms.add(parts[2].split("@")[0])
required = [
    "_ZN2rt3Cpu7do_callEjij",
    "_ZN2rt8Lockstep5applyEv",
    "_ZN2rt3Tgp4io_rEt",
    "_ZN3snd6Cpu68k6dev_r8Ej",
    "extF80M_add",
]
missing = [s for s in required if s not in syms]
if missing:
    print("ERROR: required runtime symbols not exported:", missing, file=sys.stderr)
    sys.exit(1)
print(f"  Verified {len(syms)} exported dynamic symbols in {bin_path}")
PYEOF
echo "  OK: daytona.aarch64 -> ARM aarch64, max GLIBC_${MAX_GLIBC:-none}, zero libstdc++.so, runtime ABI exported"
readelf -d "$BDIR/daytona.aarch64" | grep NEEDED

# Assemble ROM-free PortMaster directory & zip
rm -rf "$OUT" "$ZIPOUT"
mkdir -p "$OUT/daytonaarm/roms" "$OUT/daytonaarm/cache" "$OUT/daytonaarm/saves" \
         "$OUT/daytonaarm/libs.aarch64" "$OUT/daytonaarm/licenses"

cp portmaster/*.sh "$OUT/"
chmod +x "$OUT/"*.sh
cp portmaster/daytonaarm/port.json "$OUT/daytonaarm/"
cp portmaster/daytonaarm/gameinfo.xml "$OUT/daytonaarm/"
cp portmaster/daytonaarm/gamecontrollerdb.txt "$OUT/daytonaarm/"
cp portmaster/daytonaarm/PUT-ROMS-HERE.txt "$OUT/daytonaarm/roms/PUT-ROMS-HERE.txt"
cp "$BDIR/daytona.aarch64" "$OUT/daytonaarm/daytona.aarch64"
chmod +x "$OUT/daytonaarm/daytona.aarch64"

cp LICENSE "$OUT/daytonaarm/licenses/DaytonaArm-BSD3.txt"
cp THIRD_PARTY.md "$OUT/daytonaarm/licenses/THIRD_PARTY.md"
cp "$DEPS/sdl2-src/SDL2-$SDL_VER/LICENSE.txt" "$OUT/daytonaarm/licenses/SDL2-LICENSE.txt"
[ -f "$EXTERN/imgui/LICENSE.txt" ] && cp "$EXTERN/imgui/LICENSE.txt" "$OUT/daytonaarm/licenses/DearImGui-LICENSE.txt"
cp README.md "$OUT/daytonaarm/README.md"

if command -v zip >/dev/null 2>&1; then
    (cd "$OUT" && zip -qr9 "$ZIPOUT" *.sh daytonaarm)
else
    python3 -c "import shutil; shutil.make_archive('${ZIPOUT%.zip}', 'zip', '$OUT')"
fi
echo "Packaged: $ZIPOUT ($(du -h "$ZIPOUT" | cut -f1))"
echo "Next step: run tools/build-arm64-cache.sh /path/to/ports/daytonaarm $TARGET_CPU to compile your ROM's .so module(s)."
