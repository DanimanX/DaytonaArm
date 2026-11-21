// Native OpenGL ES 2.0 / Desktop OpenGL 2.1 hardware renderer for DaytonaArm
// (Anbernic RG34XX Mali-G31 MP2 and compatible AArch64 Linux handhelds).
//
// Architecture adapted from platform/vita/gpu_gl.h/.cpp:
//   * Reproduces Sega Model 2 polygon priority via per-polygon depth ranks
//     (GL_GEQUAL, cleared to 0.0, standard GLES 2.0 glDepthRangef(0, 1)).
//   * Filters 4-bit Model 2 texel indices on the GPU (GL_LUMINANCE_ALPHA) and
//     resolves luminance + palette + gamma in the fragment shader.
//   * Keeps System 24 2D layers indexed (pen numbers in 512x512 textures +
//     32 KB 128x64 palette texture), prepared on a Core 3 worker thread with
//     ARM64 NEON and uploaded via dirty-row glTexSubImage2D.
//   * Uses standard VBO buffer orphaning (GL_STREAM_DRAW) for zero glFinish stalls.
#pragma once

#include "flat_index.h"
#include "runtime/video.h"

#include <SDL.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef RGBA8
#define RGBA8(r, g, b, a) \
    ((((uint32_t)(a) & 0xFFu) << 24) | (((uint32_t)(b) & 0xFFu) << 16) | \
     (((uint32_t)(g) & 0xFFu) << 8) | (((uint32_t)(r) & 0xFFu) << 0))
#endif

namespace arm64 {

enum class AspectMode : int {
    Original43 = 0, // Exact 496:384 arcade aspect ratio (620x480 on RG34XX 720x480)
    StretchFull = 1 // Fill full handheld panel (720x480 3:2 on RG34XX)
};

struct GlConfig {
    bool fullscreen = true;
    bool vsync = true;
    int window_w = 720;
    int window_h = 480;
    int worker_2d_core = 3; // Dedicated Core 3 on Allwinner H700
};

bool gl_init(const GlConfig &cfg);
const char *gl_error();
const char *gl_renderer_string();
bool gl_is_gles();
SDL_Window *gl_window();
int gl_display_width();
int gl_display_height();
void gl_update_drawable_size();
void gl_set_vsync(bool enabled);
void gl_set_aspect(AspectMode mode);
void gl_begin_frame();
void gl_end_frame();
void gl_fini();
void gl_fill_rect(float x, float y, float w, float h, uint32_t rgba);

// ImGui UI integration (rendered in the same GLES 2.0 draw list).
struct GlUiVertex { float x, y, u, v; uint32_t rgba; };
uint32_t gl_create_ui_texture(int w, int h, const void *rgba);
void gl_delete_ui_texture(uint32_t texture);
void gl_ui_triangles(uint32_t texture, const GlUiVertex *vertices, size_t count, int x0, int y0, int x1, int y1);

class GpuGlesRenderer {
public:
    explicit GpuGlesRenderer(int worker_2d_core = 3);
    ~GpuGlesRenderer();
    GpuGlesRenderer(const GpuGlesRenderer &) = delete;
    GpuGlesRenderer &operator=(const GpuGlesRenderer &) = delete;

    bool ok() const { return ok_; }
    void reset_materials();
    void prepare_frame();
    void shutdown();
    void draw(rt::Video &video);

    // Diagnostics
    double last_gpu_ms() const { return last_gpu_ms_; }
    uint64_t last_sort_us() const { return last_sort_us_; }
    uint64_t last_texture_us() const { return last_texture_us_; }
    uint64_t last_vertex_us() const { return last_vertex_us_; }
    uint64_t last_polygon_us() const { return last_polygon_us_; }
    uint64_t last_tile_us() const { return last_tile_us_; }
    uint64_t last_upload_us() const { return last_upload_us_; }
    uint64_t last_2d_worker_us() const { return last_2d_worker_us_; }
    bool worker_2d_threaded() const { return s24_thread_.joinable(); }
    int worker_2d_core() const { return worker_2d_core_; }
    std::size_t cached_bytes() const { return cached_bytes_; }
    std::size_t cached_materials() const { return palette_index_.size(); }
    std::size_t cached_sources() const { return sources_.size(); }
    unsigned system24_uploaded_tiles() const { return system24_uploaded_tiles_; }
    std::size_t submitted_vertices() const { return submitted_vertices_; }

private:
    struct SourceKey {
        uint16_t h0 = 0, h2 = 0;
        bool operator==(const SourceKey &o) const { return h0 == o.h0 && h2 == o.h2; }
    };
    struct SourceKeyHash {
        std::size_t operator()(const SourceKey &k) const {
            return std::hash<uint32_t>()(uint32_t(k.h0) | (uint32_t(k.h2) << 16));
        }
    };
    struct Source {
        uint32_t texture = 0;
        uint32_t source_w = 1, source_h = 1;
        bool transparent = false;
        std::size_t bytes = 0;
    };

    static constexpr std::size_t kSourceCacheBytes = 24u * 1024u * 1024u;
    static constexpr unsigned kSourceBuildBudget = 32;
    static constexpr uint32_t kSourceTexelBudget = 128u * 1024u;
    static constexpr uint32_t kTextureLimit = 512;
    static constexpr uint32_t kPaletteWidth = 128;
    static constexpr uint32_t kPaletteRows = 2048;
    static constexpr uint32_t kPaletteFrameRows = 512;

    uint32_t make_texture(uint32_t w, uint32_t h, bool linear, uint32_t wrap_s, uint32_t wrap_t, uint32_t format,
                          const void *pixels);
    const Source *source_for(const rt::GeoPoly &poly, const rt::VideoMem &mem);
    int palette_row(const rt::GeoPoly &poly, const rt::VideoMem &mem);
    uint32_t solid_color(const rt::GeoPoly &poly, const rt::VideoMem &mem) const;
    void clear_cache();
    void flush_palette_gl();

    struct S24Rect { int x0, x1, y0, y1, h, v; };
    struct S24Run { int layer; uint32_t first, count; };
    struct S24Slot {
        std::array<uint32_t, 8> textures{};              // GLuint: 0-3 bg, 4-7 fg (512x512 RGBA)
        std::array<std::vector<uint32_t>, 8> shadow{};   // CPU-side shadow texels (written by Core 3)
        std::array<uint64_t, 4> dirty_rows{};            // 64-bit mask of dirty 8-pixel tile rows per layer
        uint32_t palette_texture = 0;                    // GLuint 128x64 RGBA
        std::vector<uint32_t> palette_shadow;            // CPU-side 8192-pen palette
        bool palette_dirty = false;
        uint64_t generation = UINT64_MAX;
        uint64_t palette_generation = UINT64_MAX;
        bool valid = false;
        uint32_t backdrop = 0;
        std::vector<S24Rect> rects;
        std::vector<S24Run> runs;
        std::array<std::vector<S24Rect>, 2> split;
        unsigned uploaded_tiles = 0;
    };

    static void s24_prepare(S24Slot &slot, const rt::Video &video);
    void s24_upload_gl(S24Slot &slot);
    void s24_worker_loop();
    bool s24_start_thread();
    void s24_stop_thread();
    void s24_kick(const rt::Video &video);
    void s24_join();
    void s24_emit(const S24Slot &slot, bool foreground);
    void draw_polygons(rt::Video &video);

    int worker_2d_core_ = 3;
    bool ok_ = false, shutdown_ = false;
    std::array<S24Slot, 2> s24_{};
    unsigned s24_back_ = 0;

    std::thread s24_thread_;
    std::mutex s24_mutex_;
    std::condition_variable s24_cv_start_, s24_cv_done_;
    bool s24_job_ready_ = false, s24_job_done_ = true, s24_stop_ = false, s24_init_ = false;
    bool s24_busy_ = false;
    const rt::Video *s24_video_ = nullptr;
    S24Slot *s24_job_ = nullptr;
    uint64_t s24_job_us_ = 0;

    std::unordered_map<SourceKey, Source, SourceKeyHash> sources_;
    vita::FlatIndex<uint32_t, const Source *, 12> source_cache_;
    std::vector<uint8_t> index_scratch_;
    std::size_t cached_bytes_ = 0;
    bool cache_reset_pending_ = false;

    uint32_t palette_texture_ = 0;
    std::vector<uint32_t> palette_shadow_;
    vita::FlatIndex<uint32_t, uint32_t, 12> palette_index_;
    uint32_t palette_used_ = 0;
    uint32_t palette_dirty_begin_ = UINT32_MAX;
    uint32_t palette_dirty_end_ = 0;
    uint8_t gamma_[256]{};

    double last_gpu_ms_ = 0.0;
    uint64_t last_polygon_us_ = 0, last_tile_us_ = 0, last_upload_us_ = 0, last_sort_us_ = 0, last_texture_us_ = 0,
             last_2d_worker_us_ = 0, last_vertex_us_ = 0;
    unsigned cache_resets_ = 0, material_drops_ = 0, material_builds_ = 0, material_defers_ = 0,
             system24_quads_ = 0, system24_uploaded_tiles_ = 0, clip_changes_ = 0;
    std::size_t submitted_vertices_ = 0;
};

} // namespace arm64
