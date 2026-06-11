// imgui_var_font.cpp  –  Implementation of ImVarFont
// See imgui_var_font.h for API documentation.

#include "imgui_var_font.h"
#include "imgui_internal.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <freetype/ftmm.h>       // FT_MM_Var, FT_Get_MM_Var, FT_Set_Var_Design_Coordinates
#include <freetype/ftsnames.h>   // FT_Get_Sfnt_Name, FT_SfntName
#include <freetype/ttnameid.h>   // TT_NAME_ID_*, TT_PLATFORM_*
#include <freetype/tttables.h>   // FT_IS_SFNT, FT_Get_Sfnt_Table
#include <freetype/fttypes.h>    // FT_MAKE_TAG

#include <freetype/fterrors.h>  // FT_Error_String (FreeType >= 2.10)

#include <vector>
#include <string>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <cmath>

#ifdef IMVARFONT_USE_HARFBUZZ
#include <hb.h>
#include <hb-ft.h>
#include <hb-ot.h>
#endif

#ifdef IMGUI_ENABLE_FREETYPE
#include "misc/freetype/imgui_freetype.h"
#endif

namespace ImVarFont {

// ============================================================================
// Internal Face definition
// ============================================================================

struct FontMetadata {
    std::string copyright;
    std::string designer;
    std::string manufacturer;
    std::string fullName;
    std::string version;
    std::string trademark;
    std::string description;
    std::string license;
    std::string licenseUrl;
    std::string designerUrl;
    std::string vendorUrl;
    std::string postScriptName;
    std::string uniqueId;
};

struct Face {
    FT_Library         library    = nullptr;
    FT_Face            ftFace     = nullptr;
    std::string        filePath;
    std::string        familyName;
    std::string        styleName;
    bool               isVariable = false;
    std::vector<Axis>  axes;
    std::vector<float> axisExtrap;  // synthetic extrap factor per axis (0 = none)
    FontMetadata       metadata;
    bool               hasKerningTable = false;
    bool               hasGpos         = false;
    bool               useKerning      = true;
    bool               useHarfBuzz     = true;
    bool               useKernTable    = true;
    RenderMode         renderMode      = RenderMode::Vector;
    HintingFlags       hintingFlags    = HintingFlags::Native;
    float              syncedEmPx      = -1.f;
#ifdef IMVARFONT_USE_HARFBUZZ
    hb_font_t*         hbFont          = nullptr;
    hb_buffer_t*       hbBuf           = nullptr;
#endif
};

// ============================================================================
// SFNT 'name' table helpers
// ============================================================================

static void appendUtf8(std::string& out, unsigned int cp) {
    if (cp <= 0x7F) {
        out += (char)cp;
    } else if (cp <= 0x7FF) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF) {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

static std::string decodeSfntString(const FT_SfntName& name) {
    if (!name.string || name.string_len == 0)
        return {};

    // Windows / Unicode BMP string (UTF-16BE code units)
    if (name.platform_id == TT_PLATFORM_MICROSOFT &&
        (name.encoding_id == TT_MS_ID_UNICODE_CS ||
         name.encoding_id == TT_MS_ID_SYMBOL_CS)) {
        std::string out;
        out.reserve(name.string_len);
        for (FT_UInt i = 0; i + 1 < name.string_len; i += 2) {
            const FT_UShort ch = (FT_UShort)((name.string[i] << 8) | name.string[i + 1]);
            appendUtf8(out, ch);
        }
        return out;
    }

    // Mac Roman, Windows Latin-1, and other byte-string encodings
    return std::string((const char*)name.string, name.string_len);
}

static int sfntNameEntryScore(const FT_SfntName& entry) {
    int score = 0;

    if (entry.platform_id == TT_PLATFORM_MICROSOFT) {
        score += 40;
        if (entry.encoding_id == TT_MS_ID_UNICODE_CS)
            score += 30;
        if (entry.language_id == 0x0409)      // en-US
            score += 30;
        else if ((entry.language_id & 0xFF) == 0x09)  // any English
            score += 20;
    } else if (entry.platform_id == TT_PLATFORM_MACINTOSH) {
        score += 20;
        if (entry.language_id == 0)           // English
            score += 20;
    } else {
        score += 5;
    }

    return score;
}

static std::string getSfntNameString(FT_Face face, FT_UShort name_id) {
    if (!face || !FT_IS_SFNT(face))
        return {};

    const FT_UInt count = FT_Get_Sfnt_Name_Count(face);
    if (count == 0)
        return {};

    std::string best;
    int bestScore = -1;

    for (FT_UInt i = 0; i < count; ++i) {
        FT_SfntName entry;
        if (FT_Get_Sfnt_Name(face, i, &entry) != 0)
            continue;
        if (entry.name_id != name_id)
            continue;

        const int score = sfntNameEntryScore(entry);
        if (score > bestScore) {
            bestScore = score;
            best = decodeSfntString(entry);
        }
    }

    return best;
}

static void loadMetadata(Face* f) {
    if (!f || !f->ftFace)
        return;

    FT_Face face = f->ftFace;
    auto&   m    = f->metadata;

    m.copyright      = getSfntNameString(face, TT_NAME_ID_COPYRIGHT);
    m.designer       = getSfntNameString(face, TT_NAME_ID_DESIGNER);
    m.manufacturer   = getSfntNameString(face, TT_NAME_ID_MANUFACTURER);
    m.fullName       = getSfntNameString(face, TT_NAME_ID_FULL_NAME);
    m.version        = getSfntNameString(face, TT_NAME_ID_VERSION_STRING);
    m.trademark      = getSfntNameString(face, TT_NAME_ID_TRADEMARK);
    m.description    = getSfntNameString(face, TT_NAME_ID_DESCRIPTION);
    m.license        = getSfntNameString(face, TT_NAME_ID_LICENSE);
    m.licenseUrl     = getSfntNameString(face, TT_NAME_ID_LICENSE_URL);
    m.designerUrl    = getSfntNameString(face, TT_NAME_ID_DESIGNER_URL);
    m.vendorUrl      = getSfntNameString(face, TT_NAME_ID_VENDOR_URL);
    m.postScriptName = getSfntNameString(face, TT_NAME_ID_PS_NAME);
    m.uniqueId       = getSfntNameString(face, TT_NAME_ID_UNIQUE_ID);
}

static void metaField(const char* label, const std::string& value) {
    if (value.empty())
        return;
    ImGui::TextDisabled("%s", label);
    ImGui::TextWrapped("%s", value.c_str());
    ImGui::Spacing();
}

// ============================================================================
// Lifecycle
// ============================================================================

Face* LoadFace(const char* path, char* err_buf, int err_buf_size) {
    // Helper: write a formatted error string to the caller's buffer (if supplied)
    auto writeErr = [&](const char* fmt, ...) {
        if (!err_buf || err_buf_size <= 0) return;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err_buf, (size_t)err_buf_size, fmt, ap);
        va_end(ap);
    };

    if (!path || !*path) { writeErr("Empty path"); return nullptr; }

    // Check the file exists before calling FreeType (clearer error message)
    if (FILE* fp = fopen(path, "rb")) {
        fclose(fp);
    } else {
        writeErr("File not found:\n%s", path);
        return nullptr;
    }

    auto* f = new Face();

    if (FT_Init_FreeType(&f->library) != 0) {
        writeErr("FT_Init_FreeType failed");
        delete f;
        return nullptr;
    }

    FT_Error ft_err = FT_New_Face(f->library, path, 0, &f->ftFace);
    if (ft_err != 0) {
        const char* ft_msg = FT_Error_String(ft_err);
        writeErr("FreeType: %s (0x%02x)\n%s",
                 ft_msg ? ft_msg : "unknown error", ft_err, path);
        FT_Done_FreeType(f->library);
        delete f;
        return nullptr;
    }

    f->filePath   = path;
    f->familyName = f->ftFace->family_name ? f->ftFace->family_name : "";
    f->styleName  = f->ftFace->style_name  ? f->ftFace->style_name  : "";

    // Query variable-font axes via the fvar table
    FT_MM_Var* mmVar = nullptr;
    if (FT_Get_MM_Var(f->ftFace, &mmVar) == 0 && mmVar) {
        f->isVariable = true;
        f->axes.resize(mmVar->num_axis);
        for (FT_UInt i = 0; i < mmVar->num_axis; ++i) {
            const auto& src = mmVar->axis[i];
            auto&       dst = f->axes[i];

            dst.Tag     = (ImU32)src.tag;
            const char* n = src.name ? src.name : "";
            strncpy(dst.Name, n, sizeof(dst.Name) - 1);
            dst.Name[sizeof(dst.Name) - 1] = '\0';

            // FT_Fixed is 16.16 fixed-point; divide by 65536 to get float coords
            dst.Min     = (float)src.minimum / 65536.f;
            dst.Max     = (float)src.maximum / 65536.f;
            dst.Default = (float)src.def     / 65536.f;
            dst.Value   = dst.Default;
        }
        FT_Done_MM_Var(f->library, mmVar);
    }

    f->hasKerningTable = (f->ftFace->face_flags & FT_FACE_FLAG_KERNING) != 0;
    f->useKerning      = true;
    f->useHarfBuzz     = true;
    f->useKernTable    = true;

#ifdef IMVARFONT_USE_HARFBUZZ
    // HarfBuzz requires FT_Face->size to be set before hb_ft_font_create_referenced.
    FT_Set_Char_Size(f->ftFace, 0, 64 * 512, 72, 72);
    f->hbFont = hb_ft_font_create_referenced(f->ftFace);
    f->hbBuf  = hb_buffer_create();
    hb_ft_font_set_load_flags(f->hbFont, FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING);
    if (hb_font_t* hb_font = f->hbFont) {
        if (hb_face_t* hb_face = hb_font_get_face(hb_font))
            f->hasGpos = hb_ot_layout_has_positioning(hb_face);
    }
#endif

    if (!f->hasKerningTable && !f->hasGpos) {
        fprintf(stderr,
                "ImVarFont warning: \"%s\" has no kern table and no GPOS data — "
                "pair kerning will have no effect.\n",
                path);
    }

    loadMetadata(f);
    return f;
}

void FreeFace(Face* face) {
    if (!face) return;
#ifdef IMVARFONT_USE_HARFBUZZ
    if (face->hbBuf) {
        hb_buffer_destroy(face->hbBuf);
        face->hbBuf = nullptr;
    }
    if (face->hbFont) {
        hb_font_destroy(face->hbFont);
        face->hbFont = nullptr;
    }
#endif
    if (face->ftFace)  FT_Done_Face(face->ftFace);
    if (face->library) FT_Done_FreeType(face->library);
    delete face;
}

// ============================================================================
// Introspection
// ============================================================================

bool        IsLoaded(const Face* f)      { return f && f->ftFace; }
bool        IsVariable(const Face* f)    { return f && f->isVariable; }
const char* GetFamilyName(const Face* f) { return f ? f->familyName.c_str() : ""; }
const char* GetStyleName(const Face* f)  { return f ? f->styleName.c_str()  : ""; }
const char* GetFilePath(const Face* f)   { return f ? f->filePath.c_str()   : ""; }

bool HasKerning(const Face* f) {
    if (!f) return false;
    return f->hasKerningTable || f->hasGpos;
}

bool HasKernTable(const Face* f) { return f && f->hasKerningTable; }
bool HasGpos(const Face* f)       { return f && f->hasGpos; }

bool UsesHarfBuzz(const Face* f) {
#ifdef IMVARFONT_USE_HARFBUZZ
    return f && f->hbFont != nullptr;
#else
    (void)f;
    return false;
#endif
}

bool GetUseKerning(const Face* f)  { return f && f->useKerning; }
void SetUseKerning(Face* f, bool enabled) {
    if (!f) return;
    f->useKerning = enabled;
}

bool GetUseHarfBuzz(const Face* f) { return f && f->useHarfBuzz; }
void SetUseHarfBuzz(Face* f, bool enabled) {
    if (!f) return;
#ifdef IMVARFONT_USE_HARFBUZZ
    f->useHarfBuzz = enabled && (f->hbFont != nullptr);
#else
    (void)enabled;
    f->useHarfBuzz = false;
#endif
}

bool GetUseKernTable(const Face* f) { return f && f->useKernTable; }
void SetUseKernTable(Face* f, bool enabled) {
    if (!f) return;
    f->useKernTable = enabled && f->hasKerningTable;
}

RenderMode GetRenderMode(const Face* face) {
    return face ? face->renderMode : RenderMode::Vector;
}

HintingFlags GetHintingFlags(const Face* face) {
    return face ? face->hintingFlags : HintingFlags::Native;
}

const char* GetRenderModeLabel(RenderMode mode) {
    switch (mode) {
    case RenderMode::HintedVector: return "Hinted vector";
    case RenderMode::Raster:       return "Raster";
    default:                       return "Vector";
    }
}

const char* GetHintingFlagsLabel(HintingFlags flags) {
    switch (flags) {
    case HintingFlags::Light:    return "Light";
    case HintingFlags::AutoHint: return "Auto-hint";
    default:                     return "Native";
    }
}

const char* GetKerningEngineLabel(const Face* f) {
    if (!f) return "none";
    if (!f->useKerning) return "off";
#ifdef IMVARFONT_USE_HARFBUZZ
    if (f->useHarfBuzz && f->hbFont && f->hasGpos) return "HarfBuzz (GPOS)";
#endif
    if (f->useKernTable && f->hasKerningTable) return "kern table";
    return "none";
}

// ============================================================================
// Axis control
// ============================================================================

int   GetAxisCount(const Face* f) { return f ? (int)f->axes.size() : 0; }
Axis* GetAxes(Face* f)            { return f && !f->axes.empty() ? f->axes.data() : nullptr; }

void SetAxisValue(Face* f, int axis_idx, float v, bool clamp) {
    if (!f || axis_idx < 0 || axis_idx >= (int)f->axes.size()) return;
    if (clamp) {
        const auto& ax = f->axes[axis_idx];
        if (v < ax.Min) v = ax.Min;
        if (v > ax.Max) v = ax.Max;
    }
    f->axes[axis_idx].Value = v;
}

void ResetAxes(Face* f) {
    if (!f) return;
    for (auto& ax : f->axes)
        ax.Value = ax.Default;
    ApplyAxes(f, false);
}

static float computeAxisExtrap(float v, float min, float max, float def) {
    if (v > max && max > def + 1e-6f)
        return (v - max) / (max - def);
    if (v < min && def > min + 1e-6f)
        return (v - min) / (min - def);
    return 0.f;
}

static FT_Fixed valueToFixed(float v) {
    return (FT_Fixed)(v * 65536.f + (v >= 0.f ? 0.5f : -0.5f));
}

void ApplyAxes(Face* f, bool allow_extrapolation) {
    if (!f || !f->ftFace || f->axes.empty()) return;

    f->axisExtrap.resize(f->axes.size(), 0.f);
    std::vector<FT_Fixed> coords(f->axes.size());

    for (int i = 0; i < (int)f->axes.size(); ++i) {
        const auto& ax = f->axes[i];
        float applied = ax.Value;

        if (allow_extrapolation) {
            f->axisExtrap[i] = computeAxisExtrap(ax.Value, ax.Min, ax.Max, ax.Default);
            applied = std::clamp(ax.Value, ax.Min, ax.Max);
        } else {
            f->axisExtrap[i] = 0.f;
            applied = std::clamp(ax.Value, ax.Min, ax.Max);
            f->axes[i].Value = applied;
        }

        coords[i] = valueToFixed(applied);
    }

    FT_Set_Var_Design_Coordinates(f->ftFace,
                                   (FT_UInt)coords.size(),
                                   coords.data());

    f->syncedEmPx = -1.f;

#ifdef IMVARFONT_USE_HARFBUZZ
    if (f->hbFont)
        hb_ft_font_changed(f->hbFont);
#endif
}

// ============================================================================
// ImGui widgets
// ============================================================================

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
    if (changed)
        ApplyAxes(face, allow_extrapolation);
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
    ImGui::Text("Asc    : %d", ft->ascender);
    ImGui::Text("Desc   : %d", ft->descender);
    ImGui::Text("Height : %d", ft->height);
    if (ft->bbox.xMin || ft->bbox.yMin || ft->bbox.xMax || ft->bbox.yMax) {
        ImGui::Text("BBox   : %d %d  %d %d",
                    ft->bbox.xMin, ft->bbox.yMin,
                    ft->bbox.xMax, ft->bbox.yMax);
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

// ============================================================================
// Outline decomposition → ImDrawList
// ============================================================================

// Signed area helpers for contour winding (font space, y-up).
// Curves are flattened so winding is reliable regardless of contour order.
static void addSegArea(double& acc, double x0, double y0, double x1, double y1) {
    acc += x0 * y1 - x1 * y0;
}

static void flattenQuadArea(double& cx, double& cy, double& acc,
                            double x1, double y1, double x2, double y2, int depth) {
    if (depth <= 0) {
        addSegArea(acc, cx, cy, x2, y2);
        cx = x2;
        cy = y2;
        return;
    }
    const double mx = (cx + 2.0 * x1 + x2) * 0.25;
    const double my = (cy + 2.0 * y1 + y2) * 0.25;
    flattenQuadArea(cx, cy, acc, (cx + x1) * 0.5, (cy + y1) * 0.5, mx, my, depth - 1);
    flattenQuadArea(cx, cy, acc, (x1 + x2) * 0.5, (y1 + y2) * 0.5, x2, y2, depth - 1);
}

static void flattenCubicArea(double& cx, double& cy, double& acc,
                             double x1, double y1, double x2, double y2,
                             double x3, double y3, int depth) {
    if (depth <= 0) {
        addSegArea(acc, cx, cy, x3, y3);
        cx = x3;
        cy = y3;
        return;
    }
    const double x01 = (cx + x1) * 0.5,  y01 = (cy + y1) * 0.5;
    const double x12 = (x1 + x2) * 0.5,  y12 = (y1 + y2) * 0.5;
    const double x23 = (x2 + x3) * 0.5,  y23 = (y2 + y3) * 0.5;
    const double x012 = (x01 + x12) * 0.5, y012 = (y01 + y12) * 0.5;
    const double x123 = (x12 + x23) * 0.5, y123 = (y12 + y23) * 0.5;
    const double mx = (x012 + x123) * 0.5, my = (y012 + y123) * 0.5;
    flattenCubicArea(cx, cy, acc, x01, y01, x012, y012, mx, my, depth - 1);
    flattenCubicArea(cx, cy, acc, x123, y123, x23, y23, x3, y3, depth - 1);
}

struct ContourAreasCtx {
    std::vector<double> areas;
    int   contourIdx = -1;
    double cx = 0.0, cy = 0.0;
    double fx = 0.0, fy = 0.0;
    bool  open = false;

    void closeContour() {
        if (!open || contourIdx < 0 || contourIdx >= (int)areas.size())
            return;
        addSegArea(areas[contourIdx], cx, cy, fx, fy);
        open = false;
    }
};

static int area_moveto(const FT_Vector* to, void* user) {
    auto& c = *static_cast<ContourAreasCtx*>(user);
    c.closeContour();
    ++c.contourIdx;
    c.cx = (double)to->x;
    c.cy = (double)to->y;
    c.fx = c.cx;
    c.fy = c.cy;
    c.open = true;
    return 0;
}

static int area_lineto(const FT_Vector* to, void* user) {
    auto& c = *static_cast<ContourAreasCtx*>(user);
    if (!c.open || c.contourIdx < 0) return 0;
    addSegArea(c.areas[c.contourIdx], c.cx, c.cy, (double)to->x, (double)to->y);
    c.cx = (double)to->x;
    c.cy = (double)to->y;
    return 0;
}

static int area_conicto(const FT_Vector* ctrl, const FT_Vector* to, void* user) {
    auto& c = *static_cast<ContourAreasCtx*>(user);
    if (!c.open || c.contourIdx < 0) return 0;
    flattenQuadArea(c.cx, c.cy, c.areas[c.contourIdx],
                    (double)ctrl->x, (double)ctrl->y,
                    (double)to->x, (double)to->y, 8);
    return 0;
}

static int area_cubicto(const FT_Vector* c1, const FT_Vector* c2,
                         const FT_Vector* to, void* user) {
    auto& c = *static_cast<ContourAreasCtx*>(user);
    if (!c.open || c.contourIdx < 0) return 0;
    flattenCubicArea(c.cx, c.cy, c.areas[c.contourIdx],
                     (double)c1->x, (double)c1->y,
                     (double)c2->x, (double)c2->y,
                     (double)to->x, (double)to->y, 8);
    return 0;
}

static const FT_Outline_Funcs kAreaFuncs = {
    area_moveto, area_lineto, area_conicto, area_cubicto, 0, 0
};

static bool cffSfntOutlines(FT_Face face) {
    if (!face || !FT_IS_SFNT(face)) return false;
    FT_ULong len = 0;
    if (FT_Load_Sfnt_Table(face, FT_MAKE_TAG('C', 'F', 'F', ' '), 0, nullptr, &len) == 0
        && len > 0)
        return true;
    len = 0;
    if (FT_Load_Sfnt_Table(face, FT_MAKE_TAG('C', 'F', 'F', '2'), 0, nullptr, &len) == 0
        && len > 0)
        return true;
    return false;
}

// TrueType glyf: outer contours are clockwise (negative area, y-up).
// CFF / PostScript: outer contours are counter-clockwise (positive area).
// Contour order in the glyph does not matter.
static bool contourIsFilledOuter(FT_Face face, const FT_Outline* ol, double signedArea) {
    if (std::fabs(signedArea) < 1e-3) return true;
    const bool cff     = cffSfntOutlines(face);
    const bool reverse = (ol->flags & FT_OUTLINE_REVERSE_FILL) != 0;
    bool fill = cff ? (signedArea > 0.0) : (signedArea < 0.0);
    return reverse ? !fill : fill;
}

static void computeContourFillMask(FT_Face face, const FT_Outline* ol,
                                   std::vector<bool>& out_fill_outer) {
    out_fill_outer.assign(ol->n_contours, true);
    if (ol->n_contours <= 0) return;

    ContourAreasCtx ac;
    ac.areas.assign(ol->n_contours, 0.0);
    FT_Outline_Decompose(const_cast<FT_Outline*>(ol), &kAreaFuncs, &ac);
    ac.closeContour();

    for (int i = 0; i < ol->n_contours; ++i) {
        const double area = ac.areas[i] * 0.5;
        out_fill_outer[i] = contourIsFilledOuter(face, ol, area);
    }
}

struct OutlineCtx {
    ImDrawList*   dl;
    int           contourIdx;
    float         scale;     // design-units → screen pixels
    float         scaleX;    // synthetic extrap stretch (horizontal)
    float         scaleY;    // synthetic extrap stretch (vertical)
    float         originX;   // glyph pen origin in screen space
    float         originY;   // baseline in screen space
    ImVec2        cur;       // current pen tip (screen coords), needed for quad→cubic
    ImVec2        pathFirst; // first point of the active contour
    ImU32         col;
    ImU32         holeCol;
    bool          filled;
    bool          outerPass;   // true = fill outer contours; false = punch holes
    bool          strokeOutline;
    float         thickness;
    bool          pathOpen;    // true when a contour is being built
    bool          pathStarted; // true after the first point is added to _Path
    const std::vector<bool>* contourFillOuter = nullptr;

    ImVec2 toScreen(const FT_Vector& v) const {
        const float px = originX + (float)v.x * scale;
        const float py = originY - (float)v.y * scale;  // FT y-up → screen y-down
        return { originX + (px - originX) * scaleX,
                 originY + (py - originY) * scaleY };
    }

    void flushPath() {
        if (!pathOpen)
            return;
        if (pathStarted) {
            // Close the contour explicitly instead of ImDrawFlags_Closed, which
            // can draw a stray chord when the first/last points don't coincide.
            const float dx = pathFirst.x - cur.x;
            const float dy = pathFirst.y - cur.y;
            if (dx * dx + dy * dy > 0.25f)
                dl->PathLineTo(pathFirst);
            if (filled && contourFillOuter && contourIdx >= 0
                && contourIdx < (int)contourFillOuter->size()) {
                const bool isOuter = (*contourFillOuter)[contourIdx];
                if (outerPass && isOuter)
                    dl->PathFillConcave(col);
                else if (!outerPass && !isOuter && holeCol != 0)
                    dl->PathFillConcave(holeCol);
            }
            if (strokeOutline)
                dl->PathStroke(col, thickness, ImDrawFlags_Closed);
        } else {
            dl->PathClear();
        }
        pathOpen    = false;
        pathStarted = false;
    }

    void ensurePathStarted() {
        if (!pathStarted) {
            dl->PathLineTo(cur);
            pathFirst   = cur;
            pathStarted = true;
        }
    }
};

static int outline_moveto(const FT_Vector* to, void* user) {
    auto& c = *static_cast<OutlineCtx*>(user);
    c.flushPath();
    ++c.contourIdx;
    c.cur         = c.toScreen(*to);
    c.dl->PathClear();
    c.pathOpen    = true;
    c.pathStarted = false;
    return 0;
}

static int outline_lineto(const FT_Vector* to, void* user) {
    auto& c = *static_cast<OutlineCtx*>(user);
    c.ensurePathStarted();
    c.cur = c.toScreen(*to);
    c.dl->PathLineTo(c.cur);
    return 0;
}

// FreeType conic_to = quadratic Bézier (P0=cur, P1=ctrl, P2=to).
// ImDrawList only has cubic Béziers, so we upgrade via:
//   cp1 = P0 + 2/3 * (P1 - P0)
//   cp2 = P2 + 2/3 * (P1 - P2)
static int outline_conicto(const FT_Vector* ctrl, const FT_Vector* to, void* user) {
    auto& c = *static_cast<OutlineCtx*>(user);
    c.ensurePathStarted();
    const ImVec2 p1 = c.toScreen(*ctrl);
    const ImVec2 p2 = c.toScreen(*to);
    const float  t  = 2.f / 3.f;
    const ImVec2 cp1 = { c.cur.x + t * (p1.x - c.cur.x),
                         c.cur.y + t * (p1.y - c.cur.y) };
    const ImVec2 cp2 = { p2.x    + t * (p1.x - p2.x),
                         p2.y    + t * (p1.y - p2.y) };
    c.dl->PathBezierCubicCurveTo(cp1, cp2, p2);
    c.cur = p2;
    return 0;
}

// FreeType cubic_to maps directly to ImDrawList cubic Bézier.
static int outline_cubicto(const FT_Vector* c1, const FT_Vector* c2,
                            const FT_Vector* to, void* user) {
    auto& c = *static_cast<OutlineCtx*>(user);
    c.ensurePathStarted();
    const ImVec2 p2 = c.toScreen(*to);
    c.dl->PathBezierCubicCurveTo(c.toScreen(*c1), c.toScreen(*c2), p2);
    c.cur = p2;
    return 0;
}

static const FT_Outline_Funcs kOutlineFuncs = {
    outline_moveto, outline_lineto, outline_conicto, outline_cubicto,
    0, 0
};

// ============================================================================
// Glyph load / metrics (vector vs hinted pipeline)
// ============================================================================

static bool usesHintedPipeline(const Face* face) {
    return face && face->renderMode != RenderMode::Vector;
}

static FT_Int32 buildLoadFlags(const Face* face) {
    if (!face || face->renderMode == RenderMode::Vector)
        return FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING;

    FT_Int32 flags = FT_LOAD_DEFAULT;
    switch (face->hintingFlags) {
    case HintingFlags::Light:
        flags |= FT_LOAD_TARGET_LIGHT;
        break;
    case HintingFlags::AutoHint:
        flags |= FT_LOAD_FORCE_AUTOHINT | FT_LOAD_TARGET_NORMAL;
        break;
    default:
        flags |= FT_LOAD_TARGET_NORMAL;
        break;
    }
    return flags;
}

// hb-ft expects scaled 26.6 px metrics (face char size set). It does not handle FT_LOAD_NO_SCALE.
static FT_Int32 buildHbLoadFlags(const Face* face) {
    if (!face || face->renderMode == RenderMode::Vector)
        return FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING;
    return buildLoadFlags(face);
}

static void syncFaceCharSize(Face* face, float em_px) {
    if (!face || !face->ftFace || em_px <= 0.f || !usesHintedPipeline(face))
        return;
    if (face->syncedEmPx == em_px)
        return;
    FT_Set_Char_Size(face->ftFace, 0, (FT_UInt)(em_px * 64.f + 0.5f), 72, 72);
    face->syncedEmPx = em_px;
#ifdef IMVARFONT_USE_HARFBUZZ
    if (face->hbFont) {
        hb_ft_font_set_load_flags(face->hbFont, buildHbLoadFlags(face));
        hb_ft_font_changed(face->hbFont);
    }
#endif
}

void SetRenderMode(Face* face, RenderMode mode) {
    if (!face) return;
    face->renderMode = mode;
    face->syncedEmPx = -1.f;
#ifdef IMVARFONT_USE_HARFBUZZ
    if (face->hbFont)
        hb_ft_font_set_load_flags(face->hbFont, buildHbLoadFlags(face));
#endif
}

void SetHintingFlags(Face* face, HintingFlags flags) {
    if (!face) return;
    face->hintingFlags = flags;
    face->syncedEmPx = -1.f;
#ifdef IMVARFONT_USE_HARFBUZZ
    if (face->hbFont)
        hb_ft_font_set_load_flags(face->hbFont, buildHbLoadFlags(face));
#endif
}

struct GlyphMetricsCtx {
    float    outline_scale = 1.f;
    float    em_px         = 0.f;
    FT_Int32 load_flags    = FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING;
    bool     hinted        = false;
};

static GlyphMetricsCtx makeGlyphCtx(Face* face, float em_px) {
    GlyphMetricsCtx ctx;
    ctx.em_px = em_px;
    if (!face || !face->ftFace || em_px <= 0.f)
        return ctx;

    if (face->renderMode == RenderMode::Vector) {
        ctx.outline_scale = (face->ftFace->units_per_EM > 0)
                            ? em_px / (float)face->ftFace->units_per_EM : 1.f;
        ctx.load_flags    = FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING;
        ctx.hinted        = false;
    } else {
        syncFaceCharSize(face, em_px);
        ctx.outline_scale = 1.f / 64.f;
        ctx.load_flags    = buildLoadFlags(face);
        ctx.hinted        = true;
    }
    return ctx;
}

static void getVerticalMetrics(Face* face, float em_px,
                               float* ascender, float* descender, float* line_height) {
    if (!face || !face->ftFace || em_px <= 0.f) {
        if (ascender)    *ascender    = em_px * 0.8f;
        if (descender)   *descender   = em_px * 0.2f;
        if (line_height) *line_height = em_px * 1.2f;
        return;
    }

    if (usesHintedPipeline(face)) {
        syncFaceCharSize(face, em_px);
        const FT_Size_Metrics m = face->ftFace->size->metrics;
        if (ascender)    *ascender    = (float)m.ascender / 64.f;
        if (descender)   *descender   = (float)(-m.descender) / 64.f;
        if (line_height) *line_height = (float)m.height / 64.f;
    } else {
        const FT_Face ft = face->ftFace;
        const float scale = (ft->units_per_EM > 0)
                            ? em_px / (float)ft->units_per_EM : 1.f;
        if (ascender)    *ascender    = (float)ft->ascender * scale;
        if (descender)   *descender   = -(float)ft->descender * scale;
        if (line_height) *line_height = (ft->height > 0)
                                        ? (float)ft->height * scale : em_px * 1.2f;
    }
}

static void computeExtrapScale(const Face* face, float* out_sx, float* out_sy) {
    float sx = 1.f, sy = 1.f;
    if (face && !face->axisExtrap.empty()) {
        for (size_t i = 0; i < face->axes.size(); ++i) {
            const float e = face->axisExtrap[i];
            if (std::fabs(e) < 1e-6f)
                continue;
            const ImU32 tag = face->axes[i].Tag;
            if (tag == MakeTag('w', 'g', 'h', 't')) {
                const float s = 1.f + e * 0.55f;
                sx *= s;
                sy *= s;
            } else if (tag == MakeTag('w', 'd', 't', 'h')) {
                sx *= 1.f + e * 0.75f;
            } else if (tag == MakeTag('o', 'p', 's', 'z') ||
                       tag == MakeTag('s', 'l', 'n', 't')) {
                sy *= 1.f + e * 0.55f;
            } else {
                const float s = 1.f + e * 0.4f;
                sx *= s;
                sy *= s;
            }
        }
    }
    *out_sx = sx;
    *out_sy = sy;
}

static FT_UInt glyphIndex(FT_Face ft, unsigned int codepoint) {
    return FT_Get_Char_Index(ft, (FT_ULong)codepoint);
}

static float glyphAdvancePx(Face* face, FT_Face ft, FT_UInt gi, const GlyphMetricsCtx& gctx) {
    if (gi == 0) return 0.f;
    if (FT_Load_Glyph(ft, gi, gctx.load_flags) != 0)
        return 0.f;
    float extrapX = 1.f, extrapY = 1.f;
    computeExtrapScale(face, &extrapX, &extrapY);
    if (gctx.hinted)
        return (float)ft->glyph->advance.x / 64.f * extrapX;
    return (float)ft->glyph->advance.x * gctx.outline_scale * extrapX;
}

// Pair kerning from the font's legacy kern table.
static float kerningPx(Face* face, FT_Face ft, FT_UInt gi_left, FT_UInt gi_right,
                       const GlyphMetricsCtx& gctx) {
    if (!face || !face->useKerning || gi_left == 0 || gi_right == 0)
        return 0.f;
    if (!(ft->face_flags & FT_FACE_FLAG_KERNING))
        return 0.f;

    FT_Vector kv{};
    if (gctx.hinted) {
        if (FT_Get_Kerning(ft, gi_left, gi_right, FT_KERNING_DEFAULT, &kv) != 0)
            return 0.f;
    } else {
        FT_Load_Glyph(ft, gi_left, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING);
        if (FT_Get_Kerning(ft, gi_left, gi_right, FT_KERNING_UNFITTED, &kv) != 0)
            return 0.f;
    }
    float extrapX = 1.f, extrapY = 1.f;
    computeExtrapScale(face, &extrapX, &extrapY);
    if (gctx.hinted)
        return (float)kv.x / 64.f * extrapX;
    return (float)kv.x * gctx.outline_scale * extrapX;
}

// HarfBuzz positions from hb-ft (scaled load flags) are 26.6 px after syncHbFontSize.
static float hbBufferPosPx(float v26_6, float extrap) {
    return v26_6 / 64.f * extrap;
}

static void decomposeOutline(OutlineCtx& ctx, const FT_Outline* ol) {
    ctx.contourIdx    = -1;
    ctx.pathOpen      = false;
    ctx.pathStarted   = false;
    ctx.cur           = { ctx.originX, ctx.originY };
    FT_Outline_Decompose(const_cast<FT_Outline*>(ol), &kOutlineFuncs, &ctx);
    ctx.flushPath();
}

static float renderGlyphByIndex(ImDrawList* dl, Face* face, FT_UInt gi,
                                const GlyphMetricsCtx& gctx,
                                float origin_x, float origin_y,
                                ImU32 col, ImU32 hole_col,
                                bool filled, bool strokeOutline,
                                float thickness)
{
    FT_Face ft = face->ftFace;
    if (gi == 0) return 0.f;

    if (FT_Load_Glyph(ft, gi, gctx.load_flags) != 0)
        return 0.f;

    float extrapX = 1.f, extrapY = 1.f;
    computeExtrapScale(face, &extrapX, &extrapY);

    const float adv = gctx.hinted
                      ? (float)ft->glyph->advance.x / 64.f * extrapX
                      : (float)ft->glyph->advance.x * gctx.outline_scale * extrapX;

    if (ft->glyph->format != FT_GLYPH_FORMAT_OUTLINE)
        return adv;

    if (!dl || (!filled && !strokeOutline))
        return adv;

    const FT_Outline* ol = &ft->glyph->outline;

    OutlineCtx ctx;
    ctx.dl            = dl;
    ctx.scale         = gctx.outline_scale;
    ctx.scaleX        = extrapX;
    ctx.scaleY        = extrapY;
    ctx.originX       = origin_x;
    ctx.originY       = origin_y;
    ctx.col           = col;
    ctx.holeCol       = hole_col;
    ctx.filled        = filled;
    ctx.outerPass     = true;
    ctx.strokeOutline = strokeOutline;
    ctx.thickness     = thickness;

    std::vector<bool> fillOuter;
    if (filled && ol->n_contours > 0) {
        computeContourFillMask(ft, ol, fillOuter);
        ctx.contourFillOuter = &fillOuter;

        ctx.filled        = true;
        ctx.strokeOutline = false;
        ctx.outerPass     = true;
        decomposeOutline(ctx, ol);

        if (hole_col != 0 && ol->n_contours > 1) {
            ctx.outerPass = false;
            decomposeOutline(ctx, ol);
        }
    }

    if (strokeOutline) {
        ctx.filled            = false;
        ctx.strokeOutline     = true;
        ctx.contourFillOuter  = nullptr;
        decomposeOutline(ctx, ol);
    }

    return adv;
}

static float renderGlyph(ImDrawList* dl, Face* face,
                          unsigned int codepoint,
                          const GlyphMetricsCtx& gctx,
                          float origin_x, float origin_y,
                          ImU32 col, ImU32 hole_col,
                          bool filled, bool strokeOutline,
                          float thickness)
{
    FT_Face ft = face->ftFace;
    const FT_UInt gi = FT_Get_Char_Index(ft, (FT_ULong)codepoint);
    return renderGlyphByIndex(dl, face, gi, gctx, origin_x, origin_y, col, hole_col,
                              filled, strokeOutline, thickness);
}

#ifdef IMVARFONT_USE_HARFBUZZ
// After hb_shape(), hb_glyph_info_t.codepoint is always a glyph index (GID).
static FT_UInt shapedGlyphIndex(const hb_glyph_info_t& info) {
    return (FT_UInt)info.codepoint;
}

// Match hb-ft metrics to the preview em size for this draw call.
static void syncHbFontSize(Face* face, float em_px) {
    if (!face || !face->hbFont || !face->ftFace || em_px <= 0.f) return;
    if (usesHintedPipeline(face)) {
        syncFaceCharSize(face, em_px);
        return;
    }
    FT_Set_Char_Size(face->ftFace, 0, (FT_UInt)(em_px * 64.f + 0.5f), 72, 72);
    hb_ft_font_set_load_flags(face->hbFont, buildHbLoadFlags(face));
    hb_ft_font_changed(face->hbFont);
}
#endif

static void blendCoverage(std::vector<uint8_t>& rgba, int buf_w, int buf_h,
                          int x, int y, uint8_t cov, ImU32 col) {
    if (cov == 0 || x < 0 || y < 0 || x >= buf_w || y >= buf_h)
        return;
    const float alpha = (float)cov / 255.f;
    const float inv   = 1.f - alpha;
    uint8_t* p = &rgba[(y * buf_w + x) * 4];
    p[0] = (uint8_t)((col & 0xFF) * alpha + p[0] * inv);
    p[1] = (uint8_t)(((col >> 8) & 0xFF) * alpha + p[1] * inv);
    p[2] = (uint8_t)(((col >> 16) & 0xFF) * alpha + p[2] * inv);
    p[3] = (uint8_t)(255.f * alpha + p[3] * inv);
}

static float renderGlyphBitmap(Face* face, FT_UInt gi, const GlyphMetricsCtx& gctx,
                               float origin_x, float origin_y,
                               ImU32 col, std::vector<uint8_t>& rgba,
                               int buf_w, int buf_h)
{
    FT_Face ft = face->ftFace;
    if (gi == 0) return 0.f;
    if (FT_Load_Glyph(ft, gi, gctx.load_flags) != 0)
        return 0.f;

    float extrapX = 1.f, extrapY = 1.f;
    computeExtrapScale(face, &extrapX, &extrapY);
    const float adv = gctx.hinted
                      ? (float)ft->glyph->advance.x / 64.f * extrapX
                      : (float)ft->glyph->advance.x * gctx.outline_scale * extrapX;

    if (FT_Render_Glyph(ft->glyph, FT_RENDER_MODE_NORMAL) != 0)
        return adv;

    const FT_GlyphSlot slot = ft->glyph;
    const FT_Bitmap* bmp    = &slot->bitmap;
    const int base_x        = (int)std::floor(origin_x);
    const int base_y        = (int)std::floor(origin_y);
    const int left          = slot->bitmap_left;
    const int top           = slot->bitmap_top;

    for (unsigned row = 0; row < bmp->rows; ++row) {
        for (unsigned c = 0; c < bmp->width; ++c) {
            const uint8_t cov = bmp->buffer[row * bmp->pitch + c];
            const int px = base_x + left + (int)c;
            const int py = base_y - top + (int)row;
            blendCoverage(rgba, buf_w, buf_h, px, py, cov, col);
        }
    }
    return adv;
}

// Draw, measure, or rasterize one line.
static float drawTextLine(Face* face, const char* line, int line_len,
                          float em_px, float pen_x, float base_y,
                          ImDrawList* dl, ImU32 col, ImU32 hole_col,
                          bool filled, bool strokeOutline, float thickness,
                          float letter_spacing_px,
                          std::vector<uint8_t>* raster_rgba = nullptr,
                          int raster_w = 0, int raster_h = 0)
{
    if (!face || !face->ftFace || line_len <= 0) return 0.f;

    const FT_Face ft = face->ftFace;
    const float start_x = pen_x;
    const GlyphMetricsCtx gctx = makeGlyphCtx(face, em_px);
    const bool raster = (raster_rgba != nullptr);

    float extrapX = 1.f, extrapY = 1.f;
    computeExtrapScale(face, &extrapX, &extrapY);

#ifdef IMVARFONT_USE_HARFBUZZ
    if (face->useKerning && face->useHarfBuzz && face->hasGpos
        && face->hbFont && face->hbBuf) {
        syncHbFontSize(face, em_px);

        hb_buffer_t* buf = face->hbBuf;
        hb_buffer_clear_contents(buf);
        hb_buffer_add_utf8(buf, line, line_len, 0, line_len);
        hb_buffer_guess_segment_properties(buf);
        hb_shape(face->hbFont, buf, nullptr, 0);

        unsigned count = hb_buffer_get_length(buf);
        if (count > 0) {
            hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buf, &count);
            unsigned pos_count = count;
            hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &pos_count);
            if (infos && pos && pos_count == count) {
                for (unsigned i = 0; i < count; ++i) {
                    const FT_UInt gid = shapedGlyphIndex(infos[i]);
                    const float x_off = hbBufferPosPx((float)pos[i].x_offset, extrapX);
                    const float y_off = hbBufferPosPx((float)pos[i].y_offset, extrapY);
                    const float gx    = pen_x + x_off;
                    const float gy    = base_y - y_off;

                    if (raster)
                        renderGlyphBitmap(face, gid, gctx, gx, gy, col,
                                          *raster_rgba, raster_w, raster_h);
                    else if (dl)
                        renderGlyphByIndex(dl, face, gid, gctx, gx, gy, col, hole_col,
                                             filled, strokeOutline, thickness);

                    const float hb_adv = hbBufferPosPx((float)pos[i].x_advance, extrapX);
                    const float ft_adv = glyphAdvancePx(face, ft, gid, gctx);
                    pen_x += (hb_adv > 0.001f) ? hb_adv : ft_adv;
                    if (letter_spacing_px != 0.f && i + 1 < count)
                        pen_x += letter_spacing_px;
                }
                return pen_x - start_x;
            }
        }
    }
#endif

    // UTF-8 walk + legacy kern table (fallback, or when HarfBuzz is off)
    FT_UInt prev_gi = 0;
    const char* p = line;
    const char* const line_end = line + line_len;
    while (p < line_end) {
        unsigned int cp = 0;
        const int len = ImTextCharFromUtf8(&cp, p, line_end);
        if (len == 0) break;
        p += len;

        const FT_UInt gi = glyphIndex(ft, cp);
        if (prev_gi != 0 && face->hasKerningTable && face->useKerning && face->useKernTable)
            pen_x += kerningPx(face, ft, prev_gi, gi, gctx);

        if (raster)
            pen_x += renderGlyphBitmap(face, gi, gctx, pen_x, base_y, col,
                                       *raster_rgba, raster_w, raster_h);
        else if (dl)
            pen_x += renderGlyphByIndex(dl, face, gi, gctx, pen_x, base_y, col, hole_col,
                                        filled, strokeOutline, thickness);
        else
            pen_x += glyphAdvancePx(face, ft, gi, gctx);

        if (letter_spacing_px != 0.f && p < line_end)
            pen_x += letter_spacing_px;

        prev_gi = gi;
    }
    return pen_x - start_x;
}

static float addTextLayout(ImDrawList* dl, Face* face,
                           float em_px, ImVec2 pos,
                           ImU32 col, ImU32 hole_col, const char* text,
                           bool filled, bool strokeOutline, float thickness,
                           float line_height_px, float letter_spacing_px)
{
    float ascender = 0.f, descender = 0.f, default_line_h = 0.f;
    getVerticalMetrics(face, em_px, &ascender, &descender, &default_line_h);
    (void)descender;
    const float line_h = (line_height_px > 0.f) ? line_height_px : default_line_h;

    float base_y     = pos.y + ascender;
    float max_w      = 0.f;
    const char* line_start = text;

    for (const char* p = text; ; ++p) {
        if (*p == '\n' || *p == '\0') {
            const int line_len = (int)(p - line_start);
            if (line_len > 0) {
                const float w = drawTextLine(face, line_start, line_len, em_px,
                                            pos.x, base_y, dl, col, hole_col,
                                            filled, strokeOutline, thickness,
                                            letter_spacing_px);
                if (w > max_w) max_w = w;
            }
            if (*p == '\0') break;
            base_y += line_h;
            line_start = p + 1;
        }
    }
    return max_w;
}

// ============================================================================
// Public rendering API
// ============================================================================

float AddText(ImDrawList* dl, Face* face,
              float em_px, ImVec2 pos,
              ImU32 col, const char* text,
              bool outline, float outline_thickness,
              float line_height_px, float letter_spacing_px, ImU32 hole_col)
{
    if (!dl || !face || !face->ftFace || !text || !*text) return 0.f;
    if (face->renderMode == RenderMode::Raster) return 0.f;
    return addTextLayout(dl, face, em_px, pos, col, hole_col, text,
                         true, outline, outline_thickness,
                         line_height_px, letter_spacing_px);
}

float CalcTextWidth(Face* face, float em_px, const char* text,
                    float letter_spacing_px) {
    if (!face || !face->ftFace || !text || !*text) return 0.f;

    float max_w = 0.f;
    const char* line_start = text;
    for (const char* p = text; ; ++p) {
        if (*p == '\n' || *p == '\0') {
            const int line_len = (int)(p - line_start);
            if (line_len > 0) {
                const float w = drawTextLine(face, line_start, line_len, em_px,
                                             0.f, 0.f, nullptr, 0, 0,
                                             false, false, 0.f,
                                             letter_spacing_px);
                if (w > max_w) max_w = w;
            }
            if (*p == '\0') break;
            line_start = p + 1;
        }
    }
    return max_w;
}

float CalcAscenderPx(const Face* face, float em_px) {
    float asc = 0.f, desc = 0.f, lh = 0.f;
    getVerticalMetrics(const_cast<Face*>(face), em_px, &asc, &desc, &lh);
    return asc;
}

float CalcDescenderPx(const Face* face, float em_px) {
    float asc = 0.f, desc = 0.f, lh = 0.f;
    getVerticalMetrics(const_cast<Face*>(face), em_px, &asc, &desc, &lh);
    return desc;
}

float CalcLineHeightPx(const Face* face, float em_px) {
    float asc = 0.f, desc = 0.f, lh = 0.f;
    getVerticalMetrics(const_cast<Face*>(face), em_px, &asc, &desc, &lh);
    return lh;
}

bool RasterizeText(Face* face, float em_px, const char* text, ImU32 col,
                   float line_height_px, float letter_spacing_px,
                   std::vector<uint8_t>& out_rgba,
                   int& out_w, int& out_h)
{
    out_w = 0;
    out_h = 0;
    if (!face || !face->ftFace || !text || !*text || em_px <= 0.f)
        return false;

    const RenderMode saved_mode = face->renderMode;
    if (face->renderMode == RenderMode::Vector)
        face->renderMode = RenderMode::Raster;

    float asc = 0.f, desc = 0.f, default_lh = 0.f;
    getVerticalMetrics(face, em_px, &asc, &desc, &default_lh);
    const float line_h = (line_height_px > 0.f) ? line_height_px : default_lh;

    int lines = 1;
    for (const char* p = text; *p; ++p)
        if (*p == '\n') ++lines;

    const float text_w = CalcTextWidth(face, em_px, text, letter_spacing_px);
    const float text_h = asc + desc + (lines - 1) * line_h;
    const int pad = 4;
    out_w = (int)std::ceil(text_w) + pad * 2;
    out_h = (int)std::ceil(text_h) + pad * 2;
    if (out_w <= 0 || out_h <= 0) {
        face->renderMode = saved_mode;
        return false;
    }

    out_rgba.assign((size_t)out_w * out_h * 4, 0);

    float base_y = pad + asc;
    const char* line_start = text;
    for (const char* p = text; ; ++p) {
        if (*p == '\n' || *p == '\0') {
            const int line_len = (int)(p - line_start);
            if (line_len > 0) {
                drawTextLine(face, line_start, line_len, em_px,
                             (float)pad, base_y, nullptr, col, 0,
                             false, false, 0.f, letter_spacing_px,
                             &out_rgba, out_w, out_h);
            }
            if (*p == '\0') break;
            base_y += line_h;
            line_start = p + 1;
        }
    }

    face->renderMode = saved_mode;
    face->syncedEmPx = -1.f;
    return true;
}

float GetKernTablePairPx(const Face* face, unsigned int cp_left, unsigned int cp_right,
                         float em_px) {
    if (!face || !face->ftFace || !face->hasKerningTable || em_px <= 0.f)
        return 0.f;
    const FT_Face ft = face->ftFace;
    if (!(ft->face_flags & FT_FACE_FLAG_KERNING))
        return 0.f;
    const float scale = (ft->units_per_EM > 0)
                        ? em_px / (float)ft->units_per_EM : 1.f;
    const FT_UInt gL = glyphIndex(ft, cp_left);
    const FT_UInt gR = glyphIndex(ft, cp_right);
    if (!gL || !gR) return 0.f;

    FT_Load_Glyph(ft, gL, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING);
    FT_Vector kv{};
    if (FT_Get_Kerning(ft, gL, gR, FT_KERNING_UNFITTED, &kv) != 0)
        return 0.f;

    float extrapX = 1.f, extrapY = 1.f;
    computeExtrapScale(const_cast<Face*>(face), &extrapX, &extrapY);
    (void)extrapY;
    return (float)kv.x * scale * extrapX;
}

float GetGposPairExtraPx(const Face* face, unsigned int cp_left, unsigned int cp_right,
                         float em_px) {
#ifdef IMVARFONT_USE_HARFBUZZ
    if (!face || !face->ftFace || !face->hasGpos || !face->hbFont || !face->hbBuf
        || em_px <= 0.f)
        return 0.f;

    char utf8[8];
    char* p = utf8;
    p += ImTextCharToUtf8(p, (int)cp_left);
    p += ImTextCharToUtf8(p, (int)cp_right);
    *p = '\0';
    const int len = (int)(p - utf8);

    Face* mut = const_cast<Face*>(face);
    const FT_Face ft = face->ftFace;
    const GlyphMetricsCtx gctx = makeGlyphCtx(mut, em_px);

    float extrapX = 1.f, extrapY = 1.f;
    computeExtrapScale(mut, &extrapX, &extrapY);

    syncHbFontSize(mut, em_px);

    hb_buffer_t* buf = mut->hbBuf;
    hb_buffer_clear_contents(buf);
    hb_buffer_add_utf8(buf, utf8, len, 0, len);
    hb_buffer_guess_segment_properties(buf);
    hb_shape(mut->hbFont, buf, nullptr, 0);

    unsigned count = hb_buffer_get_length(buf);
    if (count == 0) return 0.f;

    hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buf, &count);
    unsigned pos_count = count;
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &pos_count);
    if (!infos || !pos || pos_count != count) return 0.f;

    float shaped = 0.f;
    float naive  = 0.f;
    for (unsigned i = 0; i < count; ++i) {
        shaped += hbBufferPosPx((float)pos[i].x_advance, extrapX);
        naive  += glyphAdvancePx(mut, ft, shapedGlyphIndex(infos[i]), gctx);
    }
    if (count == 2) {
        // Some fonts apply pair kerning via x_offset on the second glyph.
        const float shaped_second = hbBufferPosPx((float)pos[0].x_advance, extrapX)
                                  + hbBufferPosPx((float)pos[1].x_offset, extrapX);
        const float naive_second  = glyphAdvancePx(mut, ft, shapedGlyphIndex(infos[0]), gctx);
        return shaped_second - naive_second;
    }
    return shaped - naive;
#else
    (void)face;
    (void)cp_left;
    (void)cp_right;
    (void)em_px;
    return 0.f;
#endif
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
