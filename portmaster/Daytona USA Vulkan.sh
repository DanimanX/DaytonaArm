#!/usr/bin/env bash
# Launcher for DaytonaArm Vulkan (AArch64 / SDL3 SDL_GPU Vulkan / SPIR-V)
# Loads user-built cache/daytona-aarch64.so or cache/daytona93-aarch64.so via dlopen().
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -d "$DIR/daytonaarm-vulkan" ]; then
    GAMEDIR="$DIR/daytonaarm-vulkan"
else
    GAMEDIR="$DIR"
fi

cd "$GAMEDIR"
exec ./daytona-vulkan.aarch64 --data-dir "$GAMEDIR" --gpu vulkan "$@"
