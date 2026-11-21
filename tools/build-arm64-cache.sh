#!/usr/bin/env bash
# Pre-build -O2 Cortex-A53 native AArch64 game module(s) (.so) on your PC from
# your own legal MAME Daytona USA ROM archive(s) (daytona.zip and/or daytona93.zip),
# so an Anbernic RG34XX (Allwinner H700, 1 GB RAM) boots straight into locked
# 57.52 FPS via dlopen() without compiling C++20 on-device.
#
# Usage:
#   tools/build-arm64-cache.sh [DATA_DIR] [TARGET_CPU] [--exact]
#
# Examples:
#   tools/build-arm64-cache.sh /path/to/ports/daytonaarm cortex_a53
#   tools/build-arm64-cache.sh /path/to/ports/daytonaarm cortex_a55
#
# Expects your own MAME ROM archive(s) in <DATA_DIR>/roms/ (or ./roms/):
#   daytona.zip    (Daytona USA Revision A, 1994)
#   daytona93.zip  (Daytona USA Deluxe '93)
# Writes cache/daytona-aarch64.so and/or cache/daytona93-aarch64.so into <DATA_DIR>/cache/.
set -euo pipefail
cd "$(dirname "$0")/.."

DATA_DIR="${1:-$PWD}"
TARGET_CPU="${2:-cortex_a53}"
TARGET_CPU="${TARGET_CPU//-/_}"
EXACT=0
for arg in "$@"; do
    [ "$arg" = "--exact" ] && EXACT=1
done

DEPS="$PWD/deps"
EXTERN="$PWD/extern"
BDIR="$PWD/build-arm64"
HTOOLS="$BDIR/host-tools"
TARGET_TRIPLE="aarch64-linux-gnu.2.28"
JOBS="${JOBS:-$(nproc)}"

if [ ! -x "$DEPS/zig-linux/zig" ] || [ ! -d "$EXTERN/softfloat/source" ] || [ ! -d "$EXTERN/ymfm/src" ]; then
    echo "Fetching Zig toolchain & pinned dependencies via tools/build-arm64.sh..."
    TARGET_CPU="$TARGET_CPU" tools/build-arm64.sh
fi
ZIG_BIN="$DEPS/zig-linux/zig"
export ZIG_GLOBAL_CACHE_DIR="$DEPS/zig-cache"
export ZIG_LOCAL_CACHE_DIR="$DEPS/zig-cache"

# 1. Build lightweight host recompiler tools (using Zig C++ for zero host dependency hassle)
build_host_tools() {
    mkdir -p "$HTOOLS/sf" "$HTOOLS/obj"
    if [ -x "$HTOOLS/m2import_daytona" ] && [ -x "$HTOOLS/m2import_daytona93" ] && \
       [ -x "$HTOOLS/m2recomp" ] && [ -x "$HTOOLS/m2tgprecomp" ] && [ -x "$HTOOLS/m2sndrecomp" ]; then
        return 0
    fi
    echo "=== Building host static recompilers (m2import, m2recomp, m2tgprecomp, m2sndrecomp) ==="
    local sf_list="$HTOOLS/sf_list.txt"
    python3 - "$sf_list" <<'PYEOF'
import re, sys
txt = open("cmake/softfloat_sources.cmake").read()
files = re.findall(r'\$\{SOFTFLOAT_DIR\}/([^\s\)]+\.c)', txt)
with open(sys.argv[1], "w") as f:
    for p in files:
        f.write(f"extern/softfloat/{p}\n")
PYEOF
    if [ ! -f "$HTOOLS/libsoftfloat.a" ]; then
        xargs -P "$JOBS" -I{} sh -c '
            src="$1"; out="$2/$(basename "$src" .c).o"
            [ -f "$out" ] && [ "$out" -nt "$src" ] && exit 0
            exec "$3" cc -std=c11 -O2 -w \
                -DSOFTFLOAT_FAST_INT64 -DLITTLEENDIAN=1 -DTHREAD_LOCAL=_Thread_local \
                -DSOFTFLOAT_ROUND_ODD -DINLINE_LEVEL=5 -DSOFTFLOAT_FAST_DIV32TO16 -DSOFTFLOAT_FAST_DIV64TO32 \
                -Iextern/softfloat/source/include -Iextern/softfloat/build/Linux-x86_64-GCC -Iextern/softfloat/source/8086-SSE \
                -c "$src" -o "$out"
        ' _ {} "$HTOOLS/sf" "$ZIG_BIN" < "$sf_list"
        "$ZIG_BIN" ar rcs "$HTOOLS/libsoftfloat.a" "$HTOOLS/sf"/*.o
    fi

    local hcxx="-std=c++20 -O2 -fno-fast-math -ffp-contract=off -w -DTHREAD_LOCAL=__thread \
        -DSOFTFLOAT_FAST_INT64 -DLITTLEENDIAN=1 -Isrc -Iextern/softfloat/source/include -Iextern/ymfm/src"

    # Build shared i960 + m68k + zip/archive objects
    for f in src/i960/isa.cpp src/i960/decode.cpp src/i960/reach.cpp src/i960/fp.cpp \
             src/m68k/decode.cpp src/m68k/reach.cpp src/runtime/zip.cpp src/runtime/archive.cpp; do
        local o="$HTOOLS/obj/$(echo "$f" | tr '/.' '__').o"
        [ -f "$o" ] && [ "$o" -nt "$f" ] || "$ZIG_BIN" c++ $hcxx -c "$f" -o "$o"
    done

    "$ZIG_BIN" c++ $hcxx tools/m2recomp/main.cpp \
        "$HTOOLS/obj/src_i960_isa_cpp.o" "$HTOOLS/obj/src_i960_decode_cpp.o" \
        "$HTOOLS/obj/src_i960_reach_cpp.o" "$HTOOLS/obj/src_i960_fp_cpp.o" \
        "$HTOOLS/libsoftfloat.a" -o "$HTOOLS/m2recomp"

    "$ZIG_BIN" c++ $hcxx tools/m2tgprecomp/main.cpp -o "$HTOOLS/m2tgprecomp"

    "$ZIG_BIN" c++ $hcxx tools/m2sndrecomp/main.cpp \
        "$HTOOLS/obj/src_m68k_decode_cpp.o" "$HTOOLS/obj/src_m68k_reach_cpp.o" -o "$HTOOLS/m2sndrecomp"

    for setname in daytona daytona93; do
        "$ZIG_BIN" c++ $hcxx -DM2_ROMSET=\"$setname\" \
            tools/m2import/main.cpp src/runtime/rom_import.cpp \
            "$HTOOLS/obj/src_runtime_zip_cpp.o" "$HTOOLS/obj/src_runtime_archive_cpp.o" \
            -o "$HTOOLS/m2import_$setname"
    done
}

build_host_tools

find_rom_for() {
    local setname="$1"
    for candidate in \
        "$DATA_DIR/roms/${setname}.zip" \
        "$DATA_DIR/run/${setname}.zip" \
        "$DATA_DIR/${setname}.zip" \
        "$PWD/roms/${setname}.zip"; do
        if [ -f "$candidate" ]; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

mkdir -p "$DATA_DIR/cache"
BUILT=0

for SET in daytona daytona93; do
    ROM_ZIP=$(find_rom_for "$SET" || true)
    [ -n "$ROM_ZIP" ] || continue

    echo ""
    echo "================================================================"
    echo " Building AArch64 ($TARGET_CPU, -O2) .so module for '$SET'"
    echo " ROM: $ROM_ZIP"
    echo "================================================================"

    RCACHE="$BDIR/rom_cache/$SET"
    GENROOT="$BDIR/gen_$SET"
    MODOBJ="$BDIR/mod_obj_$SET"
    rm -rf "$RCACHE" "$GENROOT" "$MODOBJ"
    mkdir -p "$RCACHE" "$GENROOT/i960" "$GENROOT/tgp" "$GENROOT/snd" "$MODOBJ"

    echo "[1/4] Importing & CRC-verifying ROM set '$SET'..."
    "$HTOOLS/m2import_$SET" "$ROM_ZIP" "$RCACHE"

    echo "[2/4] Statically recompiling i960, TGP, and 68000 programs..."
    I960_ARGS=("$RCACHE/program.bin" "$GENROOT/i960" "--seeds" "seeds/${SET}.txt")
    if [ -f "seeds/${SET}_hooks.txt" ]; then
        I960_ARGS+=("--hooks" "seeds/${SET}_hooks.txt")
    fi
    if [ "$EXACT" -eq 0 ]; then
        I960_ARGS+=("--fast_inaccuracy")
    fi
    "$HTOOLS/m2recomp" "${I960_ARGS[@]}"

    if [ "$EXACT" -eq 0 ]; then
        # Apply Dreamcast/Vita direct work-RAM access rewrite in place (matches M2_FAST_GEN=1).
        python3 - "$GENROOT/i960" <<'PYEOF'
import importlib.util, pathlib, sys
spec = importlib.util.spec_from_file_location("fast_gen", "platform/dreamcast/scripts/fast_gen.py")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
gen_dir = pathlib.Path(sys.argv[1])
stats = {"wram": 0}
for cpp in sorted(gen_dir.glob("chunk_*.cpp")):
    text = cpp.read_text()
    cpp.write_text("\n".join(mod.direct_wram(line, stats) for line in text.split("\n")), newline="\n")
print(f"  Applied direct_wram fast pass ({stats['wram']} work-RAM accesses inlined)")
PYEOF
        "$HTOOLS/m2tgprecomp" "$RCACHE/tgp_program.bin" "$GENROOT/tgp/tgp_gen.cpp" --fast_inaccuracy
    else
        "$HTOOLS/m2tgprecomp" "$RCACHE/tgp_program.bin" "$GENROOT/tgp/tgp_gen.cpp"
    fi
    "$HTOOLS/m2sndrecomp" "$RCACHE/sound_program.bin" "$GENROOT/snd/snd_gen.cpp"

    echo "[3/4] Cross-compiling generated C++20 for $TARGET_TRIPLE ($TARGET_CPU, -O2, -j$JOBS)..."
    MOD_CXXFLAGS="-target $TARGET_TRIPLE -mcpu=$TARGET_CPU -std=c++20 -O2 -fPIC \
        -fno-fast-math -ffp-contract=off -Wno-unused-label -Wno-unused-parameter \
        -DSOFTFLOAT_FAST_INT64 -DLITTLEENDIAN=1 -DTHREAD_LOCAL=__thread \
        -DM2_DYNAMIC_ROMSET=1 \
        -DM2_FAST_GEN=1 -DM2_FAST_BUS=1 -DM2_FAST_TGP=1 -DM2_FAST_CPU=1 -DM2_FAST_GEO=1 -DM2_VITA_RENDER_OPT=1 \
        -Iplatform/arm64/src -Iplatform/arm64 -Isrc -I$EXTERN/softfloat/source/include"

    SRCS=()
    for f in "$GENROOT/i960"/*.cpp "$GENROOT/tgp/tgp_gen.cpp" "$GENROOT/snd/snd_gen.cpp"; do
        SRCS+=("$f")
    done

    printf '%s\n' "${SRCS[@]}" | xargs -P "$JOBS" -I{} sh -c '
        src="$1"
        out="$2/$(basename "$src" .cpp).o"
        exec "$3" c++ '"$MOD_CXXFLAGS"' -c "$src" -o "$out"
    ' _ {} "$MODOBJ" "$ZIG_BIN"
    "$ZIG_BIN" c++ $MOD_CXXFLAGS -DM2_ROMSET="\"$SET\"" -c platform/arm64/module_export.cpp -o "$MODOBJ/module_export.o"

    OUT_SO="$DATA_DIR/cache/${SET}-aarch64.so"
    "$ZIG_BIN" c++ -target "$TARGET_TRIPLE" -mcpu="$TARGET_CPU" -O2 -shared \
        -Wl,-Bsymbolic -Wl,--version-script=platform/arm64/module_export.ver -Wl,--allow-shlib-undefined \
        "$MODOBJ"/*.o -o "$OUT_SO"

    echo "[4/4] Verifying $OUT_SO (ELF arch, GLIBC <= 2.28, single export, host symbol parity)..."
    SO_INFO=$(file "$OUT_SO")
    echo "$SO_INFO" | grep -q "ARM aarch64" || { echo "ERROR: $OUT_SO is not ARM aarch64 ($SO_INFO)" >&2; exit 1; }
    MAX_GLIBC=$(objdump -T "$OUT_SO" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1 || true)
    if [ -n "$MAX_GLIBC" ] && [ "$(printf '%s\n2.28\n' "$MAX_GLIBC" | sort -V | tail -1)" != "2.28" ]; then
        echo "ERROR: $OUT_SO requires GLIBC_$MAX_GLIBC (max allowed 2.28)" >&2
        exit 1
    fi
    EXPORTS=$(nm -D --defined-only "$OUT_SO" | awk '{print $3}' | grep -v '^$' | sort -u)
    if [ "$EXPORTS" != "daytona_module_api_v1" ]; then
        echo "WARNING: unexpected global exports in $OUT_SO:" >&2
        echo "$EXPORTS" >&2
    fi

    # Cross-check every undefined non-libc symbol in .so against daytona.aarch64 exports!
    if [ -x "$BDIR/daytona.aarch64" ]; then
        python3 - "$BDIR/daytona.aarch64" "$OUT_SO" <<'PYEOF'
import subprocess, sys
host_bin, mod_so = sys.argv[1], sys.argv[2]
host_syms = set()
for line in subprocess.check_output(["nm", "-D", "--defined-only", host_bin], text=True).splitlines():
    p = line.split()
    if len(p) >= 3:
        host_syms.add(p[2].split("@")[0])
missing = []
for line in subprocess.check_output(["nm", "-D", "-u", mod_so], text=True).splitlines():
    p = line.split()
    if len(p) >= 2:
        sym = p[-1]
        if "@GLIBC_" in sym or sym.startswith("__cxa_finalize") or sym.startswith("_ITM_") or sym == "__gmon_start__":
            continue
        base = sym.split("@")[0]
        if base not in host_syms:
            missing.append(sym)
if missing:
    print("ERROR: Unresolved symbols in .so not exported by daytona.aarch64:", file=sys.stderr)
    for m in missing:
        print("  ", m, file=sys.stderr)
    sys.exit(1)
print("  OK: 100% of dynamic module symbols resolve cleanly against daytona.aarch64!")
PYEOF
    fi

    echo "  OK: $OUT_SO ($(du -h "$OUT_SO" | cut -f1), max GLIBC_${MAX_GLIBC:-none})"
    BUILT=$((BUILT + 1))
done

if [ "$BUILT" -eq 0 ]; then
    echo "ERROR: No Daytona USA MAME ZIP ROM sets found in $DATA_DIR/roms/ (or ./roms/)." >&2
    echo "Place your own legal daytona.zip and/or daytona93.zip into $DATA_DIR/roms/ and re-run." >&2
    exit 1
fi

echo ""
echo "Done! Copy $DATA_DIR/cache/daytona*-aarch64.so into ports/daytonaarm/cache/ on your handheld SD card."
