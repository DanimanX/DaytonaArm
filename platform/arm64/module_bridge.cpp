// Runtime bridge forwarding gen::, rt::tgpgen::, and sndgen:: to the active
// dynamically loaded game module (.so). Keeps src/runtime/ completely ROM-free.
#include "module_bridge.h"
#include "runtime/rom_import.h"

#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <stdexcept>
#include <sys/stat.h>

namespace arm64 {

namespace {
const DaytonaModuleApi *g_active_api = nullptr;

bool file_exists(const std::string &path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
} // namespace

const DaytonaModuleApi *active_module() { return g_active_api; }

const char *ModuleLoader::arch_tag() {
#if defined(__aarch64__)
    return "aarch64";
#elif defined(__x86_64__)
    return "x86_64";
#elif defined(__arm__)
    return "arm";
#else
    return "native";
#endif
}

std::string ModuleLoader::find_module(const std::string &data_dir, const std::string &romset) {
    const std::string arch = arch_tag();
    const std::string candidates[] = {
        data_dir + "/cache/" + romset + "-" + arch + ".so",
        data_dir + "/cache/rec-" + romset + "-" + arch + ".so",
        data_dir + "/cache/" + romset + ".so",
        data_dir + "/" + romset + "-" + arch + ".so",
    };
    for (const std::string &p : candidates)
        if (file_exists(p)) return p;
    return "";
}

std::string ModuleLoader::find_rom(const std::string &data_dir, const std::string &romset) {
    const std::string candidates[] = {
        data_dir + "/roms/" + romset + ".zip",
        data_dir + "/run/" + romset + ".zip",
        data_dir + "/" + romset + ".zip",
    };
    for (const std::string &p : candidates)
        if (file_exists(p)) return p;
    return "";
}

bool ModuleLoader::load(const std::string &so_path, const std::string &expected_romset, std::string &error_out) {
    unload();
    error_out.clear();
    if (so_path.empty() || !file_exists(so_path)) {
        error_out = "Missing compiled module for '" + expected_romset + "'. Run tools/build-arm64-cache.sh on PC and copy cache/" +
                    expected_romset + "-" + arch_tag() + ".so to your handheld.";
        return false;
    }

    ::dlerror();
    void *handle = ::dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char *err = ::dlerror();
        error_out = std::string("dlopen failed for ") + so_path + ": " + (err ? err : "unknown error");
        return false;
    }

    ::dlerror();
    void *sym = ::dlsym(handle, kModuleEntrySymbol);
    if (!sym) {
        const char *err = ::dlerror();
        error_out = std::string("Missing symbol ") + kModuleEntrySymbol + " in " + so_path + ": " + (err ? err : "not found");
        ::dlclose(handle);
        return false;
    }

    auto entry = reinterpret_cast<ModuleEntryFn>(sym);
    const DaytonaModuleApi *api = entry();
    if (!api || api->magic != kModuleMagic) {
        error_out = "Invalid DaytonaArm module header in " + so_path;
        ::dlclose(handle);
        return false;
    }
    if (api->abi_version != kModuleAbiVersion) {
        error_out = "Module ABI mismatch (got v" + std::to_string(api->abi_version) + ", expected v" +
                    std::to_string(kModuleAbiVersion) + "). Rebuild cache on PC.";
        ::dlclose(handle);
        return false;
    }
    if (api->fast_flags != module_fast_flags()) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "Module fast_flags mismatch (got 0x%x, expected 0x%x). Rebuild cache on PC.",
                      api->fast_flags, module_fast_flags());
        error_out = buf;
        ::dlclose(handle);
        return false;
    }
    if (api->sizeof_cpu != sizeof(rt::Cpu) || api->sizeof_lockstep != sizeof(rt::Lockstep) ||
        api->sizeof_tgp != sizeof(rt::Tgp) || api->sizeof_cpu68k != sizeof(snd::Cpu68k) ||
        api->sizeof_sched != sizeof(snd::Sched)) {
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "Struct ABI mismatch in %s (cpu %u/%zu, ls %u/%zu, tgp %u/%zu, 68k %u/%zu, sched %u/%zu). Rebuild cache.",
                      so_path.c_str(), api->sizeof_cpu, sizeof(rt::Cpu), api->sizeof_lockstep, sizeof(rt::Lockstep),
                      api->sizeof_tgp, sizeof(rt::Tgp), api->sizeof_cpu68k, sizeof(snd::Cpu68k), api->sizeof_sched,
                      sizeof(snd::Sched));
        error_out = buf;
        ::dlclose(handle);
        return false;
    }
    if (!api->romset || (!expected_romset.empty() && expected_romset != api->romset)) {
        error_out = std::string("Module ROM set mismatch: file provides '") + (api->romset ? api->romset : "?") +
                    "', expected '" + expected_romset + "'.";
        ::dlclose(handle);
        return false;
    }
    if (!api->gen_has_code || !api->gen_run || !api->gen_native_instructions || !api->tgp_run || !api->snd_has_code ||
        !api->snd_run || !api->snd_native_instructions) {
        error_out = "Incomplete function table in " + so_path;
        ::dlclose(handle);
        return false;
    }

    handle_ = handle;
    api_ = api;
    path_ = so_path;
    g_active_api = api_;
#ifdef M2_DYNAMIC_ROMSET
    rt::set_rom_set_name(api_->romset);
#endif
    return true;
}

void ModuleLoader::unload() {
    if (g_active_api == api_) g_active_api = nullptr;
    api_ = nullptr;
    path_.clear();
    if (handle_) {
        ::dlclose(handle_);
        handle_ = nullptr;
    }
}

} // namespace arm64

// ---- Runtime symbol bridge (called by src/runtime/) ----------------------------

namespace gen {
bool has_code(uint32_t addr) {
    const auto *m = arm64::active_module();
    return m ? m->gen_has_code(addr) : false;
}
void run(Env &e) {
    const auto *m = arm64::active_module();
    if (!m) throw std::runtime_error("No DaytonaArm game module loaded");
    m->gen_run(e);
}
uint64_t native_instructions() {
    const auto *m = arm64::active_module();
    return m ? m->gen_native_instructions() : 0;
}
} // namespace gen

namespace rt::tgpgen {
// Both Daytona USA Deluxe '93 and Revision A upload the exact same 2,024-word TGP program.
const uint32_t program_crc32 = 0xD6D611DDu;
const uint32_t program_words = 2024u;
void run(Tgp &t, uint64_t budget) {
    const auto *m = arm64::active_module();
    if (!m) throw std::runtime_error("No DaytonaArm game module loaded (TGP)");
    m->tgp_run(t, budget);
}
} // namespace rt::tgpgen

namespace sndgen {
bool has_code(uint32_t addr) {
    const auto *m = arm64::active_module();
    return m ? m->snd_has_code(addr) : false;
}
void run(Env &e) {
    const auto *m = arm64::active_module();
    if (!m) throw std::runtime_error("No DaytonaArm game module loaded (68000)");
    m->snd_run(e);
}
uint64_t native_instructions() {
    const auto *m = arm64::active_module();
    return m ? m->snd_native_instructions() : 0;
}
} // namespace sndgen
