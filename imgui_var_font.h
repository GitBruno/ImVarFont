// imgui_var_font.h — Dear ImGui adapter for the VarFont engine
//
// Thin host layer: AxisSliders / MetadataTable / KernTableUi / SetImGuiFont and
// AddText(ImDrawList*, …). Engine API lives in varfont_core.h (namespace VarFont).
//
// Usage (minimal):
//   ImVarFont::Face* face = ImVarFont::LoadFace("MyFont.ttf");
//   if (ImVarFont::AxisSliders(face)) { /* axes applied */ }
//   ImVarFont::AddText(ImGui::GetWindowDrawList(), face,
//                      96.f, ImVec2(x, y), IM_COL32_WHITE, "Hello");
//   ImVarFont::FreeFace(face);
//
// Required: FreeType 2.x, Dear ImGui (imgui.h)
// License: MIT  (WTFPL if your name is Omar Cornut — thanks for Dear ImGui)

#pragma once
#include "varfont_core.h"
#include "imgui.h"

namespace ImVarFont {

using VarFont::Vec2;
using VarFont::Color;
using VarFont::RenderMode;
using VarFont::HintingFlags;
using VarFont::Face;
using VarFont::GlyphQuad;
using VarFont::EmitGlyphQuadFn;
using VarFont::PathEmitter;
using VarFont::GridFitMode;
using VarFont::RenderProfile;
using VarFont::Axis;
using VarFont::FeatureSetting;
using VarFont::TextStyle;
using VarFont::PlacedGlyph;

using VarFont::InitRenderer;
using VarFont::ShutdownRenderer;
using VarFont::RendererReady;
using VarFont::ForceCpuFallback;
using VarFont::BeginHostFrame;
using VarFont::SetGlyphQuadEmitter;
using VarFont::GetGlyphQuadEmitter;
using VarFont::SetPathEmitter;
using VarFont::GetPathEmitter;
using VarFont::PreferSlugRenderer;
using VarFont::PreferGpuMorphRenderer;
using VarFont::GpuMorphAvailable;
using VarFont::SlugRendererAvailable;
using VarFont::PreferGridFit;
using VarFont::PreferGridFitMode;
using VarFont::PreferGridFitMaxPx;
using VarFont::GetRenderProfile;
using VarFont::GetRenderMode;
using VarFont::SetRenderMode;
using VarFont::GetHintingFlags;
using VarFont::SetHintingFlags;
using VarFont::GetRenderModeLabel;
using VarFont::GetHintingFlagsLabel;
using VarFont::LoadFace;
using VarFont::FreeFace;
using VarFont::IsLoaded;
using VarFont::IsVariable;
using VarFont::GetFamilyName;
using VarFont::GetStyleName;
using VarFont::GetFilePath;
using VarFont::HasKerning;
using VarFont::HasKernTable;
using VarFont::HasGpos;
using VarFont::UsesHarfBuzz;
using VarFont::GetUseKerning;
using VarFont::SetUseKerning;
using VarFont::GetUseHarfBuzz;
using VarFont::SetUseHarfBuzz;
using VarFont::GetUseKernTable;
using VarFont::SetUseKernTable;
using VarFont::GetKerningEngineLabel;
using VarFont::GetKernTablePairPx;
using VarFont::GetGposPairExtraPx;
using VarFont::SetFeature;
using VarFont::SetFeatureRange;
using VarFont::SetFeaturesString;
using VarFont::ClearFeature;
using VarFont::ClearAllFeatures;
using VarFont::GetFeatureCount;
using VarFont::GetFeatures;
using VarFont::GetFeatureValue;
using VarFont::GetAxisCount;
using VarFont::GetAxes;
using VarFont::SetAxisValue;
using VarFont::ResetAxes;
using VarFont::ApplyAxes;
using VarFont::EnableMorph;
using VarFont::MorphEnabled;
using VarFont::DrawString;
using VarFont::CalcTextWidth;
using VarFont::CalcAscenderPx;
using VarFont::CalcDescenderPx;
using VarFont::CalcLineHeightPx;
using VarFont::RasterizeText;
using VarFont::LayoutGlyphs;
using VarFont::GetFtFace;
using VarFont::MakeTag;
using VarFont::TagToStr;

bool AxisSliders(Face* face, const char* str_id = "##imvarfont_axes",
                 bool allow_extrapolation = false);
void MetadataTable(const Face* face);
void KernTableUi(const Face* face, float em_px);
ImFont* SetImGuiFont(ImFontAtlas* atlas, Face* face, float size_pixels);

float AddText(ImDrawList* dl, Face* face,
              float em_px, ImVec2 pos,
              ImU32 col, const char* text);
float AddText(ImDrawList* dl, Face* face,
              float em_px, ImVec2 pos,
              ImU32 col, const char* text,
              const TextStyle& style);

} // namespace ImVarFont
