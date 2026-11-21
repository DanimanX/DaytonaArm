#!/usr/bin/env bash
# Native x86_64 host build of the DaytonaArm runtime + dynamic .so module to
# test the exact platform/arm64/ dlopen() bridge, 4-core pipeline, and GLES/GL2
# frontend end-to-end on a desktop Linux machine.
#
# Usage:
#   tools/build-host-test.sh [/path/to/roms_dir] [bench_frames]
set -euo pipefail
cd "$(dirname "$0")/.."

ROMS_DIR="${1:-}"
BENCH_FRAMES="${2:-120}"
DEPS="$PWD/deps"
EXTERN="$PWD/extern"
BDIR="$PWD/build-host-arm64"
JOBS="${JOBS:-$(nproc)}"

ZIG_BIN="$DEPS/zig-linux/zig"
if [ ! -x "$ZIG_BIN" ]; then
    echo "Run tools/build-arm64.sh first to fetch Zig and pinned dependencies." >&2
    exit 1
fi
export ZIG_GLOBAL_CACHE_DIR="$DEPS/zig-cache"
export ZIG_LOCAL_CACHE_DIR="$DEPS/zig-cache"

mkdir -p "$BDIR/sf" "$BDIR/rt" "$BDIR/ui" "$BDIR/obj" "$BDIR/cache"

SDL_CFLAGS=$(pkg-config --cflags sdl2 2>/dev/null || echo "-I$PWD/build-arm64/inc/SDL2")
SDL_LIBS=$(pkg-config --libs sdl2 2>/dev/null || echo "-lSDL2")

COMMON_DEFS="-DSOFTFLOAT_FAST_INT64 -DLITTLEENDIAN=1 -DM2_DYNAMIC_ROMSET=1 \
  -DM2_FAST_GEN=1 -DM2_FAST_BUS=1 -DM2_FAST_TGP=1 -DM2_FAST_CPU=1 -DM2_FAST_GEO=1 -DM2_VITA_RENDER_OPT=1"

CXX_FLAGS="-std=c++20 -O2 -fno-fast-math -ffp-contract=off \
  -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-but-set-variable -Wno-attributes \
  -DTHREAD_LOCAL=__thread $COMMON_DEFS \
  -Iplatform/arm64/src -Iplatform/arm64 -Iplatform/vita -Isrc \
  -I$EXTERN/softfloat/source/include -I$EXTERN/ymfm/src -I$EXTERN/imgui $SDL_CFLAGS"

echo "[1/3] Building native x86_64 DaytonaArm runtime..."
SF_LIST="$BDIR/sf.txt"
python3 - "$SF_LIST" <<'PYEOF'
import re, sys
txt = open("cmake/softfloat_sources.cmake").read()
with open(sys.argv[1], "w") as f:
    for p in re.findall(r'\$\{SOFTFLOAT_DIR\}/([^\s\)]+\.c)', txt):
        f.write(f"extern/softfloat/{p}\n")
PYEOF

if [ ! -f "$BDIR/libsoftfloat.a" ]; then
    xargs -P "$JOBS" -I{} sh -c '
        src="$1"; out="$2/$(basename "$src" .c).o"
        [ -f "$out" ] && [ "$out" -nt "$src" ] && exit 0
        exec "$3" cc -std=c11 -O2 -w \
            -DSOFTFLOAT_FAST_INT64 -DLITTLEENDIAN=1 -DTHREAD_LOCAL=_Thread_local \
            -DSOFTFLOAT_ROUND_ODD -DINLINE_LEVEL=5 -DSOFTFLOAT_FAST_DIV32TO16 -DSOFTFLOAT_FAST_DIV64TO32 \
            -Iextern/softfloat/source/include -Iextern/softfloat/build/Linux-x86_64-GCC -Iextern/softfloat/source/8086-SSE \
            -c "$src" -o "$out"
    ' _ {} "$BDIR/sf" "$ZIG_BIN" < "$SF_LIST"
    "$ZIG_BIN" ar rcs "$BDIR/libsoftfloat.a" "$BDIR/sf"/*.o
fi

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
printf '%s\n' "${RT_SRCS[@]}" | xargs -P "$JOBS" -I{} sh -c '
    src="$1"; tag=$(echo "$src" | tr "/." "__"); out="$2/${tag}.o"
    extra=""
    case "$src" in extern/ymfm/*) extra="-w" ;; esac
    exec "$3" c++ '"$CXX_FLAGS"' $extra -c "$src" -o "$out"
' _ {} "$BDIR/rt" "$ZIG_BIN"
rm -f "$BDIR/libruntime.a"
"$ZIG_BIN" ar rcs "$BDIR/libruntime.a" "$BDIR/rt"/*.o

for f in extern/imgui/imgui.cpp extern/imgui/imgui_draw.cpp extern/imgui/imgui_tables.cpp extern/imgui/imgui_widgets.cpp; do
    o="$BDIR/ui/$(basename "$f" .cpp).o"
    [ -f "$o" ] && [ "$o" -nt "$f" ] || "$ZIG_BIN" c++ -std=c++20 -O2 -w -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS -I"$EXTERN/imgui" -c "$f" -o "$o"
done
rm -f "$BDIR/libimgui.a"
"$ZIG_BIN" ar rcs "$BDIR/libimgui.a" "$BDIR/ui"/*.o

for f in platform/arm64/module_bridge.cpp platform/arm64/gpu_gles.cpp platform/arm64/main_arm64.cpp; do
    "$ZIG_BIN" c++ $CXX_FLAGS -c "$f" -o "$BDIR/obj/$(basename "$f" .cpp).o"
done

"$ZIG_BIN" c++ -O2 -fno-fast-math -ffp-contract=off -rdynamic \
    -o "$BDIR/daytona.x86_64" \
    "$BDIR/obj"/*.o \
    -Wl,--whole-archive "$BDIR/libruntime.a" "$BDIR/libsoftfloat.a" -Wl,--no-whole-archive \
    "$BDIR/libimgui.a" $SDL_LIBS -lm -ldl -lpthread

echo "Built $BDIR/daytona.x86_64"

if [ -n "$ROMS_DIR" ] && [ -f "$ROMS_DIR/daytona.zip" ] && [ -d "$PWD/build-arm64/gen_daytona" ]; then
    echo "[2/3] Building native x86_64 .so module from build-arm64/gen_daytona..."
    mkdir -p "$BDIR/mod_daytona"
    MOD_FLAGS="-std=c++20 -O2 -fPIC -fno-fast-math -ffp-contract=off -Wno-unused-label -Wno-unused-parameter \
        -DSOFTFLOAT_FAST_INT64 -DLITTLEENDIAN=1 -DTHREAD_LOCAL=__thread \
        -DM2_DYNAMIC_ROMSET=1 \
        -DM2_FAST_GEN=1 -DM2_FAST_BUS=1 -DM2_FAST_TGP=1 -DM2_FAST_CPU=1 -DM2_FAST_GEO=1 -DM2_VITA_RENDER_OPT=1 \
        -Iplatform/arm64/src -Iplatform/arm64 -Isrc -I$EXTERN/softfloat/source/include"
    SRCS=(build-arm64/gen_daytona/i960/*.cpp build-arm64/gen_daytona/tgp/tgp_gen.cpp build-arm64/gen_daytona/snd/snd_gen.cpp)
    printf '%s\n' "${SRCS[@]}" | xargs -P "$JOBS" -I{} sh -c '
        src="$1"; out="$2/$(basename "$src" .cpp).o"
        exec "$3" c++ '"$MOD_FLAGS"' -c "$src" -o "$out"
    ' _ {} "$BDIR/mod_daytona" "$ZIG_BIN"
    "$ZIG_BIN" c++ $MOD_FLAGS -DM2_ROMSET='"daytona"' -c platform/arm64/module_export.cpp -o "$BDIR/mod_daytona/module_export.o"
    "$ZIG_BIN" c++ -O2 -shared -Wl,-Bsymbolic -Wl,--version-script=platform/arm64/module_export.ver \
        -Wl,--allow-shlib-undefined "$BDIR/mod_daytona"/*.o -o "$BDIR/cache/daytona-x86_64.so"

    echo "[3/3] Running headless dlopen() end-to-end verification ($BENCH_FRAMES frames)..."
    "$BDIR/daytona.x86_64" --data-dir "$BDIR" --romset daytona \
        --module "$BDIR/cache/daytona-x86_64.so" --rom "$ROMS_DIR/daytona.zip" \
        --bench-frames "$BENCH_FRAMES"
fi
