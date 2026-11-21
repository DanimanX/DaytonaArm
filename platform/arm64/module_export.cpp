// Exported entry point compiled into each ROM-derived shared module (.so).
// Linked with a version script so only daytona_module_api_v1 is globally
// exported and all internal gen/tgpgen/sndgen calls resolve locally inside
// the .so (zero ELF interposition against the host runtime bridge).
#include "module_abi.h"

#ifndef M2_ROMSET
#error "M2_ROMSET must be defined when building a DaytonaArm game module"
#endif

extern "C" __attribute__((visibility("default")))
const arm64::DaytonaModuleApi *daytona_module_api_v1(void) {
    static const arm64::DaytonaModuleApi api = {
        arm64::kModuleMagic,
        arm64::kModuleAbiVersion,
        arm64::module_fast_flags(),
        uint32_t(sizeof(rt::Cpu)),
        uint32_t(sizeof(rt::Lockstep)),
        uint32_t(sizeof(rt::Tgp)),
        uint32_t(sizeof(snd::Cpu68k)),
        uint32_t(sizeof(snd::Sched)),
        M2_ROMSET,
        &gen::has_code,
        &gen::run,
        &gen::native_instructions,
        &rt::tgpgen::run,
        rt::tgpgen::program_crc32,
        rt::tgpgen::program_words,
        &sndgen::has_code,
        &sndgen::run,
        &sndgen::native_instructions,
    };
    return &api;
}
