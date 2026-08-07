// imgui_var_font.cpp — Dear ImGui adapter for the VarFont engine.
// See imgui_var_font.h. Everything here is host glue: axis / metadata / kerning
// widgets, the ImGui font atlas hook, and AddText(), which binds an ImDrawList
// as the engine's glyph-quad and outline-path sink for the duration of a call.

#include "imgui_var_font.h"
#include "imgui_internal.h"        // ImMax
#include "varfont_core_detail.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <freetype/ftmm.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef IMGUI_ENABLE_FREETYPE
#include "misc/freetype/imgui_freetype.h"
#endif

namespace ImVarFont {

using VarFont::detail::resolveVMetrics;
using VarFont::detail::valueToFixed;

// ============================================================================
// Widgets
// ============================================================================

static void metaField(const char* label, const std::string& value) {
    if (value.empty())
        return;
    ImGui::TextDisabled("%s", label);
    ImGui::TextWrapped("%s", value.c_str());
    ImGui::Spacing();
}

bool AxisSliders(Face* face, const char* str_id, bool allow_extrapolation) {
    if (!face || face->axes.empty()) {
        ImGui::TextDisabled("No axes");
        return false;
    }
    bool changed = false;
    ImGui::PushID(str_id);
    for (int i = 0; i < (int)face->axes.size(); ++i) {
        auto& ax = face->axes[i];
        char tag[5];
        TagToStr(ax.Tag, tag);

        ImGui::PushID(i);
        if (ax.Name[0] != '\0')
            ImGui::TextUnformatted(ax.Name);
        else
            ImGui::TextUnformatted(tag);
        if (ax.Name[0] != '\0')
            ImGui::TextDisabled("[%s]", tag);

        ImGui::SetNextItemWidth(-1.f);
        if (allow_extrapolation) {
            const float range = ImMax(ax.Max - ax.Min, 1.f);
            const float speed = ImMax(range * 0.01f, 0.05f);
            if (ImGui::DragFloat("##val", &ax.Value, speed, 0.f, 0.f, "%.2f"))
                changed = true;
            if (face->axisExtrap.size() > (size_t)i && face->axisExtrap[i] != 0.f)
                ImGui::TextDisabled("fvar %.1f–%.1f  ·  extrap %.2f×",
                                    ax.Min, ax.Max, 1.f + face->axisExtrap[i] * 0.35f);
            else
                ImGui::TextDisabled("fvar %.1f – %.1f  (def %.1f)", ax.Min, ax.Max, ax.Default);
        } else {
            if (ImGui::SliderFloat("##val", &ax.Value, ax.Min, ax.Max, "%.1f"))
                changed = true;
        }
        ImGui::Spacing();
        ImGui::PopID();
    }
    ImGui::Spacing();
    if (ImGui::Button("Reset axes", ImVec2(-1, 0))) {
        ResetAxes(face);
        changed = true;
    }
    if (changed && !face->morphEnabled)
        ApplyAxes(face, allow_extrapolation);   // morph reads Axis::Value directly
    ImGui::PopID();
    return changed;
}

void MetadataTable(const Face* face) {
    if (!face || !face->ftFace) {
        ImGui::TextDisabled("No font loaded");
        return;
    }

    const FT_Face ft = face->ftFace;
    const auto&   m  = face->metadata;
    resolveVMetrics(const_cast<Face*>(face));   // stable, instance-independent metrics

    ImGui::TextDisabled("Identity");
    ImGui::Spacing();
    ImGui::Text("Family : %s", GetFamilyName(face));
    ImGui::Text("Style  : %s", GetStyleName(face));
    if (!m.fullName.empty() && m.fullName != face->familyName)
        ImGui::Text("Full   : %s", m.fullName.c_str());
    if (!m.postScriptName.empty())
        ImGui::Text("PS     : %s", m.postScriptName.c_str());
    if (!m.version.empty())
        ImGui::Text("Version: %s", m.version.c_str());
    ImGui::Text("Type   : %s", IsVariable(face) ? "Variable" : "Static");
    ImGui::Text("Axes   : %d", GetAxisCount(face));
    ImGui::Text("Glyphs : %ld", (long)ft->num_glyphs);
    ImGui::Text("Kerning: %s", GetUseKerning(face) ? "on" : "off");
    ImGui::Text("Engine : %s", GetKerningEngineLabel(face));

    ImGui::Separator();
    ImGui::TextDisabled("Metrics");
    ImGui::Spacing();
    ImGui::Text("UPM    : %d", ft->units_per_EM);
    ImGui::Text("Asc    : %d", face->metricAsc);
    ImGui::Text("Desc   : %d", face->metricDesc);
    ImGui::Text("Height : %d", face->metricHeight);
    if (face->metricBbox[0] || face->metricBbox[1] ||
        face->metricBbox[2] || face->metricBbox[3]) {
        ImGui::Text("BBox   : %ld %ld  %ld %ld",
                    face->metricBbox[0], face->metricBbox[1],
                    face->metricBbox[2], face->metricBbox[3]);
    }

    if (!m.copyright.empty() || !m.designer.empty() || !m.manufacturer.empty() ||
        !m.trademark.empty() || !m.description.empty() || !m.license.empty() ||
        !m.licenseUrl.empty() || !m.designerUrl.empty() || !m.vendorUrl.empty() ||
        !m.uniqueId.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Name table");
        ImGui::Spacing();
        metaField("Copyright",    m.copyright);
        metaField("Designer",     m.designer);
        metaField("Manufacturer", m.manufacturer);
        metaField("Trademark",    m.trademark);
        metaField("Description",  m.description);
        metaField("License",      m.license);
        metaField("License URL",  m.licenseUrl);
        metaField("Designer URL", m.designerUrl);
        metaField("Vendor URL",   m.vendorUrl);
        metaField("Unique ID",    m.uniqueId);
    }

    if (!face->filePath.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Path");
        ImGui::TextWrapped("%s", face->filePath.c_str());
    }

    if (!face->axes.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Axis details");
        ImGui::Spacing();
        if (ImGui::BeginTable("##axes_meta", 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Tag",  ImGuiTableColumnFlags_WidthFixed,   42.f);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Min",  ImGuiTableColumnFlags_WidthFixed,   58.f);
            ImGui::TableSetupColumn("Max",  ImGuiTableColumnFlags_WidthFixed,   58.f);
            ImGui::TableSetupColumn("Def",  ImGuiTableColumnFlags_WidthFixed,   58.f);
            ImGui::TableHeadersRow();

            for (const auto& ax : face->axes) {
                char tag[5]; TagToStr(ax.Tag, tag);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(tag);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ax.Name);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", ax.Min);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", ax.Max);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", ax.Default);
            }
            ImGui::EndTable();
        }

        // Live values
        ImGui::Spacing();
        ImGui::TextDisabled("Current values");
        ImGui::Spacing();
        if (ImGui::BeginTable("##axes_cur", 2,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Axis",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 72.f);
            ImGui::TableHeadersRow();
            for (const auto& ax : face->axes) {
                char tag[5]; TagToStr(ax.Tag, tag);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ax.Name[0] != '\0')
                    ImGui::Text("%s (%s)", ax.Name, tag);
                else
                    ImGui::TextUnformatted(tag);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", ax.Value);
            }
            ImGui::EndTable();
        }
    }
}

void KernTableUi(const Face* face, float em_px) {
    if (!face || !face->ftFace) {
        ImGui::TextDisabled("No font loaded");
        return;
    }

    static char filter[64] = "";
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##kernfilter", "Filter pairs (e.g. AV, To)", filter, sizeof(filter));

    ImGui::TextDisabled("Engine: %s  ·  em %.0f px", GetKerningEngineLabel(face), em_px);
    if (!HasKerning(face)) {
        ImGui::TextDisabled("This font has no kern table or GPOS data.");
        return;
    }

    const bool show_kern = face->hasKerningTable;
    const bool show_gpos = face->hasGpos && UsesHarfBuzz(face);
    int cols = 2 + (show_kern ? 1 : 0) + (show_gpos ? 1 : 0);

    if (ImGui::BeginTable("##kerntable", cols,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                              | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                          ImVec2(0.f, 0.f))) {
        ImGui::TableSetupColumn("Left",  ImGuiTableColumnFlags_WidthFixed, 36.f);
        ImGui::TableSetupColumn("Right", ImGuiTableColumnFlags_WidthFixed, 36.f);
        if (show_kern)
            ImGui::TableSetupColumn("kern (px)", ImGuiTableColumnFlags_WidthStretch);
        if (show_gpos)
            ImGui::TableSetupColumn("GPOS Δ (px)", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        auto matchesFilter = [&](unsigned l, unsigned r) -> bool {
            if (filter[0] == '\0') return true;
            char pair[3] = {
                (l < 128) ? (char)l : '?',
                (r < 128) ? (char)r : '?',
                '\0'
            };
            return std::strstr(pair, filter) != nullptr;
        };

        int rows = 0;
        for (unsigned l = 32; l < 127; ++l) {
            for (unsigned r = 32; r < 127; ++r) {
                if (!matchesFilter(l, r)) continue;

                const float kern = show_kern ? GetKernTablePairPx(face, l, r, em_px) : 0.f;
                const float gpos = show_gpos ? GetGposPairExtraPx(face, l, r, em_px) : 0.f;
                if (std::fabs(kern) < 0.005f && std::fabs(gpos) < 0.005f)
                    continue;

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%c", (char)l);
                ImGui::TableNextColumn();
                ImGui::Text("%c", (char)r);
                if (show_kern) {
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", kern);
                }
                if (show_gpos) {
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", gpos);
                }
                ++rows;
            }
        }

        if (rows == 0) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("No non-zero pairs in ASCII range");
        }

        ImGui::EndTable();
    }
}

// ============================================================================
// Rendering  (engine emitters bound to an ImDrawList)
// ============================================================================

static void emitGlyphQuad(const VarFont::GlyphQuad& q, void* user) {
    ImDrawList* dl = static_cast<ImDrawList*>(user);
    dl->AddImage((ImTextureID)(intptr_t)q.tex,
                 ImVec2(q.x0, q.y0), ImVec2(q.x1, q.y1),
                 ImVec2(q.u0, q.v0), ImVec2(q.u1, q.v1),
                 (ImU32)q.col);
}

static void pathBegin(void* user) {
    static_cast<ImDrawList*>(user)->PathClear();
}
static void pathLineTo(float x, float y, void* user) {
    static_cast<ImDrawList*>(user)->PathLineTo(ImVec2(x, y));
}
static void pathCubicTo(float x1, float y1, float x2, float y2,
                        float x3, float y3, void* user) {
    static_cast<ImDrawList*>(user)->PathBezierCubicCurveTo(
        ImVec2(x1, y1), ImVec2(x2, y2), ImVec2(x3, y3));
}
static void pathStroke(uint32_t col, float thickness, bool closed, void* user) {
    static_cast<ImDrawList*>(user)->PathStroke(
        (ImU32)col, closed ? ImDrawFlags_Closed : ImDrawFlags_None, thickness);
}

// Binds dl as the engine's sink and restores whatever the app had installed.
// The engine has no ImGui dependency, so the frame index and HiDPI ratio it
// needs for atlas recycling are pushed from here.
struct ScopedDrawListHost {
    VarFont::EmitGlyphQuadFn   prevQuadFn   = nullptr;
    void*                      prevQuadUser = nullptr;
    const VarFont::PathEmitter* prevPath    = nullptr;
    VarFont::PathEmitter        prevPathCopy{};
    bool                        bound       = false;

    explicit ScopedDrawListHost(ImDrawList* dl) {
        if (!dl)
            return;
        prevQuadFn = VarFont::GetGlyphQuadEmitter(&prevQuadUser);
        prevPath   = VarFont::GetPathEmitter();
        if (prevPath)
            prevPathCopy = *prevPath;
        bound = true;

        if (ImGui::GetCurrentContext()) {
            const ImVec2 fb = ImGui::GetIO().DisplayFramebufferScale;
            VarFont::BeginHostFrame(ImGui::GetFrameCount(),
                                    (fb.y > 0.f) ? fb.y : 1.f);
        }

        VarFont::PathEmitter pe;
        pe.begin    = pathBegin;
        pe.line_to  = pathLineTo;
        pe.cubic_to = pathCubicTo;
        pe.stroke   = pathStroke;
        pe.user     = dl;
        VarFont::SetGlyphQuadEmitter(emitGlyphQuad, dl);
        VarFont::SetPathEmitter(&pe);
    }

    ~ScopedDrawListHost() {
        if (!bound)
            return;
        VarFont::SetGlyphQuadEmitter(prevQuadFn, prevQuadUser);
        VarFont::SetPathEmitter(prevPath ? &prevPathCopy : nullptr);
    }
};

float AddText(ImDrawList* dl, Face* face,
              float em_px, ImVec2 pos,
              ImU32 col, const char* text)
{
    ScopedDrawListHost host(dl);
    return VarFont::DrawString(face, em_px, VarFont::Vec2(pos.x, pos.y),
                             (uint32_t)col, text);
}

float AddText(ImDrawList* dl, Face* face,
              float em_px, ImVec2 pos,
              ImU32 col, const char* text,
              const TextStyle& style)
{
    ScopedDrawListHost host(dl);
    return VarFont::DrawString(face, em_px, VarFont::Vec2(pos.x, pos.y),
                             (uint32_t)col, text, style);
}

// ============================================================================
// ImGui font atlas  (requires IMGUI_ENABLE_FREETYPE)
// ============================================================================

#ifdef IMGUI_ENABLE_FREETYPE

namespace {

struct FreeTypeFaceAccess {
    FT_Face FtFace;
};

static Face*        s_axis_face    = nullptr;
static ImFontLoader s_var_loader   = {};
static bool         s_loader_ready = false;

static bool VarFontSrcInit(ImFontAtlas* atlas, ImFontConfig* src) {
    const ImFontLoader* base = ImGuiFreeType::GetFontLoader();
    if (!base || !base->FontSrcInit || !base->FontSrcInit(atlas, src))
        return false;

    if (!s_axis_face || s_axis_face->axes.empty() || !src->FontLoaderData)
        return true;

    FT_Face ft = static_cast<FreeTypeFaceAccess*>(src->FontLoaderData)->FtFace;
    if (!ft)
        return true;

    std::vector<FT_Fixed> coords(s_axis_face->axes.size());
    for (size_t i = 0; i < s_axis_face->axes.size(); ++i) {
        const auto& ax = s_axis_face->axes[i];
        const float applied = std::clamp(ax.Value, ax.Min, ax.Max);
        coords[i] = valueToFixed(applied);
    }

    FT_Set_Var_Design_Coordinates(ft, (FT_UInt)coords.size(), coords.data());
    return true;
}

static void ensureVarLoader() {
    if (s_loader_ready)
        return;
    const ImFontLoader* base = ImGuiFreeType::GetFontLoader();
    if (!base)
        return;
    s_var_loader = *base;
    s_var_loader.FontSrcInit = VarFontSrcInit;
    s_loader_ready = true;
}

} // namespace

ImFont* SetImGuiFont(ImFontAtlas* atlas, Face* face, float size_pixels) {
    if (!atlas || !face || !face->ftFace || face->filePath.empty() || size_pixels <= 0.f)
        return nullptr;

    ensureVarLoader();
    if (!s_loader_ready)
        return nullptr;

    atlas->SetFontLoader(&s_var_loader);
    s_axis_face = face;

    ImFontConfig cfg;
    snprintf(cfg.Name, sizeof(cfg.Name), "%s UI", face->familyName.c_str());

    ImFont* font = atlas->AddFontFromFileTTF(
        face->filePath.c_str(), size_pixels, &cfg);

    s_axis_face = nullptr;
    return font;
}

#endif // IMGUI_ENABLE_FREETYPE

} // namespace ImVarFont
