// imgui_var_font.h  –  Variable-font rendering and axis controls for Dear ImGui
//
// Renders glyph outlines directly into an ImDrawList using FreeType for
// outline extraction.  Variable-font design axes are exposed as ImGui slider
// widgets; any font that FreeType can open (OTF/TTF, static or variable) works.
//
// Usage (minimal):
//   ImVarFont::Face* face = ImVarFont::LoadFace("MyFont.ttf");
//   // In your frame loop:
//   if (ImVarFont::AxisSliders(face))
//       ImVarFont::ApplyAxes(face);
//   ImVarFont::AddText(ImGui::GetWindowDrawList(), face,
//                      96.f, ImVec2(x, y), IM_COL32_WHITE, "Hello");
//
// Usage (with styling):
//   ImVarFont::TextStyle st;
//   st.outline           = true;
//   st.outline_thickness = 2.f;
//   st.letter_spacing_px = 1.f;
//   ImVarFont::AddText(ImGui::GetWindowDrawList(), face,
//                      96.f, ImVec2(x, y), IM_COL32_WHITE, "Hello", st);
//
//   // Cleanup:
//   ImVarFont::FreeFace(face);
//
// Required: FreeType 2.x  (link with freetype)
// Optional: nothing else  (only imgui.h)
//
// License: MIT

#pragma once
#include "imgui.h"
#include <cstdint>
#include <vector>

namespace ImVarFont {

// ---------------------------------------------------------------------------
// Render quality
// ---------------------------------------------------------------------------

enum class RenderMode {
    Vector,        // Unhinted design-unit outlines (best for zoom / extrapolation)
    HintedVector,  // Hinted outlines at em_px, drawn as ImDrawList paths
    Raster,        // FreeType grayscale bitmap composite (best static quality)
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
    ImU32  Tag;             // 4-byte OpenType tag, e.g. 0x77676874 = 'wght'
    char   Name[64];        // Human-readable name from the font's 'fvar' table
    float  Min;
    float  Max;
    float  Default;
    float  Value;           // Current value – modify then call ApplyAxes()
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

// Scrollable inspector for non-zero kern pairs (ASCII subset).
void KernTableUi(const Face* face, float em_px);

// ---------------------------------------------------------------------------
// OpenType features  (GSUB/GPOS shaping: ligatures, small caps, stylistic sets…)
//
// Requires HarfBuzz at build time (IMVARFONT_USE_HARFBUZZ). Without HarfBuzz the
// settings are stored but have no effect on shaping. Substitution features (e.g.
// 'liga', 'smcp', 'onum', 'ss01') change which glyphs are produced, so shaping
// runs through HarfBuzz whenever any feature is set — even for fonts with no GPOS.
// ---------------------------------------------------------------------------

struct FeatureSetting {
    ImU32    Tag   = 0;            // 4-char OpenType feature tag, e.g. 'liga'
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
// ImGui widgets
// ---------------------------------------------------------------------------

// Draw one slider per axis (immediate-mode: call every frame). Returns true only
// on the frame a slider changes, and calls ApplyAxes() for you on change — you
// do not need a separate ApplyAxes() call when driving axes through this widget.
// When allow_extrapolation=true each axis shows an unclamped DragFloat
// (infinite drag + click to type). Values beyond the font's fvar range are
// clamped for FreeType but extrapolated visually in AddText().
bool AxisSliders(Face* face, const char* str_id = "##imvarfont_axes",
                 bool allow_extrapolation = false);

// Family/style name, variable flag, axis min/max/default table, current values.
void MetadataTable(const Face* face);

// Load face into atlas using FreeType, applying current axis values.
// Caller should ClearFonts() first. Returns nullptr on failure.
// Requires IMGUI_ENABLE_FREETYPE and imgui_freetype.cpp linked into the app.
ImFont* SetImGuiFont(ImFontAtlas* atlas, Face* face, float size_pixels);

// ---------------------------------------------------------------------------
// Rendering  (outlines → ImDrawList, no texture atlas required)
// ---------------------------------------------------------------------------

// Styling options for AddText (all fields have sensible defaults so you can
// set only what you need).
//
//   C++17 (portable):
//     ImVarFont::TextStyle st;
//     st.outline = true; st.line_height_px = h;
//     ImVarFont::AddText(dl, face, em, pos, col, "Hello", st);
//
//   C++20 (designated initialisers):
//     ImVarFont::AddText(dl, face, em, pos, col, "Hello",
//                        { .outline = true, .line_height_px = h });
struct TextStyle {
    bool  fill              = true;   // fill glyph contours (GPU coverage / CPU FreeType)
    bool  outline           = false;  // stroke each contour (combinable with fill)
    float outline_thickness = 1.5f;   // stroke width in pixels (used when outline=true)
    float line_height_px    = 0.f;    // vertical advance per newline; 0 = font default
    float letter_spacing_px = 0.f;    // extra gap after each glyph except the last; 0 = none
};

// Render UTF-8 text into dl.
//
//   em_px : em-square size in screen pixels (controls text size)
//   pos   : top-left corner of the text block; baseline = pos.y + ascender
//   col   : colour, e.g. IM_COL32_WHITE
//
// Short form — fill with col, no outline, font-default line height:
float AddText(ImDrawList* dl, Face* face,
              float em_px, ImVec2 pos,
              ImU32 col, const char* text);

// Full control via TextStyle (named fields; style is not defaulted to avoid
// ambiguity with the 6-arg form above):
float AddText(ImDrawList* dl, Face* face,
              float em_px, ImVec2 pos,
              ImU32 col, const char* text,
              const TextStyle& style);

// Measure total advance width without rendering (useful for centring).
float CalcTextWidth(Face* face, float em_px, const char* text,
                    float letter_spacing_px = 0.f);

// Returns ascender height in pixels above the baseline (positive value).
// Use with CalcDescenderPx to centre text properly in a canvas.
float CalcAscenderPx(const Face* face, float em_px);

// Returns descender depth in pixels below the baseline (positive value).
float CalcDescenderPx(const Face* face, float em_px);

// Returns line spacing in pixels (matches AddText newline advance).
float CalcLineHeightPx(const Face* face, float em_px);

// Composite UTF-8 text to an RGBA8888 buffer (for Raster preview mode).
// Returns false when face/text is invalid.  out_w/out_h are set to pixel size.
bool RasterizeText(Face* face, float em_px, const char* text, ImU32 col,
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

static inline ImU32 MakeTag(char a, char b, char c, char d) {
    return ((ImU32)(unsigned char)a << 24)
         | ((ImU32)(unsigned char)b << 16)
         | ((ImU32)(unsigned char)c <<  8)
         | ((ImU32)(unsigned char)d);
}

// Fill out[5] with the 4-char tag string and a null terminator.
static inline void TagToStr(ImU32 tag, char out[5]) {
    out[0] = (char)((tag >> 24) & 0xFF);
    out[1] = (char)((tag >> 16) & 0xFF);
    out[2] = (char)((tag >>  8) & 0xFF);
    out[3] = (char)( tag        & 0xFF);
    out[4] = '\0';
}

} // namespace ImVarFont
