#!/usr/bin/env bash
# Build DaytonaArm Vulkan (ROM-free distributable AArch64 runtime using SDL3
# SDL_GPU Vulkan + SPIR-V shaders + Dear ImGui SDL_GPU3) targeting
# aarch64-linux-gnu glibc <= 2.28 with static libc++/SDL3 (zero libstdc++.so
# dependency; libvulkan.so.1, Wayland, X11, KMSDRM and audio backends are
# loaded dynamically at runtime via dlopen).
#
# Shares the exact same dynamic module ABI as daytona.aarch64 (GLES 2.0):
# users load the same cache/daytona-aarch64.so and cache/daytona93-aarch64.so
# modules produced by tools/build-arm64-cache.sh.
#
# Zero copyrighted ROMs or ROM-derived game code ever enter this package.
#
# Usage:
#   tools/build-arm64-vulkan.sh
#   TARGET_CPU=cortex_a720 tools/build-arm64-vulkan.sh
set -euo pipefail
cd "$(dirname "$0")/.."
if [ -n "${TMPDIR:-}" ] && [ ! -d "$TMPDIR" ]; then
    export TMPDIR="/tmp"
fi

DEPS="$PWD/deps"
EXTERN="$PWD/extern"
BDIR="$PWD/build-arm64-vulkan"
OUT="$PWD/dist/DaytonaArm-Vulkan-aarch64"
ZIPOUT="$PWD/dist/DaytonaArm-Vulkan-aarch64.zip"
TARGET_CPU="${TARGET_CPU:-generic}"
TARGET_CPU="${TARGET_CPU//-/_}"
TARGET_TRIPLE="aarch64-linux-gnu.2.28"
JOBS="${JOBS:-$(nproc)}"

ZIG_VER="0.17.0"
HOST_ARCH="$(uname -m)"
case "$HOST_ARCH" in
    x86_64|amd64)
        ZIG_ARCH="x86_64"
        ZIG_SHA="1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026"
        ;;
    aarch64|arm64)
        ZIG_ARCH="aarch64"
        ZIG_SHA="9e8d11661d4ae3bd57702a3832781e23ad151dde5798e16a5ccd503f65234ff8"
        ;;
    *)
        echo "ERROR: unsupported host architecture for Zig: $HOST_ARCH" >&2
        exit 1
        ;;
esac

SOFTFLOAT_URL="https://github.com/ucb-bar/berkeley-softfloat-3.git"
SOFTFLOAT_REV="a0c6494cdc11865811dec815d5c0049fba9d82a8"
SDL3_URL="https://github.com/libsdl-org/SDL.git"
SDL3_REV="fa2c02bb6e21974a89ea9824bc53c9932abe5f9c"
IMGUI_URL="https://github.com/ocornut/imgui.git"
IMGUI_REV="f1cc2ae15e53a861a874c3034aae6798fde194ab"
YMFM_URL="https://github.com/aaronsgiles/ymfm.git"
YMFM_REV="81aec25ccbb98f4873a255f7551ac4dadac59b4a"
LZMA_URL="https://github.com/ip7z/7zip.git"
LZMA_REV="0766b733fe3e06dd2a7f9a3cfbf2108ac73abd17"

mkdir -p "$DEPS/dl" "$EXTERN" "$BDIR"

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

# 1. Host Zig compiler (hermetic aarch64-linux-gnu.2.28 C/C++20 compiler)
if [ ! -x "$DEPS/zig-linux/zig" ]; then
    echo "[1/5] Fetching Zig $ZIG_VER ($ZIG_ARCH-linux)..."
    fetch_file "https://ziglang.org/download/$ZIG_VER/zig-$ZIG_ARCH-linux-$ZIG_VER.tar.xz" \
        "zig-$ZIG_ARCH-linux-$ZIG_VER.tar.xz" "$ZIG_SHA"
    tar -C "$DEPS" -xJf "$DEPS/dl/zig-$ZIG_ARCH-linux-$ZIG_VER.tar.xz"
    rm -rf "$DEPS/zig-linux"
    mv "$DEPS/zig-$ZIG_ARCH-linux-$ZIG_VER" "$DEPS/zig-linux"
fi
ZIG_BIN="$DEPS/zig-linux/zig"
export ZIG_GLOBAL_CACHE_DIR="$DEPS/zig-cache"
export ZIG_LOCAL_CACHE_DIR="$DEPS/zig-cache"

# 2. Pinned upstream third-party dependencies (SoftFloat, SDL3, ymfm, LZMA, ImGui)
echo "[2/5] Checking pinned dependencies (SoftFloat, SDL3, ymfm, LZMA, ImGui)..."
fetch_git "$SOFTFLOAT_URL" "$SOFTFLOAT_REV" "$EXTERN/softfloat" "source/softfloat_state.c"
fetch_git "$SDL3_URL" "$SDL3_REV" "$EXTERN/sdl3" "CMakeLists.txt"
for p in patches/sdl3/*.patch; do
    [ -f "$p" ] || continue
    if git -C "$EXTERN/sdl3" apply --check "$PWD/$p" 2>/dev/null; then
        git -C "$EXTERN/sdl3" apply "$PWD/$p"
    fi
done
fetch_git "$YMFM_URL" "$YMFM_REV" "$EXTERN/ymfm" "src/ymfm_opn.cpp"
fetch_git "$LZMA_URL" "$LZMA_REV" "$EXTERN/lzma" "C/7zArcIn.c"
fetch_git "$IMGUI_URL" "$IMGUI_REV" "$EXTERN/imgui" "imgui.cpp"

# 3. Create Zig compiler wrappers for CMake targeting aarch64-linux-gnu.2.28
echo "[3/5] Configuring CMake with Zig ($TARGET_TRIPLE, -mcpu=$TARGET_CPU, SDL3 static + Vulkan)..."
mkdir -p "$BDIR/bin"
cat > "$BDIR/bin/zig-cc" <<EOF
#!/bin/sh
exec "$ZIG_BIN" cc -target "$TARGET_TRIPLE" -mcpu="$TARGET_CPU" "\$@"
EOF
cat > "$BDIR/bin/zig-cxx" <<EOF
#!/bin/sh
exec "$ZIG_BIN" c++ -target "$TARGET_TRIPLE" -mcpu="$TARGET_CPU" "\$@"
EOF
cat > "$BDIR/bin/zig-ar" <<EOF
#!/bin/sh
exec "$ZIG_BIN" ar "\$@"
EOF
cat > "$BDIR/bin/zig-ranlib" <<EOF
#!/bin/sh
exec "$ZIG_BIN" ranlib "\$@"
EOF
chmod +x "$BDIR/bin/zig-cc" "$BDIR/bin/zig-cxx" "$BDIR/bin/zig-ar" "$BDIR/bin/zig-ranlib"

CMAKE_GEN_ARGS=()
if command -v ninja >/dev/null 2>&1; then
    CMAKE_GEN_ARGS=(-G Ninja)
fi

# SDL3 dynamically loads libvulkan.so.1, Wayland, X11, KMSDRM and audio backends
# at runtime via dlopen() (SDL_DEPS_SHARED=ON); host headers in /usr/include are
# sufficient for cross-compilation without linking host target libraries.
#
# CMAKE_LIBRARY_ARCHITECTURE is mandatory here: the compiler is a Zig wrapper, so
# CMake cannot infer the multiarch triplet from its implicit link directories.
# Without it find_library() never looks in /usr/lib/aarch64-linux-gnu, which is
# where Debian/Ubuntu install libX11.so, libasound.so and libEGL.so -- SDL3 then
# reports "Could NOT find X11" / "Could NOT find ALSA" and aborts configuration
# with "SDL could not find X11 or Wayland development libraries".
CMAKE_EXTRA_ARGS=(
    -DCMAKE_SYSTEM_NAME=Linux
    -DCMAKE_SYSTEM_PROCESSOR=aarch64
    -DCMAKE_LIBRARY_ARCHITECTURE="${TARGET_TRIPLE%%.*}"
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_C_COMPILER="$BDIR/bin/zig-cc"
    -DCMAKE_CXX_COMPILER="$BDIR/bin/zig-cxx"
    -DCMAKE_AR="$BDIR/bin/zig-ar"
    -DCMAKE_RANLIB="$BDIR/bin/zig-ranlib"
    -DM2_DYNAMIC_VULKAN=ON
    -DM2_ROMSET=daytona
    -DSDL_SHARED=OFF
    -DSDL_STATIC=ON
    -DSDL_DEPS_SHARED=ON
    -DSDL_VULKAN=ON
    -DSDL_GPU=ON
    -DSDL_OPENGL=OFF
    -DSDL_OPENGLES=OFF
    -DSDL_WAYLAND=ON
    -DSDL_WAYLAND_SHARED=ON
    -DSDL_X11=ON
    -DSDL_X11_SHARED=ON
    -DSDL_KMSDRM=ON
    -DSDL_KMSDRM_SHARED=ON
    # KMSDRM additionally needs HAVE_OPENGL_EGL, which SDL3 only probes when
    # SDL_OPENGL/SDL_OPENGLES is ON (cmake/sdlchecks.cmake CheckEGL). Both are
    # OFF for the Vulkan runtime by design, so KMSDRM stays disabled until a
    # Vulkan-backed KMS path exists -- Wayland/X11 are the windowing targets.
    -DSDL_ALSA=ON
    -DSDL_ALSA_SHARED=ON
    -DSDL_PULSEAUDIO=ON
    -DSDL_PULSEAUDIO_SHARED=ON
    -DSDL_PIPEWIRE=ON
    -DSDL_PIPEWIRE_SHARED=ON
    -DSDL_JACK=OFF
    -DSDL_SNDIO=OFF
    -DSDL_LIBUDEV=OFF
    -DSDL_DBUS=OFF
    -DSDL_IBUS=OFF
    -DSDL_HIDAPI_LIBUSB=OFF
)

cmake -S . -B "$BDIR/cmake" "${CMAKE_GEN_ARGS[@]}" "${CMAKE_EXTRA_ARGS[@]}"

echo "[4/5] Building daytona-vulkan.aarch64 (-j$JOBS)..."
cmake --build "$BDIR/cmake" --target daytona_vulkan --parallel "$JOBS"

BIN="$BDIR/cmake/daytona-vulkan.aarch64"
[ -x "$BIN" ] || { echo "ERROR: expected binary not found at $BIN" >&2; exit 1; }
cp "$BIN" "$BDIR/daytona-vulkan.aarch64"

# 5. Verify ELF architecture, GLIBC <= 2.28, zero libstdc++.so, and exported runtime ABI symbols
echo "[5/5] Verifying daytona-vulkan.aarch64 and assembling ROM-free Vulkan package..."
FILE_INFO=$(file "$BDIR/daytona-vulkan.aarch64")
echo "$FILE_INFO" | grep -q "ARM aarch64" || { echo "ERROR: not ARM aarch64 ($FILE_INFO)" >&2; exit 1; }
MAX_GLIBC=$(objdump -T "$BDIR/daytona-vulkan.aarch64" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1 || true)
if [ -n "$MAX_GLIBC" ] && [ "$(printf '%s\n2.28\n' "$MAX_GLIBC" | sort -V | tail -1)" != "2.28" ]; then
    echo "ERROR: daytona-vulkan.aarch64 requires GLIBC_$MAX_GLIBC (max allowed 2.28)" >&2
    exit 1
fi
READELF_D=$(readelf -d "$BDIR/daytona-vulkan.aarch64")
if printf '%s\n' "$READELF_D" | grep -q "libstdc++"; then
    echo "ERROR: unexpected libstdc++.so dependency (must be statically linked by Zig)" >&2
    exit 1
fi
python3 - "$BDIR/daytona-vulkan.aarch64" <<'PYEOF'
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
echo "  OK: daytona-vulkan.aarch64 -> ARM aarch64, max GLIBC_${MAX_GLIBC:-none}, zero libstdc++.so, runtime ABI exported"
readelf -d "$BDIR/daytona-vulkan.aarch64" | grep NEEDED

# Assemble ROM-free Vulkan package
rm -rf "$OUT" "$ZIPOUT"
mkdir -p "$OUT/daytonaarm-vulkan/roms" "$OUT/daytonaarm-vulkan/cache" "$OUT/daytonaarm-vulkan/saves" \
         "$OUT/daytonaarm-vulkan/licenses"

cp "portmaster/Daytona USA Vulkan.sh" "$OUT/"
chmod +x "$OUT/Daytona USA Vulkan.sh"
cp "$BDIR/daytona-vulkan.aarch64" "$OUT/daytonaarm-vulkan/daytona-vulkan.aarch64"
chmod +x "$OUT/daytonaarm-vulkan/daytona-vulkan.aarch64"
cp portmaster/daytonaarm/PUT-ROMS-HERE.txt "$OUT/daytonaarm-vulkan/roms/PUT-ROMS-HERE.txt"
cp portmaster/daytonaarm/gamecontrollerdb.txt "$OUT/daytonaarm-vulkan/gamecontrollerdb.txt"
cp LICENSE "$OUT/daytonaarm-vulkan/licenses/DaytonaArm-BSD3.txt"
cp THIRD_PARTY.md "$OUT/daytonaarm-vulkan/licenses/THIRD_PARTY.md"
[ -f "$EXTERN/sdl3/LICENSE.txt" ] && cp "$EXTERN/sdl3/LICENSE.txt" "$OUT/daytonaarm-vulkan/licenses/SDL3-LICENSE.txt"
[ -f "$EXTERN/imgui/LICENSE.txt" ] && cp "$EXTERN/imgui/LICENSE.txt" "$OUT/daytonaarm-vulkan/licenses/DearImGui-LICENSE.txt"
cp README.md "$OUT/daytonaarm-vulkan/README.md"

if command -v zip >/dev/null 2>&1; then
    (cd "$OUT" && zip -qr9 "$ZIPOUT" "Daytona USA Vulkan.sh" daytonaarm-vulkan)
else
    python3 -c "import shutil; shutil.make_archive('${ZIPOUT%.zip}', 'zip', '$OUT')"
fi
(cd dist && sha256sum DaytonaArm-Vulkan-aarch64.zip > DaytonaArm-Vulkan-aarch64.zip.sha256)
echo "Packaged: $ZIPOUT ($(du -h "$ZIPOUT" | cut -f1))"
echo "Next step: place your MAME ROMs in roms/ and copy cache/daytona*-aarch64.so (built via tools/build-arm64-cache.sh) into cache/."
