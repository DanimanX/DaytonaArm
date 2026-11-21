// Runtime dlopen() loader for PC-precompiled DaytonaArm game modules (.so).
#pragma once

#include "module_abi.h"

#include <string>

namespace arm64 {

class ModuleLoader {
public:
    ModuleLoader() = default;
    ~ModuleLoader() { unload(); }
    ModuleLoader(const ModuleLoader &) = delete;
    ModuleLoader &operator=(const ModuleLoader &) = delete;

    // Load a compiled game module (.so) and validate ABI parity + ROM set match.
    bool load(const std::string &so_path, const std::string &expected_romset, std::string &error_out);
    void unload();

    bool loaded() const { return api_ != nullptr; }
    const DaytonaModuleApi *api() const { return api_; }
    const std::string &path() const { return path_; }
    std::string romset() const { return api_ && api_->romset ? api_->romset : ""; }

    // Default architecture suffix for cached .so modules (aarch64 or x86_64).
    static const char *arch_tag();
    // Locate <data_dir>/cache/<romset>-<arch>.so (also accepts flat <romset>.so).
    static std::string find_module(const std::string &data_dir, const std::string &romset);
    // Locate <data_dir>/roms/<romset>.zip or <data_dir>/run/<romset>.zip.
    static std::string find_rom(const std::string &data_dir, const std::string &romset);

private:
    void *handle_ = nullptr;
    const DaytonaModuleApi *api_ = nullptr;
    std::string path_;
};

// Active module pointer used by the runtime gen/tgpgen/sndgen bridge.
const DaytonaModuleApi *active_module();

} // namespace arm64
