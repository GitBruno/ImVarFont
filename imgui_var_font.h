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
//   // Cleanup:
//   ImVarFont::FreeFace(face);
//
// Required: FreeType 2.x  (link with freetype)
// Optional: nothing else  (only imgui.h)
//
// License: MIT

#pragma once
#include "imgui.h"

namespace ImVarFont {

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
// Opaque face handle
// ---------------------------------------------------------------------------
struct Face;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

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

// Render UTF-8 text as stroked glyph outlines into dl.
//
//   em_px     : em-square size in screen pixels  (controls text size)
//   pos       : top-left corner of the text block in screen coordinates
//               (baseline sits at pos.y + ascender * scale)
//   col       : stroke colour, e.g. IM_COL32(255, 255, 255, 255)
//   thickness : stroke width in pixels
//
// Returns the total advance width of the rendered string in pixels.
float AddText(ImDrawList* dl, Face* face,
              float em_px, ImVec2 pos,
              ImU32 col, const char* text,
              float thickness = 1.5f);

// Measure total advance width without rendering (useful for centring).
float CalcTextWidth(Face* face, float em_px, const char* text);

// Returns ascender height in pixels above the baseline (positive value).
// Use with CalcDescenderPx to centre text properly in a canvas.
float CalcAscenderPx(const Face* face, float em_px);

// Returns descender depth in pixels below the baseline (positive value).
float CalcDescenderPx(const Face* face, float em_px);

// Returns line spacing in pixels (matches AddText newline advance).
float CalcLineHeightPx(const Face* face, float em_px);

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
