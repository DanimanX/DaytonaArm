// Dynamic game-module ABI for DaytonaArm (distributable ARM64/Linux runtime +
// PC-compiled per-ROM shared libraries loaded via dlopen).
//
// Zero ROM bytes or ROM-derived game code ever ship in the runtime binary.
// Each user builds cache/daytona-aarch64.so and/or cache/daytona93-aarch64.so
// locally from their own MAME ROM sets using tools/build-arm64-cache.sh.
#pragma once

#include "runtime/cpu.h"
#include "runtime/gen_support.h"
#include "runtime/lockstep.h"
#include "runtime/snd_cpu.h"
#include "runtime/snd_gen_support.h"
#include "runtime/snd_sched.h"
#include "runtime/tgp.h"

#include <cstdint>

namespace arm64 {

inline constexpr uint32_t kModuleMagic = 0x44594152u; // "DYAR"
inline constexpr uint32_t kModuleAbiVersion = 1u;

inline constexpr uint32_t module_fast_flags() {
    uint32_t flags = 0;
#ifdef M2_FAST_GEN
    flags |= 1u << 0;
#endif
#ifdef M2_FAST_BUS
    flags |= 1u << 1;
#endif
#ifdef M2_FAST_TGP
    flags |= 1u << 2;
#endif
#ifdef M2_FAST_CPU
    flags |= 1u << 3;
#endif
#ifdef M2_FAST_GEO
    flags |= 1u << 4;
#endif
#ifdef M2_VITA_RENDER_OPT
    flags |= 1u << 5;
#endif
    return flags;
}

struct DaytonaModuleApi {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t fast_flags;
    uint32_t sizeof_cpu;
    uint32_t sizeof_lockstep;
    uint32_t sizeof_tgp;
    uint32_t sizeof_cpu68k;
    uint32_t sizeof_sched;
    const char *romset; // "daytona93" or "daytona"

    // Recompiled i960 main CPU
    bool (*gen_has_code)(uint32_t addr);
    void (*gen_run)(gen::Env &env);
    uint64_t (*gen_native_instructions)();

    // Recompiled TGP geometry coprocessor
    void (*tgp_run)(rt::Tgp &t, uint64_t budget);
    uint32_t tgp_program_crc32;
    uint32_t tgp_program_words;

    // Recompiled M68000 sound CPU
    bool (*snd_has_code)(uint32_t addr);
    void (*snd_run)(sndgen::Env &env);
    uint64_t (*snd_native_instructions)();
};

using ModuleEntryFn = const DaytonaModuleApi *(*)();
inline constexpr const char *kModuleEntrySymbol = "daytona_module_api_v1";

} // namespace arm64
