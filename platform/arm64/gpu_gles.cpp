// Native OpenGL ES 2.0 / Desktop OpenGL 2.1 renderer for DaytonaArm.
// Adapted from platform/vita/gpu_gl.cpp for standard Linux EGL/GLES2 (Mali-G31)
// and Desktop OpenGL 2.1 (host verification), with zero VitaSDK dependencies.
#include "gpu_gles.h"
#include "gl_funcs.h"
#include "polygon_order.h"
#include "runtime/raster_texel.h"
#include "system24_upload.h"
#include "texel_index.h"
#include "thread_affinity.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

namespace arm64 {
namespace {

inline uint64_t now_us() {
    struct timespec ts {};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000ull + uint64_t(ts.tv_nsec) / 1000ull;
}

constexpr float kSourceW = float(rt::Video::W); // 496
constexpr float kSourceH = float(rt::Video::H); // 384

// Depth buffer reproduces exact Sega Model 2 draw priority (no hardware Z-buffer
// intersections). Foreground 2D is placed in front of all polygons, background
// behind all polygons; standard GLES 2.0 glDepthRangef(0, 1) maps NDC [-1, 1]
// to [0, 1] via z_clip = (2 * d - 1) * w.
constexpr bool k2DLayersByDepth = true;
constexpr float kForegroundDepth = 0.99998f;
constexpr float kBackgroundDepth = 0.0f;

constexpr size_t kIndexedVertexLimit = 65536;
constexpr size_t kQuadIndices = kIndexedVertexLimit / 4 * 6; // 98,304 indices (192 KB)
constexpr uint8_t kLumaMask = 0xfc;

// Shaders: valid GLSL ES 1.00 (GLES 2.0) and GLSL 1.20 (Desktop GL 2.1).
const char *const kVertexShader = R"(
attribute vec3 a_pos;    // x * w, y * w (display pixels, top-left origin), w
attribute vec4 a_uvpd;   // texture u, v, palette row coordinate, priority depth [0..1]
attribute vec4 a_color;
uniform vec2 u_inv_screen;
varying vec3 v_uvp;
varying vec4 v_color;
#ifdef CHECKER
varying vec3 v_pos;
#endif
void main() {
    float w = a_pos.z;
    float z = (2.0 * a_uvpd.w - 1.0) * w;
    gl_Position = vec4(a_pos.x * (2.0 * u_inv_screen.x) - w,
                       w - a_pos.y * (2.0 * u_inv_screen.y),
                       z, w);
    v_uvp = a_uvpd.xyz;
    v_color = a_color;
#ifdef CHECKER
    v_pos = a_pos;
#endif
}
)";

const char *const kFragmentShader = R"(
varying vec3 v_uvp;
varying vec4 v_color;
#ifdef CHECKER
varying vec3 v_pos;
#endif
#ifdef TEXTURED
uniform sampler2D s_tex;
#endif
#ifdef MODEL
uniform sampler2D s_palette;
#endif
#ifdef PALETTE24
uniform sampler2D s_pal24;
#endif
void main() {
#ifdef CHECKER
    vec2 p = floor(v_pos.xy / v_pos.z);
    float s = p.x + p.y;
    if (s - 2.0 * floor(s * 0.5) < 0.5) { discard; }
#endif
#ifdef TEXTURED
    vec4 t = texture2D(s_tex, v_uvp.xy);
#ifdef ALPHA_TEST
    if (t.a < 0.5) { discard; }
#endif
#ifdef MODEL
    float k = floor(t.r * 127.5 + 0.01);
    gl_FragColor = texture2D(s_palette, vec2((k + 0.5) * (1.0 / 128.0), v_uvp.z)) * v_color;
#else
#ifdef PALETTE24
    float k = floor(t.r * 127.5 + 0.01);
    float j = floor(t.g * 63.75 + 0.01);
    gl_FragColor = texture2D(s_pal24, vec2((k + 0.5) * (1.0 / 128.0), (j + 0.5) * (1.0 / 64.0))) * v_color;
#else
    gl_FragColor = t * v_color;
#endif
#endif
#else
    gl_FragColor = v_color;
#endif
}
)";

enum class Prog : uint8_t { Flat, Layer, Model, ModelAlpha, FlatChecker, ModelChecker, Ui, Count };
struct ProgramDef { const char *name; const char *defines; };
constexpr ProgramDef kPrograms[int(Prog::Count)] = {
    {"Flat", ""},
    {"Layer", "#define TEXTURED\n#define ALPHA_TEST\n#define PALETTE24\n"},
    {"Model", "#define TEXTURED\n#define MODEL\n"},
    {"ModelAlpha", "#define TEXTURED\n#define MODEL\n#define ALPHA_TEST\n"},
    {"FlatChecker", "#define CHECKER\n"},
    {"ModelChecker", "#define TEXTURED\n#define MODEL\n#define ALPHA_TEST\n#define CHECKER\n"},
    {"Ui", "#define TEXTURED\n"},
};
inline bool textured(Prog p) { return p != Prog::Flat && p != Prog::FlatChecker; }

struct Vertex {
    float x, y, w;
    float u, v;
    float p;
    float d;
    uint32_t color;
};
static_assert(sizeof(Vertex) == 32, "Vertex layout must stay tightly packed (32 bytes)");

enum class Depth : uint8_t { Off, TestWrite, Test };

struct Cmd {
    Prog prog;
    GLuint texture;
    uint32_t first, count;
    Depth depth;
    int clip;
    bool quads = false;
};

struct PolyRef {
    const rt::GeoPoly *poly;
    float base_x, base_y;
    float u_scale, v_scale, palette;
    float depth;
    uint32_t color;
};

struct Batch {
    int clip = -1;
    Prog prog = Prog::Flat;
    GLuint texture = 0;
    std::vector<PolyRef> poly_refs;
    unsigned valid_polys = 0;
    size_t valid_vertices = 0;
};

struct VertexArena {
    std::vector<Vertex> buf;
    size_t size = 0;

    Vertex *append(size_t n) {
        if (size + n > buf.size()) {
            const size_t cap = std::max<size_t>(buf.empty() ? (size_t(1) << 16) : buf.size() * 2, size + n);
            buf.resize(cap);
        }
        Vertex *p = buf.data() + size;
        size += n;
        return p;
    }
    Vertex *data() { return buf.data(); }
    void release() { buf.clear(); buf.shrink_to_fit(); size = 0; }
};

struct State {
    GlFuncs gl;
    SDL_Window *window = nullptr;
    SDL_GLContext ctx = nullptr;
    bool ready = false;
    bool is_gles = false;
    std::string renderer_str;
    int display_w = 720, display_h = 480;
    AspectMode aspect = AspectMode::Original43;
    float scale_x = 1.0f, scale_y = 1.0f, offset_x = 0.0f, offset_y = 0.0f;

    GLuint program[int(Prog::Count)] = {};
    GLint u_inv_screen[int(Prog::Count)] = {};
    GLuint vbo = 0;
    GLuint quad_ibo = 0;
    GLuint palette_texture = 0;
    GLuint s24_palette_texture = 0;

    VertexArena verts;
    std::vector<Cmd> cmds;
    std::vector<std::array<int, 4>> clips;
    int clip = -1;
    std::vector<Batch> batches;
    size_t batch_count = 0;
    vita::FlatIndex<uint64_t, uint32_t, 12> batch_index;
    vita::PolygonOrder order;
    std::vector<float> depth;

    char error[512] = {};
} g;

void (APIENTRY *p_glUniform2f)(GLint, GLfloat, GLfloat) = nullptr;

void update_viewport_transform() {
    const float dw = float(std::max(g.display_w, 1));
    const float dh = float(std::max(g.display_h, 1));
    if (g.aspect == AspectMode::StretchFull) {
        g.scale_x = dw / kSourceW;
        g.scale_y = dh / kSourceH;
        g.offset_x = 0.0f;
        g.offset_y = 0.0f;
    } else {
        const float s = std::min(dw / kSourceW, dh / kSourceH);
        g.scale_x = s;
        g.scale_y = s;
        g.offset_x = (dw - kSourceW * s) * 0.5f;
        g.offset_y = (dh - kSourceH * s) * 0.5f;
    }
}

inline float sx(float x) { return g.offset_x + x * g.scale_x; }
inline float sy(float y) { return g.offset_y + y * g.scale_y; }

void set_error(const char *what, const char *detail = nullptr) {
    std::snprintf(g.error, sizeof g.error, "%s%s%s", what, detail && *detail ? ": " : "", detail ? detail : "");
    std::fprintf(stderr, "[DaytonaArm GL] %s\n", g.error);
}

void push(Prog prog, GLuint texture, const Vertex *vertices, size_t count, Depth depth = Depth::Off) {
    if (!count) return;
    const uint32_t first = uint32_t(g.verts.size);
    Vertex *dst = g.verts.append(count);
    std::memcpy(dst, vertices, count * sizeof(Vertex));
    if (!g.cmds.empty()) {
        Cmd &last = g.cmds.back();
        if (!last.quads && last.prog == prog && last.texture == texture && last.depth == depth && last.clip == g.clip) {
            last.count += uint32_t(count);
            return;
        }
    }
    g.cmds.push_back({prog, texture, first, uint32_t(count), depth, g.clip, false});
}

void push_quad(Prog prog, GLuint texture, float x0, float y0, float x1, float y1, float u0, float v0, float u1,
               float v1, uint32_t color, Depth depth = Depth::Off, float d = 0.0f) {
    const Vertex q[6] = {
        {x0, y0, 1, u0, v0, 0, d, color}, {x1, y0, 1, u1, v0, 0, d, color}, {x1, y1, 1, u1, v1, 0, d, color},
        {x0, y0, 1, u0, v0, 0, d, color}, {x1, y1, 1, u1, v1, 0, d, color}, {x0, y1, 1, u0, v1, 0, d, color},
    };
    push(prog, texture, q, 6, depth);
}

int add_clip(const std::array<int, 4> &rect) {
    for (size_t i = 0; i < g.clips.size(); ++i)
        if (g.clips[i] == rect) return int(i);
    g.clips.push_back(rect);
    return int(g.clips.size() - 1);
}

uint32_t batch_for(int clip, Prog prog, GLuint texture) {
    const uint64_t key = (uint64_t(clip & 0xffff) << 40) | (uint64_t(prog) << 32) | texture;
    if (const uint32_t *found = g.batch_index.find(key)) return *found;
    if (g.batch_count == g.batches.size()) g.batches.emplace_back();
    Batch &b = g.batches[g.batch_count];
    b.clip = clip;
    b.prog = prog;
    b.texture = texture;
    b.poly_refs.clear();
    g.batch_index.insert(key, uint32_t(g.batch_count));
    return uint32_t(g.batch_count++);
}

Vertex *append_quads(size_t quads) {
    if (!g.quad_ibo || !quads) return nullptr;
    const size_t pad = (4 - g.verts.size % 4) % 4;
    if (g.verts.size + pad + quads * 4 > kIndexedVertexLimit) return nullptr;
    g.verts.append(pad + quads * 4);
    return g.verts.data() + (g.verts.size - quads * 4);
}

size_t write_fan_quads(Vertex *dst, const Vertex *v, int n) {
    size_t written = 0;
    for (int a = 1; a + 1 < n; a += 2) {
        *dst++ = v[0];
        *dst++ = v[a];
        *dst++ = v[a + 1];
        *dst++ = v[std::min(a + 2, n - 1)];
        written += 4;
    }
    return written;
}

void flush_batches() {
    for (size_t bi = 0; bi < g.batch_count; ++bi) {
        Batch *b = &g.batches[bi];
        g.clip = b->clip;
        b->valid_polys = 0;
        b->valid_vertices = 0;
        if (b->poly_refs.empty()) continue;

        size_t quads = 0;
        for (const PolyRef &ref : b->poly_refs) quads += size_t(ref.poly->num_vertices - 1) / 2;
        Vertex *quad_dst = append_quads(quads);
        const bool indexed = quad_dst != nullptr;
        const uint32_t first = indexed ? uint32_t(quad_dst - g.verts.data()) : uint32_t(g.verts.size);
        uint32_t written = 0;
        for (const PolyRef &ref : b->poly_refs) {
            const rt::GeoPoly &poly = *ref.poly;
            const int n = poly.num_vertices;
            Vertex v[8];
            bool valid = true;
            for (int i = 0; i < n && valid; ++i) {
                const float w = poly.v[i].p[0];
                v[i] = {ref.base_x * w + poly.v[i].x * g.scale_x,
                        ref.base_y * w - poly.v[i].y * g.scale_y,
                        w,
                        poly.v[i].p[1] * ref.u_scale,
                        poly.v[i].p[2] * ref.v_scale,
                        ref.palette,
                        ref.depth,
                        ref.color};
                valid = w > 0.0f && std::isfinite(w) && std::isfinite(v[i].x) && std::isfinite(v[i].y) &&
                        std::isfinite(v[i].u) && std::isfinite(v[i].v);
            }
            if (!valid) continue;
            if (indexed) {
                written += uint32_t(write_fan_quads(quad_dst + written, v, n));
                ++b->valid_polys;
                continue;
            }
            const size_t count = size_t(n - 2) * 3;
            Vertex *dst = g.verts.append(count);
            for (int i = 1; i + 1 < n; ++i) {
                *dst++ = v[0];
                *dst++ = v[i];
                *dst++ = v[i + 1];
            }
            written += uint32_t(count);
            ++b->valid_polys;
        }
        b->valid_vertices = written;
        if (indexed) g.verts.size = first + written;
        if (written) g.cmds.push_back({b->prog, b->texture, first, written, Depth::TestWrite, g.clip, indexed});
    }
    g.clip = -1;
}

void execute() {
    if (g.cmds.empty() || !g.verts.size) return;
    auto &gl = g.gl;
    gl.glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    // Orphan VBO storage every frame so Mali-G31 pipelines without CPU stalls.
    gl.glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(g.verts.size * sizeof(Vertex)), g.verts.data(), GL_STREAM_DRAW);
    if (g.quad_ibo) gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.quad_ibo);
    for (GLuint i = 0; i < 3; ++i) gl.glEnableVertexAttribArray(i);
    gl.glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void *>(0));
    gl.glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void *>(12));
    gl.glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), reinterpret_cast<const void *>(28));

    gl.glActiveTexture(GL_TEXTURE1);
    gl.glBindTexture(GL_TEXTURE_2D, g.palette_texture);
    gl.glActiveTexture(GL_TEXTURE2);
    gl.glBindTexture(GL_TEXTURE_2D, g.s24_palette_texture);
    gl.glActiveTexture(GL_TEXTURE0);

    const float inv_w = 1.0f / float(std::max(g.display_w, 1));
    const float inv_h = 1.0f / float(std::max(g.display_h, 1));

    GLuint program = 0, texture = 0;
    int blend = -1, depth_test = -1, depth_write = 1, clip = -2;
    for (const Cmd &c : g.cmds) {
        const int pi = int(c.prog);
        if (g.program[pi] != program) {
            program = g.program[pi];
            gl.glUseProgram(program);
            if (g.u_inv_screen[pi] >= 0 && p_glUniform2f) p_glUniform2f(g.u_inv_screen[pi], inv_w, inv_h);
        }
        const int want_blend = ((c.prog == Prog::Flat || c.prog == Prog::Ui) && c.depth == Depth::Off) ? 1 : 0;
        if (want_blend != blend) {
            if (want_blend) gl.glEnable(GL_BLEND);
            else gl.glDisable(GL_BLEND);
            blend = want_blend;
        }
        const int want_test = c.depth != Depth::Off ? 1 : 0;
        if (want_test != depth_test) {
            if (want_test) gl.glEnable(GL_DEPTH_TEST);
            else gl.glDisable(GL_DEPTH_TEST);
            depth_test = want_test;
        }
        const int want_write = c.depth != Depth::Test ? 1 : 0;
        if (want_test && want_write != depth_write) {
            gl.glDepthMask(want_write ? GL_TRUE : GL_FALSE);
            depth_write = want_write;
        }
        if (textured(c.prog) && c.texture != texture) {
            gl.glBindTexture(GL_TEXTURE_2D, c.texture);
            texture = c.texture;
        }
        if (c.clip != clip) {
            if (c.clip < 0) {
                gl.glDisable(GL_SCISSOR_TEST);
            } else {
                const std::array<int, 4> &r = g.clips[size_t(c.clip)];
                gl.glEnable(GL_SCISSOR_TEST);
                gl.glScissor(r[0], g.display_h - r[3], r[2] - r[0], r[3] - r[1]);
            }
            clip = c.clip;
        }
        if (c.quads) {
            const size_t offset = size_t(c.first / 4) * 6 * sizeof(uint16_t);
            gl.glDrawElements(GL_TRIANGLES, GLsizei(c.count / 4 * 6), GL_UNSIGNED_SHORT,
                              reinterpret_cast<const void *>(offset));
        } else {
            gl.glDrawArrays(GL_TRIANGLES, GLint(c.first), GLsizei(c.count));
        }
    }
    gl.glDisable(GL_SCISSOR_TEST);
    gl.glDisable(GL_DEPTH_TEST);
    if (depth_write != 1) gl.glDepthMask(GL_TRUE);
    if (g.quad_ibo) gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    gl.glBindBuffer(GL_ARRAY_BUFFER, 0);
}

GLuint compile_shader(GLenum type, const char *header, const char *defines, const char *body, const char *name) {
    auto &gl = g.gl;
    const GLuint shader = gl.glCreateShader(type);
    const GLchar *sources[] = {header, defines, body};
    gl.glShaderSource(shader, 3, sources, nullptr);
    gl.glCompileShader(shader);
    GLint ok = GL_FALSE;
    gl.glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE) return shader;
    char log[400] = {};
    gl.glGetShaderInfoLog(shader, sizeof log, nullptr, log);
    set_error(name, log);
    gl.glDeleteShader(shader);
    return 0;
}

GLuint build_program(const ProgramDef &def, GLint &u_inv_out) {
    auto &gl = g.gl;
    const char *header = g.is_gles
                             ? "#version 100\nprecision lowp int;\nprecision highp float;\n"
                             : "#version 120\n";
    const GLuint vs = compile_shader(GL_VERTEX_SHADER, header, def.defines, kVertexShader, def.name);
    const GLuint fs = vs ? compile_shader(GL_FRAGMENT_SHADER, header, def.defines, kFragmentShader, def.name) : 0;
    GLuint program = 0;
    u_inv_out = -1;
    if (vs && fs) {
        program = gl.glCreateProgram();
        gl.glAttachShader(program, vs);
        gl.glAttachShader(program, fs);
        gl.glBindAttribLocation(program, 0, "a_pos");
        gl.glBindAttribLocation(program, 1, "a_uvpd");
        gl.glBindAttribLocation(program, 2, "a_color");
        gl.glLinkProgram(program);
        GLint ok = GL_FALSE;
        gl.glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (ok == GL_TRUE) {
            gl.glUseProgram(program);
            if (GLint loc = gl.glGetUniformLocation(program, "s_tex"); loc >= 0) gl.glUniform1i(loc, 0);
            if (GLint loc = gl.glGetUniformLocation(program, "s_palette"); loc >= 0) gl.glUniform1i(loc, 1);
            if (GLint loc = gl.glGetUniformLocation(program, "s_pal24"); loc >= 0) gl.glUniform1i(loc, 2);
            u_inv_out = gl.glGetUniformLocation(program, "u_inv_screen");
            gl.glUseProgram(0);
        } else {
            char log[400] = {};
            gl.glGetProgramInfoLog(program, sizeof log, nullptr, log);
            set_error(def.name, log);
            gl.glDeleteProgram(program);
            program = 0;
        }
    }
    if (vs) gl.glDeleteShader(vs);
    if (fs) gl.glDeleteShader(fs);
    return program;
}

uint16_t le16(const uint8_t *base, uint32_t index) {
    return uint16_t(base[index * 2] | (base[index * 2 + 1] << 8));
}

uint32_t argb_to_rgba(uint32_t argb) {
    return (argb & 0xff00ff00u) | ((argb >> 16) & 0xffu) | ((argb & 0xffu) << 16);
}

// Writes changed System 24 tiles into CPU shadow buffers using ARM64 NEON
// (from platform/vita/system24_upload.h) and records a 64-bit dirty tile-row mask.
unsigned update_system24_layer_shadow(const rt::Video &video, int layer, uint64_t previous,
                                      uint32_t *background, uint32_t *foreground, uint64_t &dirty_rows_out) {
    const bool full = previous == UINT64_MAX;
    const bool opaque_background = layer >= 2;
    const bool split_background = opaque_background && (video.system24_word(0x5006) & 0x6000);
    const uint32_t a = opaque_background ? ~0u : 0u;
    const uint32_t b = (opaque_background && !split_background) ? ~0u : 0u;
    const uint16_t *pixels = video.system24_pixels(layer);
    const uint8_t *flags = video.system24_flags(layer);
    unsigned uploaded = 0;
    for (unsigned ty = 0; ty < 64; ++ty) {
        uint64_t dirty = ~uint64_t(0);
        if (!full) {
            dirty = 0;
            for (unsigned tx = 0; tx < 64; ++tx)
                if (video.system24_tile_generation(layer, ty * 64u + tx) > previous) dirty |= uint64_t(1) << tx;
            if (!dirty) continue;
        }
        dirty_rows_out |= uint64_t(1) << ty;
        uploaded += unsigned(std::popcount(dirty));
        std::array<uint8_t, 64> run_first{}, run_length{};
        unsigned runs = 0;
        for (uint64_t rest = dirty; rest;) {
            const unsigned first = unsigned(std::countr_zero(rest));
            const uint64_t from_first = rest >> first;
            const unsigned length = ~from_first ? unsigned(std::countr_zero(~from_first)) : 64u - first;
            run_first[runs] = uint8_t(first);
            run_length[runs] = uint8_t(length);
            ++runs;
            rest = first + length >= 64 ? 0 : rest & (~uint64_t(0) << (first + length));
        }
        for (unsigned y = ty * 8u; y < ty * 8u + 8u; ++y) {
            const size_t line = size_t(y) * 512u;
            uint32_t *back_row = background + line;
            uint32_t *front_row = foreground + line;
            for (unsigned r = 0; r < runs; ++r) {
                const unsigned x = run_first[r] * 8u;
                vita::detail::system24_index_run(pixels + line + x, flags + line + x, run_length[r] * 8u,
                                                 back_row + x, front_row + x, a, b);
            }
        }
    }
    return uploaded;
}

} // namespace

// ---- Frame-level GLES 2.0 API ------------------------------------------------

const char *gl_error() { return g.error; }
const char *gl_renderer_string() { return g.renderer_str.c_str(); }
bool gl_is_gles() { return g.is_gles; }
SDL_Window *gl_window() { return g.window; }
int gl_display_width() { return g.display_w; }
int gl_display_height() { return g.display_h; }

void gl_update_drawable_size() {
    if (!g.window) return;
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(g.window, &w, &h);
    if (w > 0 && h > 0) {
        g.display_w = w;
        g.display_h = h;
        update_viewport_transform();
        if (g.ready) g.gl.glViewport(0, 0, g.display_w, g.display_h);
    }
}

void gl_set_vsync(bool enabled) {
    if (!g.ctx) return;
    if (enabled) {
        if (SDL_GL_SetSwapInterval(1) != 0) SDL_GL_SetSwapInterval(0);
    } else {
        SDL_GL_SetSwapInterval(0);
    }
}

void gl_set_aspect(AspectMode mode) {
    g.aspect = mode;
    update_viewport_transform();
}

bool gl_init(const GlConfig &cfg) {
    g.error[0] = 0;

    // Prefer OpenGL ES 2.0 on ARM64 handhelds (or when DAYTONA_GLES=1); fall back
    // cleanly to Desktop GL 2.1 when running on desktop Linux hosts.
    const char *env_gles = std::getenv("DAYTONA_GLES");
#if defined(DAYTONA_GLES) || defined(__aarch64__) || defined(__arm__)
    const bool prefer_gles = !(env_gles && std::strcmp(env_gles, "0") == 0);
#else
    const bool prefer_gles = (env_gles && std::strcmp(env_gles, "1") == 0);
#endif

    auto try_create = [&](bool use_es, int depth_bits) -> bool {
        if (g.ctx) { SDL_GL_DeleteContext(g.ctx); g.ctx = nullptr; }
        if (g.window) { SDL_DestroyWindow(g.window); g.window = nullptr; }
        SDL_GL_ResetAttributes();
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, depth_bits);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        if (use_es) {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        } else {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        }
        uint32_t flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;
        if (cfg.fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        else flags |= SDL_WINDOW_RESIZABLE;
        g.window = SDL_CreateWindow("DaytonaArm — Daytona USA", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    cfg.window_w, cfg.window_h, flags);
        if (!g.window) return false;
        g.ctx = SDL_GL_CreateContext(g.window);
        if (!g.ctx) {
            SDL_DestroyWindow(g.window);
            g.window = nullptr;
            return false;
        }
        return true;
    };

    bool created = false;
    if (prefer_gles) {
        created = try_create(true, 24) || try_create(true, 16) || try_create(false, 24) || try_create(false, 16);
    } else {
        created = try_create(false, 24) || try_create(false, 16) || try_create(true, 24) || try_create(true, 16);
    }
    if (!created) {
        set_error("SDL GL window/context creation failed", SDL_GetError());
        return false;
    }

    if (!g.gl.load()) {
        set_error("Failed to load required OpenGL ES 2.0 entry points");
        gl_fini();
        return false;
    }
    p_glUniform2f = reinterpret_cast<void (APIENTRY *)(GLint, GLfloat, GLfloat)>(SDL_GL_GetProcAddress("glUniform2f"));
    if (!p_glUniform2f) {
        set_error("Failed to load glUniform2f");
        gl_fini();
        return false;
    }

    const char *ver = reinterpret_cast<const char *>(g.gl.glGetString(GL_VERSION));
    const char *ren = reinterpret_cast<const char *>(g.gl.glGetString(GL_RENDERER));
    g.is_gles = (ver && std::strstr(ver, "OpenGL ES") != nullptr);
    g.renderer_str = std::string(g.is_gles ? "GLES2: " : "GL2: ") + (ren ? ren : "unknown") +
                     " (" + (ver ? ver : "?") + ")";
    std::fprintf(stderr, "[DaytonaArm] Graphics: %s\n", g.renderer_str.c_str());

    SDL_ShowCursor(SDL_DISABLE);
    gl_set_vsync(cfg.vsync);
    gl_update_drawable_size();

    auto &gl = g.gl;
    gl.glViewport(0, 0, g.display_w, g.display_h);
    gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl.clear_depth(0.0f);
    gl.glDepthFunc(GL_GEQUAL);
    gl.depth_range(0.0f, 1.0f);
    gl.glDepthMask(GL_TRUE);
    gl.glDisable(GL_DEPTH_TEST);
    gl.glDisable(GL_CULL_FACE);
    gl.glDisable(GL_SCISSOR_TEST);
    gl.glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    for (int i = 0; i < int(Prog::Count); ++i) {
        g.program[i] = build_program(kPrograms[i], g.u_inv_screen[i]);
    }
    if (!g.program[int(Prog::Flat)] || !g.program[int(Prog::Layer)] || !g.program[int(Prog::Model)] ||
        !g.program[int(Prog::ModelAlpha)] || !g.program[int(Prog::Ui)]) {
        gl_fini();
        return false;
    }
    if (!g.program[int(Prog::FlatChecker)] || !g.program[int(Prog::ModelChecker)]) {
        g.error[0] = 0;
        if (g.program[int(Prog::FlatChecker)]) gl.glDeleteProgram(g.program[int(Prog::FlatChecker)]);
        if (g.program[int(Prog::ModelChecker)]) gl.glDeleteProgram(g.program[int(Prog::ModelChecker)]);
        g.program[int(Prog::FlatChecker)] = g.program[int(Prog::Flat)];
        g.u_inv_screen[int(Prog::FlatChecker)] = g.u_inv_screen[int(Prog::Flat)];
        g.program[int(Prog::ModelChecker)] = g.program[int(Prog::ModelAlpha)];
        g.u_inv_screen[int(Prog::ModelChecker)] = g.u_inv_screen[int(Prog::ModelAlpha)];
    }

    gl.glGenBuffers(1, &g.vbo);
    if (!g.vbo) {
        set_error("VBO creation failed");
        gl_fini();
        return false;
    }
    g.verts.buf.resize(size_t(1) << 16);
    g.cmds.reserve(1024);

    // Static 16-bit index buffer turning groups of 4 vertices into quad pairs.
    std::vector<uint16_t> indices(kQuadIndices);
    for (size_t q = 0; q < kIndexedVertexLimit / 4; ++q) {
        const uint16_t v = uint16_t(q * 4);
        uint16_t *idx = &indices[q * 6];
        idx[0] = v; idx[1] = uint16_t(v + 1); idx[2] = uint16_t(v + 2);
        idx[3] = v; idx[4] = uint16_t(v + 2); idx[5] = uint16_t(v + 3);
    }
    gl.glGenBuffers(1, &g.quad_ibo);
    if (g.quad_ibo) {
        gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.quad_ibo);
        gl.glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(indices.size() * sizeof(uint16_t)), indices.data(),
                        GL_STATIC_DRAW);
        gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }

    g.ready = true;
    return true;
}

void gl_begin_frame() {
    g.verts.size = 0;
    g.cmds.clear();
    g.clips.clear();
    g.clip = -1;
    auto &gl = g.gl;
    gl.glViewport(0, 0, g.display_w, g.display_h);
    gl.glDisable(GL_SCISSOR_TEST);
    gl.glDepthMask(GL_TRUE);
    gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void gl_end_frame() {
    execute();
    if (g.window) SDL_GL_SwapWindow(g.window);
}

void gl_fini() {
    if (g.ctx) {
        auto &gl = g.gl;
        if (gl.glFinish) gl.glFinish();
        for (int i = 0; i < int(Prog::Count); ++i) {
            bool shared = false;
            for (int j = 0; j < i; ++j) shared = shared || g.program[j] == g.program[i];
            if (g.program[i] && !shared && gl.glDeleteProgram) gl.glDeleteProgram(g.program[i]);
            g.program[i] = 0;
        }
        if (g.vbo && gl.glDeleteBuffers) gl.glDeleteBuffers(1, &g.vbo);
        if (g.quad_ibo && gl.glDeleteBuffers) gl.glDeleteBuffers(1, &g.quad_ibo);
        g.vbo = g.quad_ibo = 0;
        SDL_GL_DeleteContext(g.ctx);
        g.ctx = nullptr;
    }
    if (g.window) {
        SDL_DestroyWindow(g.window);
        g.window = nullptr;
    }
    g.verts.release();
    g.ready = false;
}

void gl_fill_rect(float x, float y, float w, float h, uint32_t rgba) {
    push_quad(Prog::Flat, 0, x, y, x + w, y + h, 0.0f, 0.0f, 0.0f, 0.0f, rgba);
}

uint32_t gl_create_ui_texture(int w, int h, const void *rgba) {
    if (w <= 0 || h <= 0 || !rgba || !g.ready) return 0;
    auto &gl = g.gl;
    GLuint tex = 0;
    gl.glGenTextures(1, &tex);
    if (!tex) return 0;
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindTexture(GL_TEXTURE_2D, tex);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return tex;
}

void gl_delete_ui_texture(uint32_t texture) {
    if (!texture || !g.ready) return;
    GLuint t = texture;
    g.gl.glDeleteTextures(1, &t);
}

void gl_ui_triangles(uint32_t texture, const GlUiVertex *vertices, size_t count, int x0, int y0, int x1, int y1) {
    count -= count % 3;
    if (!texture || !count) return;
    x0 = std::clamp(x0, 0, g.display_w);
    x1 = std::clamp(x1, 0, g.display_w);
    y0 = std::clamp(y0, 0, g.display_h);
    y1 = std::clamp(y1, 0, g.display_h);
    if (x1 <= x0 || y1 <= y0) return;
    Vertex *dst = g.verts.append(count);
    const uint32_t first = uint32_t(dst - g.verts.data());
    for (size_t i = 0; i < count; ++i) {
        const GlUiVertex &v = vertices[i];
        dst[i] = {v.x, v.y, 1.0f, v.u, v.v, 0.0f, 0.0f, v.rgba};
    }
    const int clip = add_clip({x0, y0, x1, y1});
    if (!g.cmds.empty()) {
        Cmd &last = g.cmds.back();
        if (!last.quads && last.prog == Prog::Ui && last.texture == texture && last.clip == clip &&
            last.first + last.count == first) {
            last.count += uint32_t(count);
            return;
        }
    }
    g.cmds.push_back({Prog::Ui, texture, first, uint32_t(count), Depth::Off, clip, false});
}

// ---- GpuGlesRenderer ---------------------------------------------------------

GpuGlesRenderer::GpuGlesRenderer(int worker_2d_core) : worker_2d_core_(worker_2d_core) {
    for (int i = 0; i < 256; ++i) gamma_[i] = uint8_t(std::max((double(i) - 64.0) * 255.0 / 191.0, 0.0));
    bool ok = g.ready;
    for (S24Slot &slot : s24_) {
        for (size_t i = 0; i < slot.textures.size(); ++i) {
            slot.shadow[i].assign(512u * 512u, 0u);
            slot.textures[i] = make_texture(512, 512, false, GL_REPEAT, GL_REPEAT, GL_RGBA, slot.shadow[i].data());
            ok = ok && slot.textures[i] != 0;
        }
        slot.dirty_rows.fill(0);
        slot.palette_shadow.assign(vita::kSystem24PaletteWidth * vita::kSystem24PaletteHeight, 0u);
        slot.palette_texture = make_texture(vita::kSystem24PaletteWidth, vita::kSystem24PaletteHeight, false,
                                            GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_RGBA, slot.palette_shadow.data());
        slot.palette_dirty = false;
        ok = ok && slot.palette_texture != 0;
        slot.rects.reserve(4096);
        slot.runs.reserve(64);
        for (auto &sp : slot.split) sp.reserve(1024);
    }
    palette_shadow_.assign(size_t(kPaletteWidth) * kPaletteRows, 0u);
    palette_texture_ = make_texture(kPaletteWidth, kPaletteRows, false, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_RGBA,
                                    palette_shadow_.data());
    g.palette_texture = palette_texture_;
    ok = ok && palette_texture_ != 0;
    sources_.reserve(1024);
    ok_ = ok;
    if (ok_ && !s24_start_thread()) s24_stop_thread();
}

GpuGlesRenderer::~GpuGlesRenderer() { shutdown(); }

uint32_t GpuGlesRenderer::make_texture(uint32_t w, uint32_t h, bool linear, uint32_t wrap_s, uint32_t wrap_t,
                                       uint32_t format, const void *pixels) {
    auto &gl = g.gl;
    GLuint tex = 0;
    gl.glGenTextures(1, &tex);
    if (!tex) return 0;
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindTexture(GL_TEXTURE_2D, tex);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GLint(wrap_s));
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GLint(wrap_t));
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GLint(format), GLsizei(w), GLsizei(h), 0, GLenum(format), GL_UNSIGNED_BYTE,
                    pixels);
    return tex;
}

void GpuGlesRenderer::clear_cache() {
    auto &gl = g.gl;
    for (auto &entry : sources_) {
        GLuint tex = entry.second.texture;
        if (tex && g.ready) gl.glDeleteTextures(1, &tex);
    }
    sources_.clear();
    source_cache_.clear();
    cached_bytes_ = 0;
    cache_reset_pending_ = false;
    palette_index_.clear();
    palette_used_ = 0;
    palette_dirty_begin_ = UINT32_MAX;
    palette_dirty_end_ = 0;
}

void GpuGlesRenderer::reset_materials() {
    s24_join();
    clear_cache();
    for (S24Slot &slot : s24_) {
        slot.valid = false;
        slot.generation = slot.palette_generation = UINT64_MAX;
        slot.dirty_rows.fill(0);
        slot.palette_dirty = false;
    }
}

void GpuGlesRenderer::prepare_frame() {
    if (cache_reset_pending_) {
        clear_cache();
        ++cache_resets_;
    }
}

void GpuGlesRenderer::shutdown() {
    if (shutdown_) return;
    s24_stop_thread();
    clear_cache();
    if (palette_texture_ && g.ready) {
        GLuint t = palette_texture_;
        g.gl.glDeleteTextures(1, &t);
    }
    palette_texture_ = 0;
    g.palette_texture = 0;
    g.s24_palette_texture = 0;
    for (S24Slot &slot : s24_) {
        for (uint32_t &texture : slot.textures) {
            if (texture && g.ready) {
                GLuint t = texture;
                g.gl.glDeleteTextures(1, &t);
            }
            texture = 0;
        }
        if (slot.palette_texture && g.ready) {
            GLuint t = slot.palette_texture;
            g.gl.glDeleteTextures(1, &t);
        }
        slot.palette_texture = 0;
        slot.valid = false;
    }
    shutdown_ = true;
    ok_ = false;
}

const GpuGlesRenderer::Source *GpuGlesRenderer::source_for(const rt::GeoPoly &poly, const rt::VideoMem &mem) {
    const SourceKey key{uint16_t(poly.texheader[0] & 0x23ff), uint16_t(poly.texheader[2] & 0x1fff)};
    const uint32_t key32 = uint32_t(key.h0) | (uint32_t(key.h2) << 16);
    if (const Source *const *cached = source_cache_.find(key32)) return *cached;
    auto found = sources_.find(key);
    if (found != sources_.end()) {
        source_cache_.insert(key32, &found->second);
        return &found->second;
    }
    if (cached_bytes_ >= kSourceCacheBytes) {
        cache_reset_pending_ = true;
        ++material_drops_;
        return nullptr;
    }

    Source s;
    s.source_w = 32u << (poly.texheader[0] & 7);
    s.source_h = 32u << ((poly.texheader[0] >> 3) & 7);
    const uint32_t texels = std::min(s.source_w, kTextureLimit) * std::min(s.source_h, kTextureLimit);
    if (material_builds_ > 0 &&
        (material_builds_ >= kSourceBuildBudget || (last_texture_us_ > 0 && texels > kSourceTexelBudget))) {
        ++material_defers_;
        return nullptr;
    }

    const uint64_t build_begin = now_us();
    s.transparent = (poly.texheader[0] >> 13) & 1;
    const uint32_t step_x = std::max(1u, (s.source_w + kTextureLimit - 1) / kTextureLimit);
    const uint32_t step_y = std::max(1u, (s.source_h + kTextureLimit - 1) / kTextureLimit);
    const uint32_t w = (s.source_w + step_x - 1) / step_x;
    const uint32_t h = (s.source_h + step_y - 1) / step_y;
    const uint32_t bx = 32u * (poly.texheader[2] & 0x3f);
    const uint32_t by = 32u * ((poly.texheader[2] >> 6) & 0x1f);
    const uint32_t *sheet = (poly.texheader[2] & 0x1000) ? mem.tex1 : mem.tex0;

    std::vector<uint8_t> &la = index_scratch_;
    la.resize(size_t(w) * h * 2u);
    const uint8_t transparent_alpha = s.transparent ? 0 : 255;
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t ty = std::min(y * step_y, s.source_h - 1);
        uint8_t *row = &la[size_t(y) * w * 2u];
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t tx = x * step_x;
            const uint32_t index = vita::texel_index(bx, by, tx, ty, sheet);
            row[x * 2u] = uint8_t(index * 16);
            row[x * 2u + 1] = index == 15 ? transparent_alpha : 255;
        }
    }
    if (s.transparent) {
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const size_t i = (size_t(y) * w + x) * 2u;
                if (la[i + 1]) continue;
                const size_t n[4] = {(size_t(y) * w + (x + 1) % w) * 2u, (size_t(y) * w + (x + w - 1) % w) * 2u,
                                     (size_t((y + 1) % h) * w + x) * 2u, (size_t((y + h - 1) % h) * w + x) * 2u};
                for (size_t j : n)
                    if (la[j + 1]) { la[i] = la[j]; break; }
            }
    }
    s.texture = make_texture(w, h, true, ((poly.texheader[0] >> 8) & 1) ? GL_MIRRORED_REPEAT : GL_REPEAT,
                             ((poly.texheader[0] >> 9) & 1) ? GL_MIRRORED_REPEAT : GL_REPEAT, GL_LUMINANCE_ALPHA,
                             la.data());
    if (!s.texture) { ++material_drops_; return nullptr; }
    s.bytes = la.size();
    cached_bytes_ += s.bytes;
    ++material_builds_;
    last_texture_us_ += now_us() - build_begin;
    const Source *built = &sources_.emplace(key, s).first->second;
    source_cache_.insert(key32, built);
    return built;
}

int GpuGlesRenderer::palette_row(const rt::GeoPoly &poly, const rt::VideoMem &mem) {
    const uint32_t lumabase = poly.texheader[1] & 0xff;
    const uint32_t color_index = (poly.texheader[3] >> 6) & 0x3ff;
    const uint32_t luma_scale = kLumaMask ? (poly.luma & kLumaMask) : 0xff;
    const uint32_t key = lumabase | (color_index << 8) | (luma_scale << 18);
    if (const uint32_t *found = palette_index_.find(key)) return int(*found);
    if (palette_used_ >= kPaletteRows) { ++material_drops_; return -1; }

    const uint32_t row = palette_used_++;
    const uint32_t color = le16(mem.palram, color_index + 0x1000) & 0x7fff;
    uint32_t *out = &palette_shadow_[size_t(row) * kPaletteWidth];
    for (uint32_t k = 0; k < kPaletteWidth; ++k) {
        const uint32_t luma = std::min(uint32_t(mem.lumaram[((lumabase << 7) + k) * 4]) * luma_scale / 256, 0x3fu);
        const uint8_t r = gamma_[le16(mem.colorxlat, (((color >> 0) & 0x1f) << 8) + luma) & 0xff];
        const uint8_t gg = gamma_[le16(mem.colorxlat, 0x4000 / 2 + (((color >> 5) & 0x1f) << 8) + luma) & 0xff];
        const uint8_t b = gamma_[le16(mem.colorxlat, 0x8000 / 2 + (((color >> 10) & 0x1f) << 8) + luma) & 0xff];
        out[k] = RGBA8(r, gg, b, 255);
    }
    palette_index_.insert(key, row);
    palette_dirty_begin_ = std::min(palette_dirty_begin_, row);
    palette_dirty_end_ = std::max(palette_dirty_end_, row + 1);
    return int(row);
}

void GpuGlesRenderer::flush_palette_gl() {
    if (palette_dirty_end_ <= palette_dirty_begin_ || !palette_texture_) return;
    auto &gl = g.gl;
    gl.glActiveTexture(GL_TEXTURE1);
    gl.glBindTexture(GL_TEXTURE_2D, palette_texture_);
    gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, GLint(palette_dirty_begin_), GLsizei(kPaletteWidth),
                       GLsizei(palette_dirty_end_ - palette_dirty_begin_), GL_RGBA, GL_UNSIGNED_BYTE,
                       &palette_shadow_[size_t(palette_dirty_begin_) * kPaletteWidth]);
    gl.glActiveTexture(GL_TEXTURE0);
    palette_dirty_begin_ = UINT32_MAX;
    palette_dirty_end_ = 0;
}

uint32_t GpuGlesRenderer::solid_color(const rt::GeoPoly &poly, const rt::VideoMem &mem) const {
    const uint32_t color = le16(mem.palram, ((poly.texheader[3] >> 6) & 0x3ff) + 0x1000);
    const uint32_t luma = poly.luma >> 2;
    const uint8_t r = gamma_[le16(mem.colorxlat, (((color >> 0) & 0x1f) << 8) + luma) & 0xff];
    const uint8_t gg = gamma_[le16(mem.colorxlat, 0x4000 / 2 + (((color >> 5) & 0x1f) << 8) + luma) & 0xff];
    const uint8_t b = gamma_[le16(mem.colorxlat, 0x8000 / 2 + (((color >> 10) & 0x1f) << 8) + luma) & 0xff];
    return RGBA8(r, gg, b, 255);
}

void GpuGlesRenderer::draw_polygons(rt::Video &video) {
    const auto &polys = video.gpu_polys();
    const rt::VideoMem &mem = video.gpu_mem();
    material_drops_ = material_builds_ = material_defers_ = 0;
    submitted_vertices_ = 0;
    g.batch_count = 0;
    g.batch_index.clear();
    if (palette_used_ > kPaletteRows - kPaletteFrameRows) {
        palette_index_.clear();
        palette_used_ = 0;
        palette_dirty_begin_ = UINT32_MAX;
        palette_dirty_end_ = 0;
        ++cache_resets_;
    }

    enum class Skip : uint8_t { None, Window, Clip, Texture, Renderer };
    struct Material {
        bool cached = false;
        uint16_t th[4] = {};
        int viewport[4] = {}, window = 0;
        Skip skip = Skip::None;
        bool textured = false, checker = false;
        int clip = -1;
        Prog prog = Prog::Flat;
        GLuint texture = 0;
        bool has_batch = false;
        uint32_t batch = 0;
        float u_scale = 0, v_scale = 0;
    } mat;
    struct Light {
        bool cached = false;
        uint8_t key = 0;
        bool ok = false;
        float palette = 0;
        uint32_t color = 0xffffffffu;
    } light;

    const int windows = video.gpu_windows(), rx = video.render_x(), ry = video.render_y();
    const int crtc_x = video.crtc_x(), crtc_y = video.crtc_y();

    auto same_material = [&](const rt::GeoPoly &p) {
        return mat.cached && p.texheader[0] == mat.th[0] && p.texheader[1] == mat.th[1] &&
               p.texheader[2] == mat.th[2] && p.texheader[3] == mat.th[3] && p.window == mat.window &&
               p.viewport[0] == mat.viewport[0] && p.viewport[1] == mat.viewport[1] &&
               p.viewport[2] == mat.viewport[2] && p.viewport[3] == mat.viewport[3];
    };
    auto set_material = [&](const rt::GeoPoly &p) {
        mat = Material{};
        light.cached = false;
        mat.cached = true;
        for (int i = 0; i < 4; ++i) { mat.th[i] = p.texheader[i]; mat.viewport[i] = p.viewport[i]; }
        mat.window = p.window;
        if (p.window > windows) { mat.skip = Skip::Window; return; }
        const int l = std::max<int>(p.viewport[0] + rx, 0);
        const int r = std::min<int>(p.viewport[2] + rx, rt::Video::W - 1);
        const int t = std::max<int>(384 - p.viewport[3] + ry, 0);
        const int b = std::min<int>(384 - p.viewport[1] + ry, rt::Video::H - 1);
        if (l > r || t > b) { mat.skip = Skip::Clip; return; }
        mat.clip = add_clip({int(sx(float(l))), int(sy(float(t))), int(sx(float(r + 1))), int(sy(float(b + 1)))});
        const int renderer = (p.texheader[0] >> 13) & 3;
        mat.checker = (p.texheader[0] & 0x8000) != 0;
        if (renderer & 2) {
            const Source *src = source_for(p, mem);
            if (!src) { mat.skip = Skip::Texture; return; }
            mat.prog = mat.checker ? Prog::ModelChecker : src->transparent ? Prog::ModelAlpha : Prog::Model;
            mat.texture = src->texture;
            mat.textured = true;
            mat.u_scale = 1.0f / (8.0f * float(src->source_w));
            mat.v_scale = 1.0f / (8.0f * float(src->source_h));
        } else if (renderer == 0) {
            mat.prog = mat.checker ? Prog::FlatChecker : Prog::Flat;
        } else {
            mat.skip = Skip::Renderer;
        }
    };
    auto luma_key = [&](const rt::GeoPoly &p) -> uint8_t {
        if (mat.textured) return kLumaMask ? uint8_t(p.luma & kLumaMask) : uint8_t(0);
        return uint8_t(p.luma & 0xfc);
    };
    auto set_light = [&](const rt::GeoPoly &p) {
        light = Light{};
        light.cached = true;
        light.key = luma_key(p);
        if (mat.textured) {
            const int row = palette_row(p, mem);
            if (row < 0) return;
            light.palette = (float(row) + 0.5f) / float(kPaletteRows);
        } else {
            light.color = solid_color(p, mem);
        }
        light.ok = true;
    };

    const uint64_t sort_begin = now_us();
    if (!polys.empty()) {
        const auto &order = g.order.sort(polys);
        g.depth.resize(polys.size());
        const float step = 1.0f / float(polys.size() + 1);
        for (size_t rank = 0; rank < order.size(); ++rank)
            g.depth[order[rank].index] = 1.0f - float(rank + 1) * step;
    }
    last_sort_us_ = now_us() - sort_begin;

    for (size_t pi = 0; pi < polys.size(); ++pi) {
        const rt::GeoPoly &poly = polys[pi];
        const int n = poly.num_vertices;
        if (n < 3 || n > 8) continue;
        if (!same_material(poly)) set_material(poly);
        if (mat.skip != Skip::None) continue;
        if (!light.cached || luma_key(poly) != light.key) set_light(poly);
        if (!light.ok) continue;
        if (!mat.has_batch) {
            mat.batch = batch_for(mat.clip, mat.prog, mat.texture);
            mat.has_batch = true;
        }
        const float base_x = sx(float(crtc_x + poly.center[0]));
        const float base_y = sy(float(384 - poly.center[1] + crtc_y));
        g.batches[mat.batch].poly_refs.push_back(
            {&poly, base_x, base_y, mat.u_scale, mat.v_scale, light.palette, g.depth[pi], light.color});
    }
    clip_changes_ = unsigned(g.clips.size());

    const uint64_t vertex_begin = now_us();
    flush_batches();
    flush_palette_gl();
    last_vertex_us_ = now_us() - vertex_begin;

    for (size_t i = 0; i < g.batch_count; ++i) submitted_vertices_ += g.batches[i].valid_vertices;
}

// ---- System 24 2D worker (Core 3) + dirty-row GL upload ----------------------

void GpuGlesRenderer::s24_prepare(S24Slot &slot, const rt::Video &video) {
    slot.uploaded_tiles = 0;
    if (slot.palette_generation == UINT64_MAX || slot.palette_generation != video.system24_palette_generation()) {
        vita::upload_system24_palette(video, slot.palette_shadow.data(), vita::kSystem24PaletteWidth);
        slot.palette_generation = video.system24_palette_generation();
        slot.palette_dirty = true;
    }
    if (slot.generation != video.system24_texture_generation()) {
        for (int layer = 0; layer < 4; ++layer) {
            slot.uploaded_tiles += update_system24_layer_shadow(
                video, layer, slot.generation,
                slot.shadow[size_t(layer)].data(),
                slot.shadow[size_t(layer + 4)].data(),
                slot.dirty_rows[size_t(layer)]);
        }
        slot.generation = video.system24_texture_generation();
    }
    slot.backdrop = argb_to_rgba(video.system24_pen(0));

    using Rect = S24Rect;
    auto &rects = slot.rects;
    auto &runs = slot.runs;
    rects.clear();
    runs.clear();
    auto add_run = [&](int layer, const std::vector<Rect> &list) {
        if (list.empty()) return;
        runs.push_back({layer, uint32_t(rects.size()), uint32_t(list.size())});
        rects.insert(rects.end(), list.begin(), list.end());
    };
    for (int layer = 3; layer >= 0; --layer) {
        const uint16_t hreg = video.system24_word(0x5000u + unsigned(layer));
        const uint16_t vreg = video.system24_word(0x5004u + unsigned(layer));
        if (vreg & 0x8000) continue;
        const uint16_t ctrl = video.system24_word(0x5004u + unsigned(layer & 2));
        const int split_mode = (ctrl >> 13) & 3;
        const bool line_scroll = (hreg & 0x8000) != 0;
        const uint32_t line_base = 0x4000u + 0x200u * unsigned(layer);

        if (split_mode) {
            if (layer & 1) continue;
            auto &split_rects = slot.split;
            split_rects[0].clear();
            split_rects[1].clear();
            const int source_y = vreg & 511;
            auto add = [&](int source_layer, int x0, int x1, int y0, int y1, int scroll) {
                x0 = std::clamp(x0, 0, rt::Video::W);
                x1 = std::clamp(x1, 0, rt::Video::W);
                y0 = std::clamp(y0, 0, rt::Video::H);
                y1 = std::clamp(y1, 0, rt::Video::H);
                if (x0 < x1 && y0 < y1)
                    split_rects[size_t(source_layer - layer)].push_back({x0, x1, y0, y1, scroll, source_y + y0});
            };
            if (split_mode == 1) {
                const int neg_v = (-int(vreg)) & 0x3ff;
                const int cut = neg_v & 511;
                int first_layer = layer;
                if (!(neg_v & 0x200)) first_layer ^= 1;
                if (line_scroll) {
                    for (int y = 0; y < rt::Video::H;) {
                        const int scroll = (-int(video.system24_word(line_base + unsigned(y)))) & 511;
                        const int source_layer = y >= cut ? first_layer ^ 1 : first_layer;
                        int y1 = y + 1;
                        while (y1 < rt::Video::H &&
                               ((-int(video.system24_word(line_base + unsigned(y1)))) & 511) == scroll &&
                               (y1 >= cut ? first_layer ^ 1 : first_layer) == source_layer)
                            ++y1;
                        add(source_layer, 0, rt::Video::W, y, y1, scroll);
                        y = y1;
                    }
                } else {
                    const int scroll = (-int(hreg)) & 511;
                    add(first_layer, 0, rt::Video::W, 0, cut, scroll);
                    add(first_layer ^ 1, 0, rt::Video::W, cut, rt::Video::H, scroll);
                }
            } else {
                if (line_scroll) {
                    for (int y = 0; y < rt::Video::H; ++y) {
                        const int raw = video.system24_word(line_base + unsigned(y));
                        const int cut = raw & 511;
                        const int first_layer = (raw & 0x200) ? layer : layer ^ 1;
                        const int scroll = (-cut) & 511;
                        add(first_layer, 0, cut, y, y + 1, scroll);
                        add(first_layer ^ 1, cut, rt::Video::W, y, y + 1, scroll);
                    }
                } else {
                    const int cut = hreg & 511;
                    const int first_layer = (hreg & 0x200) ? layer : layer ^ 1;
                    const int scroll = (-cut) & 511;
                    add(first_layer, 0, cut, 0, rt::Video::H, scroll);
                    add(first_layer ^ 1, cut, rt::Video::W, 0, rt::Video::H, scroll);
                }
            }
            add_run(layer, split_rects[0]);
            add_run(layer + 1, split_rects[1]);
            continue;
        }

        const bool win = (layer & 1) != 0;
        const uint32_t mask_base = layer >= 2 ? 0x6800u : 0x6000u;
        auto line_scroll_of = [&](int y) {
            return line_scroll ? ((-int(video.system24_word(line_base + unsigned(y)))) & 511) : ((-int(hreg)) & 511);
        };
        const uint32_t run_first = uint32_t(rects.size());
        for (int y = 0; y < rt::Video::H;) {
            const int h = line_scroll_of(y);
            std::array<uint16_t, 4> masks{};
            for (int w = 0; w < 4; ++w) masks[size_t(w)] = video.system24_word(mask_base + unsigned(y * 4 + w));
            int y1 = y + 1;
            for (; y1 < rt::Video::H && line_scroll_of(y1) == h; ++y1) {
                bool same = true;
                for (int w = 0; same && w < 4; ++w)
                    same = video.system24_word(mask_base + unsigned(y1 * 4 + w)) == masks[size_t(w)];
                if (!same) break;
            }
            auto visible = [&](int block) { return ((masks[size_t(block / 16)] & (0x8000u >> (block & 15))) != 0) == win; };
            for (int block = 0; block * 8 < rt::Video::W;) {
                while (block * 8 < rt::Video::W && !visible(block)) ++block;
                const int first = block;
                while (block * 8 < rt::Video::W && visible(block)) ++block;
                if (first < block)
                    rects.push_back({first * 8, std::min(block * 8, rt::Video::W), y, y1, h, (vreg & 511) + y});
            }
            y = y1;
        }
        if (rects.size() > run_first) runs.push_back({layer, run_first, uint32_t(rects.size()) - run_first});
    }
    slot.valid = true;
}

void GpuGlesRenderer::s24_upload_gl(S24Slot &slot) {
    auto &gl = g.gl;
    if (slot.palette_dirty) {
        gl.glActiveTexture(GL_TEXTURE2);
        gl.glBindTexture(GL_TEXTURE_2D, slot.palette_texture);
        gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, vita::kSystem24PaletteWidth, vita::kSystem24PaletteHeight, GL_RGBA,
                           GL_UNSIGNED_BYTE, slot.palette_shadow.data());
        slot.palette_dirty = false;
    }
    gl.glActiveTexture(GL_TEXTURE0);
    for (int layer = 0; layer < 4; ++layer) {
        uint64_t mask = slot.dirty_rows[size_t(layer)];
        if (!mask) continue;
        slot.dirty_rows[size_t(layer)] = 0;
        // Group consecutive dirty 8-pixel tile rows into horizontal bands to minimize GL calls.
        while (mask) {
            const unsigned first = unsigned(std::countr_zero(mask));
            const uint64_t shifted = mask >> first;
            const unsigned len = ~shifted ? unsigned(std::countr_zero(~shifted)) : 64u - first;
            mask = (first + len >= 64u) ? 0 : (mask & (~uint64_t(0) << (first + len)));
            const GLint y = GLint(first * 8u);
            const GLsizei h = GLsizei(len * 8u);
            gl.glBindTexture(GL_TEXTURE_2D, slot.textures[size_t(layer)]);
            gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, 512, h, GL_RGBA, GL_UNSIGNED_BYTE,
                               slot.shadow[size_t(layer)].data() + size_t(y) * 512u);
            gl.glBindTexture(GL_TEXTURE_2D, slot.textures[size_t(layer + 4)]);
            gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, 512, h, GL_RGBA, GL_UNSIGNED_BYTE,
                               slot.shadow[size_t(layer + 4)].data() + size_t(y) * 512u);
        }
    }
}

void GpuGlesRenderer::s24_worker_loop() {
    set_current_thread_name("daytona_2d");
    pin_current_thread(worker_2d_core_);
    {
        std::lock_guard<std::mutex> lk(s24_mutex_);
        s24_init_ = true;
    }
    s24_cv_done_.notify_one();

    for (;;) {
        {
            std::unique_lock<std::mutex> lk(s24_mutex_);
            s24_cv_start_.wait(lk, [&] { return s24_job_ready_ || s24_stop_; });
            if (s24_stop_) break;
            s24_job_ready_ = false;
        }
        const uint64_t begin = now_us();
        S24Slot &slot = *s24_job_;
        try {
            s24_prepare(slot, *s24_video_);
        } catch (...) {
            slot.valid = false;
            slot.generation = slot.palette_generation = UINT64_MAX;
        }
        s24_job_us_ = now_us() - begin;
        {
            std::lock_guard<std::mutex> lk(s24_mutex_);
            s24_job_done_ = true;
        }
        s24_cv_done_.notify_one();
    }
}

bool GpuGlesRenderer::s24_start_thread() {
    if (s24_thread_.joinable()) return true;
    {
        std::lock_guard<std::mutex> lk(s24_mutex_);
        s24_stop_ = false;
        s24_job_ready_ = false;
        s24_job_done_ = true;
        s24_init_ = false;
    }
    try {
        s24_thread_ = std::thread(&GpuGlesRenderer::s24_worker_loop, this);
    } catch (...) {
        return false;
    }
    std::unique_lock<std::mutex> lk(s24_mutex_);
    s24_cv_done_.wait(lk, [&] { return s24_init_; });
    return true;
}

void GpuGlesRenderer::s24_stop_thread() {
    s24_join();
    if (s24_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lk(s24_mutex_);
            s24_stop_ = true;
        }
        s24_cv_start_.notify_one();
        s24_thread_.join();
    }
}

void GpuGlesRenderer::s24_kick(const rt::Video &video) {
    S24Slot &slot = s24_[s24_back_];
    if (!s24_thread_.joinable()) {
        const uint64_t begin = now_us();
        try { s24_prepare(slot, video); }
        catch (...) { slot.valid = false; slot.generation = slot.palette_generation = UINT64_MAX; }
        s24_job_us_ = now_us() - begin;
        return;
    }
    {
        std::lock_guard<std::mutex> lk(s24_mutex_);
        s24_video_ = &video;
        s24_job_ = &slot;
        s24_job_done_ = false;
        s24_job_ready_ = true;
        s24_busy_ = true;
    }
    s24_cv_start_.notify_one();
}

void GpuGlesRenderer::s24_join() {
    if (!s24_busy_) return;
    std::unique_lock<std::mutex> lk(s24_mutex_);
    s24_cv_done_.wait(lk, [&] { return s24_job_done_; });
    s24_busy_ = false;
}

void GpuGlesRenderer::s24_emit(const S24Slot &slot, bool foreground) {
    const Depth depth = !k2DLayersByDepth ? Depth::Off : foreground ? Depth::TestWrite : Depth::Test;
    const float d = !k2DLayersByDepth ? 0.0f : foreground ? kForegroundDepth : kBackgroundDepth;
    if (!foreground) {
        push_quad(Prog::Flat, 0, sx(0.0f), sy(0.0f), sx(kSourceW), sy(kSourceH),
                  0.0f, 0.0f, 0.0f, 0.0f, slot.backdrop, depth, d);
    }
    for (const S24Run &run : slot.runs) {
        const GLuint texture = slot.textures[size_t((foreground ? 4 : 0) + run.layer)];
        Vertex *dst = append_quads(run.count);
        const bool indexed = dst != nullptr;
        const size_t count = size_t(run.count) * (indexed ? 4u : 6u);
        if (!indexed) dst = g.verts.append(count);
        const uint32_t first = uint32_t(dst - g.verts.data());
        for (uint32_t i = run.first; i < run.first + run.count; ++i) {
            const S24Rect &r = slot.rects[i];
            const float u0 = float(r.x0 + r.h) / 512.0f, u1 = float(r.x1 + r.h) / 512.0f;
            const float v0 = float(r.v) / 512.0f, v1 = float(r.v + (r.y1 - r.y0)) / 512.0f;
            const float x0 = sx(float(r.x0)), x1 = sx(float(r.x1)), y0 = sy(float(r.y0)), y1 = sy(float(r.y1));
            *dst++ = {x0, y0, 1, u0, v0, 0, d, 0xffffffffu};
            *dst++ = {x1, y0, 1, u1, v0, 0, d, 0xffffffffu};
            *dst++ = {x1, y1, 1, u1, v1, 0, d, 0xffffffffu};
            if (!indexed) {
                *dst++ = {x0, y0, 1, u0, v0, 0, d, 0xffffffffu};
                *dst++ = {x1, y1, 1, u1, v1, 0, d, 0xffffffffu};
            }
            *dst++ = {x0, y1, 1, u0, v1, 0, d, 0xffffffffu};
        }
        g.cmds.push_back({Prog::Layer, texture, first, uint32_t(count), depth, g.clip, indexed});
        system24_quads_ += run.count;
    }
}

void GpuGlesRenderer::draw(rt::Video &video) {
    const uint64_t begin = now_us();
    last_polygon_us_ = last_tile_us_ = last_upload_us_ = last_sort_us_ = last_texture_us_ = 0;

    S24Slot &front = s24_[s24_back_ ^ 1u];
    s24_kick(video); // Core 3 prepares back slot shadow buffers in parallel with polygon work

    const uint64_t t0 = now_us();
    system24_quads_ = 0;
    if (front.valid) s24_upload_gl(front); // Upload dirty rows of front slot to GLES textures
    g.s24_palette_texture = front.palette_texture;
    if (front.valid) s24_emit(front, k2DLayersByDepth);

    const uint64_t t1 = now_us();
    draw_polygons(video);

    const uint64_t t2 = now_us();
    if (front.valid) s24_emit(front, !k2DLayersByDepth);

    const uint64_t t3 = now_us();
    s24_join();
    const uint64_t t4 = now_us();

    system24_uploaded_tiles_ = s24_[s24_back_].uploaded_tiles;
    s24_back_ ^= 1u;
    last_2d_worker_us_ = s24_job_us_;
    last_upload_us_ = (t0 - begin) + (t4 - t3);
    last_polygon_us_ = t2 - t1;
    last_tile_us_ = (t1 - t0) + (t3 - t2);
    last_gpu_ms_ = double(t4 - begin) / 1000.0;
}

} // namespace arm64
