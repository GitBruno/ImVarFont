// varfont_core.h  –  Variable-font engine: outlines, morphing, layout, fill
//
// Renders glyph outlines with FreeType and fills them through the GPU coverage
// atlas in varfont_gl.  No Dear ImGui dependency: the engine hands finished
// glyph quads (and, optionally, outline paths) to a host through small function
// pointers, so ImGui, openFrameworks, RigKit or a bare GL loop can composite
// them.  imgui_var_font.h is the Dear ImGui adapter over this API.
//
// Usage (minimal, non-ImGui host):
//   VarFont::Face* face = VarFont::LoadFace("MyFont.ttf");
//   VarFont::InitRenderer(glfwGetProcAddress);
//   VarFont::SetGlyphQuadEmitter(&myEmit, this);
//   // Per frame:
//   VarFont::BeginHostFrame(frameIndex, framebufferScale);
//   VarFont::DrawString(face, 96.f, {x, y}, VARFONT_COL32_WHITE, "Hello");
//   // Cleanup:
//   VarFont::FreeFace(face);
//
// Required: FreeType 2.x  (link with freetype)
// Optional: HarfBuzz  (VARFONT_USE_HARFBUZZ / IMVARFONT_USE_HARFBUZZ)
//
// License: MIT  (WTFPL if your name is Omar Cornut — thanks for Dear ImGui)

#pragma once
#include <cstdint>
#include <vector>

namespace VarFont {

// ---------------------------------------------------------------------------
// Small value types
//
// Colours use the same 32-bit packing as Dear ImGui's IM_COL32: R in bits 0-7,
// G in 8-15, B in 16-23, A in 24-31.  An ImU32 can be passed straight through.
// ---------------------------------------------------------------------------

struct Vec2 {
    float x = 0.f;
    float y = 0.f;

    constexpr Vec2() = default;
    constexpr Vec2(float _x, float _y) : x(_x), y(_y) {}
};

using Color = uint32_t;

#define VARFONT_COL32(R, G, B, A)                                              \
    (((uint32_t)(A) << 24) | ((uint32_t)(B) << 16) | ((uint32_t)(G) << 8) |    \
     ((uint32_t)(R)))
#define VARFONT_COL32_WHITE VARFONT_COL32(255, 255, 255, 255)
#define VARFONT_COL32_BLACK VARFONT_COL32(0, 0, 0, 255)

// ---------------------------------------------------------------------------
// Render quality
// ---------------------------------------------------------------------------

enum class RenderMode {
    Vector,        // Unhinted design-unit outlines (best for zoom / extrapolation)
    HintedVector,  // Hinted outlines at em_px
    Raster,        // FreeType grayscale bitmap composite (best static quality)
    LoopBlinn,     // Unhinted outlines filled analytically (Loop-Blinn quadratics)
    LoopBlinnLive, // Loop-Blinn re-rendered every frame (no atlas cache); for
                   // re-rasterization-free zoom / variable-axis morphing
};

enum class HintingFlags {
    Native,    // Font native hinter + FT_LOAD_TARGET_NORMAL
    Light,     // FT_LOAD_TARGET_LIGHT (ClearType-style vertical snap)
    AutoHint,  // FT_LOAD_FORCE_AUTOHINT
};

struct Face;

// ---------------------------------------------------------------------------
// GPU renderer lifecycle
//
// The analytic glyph renderer rasterizes coverage on the GPU. Call InitRenderer
// once a GL context is current, passing your GL proc loader (e.g.
// glfwGetProcAddress), and ShutdownRenderer before the context is destroyed.
// When InitRenderer succeeds but the GPU coverage path is unavailable (OpenGL
// ES 2 / WebGL1, or no blendable float target), filled text is rasterized on the
// CPU with FreeType into the same atlas. If InitRenderer is never called (or
// fails outright), filled text is skipped.
//
// ForceCpuFallback(true) disables the GPU path so the CPU fallback can be
// exercised on a desktop GL build — for testing only. It can be toggled live
// (e.g. from a checkbox); cached glyphs re-render on the next frame.
// ---------------------------------------------------------------------------
bool InitRenderer(void* (*gl_get_proc_address)(const char*));
void ShutdownRenderer();
bool RendererReady();
void ForceCpuFallback(bool enable);

// ---------------------------------------------------------------------------
// Host seam
//
// VarFont owns Face/morph/layout; varfont_gl owns the GPU coverage atlas. The
// host composites what comes out:
//   • filled glyphs — a GlyphQuad emitter (textured quad, y-down screen px)
//   • outline strokes — a PathEmitter (contour points + a stroke call)
// Both default to null, in which case that part of the draw is skipped.
// Scene-layer FBO blends (e.g. ofxCompositorKit) are app-level; not this API.
// ---------------------------------------------------------------------------
struct GlyphQuad {
    unsigned int tex = 0;
    float        x0  = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f; // screen px, y-down
    float        u0  = 0.f, v0 = 0.f, u1 = 1.f, v1 = 1.f;
    uint32_t     col = 0xffffffffu; // VARFONT_COL32 packing
};
using EmitGlyphQuadFn = void (*)(const GlyphQuad& q, void* user);

// Call once per host frame before filled-glyph draws. framebuffer_scale is the
// window→framebuffer ratio (HiDPI); pass 0 to keep the previous override. With
// no override the engine assumes 1.0 — it never queries a UI toolkit for it.
//
// A host that never calls this still works: each DrawString() batch then counts
// as its own frame for atlas recycling.
void BeginHostFrame(int frame_index, float framebuffer_scale = 0.f);

// Filled atlas quads go here. Pass nullptr to drop filled glyphs.
void SetGlyphQuadEmitter(EmitGlyphQuadFn fn, void* user = nullptr);

// Current emitter, for hosts that install one temporarily (see the ImGui
// adapter's AddText). out_user receives the paired user pointer when non-null.
EmitGlyphQuadFn GetGlyphQuadEmitter(void** out_user = nullptr);

// Outline stroking sink. One contour is emitted as begin() followed by line_to
// / cubic_to in screen pixels (y-down), then stroke() closes and strokes it.
// Quadratic segments are elevated to cubics, so a host only needs a cubic path
// primitive.
struct PathEmitter {
    void (*begin)(void* user)                                         = nullptr;
    void (*line_to)(float x, float y, void* user)                     = nullptr;
    void (*cubic_to)(float x1, float y1, float x2, float y2,
                     float x3, float y3, void* user)                  = nullptr;
    void (*stroke)(uint32_t col, float thickness, bool closed,
                   void* user)                                        = nullptr;
    void* user                                                        = nullptr;
};

// Install the outline sink. Pass nullptr to skip outline stroking entirely
// (TextStyle::outline then has no effect).
void               SetPathEmitter(const PathEmitter* emitter);
const PathEmitter* GetPathEmitter();

// Route the live/morph render path through the public-domain Slug analytic-
// coverage rasterizer when available (SlugRendererAvailable()). Single-pass,
// supersample-free, curves fed directly to the GPU. Toggle live (e.g. a
// checkbox) to A/B against the default signed-area coverage path.
void PreferSlugRenderer(bool enable);
bool PreferSlugRenderer();

// Reconstruct morph glyphs on the GPU (base + Σ frac·delta from a static per-glyph
// delta buffer, axis fractions as uniforms) instead of CPU-blending and re-uploading
// curves each frame. An axis drag or zoom becomes a uniform update with no per-glyph
// CPU outline work. Requires GpuMorphAvailable(); falls back transparently otherwise.
// Default off. Toggle freely (live glyphs re-render through the active path).
void PreferGpuMorphRenderer(bool enable);
bool PreferGpuMorphRenderer();
bool GpuMorphAvailable();
bool SlugRendererAvailable();

// Grid-fit small sizes (FreeType hinting). Hinting only matters at small sizes, and the
// only way to be truly on par with FreeType is to use FreeType: when enabled, filled
// glyphs below a pixel-size cutoff are rendered through FreeType's own autohinter +
// grayscale raster (FT_LOAD_FORCE_AUTOHINT + FT_Render_Glyph) straight into the glyph
// cache -- literally FreeType, no shape distortion. Each snapshot is keyed by glyph,
// em, and the current variation instance (axis values); static text reuses the cache,
// while a live morph re-rasterizes into a transient page when axes move (cheap at
// small ppem). Above the cutoff it is a no-op and the analytic outline / GPU morph
// fast path is used (large text needs no hinting). Off by default; toggle freely.
void PreferGridFit(bool enable);
bool PreferGridFit();

// Grid-fit mode:
//   Off      - no hinting (analytic outline / GPU morph; best for large/animated text).
//   FreeType - FreeType's own autohinter + grayscale raster into the glyph cache below
//              the size cutoff. Guaranteed FreeType parity; runs static and under morph.
// PreferGridFit(bool) is a thin alias: true => FreeType, false => Off.
enum class GridFitMode { Off, FreeType };
void        PreferGridFitMode(GridFitMode mode);
GridFitMode PreferGridFitMode();

// Pixel-size cutoff (logical px/em) below which grid-fit hinting applies; at or above it
// hinting is a no-op. Default 28. Clamped to a sane range.
void  PreferGridFitMaxPx(float px);
float PreferGridFitMaxPx();

// Per-frame CPU render profile for the most recently completed frame. Lets a HUD
// attribute frame cost to the morph outline blend vs. the rasterization submission,
// and infer "GPU/other" as (wall-clock frame − blendMs − rasterMs). Timers are
// CPU-side only, so a frame where blendMs+rasterMs is far below the frame time is
// GPU-bound (fill/vsync); one where they dominate is CPU-bound (blend or GL submit).
struct RenderProfile {
    float blendMs  = 0.f;  // CPU time in the morph outline blend (cells, FT, reconstruct)
    float rasterMs = 0.f;  // CPU time submitting glyph rasterization (recon + coverage draws)
    int   glyphs   = 0;    // glyphs drawn through the fill path this frame
    int   rebuilds = 0;    // morph cells (re)sampled this frame (0 = pure reuse/idle)
};
RenderProfile GetRenderProfile();

RenderMode    GetRenderMode(const Face* face);
void          SetRenderMode(Face* face, RenderMode mode);
HintingFlags  GetHintingFlags(const Face* face);
void          SetHintingFlags(Face* face, HintingFlags flags);
const char*   GetRenderModeLabel(RenderMode mode);
const char*   GetHintingFlagsLabel(HintingFlags flags);

// ---------------------------------------------------------------------------
// Axis  –  one per design axis in the font
// ---------------------------------------------------------------------------
struct Axis {
    uint32_t Tag;           // 4-byte OpenType tag, e.g. 0x77676874 = 'wght'
    char     Name[64];      // Human-readable name from the font's 'fvar' table
    float    Min;
    float    Max;
    float    Default;
    float    Value;         // Current value – modify then call ApplyAxes()
};

// ---------------------------------------------------------------------------
// Opaque face handle (defined internally)
// ---------------------------------------------------------------------------

// Lifecycle

// Load a font file.  Works with static and variable OTF/TTF fonts.
// Returns nullptr on failure.
// If err_buf/err_buf_size are provided, a human-readable message is written on failure.
Face* LoadFace(const char* path, char* err_buf = nullptr, int err_buf_size = 0);

// Release all resources.  Safe to call with nullptr.
void  FreeFace(Face* face);

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

bool        IsLoaded(const Face* face);
bool        IsVariable(const Face* face);
const char* GetFamilyName(const Face* face);  // e.g. "Inter"
const char* GetStyleName(const Face* face);   // e.g. "Regular"
const char* GetFilePath(const Face* face);

// True when the font has any kerning source (legacy kern table and/or GPOS).
bool HasKerning(const Face* face);

// True when the font file contains a legacy 'kern' table.
bool HasKernTable(const Face* face);

// True when the font file contains GPOS positioning data (requires HarfBuzz at build time).
bool HasGpos(const Face* face);

// True when HarfBuzz is linked and this face was shaped with it.
bool UsesHarfBuzz(const Face* face);

// Kerning master switch.
bool GetUseKerning(const Face* face);
void SetUseKerning(Face* face, bool enabled);

// Use HarfBuzz / GPOS for pair spacing when kerning is on.
bool GetUseHarfBuzz(const Face* face);
void SetUseHarfBuzz(Face* face, bool enabled);

// Use the legacy 'kern' table for pair spacing when kerning is on.
bool GetUseKernTable(const Face* face);
void SetUseKernTable(Face* face, bool enabled);

// Active backend label: "off", "HarfBuzz (GPOS)", "kern table", "none", etc.
const char* GetKerningEngineLabel(const Face* face);

// Legacy 'kern' table pair value in pixels (0 if unavailable).
float GetKernTablePairPx(const Face* face, unsigned int cp_left, unsigned int cp_right,
                         float em_px);

// Extra pair spacing from GPOS via HarfBuzz at em_px (0 if unavailable).
float GetGposPairExtraPx(const Face* face, unsigned int cp_left, unsigned int cp_right,
                          float em_px);

// ---------------------------------------------------------------------------
// OpenType features  (GSUB/GPOS shaping: ligatures, small caps, stylistic sets…)
//
// Requires HarfBuzz at build time (IMVARFONT_USE_HARFBUZZ). Without HarfBuzz the
// settings are stored but have no effect on shaping. Substitution features (e.g.
// 'liga', 'smcp', 'onum', 'ss01') change which glyphs are produced, so shaping
// runs through HarfBuzz whenever any feature is set — even for fonts with no GPOS.
// ---------------------------------------------------------------------------

struct FeatureSetting {
    uint32_t Tag   = 0;           // 4-char OpenType feature tag, e.g. 'liga'
    uint32_t Value = 1;           // 0 = off, 1 = on, N = select alternate N
    uint32_t Start = 0;           // first cluster the setting applies to
    uint32_t End   = 0xFFFFFFFFu; // one-past-last cluster (global by default)
};

// Enable/select a feature by 4-char tag ("liga", "smcp", "ss01", "onum"…).
// value: 0 disables, 1 enables, N selects alternate N (for 'aalt'/'salt'/'cvNN').
void SetFeature(Face* face, const char* tag, uint32_t value = 1);
void SetFeatureRange(Face* face, const char* tag, uint32_t value,
                     uint32_t start, uint32_t end);

// Parse a comma/space separated list and replace the current feature set, e.g.
// "liga, ss01=1, onum, -kern, +smcp". Returns the number of features applied.
int  SetFeaturesString(Face* face, const char* features);

void ClearFeature(Face* face, const char* tag);
void ClearAllFeatures(Face* face);

int                   GetFeatureCount(const Face* face);
const FeatureSetting* GetFeatures(const Face* face);   // valid until FreeFace()
bool                  GetFeatureValue(const Face* face, const char* tag,
                                      uint32_t* out_value);

// ---------------------------------------------------------------------------
// Axis control
// ---------------------------------------------------------------------------

int   GetAxisCount(const Face* face);
Axis* GetAxes(Face* face);               // Pointer valid until FreeFace()

// Write v to axes[axis_idx].Value, clamping to [Min, Max] unless clamp=false.
// Pass clamp=false to allow extrapolation beyond the font's defined axis range.
void  SetAxisValue(Face* face, int axis_idx, float v, bool clamp = true);

// Reset all axes to their Default values and call ApplyAxes().
void  ResetAxes(Face* face);

// Push the current Axis::Value array to FreeType.
// When allow_extrapolation=true, values beyond fvar min/max are clamped for
// FreeType but a synthetic outline stretch is applied at render time.
void  ApplyAxes(Face* face, bool allow_extrapolation = false);

// ---------------------------------------------------------------------------
// Axis morphing: re-rasterization-free variable-font interpolation.
// When enabled (best paired with RenderMode::LoopBlinnLive), changing Axis::Value
// no longer re-instances FreeType: the glyph outline is blended each frame from
// a cached base outline plus per-axis main-effect deltas (O(axes), not 2^axes),
// so dragging an axis is as cheap as moving a quad. You set Axis::Value directly
// (no ApplyAxes needed) and the next DrawString reflects it. Scales to high-axis
// parametric fonts (e.g. Roboto Flex, 13 axes); inert axes are pruned per glyph.
// allow_extrapolation lets Value exceed fvar min/max via linear continuation of
// the outer master delta.
void  EnableMorph(Face* face, bool enable, bool allow_extrapolation = false);
bool  MorphEnabled(const Face* face);

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// Styling options for DrawString (all fields have sensible defaults so you can
// set only what you need).
//
//   C++17 (portable):
//     VarFont::TextStyle st;
//     st.outline = true; st.line_height_px = h;
//     VarFont::DrawString(face, em, pos, col, "Hello", st);
//
//   C++20 (designated initialisers):
//     VarFont::DrawString(face, em, pos, col, "Hello",
//                       { .outline = true, .line_height_px = h });
struct TextStyle {
    bool  fill              = true;   // fill glyph contours (GPU coverage / CPU FreeType)
    bool  outline           = false;  // stroke each contour (needs a PathEmitter)
    float outline_thickness = 1.5f;   // stroke width in pixels (used when outline=true)
    float line_height_px    = 0.f;    // vertical advance per newline; 0 = font default
    float letter_spacing_px = 0.f;    // extra gap after each glyph except the last; 0 = none
};

// Render UTF-8 text through the installed emitters. Returns the widest line's
// advance width in pixels.
//
//   em_px : em-square size in screen pixels (controls text size)
//   pos   : top-left corner of the text block; baseline = pos.y + ascender
//   col   : colour, e.g. VARFONT_COL32_WHITE
//
// Filled glyphs need a GlyphQuad emitter, outline strokes need a PathEmitter;
// whichever is missing is skipped.
//
// Short form — fill with col, no outline, font-default line height:
float DrawString(Face* face, float em_px, Vec2 pos, uint32_t col, const char* text);

// Full control via TextStyle (named fields; style is not defaulted to avoid
// ambiguity with the 5-arg form above):
float DrawString(Face* face, float em_px, Vec2 pos, uint32_t col, const char* text,
               const TextStyle& style);

// Measure total advance width without rendering (useful for centring).
float CalcTextWidth(Face* face, float em_px, const char* text,
                    float letter_spacing_px = 0.f);

// Returns ascender height in pixels above the baseline (positive value).
// Use with CalcDescenderPx to centre text properly in a canvas.
float CalcAscenderPx(const Face* face, float em_px);

// Returns descender depth in pixels below the baseline (positive value).
float CalcDescenderPx(const Face* face, float em_px);

// Returns line spacing in pixels (matches DrawString newline advance).
float CalcLineHeightPx(const Face* face, float em_px);

// Composite UTF-8 text to an RGBA8888 buffer (for Raster preview mode).
// Returns false when face/text is invalid.  out_w/out_h are set to pixel size.
bool RasterizeText(Face* face, float em_px, const char* text, uint32_t col,
                   float line_height_px, float letter_spacing_px,
                   std::vector<uint8_t>& out_rgba,
                   int& out_w, int& out_h);

// ---------------------------------------------------------------------------
// Layout export (for plotter / ofPath integrations)
// ---------------------------------------------------------------------------

struct PlacedGlyph {
    uint32_t glyph_index = 0;
    float    x           = 0.f;
    float    y           = 0.f;
};

// Lay out UTF-8 text and return pen positions in the same units as em_px.
void LayoutGlyphs(Face* face, const char* text, float em_px,
                  float line_height_px, float letter_spacing_px,
                  std::vector<PlacedGlyph>& out);

// Opaque access to the underlying FreeType face (cast to FT_Face in .cpp).
void* GetFtFace(Face* face);

// ---------------------------------------------------------------------------
// Tag helpers
// ---------------------------------------------------------------------------

static inline uint32_t MakeTag(char a, char b, char c, char d) {
    return ((uint32_t)(unsigned char)a << 24)
         | ((uint32_t)(unsigned char)b << 16)
         | ((uint32_t)(unsigned char)c <<  8)
         | ((uint32_t)(unsigned char)d);
}

// Fill out[5] with the 4-char tag string and a null terminator.
static inline void TagToStr(uint32_t tag, char out[5]) {
    out[0] = (char)((tag >> 24) & 0xFF);
    out[1] = (char)((tag >> 16) & 0xFF);
    out[2] = (char)((tag >>  8) & 0xFF);
    out[3] = (char)( tag        & 0xFF);
    out[4] = '\0';
}

} // namespace VarFont
