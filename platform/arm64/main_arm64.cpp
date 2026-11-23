// DaytonaArm — Distributable ARM64 / OpenGL ES 2.0 / SDL2 frontend for
// daytona-arcade-recomp (Sega Model 2 Daytona USA static recompilation).
//
// Engineered for Anbernic RG34XX (Allwinner H700: 4x Cortex-A53 @ 1.5 GHz,
// Mali-G31 MP2, 1 GB LPDDR4, 720x480) and compatible PortMaster Linux handhelds:
//   * Core 0: Main loop, recompiled i960 + TGP, System 24 tile cache, GLES 2.0 submit
//   * Core 1: Pipelined MAME geometrizer thread (platform/arm64/src/runtime/geo.cpp)
//   * Core 2: Reference sound board worker (recompiled 68k + 2x MultiPCM + YM3438)
//   * Core 3: System 24 NEON tile indexer worker + SDL audio callback
//   * Game code loaded dynamically from PC-built cache/<romset>-<arch>.so via dlopen()
#define SDL_MAIN_HANDLED

#include "app/link_socket.h"
#include "audio.h"
#include "controls.h"
#include "gpu_gles.h"
#include "imgui_gles.h"
#include "module_bridge.h"
#include "native_audio.h"
#include "runtime/drive_board.h"
#include "runtime/game_loop.h"
#include "runtime/geo.h"
#include "runtime/native_sound_engine.h"
#include "runtime/rom_import.h"
#include "runtime/test_hold.h"
#include "sound_worker.h"
#include "thread_affinity.h"

#include <SDL.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr int kGeoCore = 1;
constexpr int kSoundCore = 2;
constexpr int kWorker2DCore = 3;

uint64_t ticks_us() {
    struct timespec ts {};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000ull + uint64_t(ts.tv_nsec) / 1000ull;
}

void ensure_dir(const std::string &path) {
    if (path.empty()) return;
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur.push_back(path[i]);
        if (path[i] == '/' && cur.size() > 1) ::mkdir(cur.c_str(), 0755);
    }
    ::mkdir(cur.c_str(), 0755);
}

std::string executable_dir(const char *argv0) {
    char buf[4096] = {};
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string p = (n > 0) ? std::string(buf, size_t(n)) : (argv0 ? std::string(argv0) : std::string("."));
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

bool load_bytes(const std::string &path, uint8_t *data, size_t size) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    const bool ok = std::fread(data, 1, size, f) == size && std::fgetc(f) == EOF;
    std::fclose(f);
    return ok;
}

bool save_bytes(const std::string &path, const uint8_t *data, size_t size) {
    const std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data, 1, size, f) == size;
    if (std::fflush(f) != 0) ok = false;
    if (std::fclose(f) != 0) ok = false;
    if (!ok) {
        std::remove(tmp.c_str());
        return false;
    }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

rt::Geo *geometry_of(rt::GameLoop *game) {
    return game ? rt::Geo::find(game->board().tgp().buffer_data()) : nullptr;
}

constexpr const char *kBindKeys[arm64::ActionCount] = {
    "bind_accelerate", "bind_brake", "bind_steer_left", "bind_steer_right",
    "bind_shift_up", "bind_shift_down", "bind_start", "bind_coin",
    "bind_view1", "bind_view2", "bind_view3", "bind_view4"
};

struct Settings {
    std::string romset = "daytona"; // "daytona" (Rev A 1994) or "daytona93" (Deluxe '93)
    int aspect = 0;                 // 0 = 4:3 Original, 1 = 3:2 Fill/Stretch
    bool vsync = true;
    bool sync_60hz = false;         // false = authentic 57.524 Hz arcade pacing, true = 60 Hz display sync
    int draw_distance = 0;          // -2..+2
    int frame_skip = 0;             // 0 = every frame, 1 = every 2nd, 2 = every 3rd
    int volume = 90;
    int music_volume = 100;
    int effects_volume = 50;
    bool mute = false;
    bool native_audio = false;
    bool gamepad_fix = true;        // D-pad arcade centring spring model (ideal for RG34XX)
    bool gamepad_menu = true;       // Step-wise menu selection
    int steer_curve = 0;            // 0 Linear, 1 Soft, 2 Extra Soft (analog sticks)
    int deadzone = 12;
    bool steer_invert = false;      // D-pad and analog
    bool stick_invert = false;      // analog stick only (device wiring differs from SDL's convention)
    bool skip_launcher = false;
    bool show_fps = false;
    bool link_enabled = false;
    bool link_sync = false;
    int link_port = 15112;
    int next_port = 15112;
    int next_ip[4] = {192, 168, 1, 2};
    arm64::Bindings bindings;

    std::string next_address() const {
        return std::to_string(next_ip[0]) + "." + std::to_string(next_ip[1]) + "." +
               std::to_string(next_ip[2]) + "." + std::to_string(next_ip[3]);
    }

    void defaults(const std::string &keep_romset = "") {
        std::string keep = keep_romset.empty() ? romset : keep_romset;
        *this = Settings{};
        if (keep == "daytona" || keep == "daytona93") romset = keep;
    }

    void sanitize() {
        if (romset != "daytona" && romset != "daytona93") romset = "daytona";
        aspect = std::clamp(aspect, 0, 1);
        draw_distance = std::clamp(draw_distance, -2, 2);
        frame_skip = std::clamp(frame_skip, 0, 2);
        volume = std::clamp(volume, 0, 100);
        music_volume = std::clamp(music_volume, 0, 100);
        effects_volume = std::clamp(effects_volume, 0, 100);
        steer_curve = std::clamp(steer_curve, 0, 2);
        deadzone = std::clamp(deadzone, 0, 40);
        link_port = std::clamp(link_port, 1, 65535);
        next_port = std::clamp(next_port, 1, 65535);
        for (int &o : next_ip) o = std::clamp(o, 0, 255);
        bindings.sanitize();
    }

    void load(const std::string &path) {
        FILE *f = std::fopen(path.c_str(), "r");
        if (!f) return;
        char line[128], key[48], sval[64];
        while (std::fgets(line, sizeof line, f)) {
            if (std::sscanf(line, "%47[^=]=%63s", key, sval) != 2) continue;
            const int value = std::atoi(sval);
            if (!std::strcmp(key, "romset")) romset = sval;
            else if (!std::strcmp(key, "aspect")) aspect = value;
            else if (!std::strcmp(key, "vsync")) vsync = value != 0;
            else if (!std::strcmp(key, "sync_60hz")) sync_60hz = value != 0;
            else if (!std::strcmp(key, "draw_distance")) draw_distance = value;
            else if (!std::strcmp(key, "frame_skip")) frame_skip = value;
            else if (!std::strcmp(key, "volume")) volume = value;
            else if (!std::strcmp(key, "music_volume")) music_volume = value;
            else if (!std::strcmp(key, "effects_volume")) effects_volume = value;
            else if (!std::strcmp(key, "mute")) mute = value != 0;
            else if (!std::strcmp(key, "native_audio")) native_audio = value != 0;
            else if (!std::strcmp(key, "gamepad_fix")) gamepad_fix = value != 0;
            else if (!std::strcmp(key, "gamepad_menu")) gamepad_menu = value != 0;
            else if (!std::strcmp(key, "steer_curve")) steer_curve = value;
            else if (!std::strcmp(key, "deadzone")) deadzone = value;
            else if (!std::strcmp(key, "steer_invert")) steer_invert = value != 0;
            else if (!std::strcmp(key, "stick_invert")) stick_invert = value != 0;
            else if (!std::strcmp(key, "skip_launcher")) skip_launcher = value != 0;
            else if (!std::strcmp(key, "show_fps")) show_fps = value != 0;
            else if (!std::strcmp(key, "link_enabled")) link_enabled = value != 0;
            else if (!std::strcmp(key, "link_sync")) link_sync = value != 0;
            else if (!std::strcmp(key, "link_port")) link_port = value;
            else if (!std::strcmp(key, "next_port")) next_port = value;
            else if (!std::strcmp(key, "next_ip0")) next_ip[0] = value;
            else if (!std::strcmp(key, "next_ip1")) next_ip[1] = value;
            else if (!std::strcmp(key, "next_ip2")) next_ip[2] = value;
            else if (!std::strcmp(key, "next_ip3")) next_ip[3] = value;
            else {
                for (int a = 0; a < arm64::ActionCount; ++a)
                    if (!std::strcmp(key, kBindKeys[a])) bindings.button[a] = uint32_t(std::strtoul(sval, nullptr, 10));
            }
        }
        std::fclose(f);
        sanitize();
    }

    bool save(const std::string &path) const {
        const std::string tmp = path + ".tmp";
        FILE *f = std::fopen(tmp.c_str(), "w");
        if (!f) return false;
        std::fprintf(f, "romset=%s\naspect=%d\nvsync=%d\nsync_60hz=%d\ndraw_distance=%d\nframe_skip=%d\n",
                     romset.c_str(), aspect, int(vsync), int(sync_60hz), draw_distance, frame_skip);
        std::fprintf(f, "volume=%d\nmusic_volume=%d\neffects_volume=%d\nmute=%d\nnative_audio=%d\n",
                     volume, music_volume, effects_volume, int(mute), int(native_audio));
        std::fprintf(f, "gamepad_fix=%d\ngamepad_menu=%d\nsteer_curve=%d\ndeadzone=%d\nsteer_invert=%d\n",
                     int(gamepad_fix), int(gamepad_menu), steer_curve, deadzone, int(steer_invert));
        std::fprintf(f, "stick_invert=%d\n", int(stick_invert));
        std::fprintf(f, "skip_launcher=%d\nshow_fps=%d\n", int(skip_launcher), int(show_fps));
        std::fprintf(f, "link_enabled=%d\nlink_sync=%d\nlink_port=%d\nnext_port=%d\n",
                     int(link_enabled), int(link_sync), link_port, next_port);
        for (int i = 0; i < 4; ++i) std::fprintf(f, "next_ip%d=%d\n", i, next_ip[i]);
        for (int a = 0; a < arm64::ActionCount; ++a)
            std::fprintf(f, "%s=%u\n", kBindKeys[a], unsigned(bindings.button[a]));
        bool ok = std::fflush(f) == 0;
        if (std::fclose(f) != 0) ok = false;
        if (!ok) { std::remove(tmp.c_str()); return false; }
        std::remove(path.c_str());
        return std::rename(tmp.c_str(), path.c_str()) == 0;
    }
};

struct CliOptions {
    std::string data_dir;
    std::string romset;
    std::string explicit_module;
    std::string explicit_rom;
    bool autostart = false;
    bool fullscreen = true;
    bool show_fps = false;
    int bench_frames = 0;
};

void print_usage(const char *prog) {
    std::printf(
        "DaytonaArm — Sega Model 2 Daytona USA ARM64 / OpenGL ES 2.0 Runtime\n"
        "Usage: %s [options]\n"
        "  --data-dir PATH      Data folder containing roms/, cache/, saves/\n"
        "  --romset NAME        Select ROM set: daytona (Rev A 1994) or daytona93 (Deluxe '93)\n"
        "  --module FILE.so     Override compiled game module path\n"
        "  --rom FILE.zip       Override MAME ROM archive path\n"
        "  --autostart          Boot straight into the selected game\n"
        "  --fullscreen         Launch fullscreen (default)\n"
        "  --windowed           Launch in a 720x480 window\n"
        "  --fps                Enable FPS readout\n"
        "  --bench-frames N     Headless benchmark/verification of N frames through dlopen()\n"
        "  --help               Show this help message\n",
        prog);
}

int run_headless_bench(const CliOptions &cli, const std::string &data_dir, const std::string &romset) {
    arm64::ModuleLoader loader;
    std::string so_path = cli.explicit_module.empty()
                              ? arm64::ModuleLoader::find_module(data_dir, romset)
                              : cli.explicit_module;
    std::string rom_path = cli.explicit_rom.empty()
                               ? arm64::ModuleLoader::find_rom(data_dir, romset)
                               : cli.explicit_rom;
    std::string err;
    if (!loader.load(so_path, romset, err)) {
        std::fprintf(stderr, "[bench] module load error: %s\n", err.c_str());
        return 1;
    }
    if (rom_path.empty()) {
        std::fprintf(stderr, "[bench] ROM not found for %s in %s/roms/\n", romset.c_str(), data_dir.c_str());
        return 1;
    }
    std::printf("[bench] Loaded module %s (romset=%s, i960_insns=%llu, snd_insns=%llu)\n",
                so_path.c_str(), loader.romset().c_str(),
                (unsigned long long)gen::native_instructions(),
                (unsigned long long)sndgen::native_instructions());
    try {
        auto images = rt::import_rom_set(rom_path);
        rt::GameLoop game(std::move(images), true);
        game.board().video().set_external_3d(true);
        if (rt::Geo *geo = geometry_of(&game)) geo->configure(rt::GeoMode::Pipelined, kGeoCore, nullptr);

        arm64::SoundWorker sound_worker;
        sound_worker.set_core(kSoundCore);
        sound_worker.open();
        rt::GameLoop::SoundPacket active_sound;
        bool sound_in_flight = false;

        struct NullAudio {
            void push(snd::SoundBoard &b) {
                (void)b.take_fm();
                (void)b.take_pcm();
            }
        } null_audio;

        rt::Inputs inputs{};
        const uint64_t t0 = ticks_us();
        size_t total_polys = 0;
        for (int f = 0; f < cli.bench_frames; ++f) {
            auto pkt = game.run_frame_sound_packet(inputs);
            if (sound_in_flight) {
                sound_worker.finish();
                sound_in_flight = false;
            }
            active_sound = std::move(pkt);
            sound_worker.dispatch_packet(active_sound, null_audio, nullptr);
            sound_in_flight = true;
            total_polys += game.board().video().gpu_polys().size();
        }
        if (sound_in_flight) sound_worker.finish();
        sound_worker.close();

        const double sec = double(ticks_us() - t0) / 1e6;
        const double fps = sec > 0.0 ? double(cli.bench_frames) / sec : 0.0;
        std::printf("[bench] Completed %d frames in %.3f s (%.1f FPS, %.2f ms/frame) | i960=%llu insns | polys=%zu\n",
                    cli.bench_frames, sec, fps, sec * 1000.0 / double(cli.bench_frames),
                    (unsigned long long)game.instructions(), total_polys);
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[bench] runtime error: %s\n", e.what());
        return 1;
    }
}

rt::TestHold test_hold;

constexpr int kOptionCount = 25;
constexpr int kBindRows = arm64::ActionCount + 2;

void draw_menu(bool have_game, bool &options, bool &binds, int &selection, int capture,
               const Settings &settings, const std::string &status, double fps,
               const std::string &data_dir) {
    const float w = float(arm64::gl_display_width());
    const float h = float(arm64::gl_display_height());
    ImGui::SetNextWindowPos({8.0f, 8.0f});
    ImGui::SetNextWindowSize({w - 16.0f, h - 16.0f});
    ImGui::Begin("DaytonaArm — Daytona USA", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    ImGui::Text("DaytonaArm | %.1f FPS | %s | %s", fps,
                settings.romset == "daytona" ? "Rev A (1994)" : "Deluxe '93",
                arm64::gl_is_gles() ? "GLES 2.0" : "OpenGL 2.1");
    ImGui::TextDisabled("D-Pad Navigate | A Select | B Back | Start+Select/Menu Pause");
    ImGui::Separator();

    static int prev_sel = -1;
    static bool prev_opt = false, prev_bind = false;
    const bool scroll = selection != prev_sel || options != prev_opt || binds != prev_bind;

    if (ImGui::BeginChild("body", {0.0f, -74.0f}, ImGuiChildFlags_None)) {
        if (binds) {
            char row[96];
            for (int i = 0; i < kBindRows; ++i) {
                const char *label = (i == arm64::ActionCount) ? "RESET CONTROLS TO DEFAULTS" : "BACK";
                if (i < arm64::ActionCount) {
                    std::snprintf(row, sizeof row, "%-14s : %s", arm64::action_name(i),
                                  capture == i ? "PRESS A BUTTON..." : arm64::button_name(settings.bindings.button[i]));
                    label = row;
                }
                if (ImGui::Selectable(label, selection == i, 0, {0.0f, 26.0f})) selection = i;
                if (selection == i && scroll) ImGui::SetScrollHereY();
            }
        } else if (!options) {
            const bool has_rev = !arm64::ModuleLoader::find_module(data_dir, "daytona").empty() &&
                                 !arm64::ModuleLoader::find_rom(data_dir, "daytona").empty();
            const bool has_93 = !arm64::ModuleLoader::find_module(data_dir, "daytona93").empty() &&
                                !arm64::ModuleLoader::find_rom(data_dir, "daytona93").empty();
            char rom_row[128];
            std::snprintf(rom_row, sizeof rom_row, "ROM Set: %s  [%s]",
                          settings.romset == "daytona" ? "Daytona USA Rev A (1994)" : "Daytona USA Deluxe '93",
                          (settings.romset == "daytona" ? has_rev : has_93) ? "READY" : "NEEDS ROM + .SO CACHE");
            const char *labels[6] = {
                have_game ? "Resume / Start Selected ROM" : "Start Game",
                "Reset Game",
                rom_row,
                "Options",
                "Controls",
                "Quit"
            };
            for (int i = 0; i < 6; ++i) {
                if (ImGui::Selectable(labels[i], selection == i, 0, {0.0f, 30.0f})) selection = i;
                if (selection == i && scroll) ImGui::SetScrollHereY();
            }
        } else {
            char lines[kOptionCount][96];
            static const char *kDistances[] = {"SHORTEST (-2)", "SHORTER (-1)", "DEFAULT (0)", "FURTHER (+1)", "FURTHEST (+2)"};
            static const char *kCurves[] = {"LINEAR", "SOFT", "EXTRA SOFT"};
            static const char *kSkip[] = {"OFF (EVERY FRAME)", "EVERY 2ND FRAME", "EVERY 3RD FRAME"};

            std::snprintf(lines[0], sizeof lines[0], "ROM SET: %s",
                          settings.romset == "daytona" ? "DAYTONA (1994 REV A)" : "DAYTONA93 (DELUXE '93)");
            std::snprintf(lines[1], sizeof lines[1], "ASPECT RATIO: %s",
                          settings.aspect == 0 ? "4:3 ORIGINAL" : "3:2 FULL SCREEN");
            std::snprintf(lines[2], sizeof lines[2], "VSYNC: %s", settings.vsync ? "ON" : "OFF");
            std::snprintf(lines[3], sizeof lines[3], "PACING: %s",
                          settings.sync_60hz ? "60.00 HZ DISPLAY SYNC" : "57.52 HZ ARCADE NATIVE");
            std::snprintf(lines[4], sizeof lines[4], "DRAW DISTANCE: %s", kDistances[settings.draw_distance + 2]);
            std::snprintf(lines[5], sizeof lines[5], "DRAW MODE: %s", kSkip[settings.frame_skip]);
            std::snprintf(lines[6], sizeof lines[6], "AUDIO ENGINE: %s",
                          settings.native_audio ? "NATIVE (LOW CPU)" : "REFERENCE (68K+PCM)");
            std::snprintf(lines[7], sizeof lines[7], "VOLUME: %d%%", settings.volume);
            std::snprintf(lines[8], sizeof lines[8], "MUSIC VOLUME: %d%%", settings.music_volume);
            std::snprintf(lines[9], sizeof lines[9], "EFFECTS VOLUME: %d%%", settings.effects_volume);
            std::snprintf(lines[10], sizeof lines[10], "MUTE: %s", settings.mute ? "ON" : "OFF");
            std::snprintf(lines[11], sizeof lines[11], "D-PAD SPRING STEERING (RG34XX): %s",
                          settings.gamepad_fix ? "ON" : "OFF");
            std::snprintf(lines[12], sizeof lines[12], "MENU STEP STEERING: %s",
                          settings.gamepad_menu ? "ON" : "OFF");
            std::snprintf(lines[13], sizeof lines[13], "ANALOG STEERING CURVE: %s", kCurves[settings.steer_curve]);
            std::snprintf(lines[14], sizeof lines[14], "ANALOG DEADZONE: %d%%", settings.deadzone);
            std::snprintf(lines[15], sizeof lines[15], "INVERT STEERING: %s", settings.steer_invert ? "ON" : "OFF");
            std::snprintf(lines[16], sizeof lines[16], "INVERT ANALOG STICK ONLY: %s",
                          settings.stick_invert ? "ON" : "OFF");
            std::snprintf(lines[17], sizeof lines[17], "SKIP LAUNCHER ON BOOT: %s", settings.skip_launcher ? "ON" : "OFF");
            std::snprintf(lines[18], sizeof lines[18], "LOG FPS TO CONSOLE: %s", settings.show_fps ? "ON" : "OFF");
            std::snprintf(lines[19], sizeof lines[19], "HOLD TEST BUTTON: %s",
                          test_hold.armed() ? "ARMED (RESUME TO ENTER TEST)" : "OFF");
            std::snprintf(lines[20], sizeof lines[20], "LAN LINK PLAY (REV A): %s",
                          settings.link_enabled ? "ON" : "OFF");
            std::snprintf(lines[21], sizeof lines[21], "LINK NEXT IP: %s:%d (LISTEN %d)",
                          settings.next_address().c_str(), settings.next_port, settings.link_port);
            std::snprintf(lines[22], sizeof lines[22], "LINK FRAME SYNC: %s", settings.link_sync ? "ON" : "OFF");
            std::snprintf(lines[23], sizeof lines[23], "RESET DEFAULTS");
            std::snprintf(lines[24], sizeof lines[24], "BACK");

            for (int i = 0; i < kOptionCount; ++i) {
                if (ImGui::Selectable(lines[i], selection == i, 0, {0.0f, 26.0f})) selection = i;
                if (selection == i && scroll) ImGui::SetScrollHereY();
            }
        }
    }
    ImGui::EndChild();
    prev_sel = selection;
    prev_opt = options;
    prev_bind = binds;

    ImGui::Separator();
    ImGui::TextWrapped("%s", status.c_str());
    ImGui::End();
}

} // namespace

int main(int argc, char **argv) {
    CliOptions cli;
    cli.data_dir = executable_dir(argc > 0 ? argv[0] : nullptr);
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
            print_usage(argv[0]);
            return 0;
        } else if (!std::strcmp(argv[i], "--data-dir") && i + 1 < argc) {
            cli.data_dir = argv[++i];
        } else if (!std::strcmp(argv[i], "--romset") && i + 1 < argc) {
            cli.romset = argv[++i];
        } else if (!std::strcmp(argv[i], "--module") && i + 1 < argc) {
            cli.explicit_module = argv[++i];
        } else if (!std::strcmp(argv[i], "--rom") && i + 1 < argc) {
            cli.explicit_rom = argv[++i];
        } else if (!std::strcmp(argv[i], "--autostart")) {
            cli.autostart = true;
        } else if (!std::strcmp(argv[i], "--fullscreen")) {
            cli.fullscreen = true;
        } else if (!std::strcmp(argv[i], "--windowed")) {
            cli.fullscreen = false;
        } else if (!std::strcmp(argv[i], "--fps")) {
            cli.show_fps = true;
        } else if (!std::strcmp(argv[i], "--bench-frames") && i + 1 < argc) {
            cli.bench_frames = std::max(1, std::atoi(argv[++i]));
        }
    }

    ensure_dir(cli.data_dir + "/saves");
    const std::string cfg_path = cli.data_dir + "/daytonaarm.cfg";

    Settings settings;
    settings.load(cfg_path);
    if (!cli.romset.empty()) {
        settings.romset = cli.romset;
        settings.sanitize();
    } else if (arm64::ModuleLoader::find_module(cli.data_dir, settings.romset).empty()) {
        const std::string other = settings.romset == "daytona" ? "daytona93" : "daytona";
        if (!arm64::ModuleLoader::find_module(cli.data_dir, other).empty()) settings.romset = other;
    }
    if (cli.show_fps) settings.show_fps = true;

    // Spread the main thread over every core instead of pinning it to Core 0.
    // This call happens before SDL_Init()/gl_init(), and children inherit the
    // mask: pinning to one core here also pins PulseAudio and every libmali
    // worker thread to that core, which starves the render loop (see
    // thread_affinity.h). Dedicated worker cores are still claimed by the
    // geometry, sound and System 24 threads pinning themselves.
    arm64::pin_current_thread_all();

    if (cli.bench_frames > 0) return run_headless_bench(cli, cli.data_dir, settings.romset);

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
        std::fprintf(stderr, "[DaytonaArm] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    arm64::GlConfig gl_cfg;
    gl_cfg.fullscreen = cli.fullscreen;
    gl_cfg.vsync = settings.vsync;
    gl_cfg.worker_2d_core = kWorker2DCore;
    if (!arm64::gl_init(gl_cfg)) {
        std::fprintf(stderr, "[DaytonaArm] OpenGL ES init failed: %s\n", arm64::gl_error());
        SDL_Quit();
        return 1;
    }

    arm64::ImGuiGles ui;
    if (!ui.init()) {
        std::fprintf(stderr, "[DaytonaArm] ImGui init failed\n");
        arm64::gl_fini();
        SDL_Quit();
        return 1;
    }

    arm64::GpuGlesRenderer gpu(kWorker2DCore);
    if (!gpu.ok()) {
        std::fprintf(stderr, "[DaytonaArm] GPU texture setup failed\n");
        gpu.shutdown();
        ui.shutdown();
        arm64::gl_fini();
        SDL_Quit();
        return 1;
    }

    arm64::PadManager pads;
    pads.init(cli.data_dir);

    arm64::ModuleLoader module_loader;
    arm64::Audio audio;
    arm64::NativeAudio<snd::NativeSoundEngine> native_audio;
    arm64::SoundWorker sound_worker;
    sound_worker.set_core(kSoundCore);
    sound_worker.open();

    std::unique_ptr<app::TcpLink> link;
    std::unique_ptr<rt::GameLoop> game;
    rt::GameLoop::SoundPacket active_sound;
    bool sound_in_flight = false;
    bool active_native_audio = false;
    std::string active_romset;

    arm64::Controls controls;
    rt::DriveBoard drive_board;
    arm64::FrameClock clock(settings.sync_60hz ? 60.0 : rt::GameLoop::kFrameHz, 1);

    std::string status = "Select Start Game to race. Place ROMs in roms/ and PC-built .so in cache/.";
    double display_fps = 0.0;

    auto save_dir_for = [&](const std::string &rs) {
        const std::string dir = cli.data_dir + "/saves/" + rs;
        ensure_dir(dir);
        return dir;
    };

    auto finish_sound = [&]() -> bool {
        if (!sound_in_flight) return true;
        sound_in_flight = false;
        try {
            sound_worker.finish();
            return true;
        } catch (const std::exception &e) {
            status = std::string("Audio error: ") + e.what();
            audio.pause();
            native_audio.pause();
            active_sound = {};
            game.reset();
            return false;
        }
    };

    auto save_nv = [&]() {
        if (!finish_sound() || !game || active_romset.empty()) return;
        const std::string dir = save_dir_for(active_romset);
        save_bytes(dir + "/ioboard_eeprom.bin", game->board().io().eeprom.data(), game->board().io().eeprom.size());
        save_bytes(dir + "/backup_ram.bin", game->board().backup_ram().data(), game->board().backup_ram().size());
    };

    auto apply_preferences = [&]() {
        arm64::gl_set_vsync(settings.vsync);
        arm64::gl_set_aspect(settings.aspect == 0 ? arm64::AspectMode::Original43 : arm64::AspectMode::StretchFull);
        clock.set_hz(settings.sync_60hz ? 60.0 : rt::GameLoop::kFrameHz);
        audio.volume(float(settings.volume) / 100.0f);
        audio.mute(settings.mute);
        native_audio.volume(float(settings.volume) / 100.0f);
        native_audio.volumes(float(settings.music_volume) / 100.0f, float(settings.effects_volume) / 100.0f);
        native_audio.mute(settings.mute);
        controls.set_deadzone(float(settings.deadzone) / 100.0f);
        controls.set_steer_invert(settings.steer_invert);
        controls.set_stick_invert(settings.stick_invert);
        controls.set_steer_curve(settings.steer_curve);
        controls.set_gamepad_fix(settings.gamepad_fix);
        controls.set_gamepad_menu(settings.gamepad_menu);
        controls.set_bindings(settings.bindings);
        rt::GameLoop::set_draw_distance(settings.draw_distance);
        if (game) game->set_frame_skip(settings.frame_skip);
    };
    apply_preferences();

    auto start_game = [&]() -> bool {
        if (!finish_sound()) return false;
        save_nv();
        active_sound = {};
        native_audio.close();
        audio.close();
        game.reset();
        link.reset();
        gpu.reset_materials();
        drive_board = rt::DriveBoard{};

        const std::string target_set = settings.romset;
        const std::string so_path =
            (!cli.explicit_module.empty() && target_set == cli.romset)
                ? cli.explicit_module
                : arm64::ModuleLoader::find_module(cli.data_dir, target_set);
        const std::string rom_path =
            (!cli.explicit_rom.empty() && target_set == cli.romset)
                ? cli.explicit_rom
                : arm64::ModuleLoader::find_rom(cli.data_dir, target_set);

        status = "Loading " + target_set + "...";
        gpu.prepare_frame();
        arm64::gl_begin_frame();
        ui.frame(1.0f / 60.0f);
        ImGui::SetNextWindowPos({16.0f, 16.0f});
        ImGui::SetNextWindowSize({float(arm64::gl_display_width()) - 32.0f, float(arm64::gl_display_height()) - 32.0f});
        ImGui::Begin("Starting Daytona USA", nullptr, ImGuiWindowFlags_NoDecoration);
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::End();
        ui.render();
        arm64::gl_end_frame();

        std::string mod_err;
        if (!module_loader.load(so_path, target_set, mod_err)) {
            status = mod_err;
            std::fprintf(stderr, "[DaytonaArm] %s\n", mod_err.c_str());
            return false;
        }
        if (rom_path.empty()) {
            status = "Missing ROM archive roms/" + target_set + ".zip. Copy your MAME ZIP into " + cli.data_dir + "/roms/.";
            std::fprintf(stderr, "[DaytonaArm] %s\n", status.c_str());
            return false;
        }

        try {
            if (settings.link_enabled && target_set != "daytona")
                throw std::runtime_error("LAN Link Play requires Revision A (daytona). Switch ROM set or disable Link.");

            auto images = rt::import_rom_set(rom_path);
            active_native_audio = settings.native_audio;
            if (active_native_audio) {
                sound_worker.close();
                auto engine = std::make_unique<snd::NativeSoundEngine>(
                    std::move(images.sound_program), std::move(images.pcm1), std::move(images.pcm2));
                if (!native_audio.open(std::move(engine), nullptr))
                    throw std::runtime_error("Native audio failed to open; switch to Reference audio.");
            } else {
                if (!sound_worker.threaded()) sound_worker.open();
                audio.open();
            }

            game = std::make_unique<rt::GameLoop>(std::move(images), !active_native_audio);
            active_romset = target_set;
            if (test_hold.armed()) test_hold.arm();

            const std::string sdir = save_dir_for(active_romset);
            load_bytes(sdir + "/ioboard_eeprom.bin", game->board().io().eeprom.data(), game->board().io().eeprom.size());
            load_bytes(sdir + "/backup_ram.bin", game->board().backup_ram().data(), game->board().backup_ram().size());

            if (settings.link_enabled) {
                link = std::make_unique<app::TcpLink>(
                    uint16_t(settings.link_port),
                    settings.next_address() + ":" + std::to_string(settings.next_port));
                if (!link->error().empty()) throw std::runtime_error("Link error: " + link->error());
                game->board().set_link(link.get(), settings.link_sync);
            }

            game->board().video().set_external_3d(true);
            game->board().video().set_gpu_background(false);
            if (rt::Geo *geo = geometry_of(game.get()))
                geo->configure(rt::GeoMode::Pipelined, kGeoCore, nullptr);

            gpu.reset_materials();
            controls = arm64::Controls{};
            apply_preferences();
            clock.reset();
            status = "Running " + active_romset + " (" + std::string(arm64::gl_renderer_string()) + ").";
            return true;
        } catch (const std::exception &e) {
            native_audio.close();
            audio.pause();
            game.reset();
            link.reset();
            status = std::string("Boot failed: ") + e.what();
            std::fprintf(stderr, "[DaytonaArm] %s\n", status.c_str());
            return false;
        }
    };

    bool running = true;
    bool menu = true;
    bool options = false;
    bool binds = false;
    bool wait_release = true;
    int selection = 0;
    int capture = -1;
    uint32_t previous_buttons = 0;

    if (cli.autostart || settings.skip_launcher) {
        if (start_game()) {
            menu = false;
            wait_release = true;
        }
    }

    uint64_t last_us = ticks_us();
    uint64_t fps_start = last_us;
    uint64_t fps_frames = 0;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = false;
            } else if (ev.type == SDL_WINDOWEVENT &&
                       (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || ev.window.event == SDL_WINDOWEVENT_RESIZED)) {
                arm64::gl_update_drawable_size();
            } else {
                pads.handle_event(ev);
            }
        }

        const uint64_t now = ticks_us();
        const double elapsed = double(now - last_us) / 1e6;
        last_us = now;

        const uint8_t *keys = SDL_GetKeyboardState(nullptr);
        const arm64::PadState pad = pads.poll(keys);
        uint32_t pressed = pad.buttons & ~previous_buttons;
        previous_buttons = pad.buttons;
        if (wait_release) {
            pressed = 0;
            controls.latch(pad.buttons);
            if (!pad.buttons) wait_release = false;
        }

        if (!menu && !wait_release && arm64::menu_chord(pad.buttons)) {
            menu = true;
            options = binds = false;
            selection = 0;
            capture = -1;
            finish_sound();
            audio.pause();
            native_audio.pause();
            save_nv();
            clock.reset();
            wait_release = true;
        }

        if (menu && !wait_release) {
            if (binds && capture >= 0) {
                const uint32_t bindable = pressed & arm64::kBindableMask;
                if (bindable) {
                    const uint32_t btn = bindable & (~bindable + 1);
                    settings.bindings.bind(capture, btn);
                    apply_preferences();
                    settings.save(cfg_path);
                    status = std::string(arm64::action_name(capture)) + " bound to " + arm64::button_name(btn);
                    capture = -1;
                    wait_release = true;
                }
            } else if (binds) {
                if (pressed & arm64::Up) selection = (selection + kBindRows - 1) % kBindRows;
                if (pressed & arm64::Down) selection = (selection + 1) % kBindRows;
                const int dir = (pressed & arm64::Left) ? -1 : ((pressed & arm64::Right) ? 1 : 0);
                if (pressed & arm64::BtnB) { binds = false; selection = 4; wait_release = true; }
                else if (selection < arm64::ActionCount && (pressed & arm64::BtnA)) {
                    capture = selection;
                    wait_release = true;
                    status = std::string("Press the new button for ") + arm64::action_name(selection);
                } else if (selection < arm64::ActionCount && dir) {
                    settings.bindings.cycle(selection, dir);
                    apply_preferences();
                    settings.save(cfg_path);
                } else if (selection == arm64::ActionCount && (pressed & arm64::BtnA)) {
                    settings.bindings = arm64::Bindings{};
                    apply_preferences();
                    settings.save(cfg_path);
                    status = "Controls reset to RG34XX defaults.";
                } else if (selection == arm64::ActionCount + 1 && (pressed & arm64::BtnA)) {
                    binds = false;
                    selection = 4;
                    wait_release = true;
                }
            } else if (options) {
                if (pressed & arm64::Up) selection = (selection + kOptionCount - 1) % kOptionCount;
                if (pressed & arm64::Down) selection = (selection + 1) % kOptionCount;
                const int dir = (pressed & arm64::Left) ? -1 : ((pressed & arm64::Right) ? 1 : 0);
                const bool act = (pressed & arm64::BtnA) != 0;
                if (pressed & arm64::BtnB) { options = false; selection = 3; wait_release = true; }
                else if (dir || act) {
                    bool changed = false;
                    switch (selection) {
                    case 0: settings.romset = (settings.romset == "daytona") ? "daytona93" : "daytona"; changed = true; break;
                    case 1: settings.aspect = 1 - settings.aspect; changed = true; break;
                    case 2: settings.vsync = !settings.vsync; changed = true; break;
                    case 3: settings.sync_60hz = !settings.sync_60hz; changed = true; break;
                    case 4: settings.draw_distance = std::clamp(settings.draw_distance + (dir < 0 ? -1 : 1), -2, 2); changed = true; break;
                    case 5: settings.frame_skip = (settings.frame_skip + (dir < 0 ? 2 : 1)) % 3; changed = true; break;
                    case 6: settings.native_audio = !settings.native_audio; changed = true; status = "Audio engine changed (Reset Game to apply)."; break;
                    case 7: settings.volume = std::clamp(settings.volume + (dir < 0 ? -10 : 10), 0, 100); changed = true; break;
                    case 8: settings.music_volume = std::clamp(settings.music_volume + (dir < 0 ? -5 : 5), 0, 100); changed = true; break;
                    case 9: settings.effects_volume = std::clamp(settings.effects_volume + (dir < 0 ? -5 : 5), 0, 100); changed = true; break;
                    case 10: settings.mute = !settings.mute; changed = true; break;
                    case 11: settings.gamepad_fix = !settings.gamepad_fix; changed = true; break;
                    case 12: settings.gamepad_menu = !settings.gamepad_menu; changed = true; break;
                    case 13: settings.steer_curve = (settings.steer_curve + (dir < 0 ? 2 : 1)) % 3; changed = true; break;
                    case 14: settings.deadzone = std::clamp(settings.deadzone + (dir < 0 ? -5 : 5), 0, 40); changed = true; break;
                    case 15: settings.steer_invert = !settings.steer_invert; changed = true; break;
                    case 16: settings.stick_invert = !settings.stick_invert; changed = true; break;
                    case 17: settings.skip_launcher = !settings.skip_launcher; changed = true; break;
                    case 18: settings.show_fps = !settings.show_fps; changed = true; break;
                    case 19: if (test_hold.armed()) test_hold.cancel(); else test_hold.arm(); break;
                    case 20: settings.link_enabled = !settings.link_enabled; changed = true; break;
                    case 21: settings.next_ip[3] = (settings.next_ip[3] + (dir < 0 ? 255 : 1)) % 256; changed = true; break;
                    case 22: settings.link_sync = !settings.link_sync; changed = true; break;
                    case 23: if (act) { test_hold.cancel(); settings.defaults(); changed = true; status = "Settings reset to defaults."; } break;
                    case 24: if (act) { options = false; selection = 3; wait_release = true; } break;
                    default: break;
                    }
                    if (changed) {
                        apply_preferences();
                        settings.save(cfg_path);
                    }
                }
            } else {
                if (pressed & arm64::Up) selection = (selection + 5) % 6;
                if (pressed & arm64::Down) selection = (selection + 1) % 6;
                if ((pressed & arm64::BtnB) && game) {
                    menu = false;
                    clock.reset();
                    wait_release = true;
                } else if (selection == 2 && (pressed & (arm64::Left | arm64::Right | arm64::BtnA))) {
                    settings.romset = (settings.romset == "daytona") ? "daytona93" : "daytona";
                    settings.save(cfg_path);
                } else if (pressed & arm64::BtnA) {
                    if (selection == 0) {
                        if ((game && active_romset == settings.romset) || start_game()) {
                            menu = false;
                            clock.reset();
                            wait_release = true;
                        }
                    } else if (selection == 1) {
                        if (start_game()) { menu = false; clock.reset(); wait_release = true; }
                    } else if (selection == 3) {
                        options = true; selection = 0; wait_release = true;
                    } else if (selection == 4) {
                        binds = true; selection = 0; wait_release = true;
                    } else if (selection == 5) {
                        save_nv();
                        settings.save(cfg_path);
                        running = false;
                    }
                }
            }
        }

        bool simulated = false;
        if (game && !menu) {
            if (active_native_audio) native_audio.resume();
            const int frames = clock.advance(elapsed);
            for (int n = 0; n < frames; ++n) {
                try {
                    // Feed drive board spring + speed into the RG34XX D-pad spring steering model.
                    auto cmds = std::exchange(game->board().io().drive_commands, {});
                    for (uint8_t c : cmds) drive_board.command(c);
                    const int32_t raw_spd = int32_t(game->board().read_dword(0x505198));
                    const float kmh = std::max(0.0f, 1.055f * float(raw_spd) - 14.5f);
                    controls.update_board_state(drive_board, game->menu_screen(), kmh / 215.0f);

                    rt::Inputs inputs = controls.sample(wait_release ? arm64::PadState{} : pad);
                    test_hold.apply(game->frames(), inputs.in0);
                    auto next_sound = game->run_frame_sound_packet(inputs);
                    ++fps_frames;

                    if (active_native_audio) {
                        const auto bytes = game->board().take_sound_bytes();
                        const auto health = native_audio.stats();
                        if (health.failed || health.invalid)
                            throw std::runtime_error("Native audio fault; switch to Reference audio.");
                        if (!native_audio.send(bytes.data(), bytes.size()))
                            throw std::runtime_error("Native audio queue overflow.");
                    } else {
                        if (!finish_sound()) break;
                        if (auto *snd = game->sound())
                            snd->set_volumes(float(settings.music_volume) / 100.0f,
                                             float(settings.effects_volume) / 100.0f);
                        active_sound = std::move(next_sound);
                        sound_worker.dispatch_packet(active_sound, audio, nullptr);
                        sound_in_flight = true;
                    }
                    simulated = true;
                } catch (const std::exception &e) {
                    finish_sound();
                    status = std::string("Runtime fault: ") + e.what();
                    menu = true;
                    audio.pause();
                    native_audio.close();
                    active_sound = {};
                    game.reset();
                    break;
                }
            }
        } else {
            clock.reset();
        }

        if (link) link->poll();

        if (menu || !game || simulated) {
            gpu.prepare_frame();
            arm64::gl_begin_frame();
            if (menu) {
                ui.frame(float(elapsed));
                draw_menu(bool(game), options, binds, selection, capture, settings, status, display_fps, cli.data_dir);
                ui.render();
            } else if (game) {
                gpu.draw(game->board().video());
            }
            arm64::gl_end_frame();
        }

        const uint64_t after = ticks_us();
        if (after - fps_start >= 2000000ull) {
            const double sec = double(after - fps_start) / 1e6;
            display_fps = sec > 0.0 ? double(fps_frames) / sec : 0.0;
            if (settings.show_fps && game && !menu) {
                // Reference audio is produced one fixed 1/57.524 s block per game frame
                // (GameLoop::execute_deferred_sound), so whenever the frame rate sits
                // below 57.524 Hz the sound board under-produces. AudioRate can follow
                // that down to -5% (54.6 FPS) by playing slower; past that the queue
                // empties and the output re-primes with a silent gap. Report the queue
                // and underruns so a stutter can be attributed instead of guessed at.
                if (!active_native_audio) {
                    const auto astats = audio.stats();
                    std::fprintf(stderr,
                                 "[DaytonaArm] %.2f FPS | gpu_cpu=%.2f ms | 2d_worker=%.2f ms | verts=%zu"
                                 " | audio: underruns=%u ratio=%.4f queued=%.1f ms\n",
                                 display_fps, gpu.last_gpu_ms(), double(gpu.last_2d_worker_us()) / 1000.0,
                                 gpu.submitted_vertices(), astats.underruns, astats.ratio, astats.queued_ms);
                } else {
                    std::fprintf(stderr, "[DaytonaArm] %.2f FPS | gpu_cpu=%.2f ms | 2d_worker=%.2f ms | verts=%zu\n",
                                 display_fps, gpu.last_gpu_ms(), double(gpu.last_2d_worker_us()) / 1000.0,
                                 gpu.submitted_vertices());
                }
            }
            fps_start = after;
            fps_frames = 0;
        }
        if (menu || !simulated) SDL_Delay(1);
    }

    finish_sound();
    sound_worker.close();
    save_nv();
    settings.save(cfg_path);
    native_audio.close();
    audio.close();
    game.reset();
    link.reset();
    module_loader.unload();
    gpu.shutdown();
    ui.shutdown();
    pads.close();
    arm64::gl_fini();
    SDL_Quit();
    return 0;
}
