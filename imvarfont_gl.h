// imvarfont_gl.h  -  GPU analytic-coverage glyph rasterizer (internal)
//
// Backend-agnostic, dependency-free GL helper used by the analytic glyph
// renderer. It knows nothing about FreeType or fonts: it takes a soup of line
// edges (in cell pixel space) and produces a single-channel coverage texture
// via signed-area accumulation (font-rs / Pathfinder style), with exact,
// conflation-free anti-aliasing and no MSAA.
//
// GL entry points are loaded through a caller-provided proc loader (e.g.
// glfwGetProcAddress) so the library pulls in no GL loader of its own.
//
// The seam (RenderGlyph taking an edge soup + cell size) is deliberately narrow
// so a future Loop-Blinn or per-pixel banding backend can replace only this file.

#pragma once

namespace ImVarFont {
namespace glr {

// Proc loader signature, e.g. (GLProc)glfwGetProcAddress.
typedef void* (*GLProc)(const char*);

// One rasterized glyph, packed into a shared RGBA8 atlas page (shelf packer).
// Small/medium cells share atlas pages so many glyphs batch into one draw call;
// cells too large for the atlas fall back to a dedicated texture. The texture
// holds (1,1,1,coverage) so ImGui's default shader composites it as
// colour*coverage with no custom shader.
//
// (u0,v0) is the UV that maps to the on-screen top-left (pmin); (u1,v1) maps to
// the bottom-right (pmax) — i.e. already oriented for ImGui, no flip needed.
//
// `gen` stamps which atlas generation this entry belongs to. The atlas is owned
// by the renderer (never freed per-glyph); when it is reset the generation is
// bumped, so callers must treat a cached entry whose gen != AtlasGen() as a miss
// and re-render it.
struct GlyphTex {
    unsigned int tex   = 0;
    float        u0    = 0.f, v0 = 0.f, u1 = 1.f, v1 = 1.f;
    int          w     = 0;
    int          h     = 0;
    unsigned int gen   = 0;
    bool         valid = false;
};

// Testing/diagnostics: force the CPU raster fallback (disable the GPU coverage
// path) even where it is available. Can be toggled at any time — cached glyphs
// re-render through the active path on the next frame. Lets the fallback used on
// ES2 / WebGL1 be exercised on a desktop GL build.
void SetForceCpuFallback(bool enable);

// Initialize GL resources (shaders, FBOs, VAO/VBO). Safe to call once a GL
// context is current. Returns false if GL entry points or programs failed.
bool Init(GLProc get_proc);

// Release all GL resources. Safe to call with no context (best-effort).
void Shutdown();

// True when Init succeeded and the renderer is usable this frame (atlas uploads
// and compositing work). This can be true even when the GPU coverage path is
// not available — see CoverageReady().
bool Ready();

// True when the GPU analytic-coverage path is usable: a blendable float color
// buffer plus the coverage/resolve programs are available. When this is false
// but Ready() is true (e.g. OpenGL ES 2 / WebGL1, or a driver lacking blendable
// half-float), the caller should rasterize coverage on the CPU and hand it to
// UploadGlyph() instead of calling RenderGlyph().
bool CoverageReady();

// Current atlas generation. Cached GlyphTex entries whose `gen` differs from
// this value point into a recycled atlas region and must be re-rendered.
unsigned int AtlasGen();

// Call once at the start of each frame (before any RenderGlyph this frame). Any
// pending atlas recycle is applied here, so recycling can never overwrite cells
// whose quads were already emitted in the previous frame.
void BeginFrame();

// Rasterize the given line edges and pack the coverage into the atlas.
//   edges       : flat array [x0,y0,x1,y1, ...] in CELL PIXEL space, y-down,
//                 already offset so the glyph sits inside [0,w] x [0,h].
//   edge_count  : number of edges (so the array has edge_count*4 floats).
//   w,h         : cell size in pixels (> 0).
//   gamma       : coverage gamma applied at resolve (e.g. 1/1.4 for text);
//                 pass 1.0 for linear coverage.
// Returns an invalid GlyphTex on failure. The texture is owned by the renderer;
// do not free it. Re-rendering when gen != AtlasGen() reclaims stale space.
GlyphTex RenderGlyph(const float* edges, int edge_count, int w, int h, float gamma);

// Pack a CPU-rasterized coverage bitmap into the atlas (portable fallback for
// when CoverageReady() is false). `a8` is `w*h` single-channel coverage stored
// top-down (row 0 = glyph top); it is expanded to (255,255,255,coverage) so the
// returned cell composites identically to the GPU path. Same ownership and
// generation rules as RenderGlyph (do not free; re-render when gen != AtlasGen).
GlyphTex UploadGlyph(const unsigned char* a8, int w, int h);

} // namespace glr
} // namespace ImVarFont
