// varfont_gl.h — GPU analytic glyph coverage (VarFont renderer layer)
//
// Owns the coverage atlas / FBOs / shaders. No Dear ImGui dependency — ImGui
// (or OF / your engine) is a host that composites GlyphTex / GlyphQuad output.
//
// Narrow GL seam between Face/morph/layout and the GPU: atlas packing, live
// pages, and multiple coverage backends in this module:
//   - signed-area accumulation (RenderGlyph, edge soup)
//   - Loop-Blinn quadratics (RenderGlyphCurves)
//   - Slug exact-curve coverage (RenderGlyphSlug)
//   - GPU morph reconstruction (UpdateMorphCurves + RenderMorphGlyph)
//
// GL entry points load via caller proc (e.g. glfwGetProcAddress); no GLEW/glad.

#pragma once

namespace ImVarFont {
namespace glr {

// Proc loader signature, e.g. (GLProc)glfwGetProcAddress.
typedef void* (*GLProc)(const char*);

// One rasterized glyph, packed into a shared RGBA8 atlas page (shelf packer).
// Small/medium cells share atlas pages so many glyphs batch into one draw call;
// cells too large for the atlas fall back to a dedicated texture. The texture
// holds (1,1,1,coverage), so a stock textured-quad shader (ImGui's included)
// composites it as colour*coverage with nothing custom.
//
// (u0,v0) is the UV that maps to the on-screen top-left (pmin); (u1,v1) maps to
// the bottom-right (pmax) — already screen-oriented, no flip needed.
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

// Route the live/morph render path through the Slug analytic-coverage rasterizer
// (RenderGlyphSlug) when it is available, instead of the signed-area coverage
// path. No effect where SlugReady() is false. Toggle freely; live glyphs re-render
// through the active path next frame. Lets the example app A/B the two renderers.
void SetPreferSlug(bool enable);
bool PreferSlug();

// Runtime gate for the per-fragment convex-hull cull in the exact-curve coverage
// shader. The cull is exact (it only skips curves that cannot touch a fragment),
// so toggling it never changes output — it exists so a benchmark can measure the
// cull's speedup as a clean A/B on a single build. On by default.
void SetCullEnabled(bool enable);
bool CullEnabled();

// Number of transient ("live") atlas pages currently allocated. Live/morph cells
// are rewound (not freed) each frame, so this count must stay bounded over a long
// morph; tests assert it does (a permanent guard for the live-atlas growth leak).
int  LivePageCount();

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
// live=true targets the transient page set (rewound every BeginFrame) instead of
// the persistent atlas, so morph/animated glyphs re-render each frame with no
// caching — same semantics as RenderGlyphCurves(live). The signed-area coverage
// path is deterministic and threshold-free, so it is the preferred live renderer
// (the supersampled Loop-Blinn live path can sparkle at winding thresholds).
GlyphTex RenderGlyph(const float* edges, int edge_count, int w, int h, float gamma,
                     bool live = false);

// One curve segment in CELL PIXEL space, y-down (same space as RenderGlyph's
// edges). type 1 = line (p0->p1, uses p[0..3]); type 2 = quadratic Bezier
// (p0, control, p2, uses p[0..5]).
struct Curve {
    int   type;
    float p[6];   // x0,y0, x1,y1, x2,y2
};

// Loop-Blinn analytic fill: rasterize a quadratic/line curve soup into the atlas
// using supersampled non-zero-winding accumulation (quadratics evaluated
// analytically, no flattening). Same ownership/generation rules as RenderGlyph.
// Returns an invalid GlyphTex when the Loop-Blinn path is unavailable.
//
// live=true targets a transient page set that is rewound every BeginFrame instead
// of the persistent atlas: callers re-render glyphs each frame (no caching) so
// zoom/axis morphing never re-rasterizes on the CPU. Returned cells are valid only
// until the next BeginFrame.
GlyphTex RenderGlyphCurves(const Curve* curves, int count, int w, int h, float gamma,
                           bool live = false);

// True when the Loop-Blinn fill path is usable (its programs linked + a blendable
// float target). Like CoverageReady(), reports false under ForceCpuFallback.
bool LoopBlinnReady();

// Slug-style analytic-coverage fill: rasterize a quadratic/line curve soup into
// the atlas in a single, supersample-free pass using Eric Lengyel's public-domain
// Slug coverage algorithm (per-fragment dual-ray Bézier coverage), brute-forcing
// every curve in the cell. Same coordinate convention, ownership and generation
// rules as RenderGlyphCurves; `type` 1 = line, 2 = quadratic. Returns an invalid
// GlyphTex when the Slug path is unavailable (SlugReady() == false).
//
// live=true targets the transient page set rewound every BeginFrame (no caching),
// so a morphing glyph re-renders each frame from CPU-blended curves when GPU
// reconstruction is off, or from RenderMorphGlyph when PreferGpuMorph is on.
GlyphTex RenderGlyphSlug(const Curve* curves, int count, int w, int h, float gamma,
                         bool live = false);

// True when the Slug analytic-coverage path is usable (program linked + curve
// texture allocated). Like CoverageReady(), reports false under ForceCpuFallback.
bool SlugReady();

// ---- GPU morph reconstruction ------------------------------------------------
// Reconstruct a variable glyph's control points on the GPU from a static delta
// buffer, so a morph (axis drag) OR a zoom is just a uniform update — no per-glyph
// CPU outline work and no per-frame curve upload. The reconstructed curves feed
// the exact-curve coverage shader (same as RenderGlyphSlug).
//
// MorphReady() gates the path: needs the reconstruct program AND the exact-curve
// coverage program AND (on ES) a render-to-RGBA32F-capable target. MorphMaxTerms()
// is the largest correction-term count the GPU path accepts (order-1 axes + order-2
// pairs); above it, reconstruct on the CPU and use RenderGlyphSlug instead.
bool MorphReady();
int  MorphMaxTerms();

// (Re)upload a glyph's static delta buffer. Call once when the morph cell is
// (re)built. `*tex` is a caller-owned GL texture handle stored in the cell; pass a
// handle initialized to 0 and glr allocates it (reuses it on later updates).
//   base       : `count` quad curves in DESIGN units (lines as degenerate quads,
//                control = midpoint). p[0..5] = p0.xy, p1.xy, p2.xy.
//   deltas     : termCount*count quads, term-major: deltas[t*count + i] is curve i's
//                control-point delta for correction term t (DESIGN units). Terms are
//                the order-1 per-axis main effects followed by the order-2 pairs.
//   termCount  : number of correction terms (0..MorphMaxTerms()).
// Returns false if the GPU morph path is unavailable or inputs are out of range.
bool UpdateMorphCurves(unsigned int* tex, const Curve* base, const Curve* deltas,
                       int count, int termCount);

// Render a morphed glyph: reconstruct (base + Σ weight·delta) in design units, map to
// the cell with (originX,originY) + (scaleX,scaleY), and run exact-curve coverage.
//   dataTex    : handle previously filled by UpdateMorphCurves.
//   weights    : termCount term weights, term-major matching `deltas` (axis term =
//                frac_i; pair term = frac_i·frac_j).
//   origin/scale: cell-pixel affine, y-up: cx = originX + scaleX·x,
//                 cy = originY + scaleY·y (caller folds bbox + padding + y-flip in).
//   w,h        : cell size in pixels. live=true -> transient page (rewound per frame).
// Same ownership/generation rules as RenderGlyphSlug. Returns invalid on failure.
GlyphTex RenderMorphGlyph(unsigned int dataTex, int count, int termCount,
                          const float* weights, float originX, float originY,
                          float scaleX, float scaleY, int w, int h,
                          float gamma, bool live = false);

// Release a delta-buffer texture (call when the cell is dropped). Safe on 0.
void DeleteMorphCurves(unsigned int* tex);

// Pack a CPU-rasterized coverage bitmap into the atlas (portable fallback for
// when CoverageReady() is false, and the FreeType-raster grid-fit path). `a8` is
// `w*h` single-channel coverage stored top-down (row 0 = glyph top); it is expanded
// to (255,255,255,coverage) so the returned cell composites identically to the GPU
// path. Same ownership and generation rules as RenderGlyph (do not free; re-render
// when gen != AtlasGen). live=true uses the transient per-frame page (rewound each
// frame) for cells that are re-uploaded every frame (e.g. raster under a live morph).
GlyphTex UploadGlyph(const unsigned char* a8, int w, int h, bool live = false);

} // namespace glr
} // namespace ImVarFont

// Public engine-level namespace: VarFont is the ImGui-free engine brand.
// `ImVarFont::glr` remains the canonical spelling for source compatibility.
namespace VarFont { namespace gl = ImVarFont::glr; }
