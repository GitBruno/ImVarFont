// imgui_var_font.cpp  –  Implementation of ImVarFont
// See imgui_var_font.h for API documentation.

#include "imgui_var_font.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <freetype/ftmm.h>       // FT_MM_Var, FT_Get_MM_Var, FT_Set_Var_Design_Coordinates
#include <freetype/ftsnames.h>   // FT_Get_Sfnt_Name, FT_SfntName
#include <freetype/ttnameid.h>   // TT_NAME_ID_*, TT_PLATFORM_*
#include <freetype/tttables.h>   // FT_IS_SFNT

#include <freetype/fterrors.h>  // FT_Error_String (FreeType >= 2.10)

#include <vector>
#include <string>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <cmath>

#ifdef IMGUI_ENABLE_FREETYPE
#include "imgui_internal.h"
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

    loadMetadata(f);
    return f;
}

void FreeFace(Face* face) {
    if (!face) return;
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

struct OutlineCtx {
    ImDrawList* dl;
    float       scale;     // design-units → screen pixels
    float       scaleX;    // synthetic extrap stretch (horizontal)
    float       scaleY;    // synthetic extrap stretch (vertical)
    float       originX;   // glyph pen origin in screen space
    float       originY;   // baseline in screen space
    ImVec2      cur;       // current pen tip (screen coords), needed for quad→cubic
    ImVec2      pathFirst; // first point of the active contour
    ImU32       col;
    float       thickness;
    bool        pathOpen;    // true when a contour is being built
    bool        pathStarted; // true after the first point is added to _Path

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
            dl->PathStroke(col, thickness, ImDrawFlags_None);
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
// Minimal self-contained UTF-8 decoder (avoids pulling in imgui_internal.h)
// Returns bytes consumed and writes the codepoint to *out.
// ============================================================================

static int utf8_decode(unsigned int* out, const char* s) {
    const unsigned char* p = (const unsigned char*)s;
    if (!*p)       { *out = 0; return 0; }
    if (*p < 0x80) { *out = *p; return 1; }
    if (*p < 0xE0) { *out = ((*p & 0x1F) << 6)  |  (p[1] & 0x3F);                                    return 2; }
    if (*p < 0xF0) { *out = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6)  |  (p[2] & 0x3F);           return 3; }
                     *out = ((*p & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
                     return 4;
}

// ============================================================================
// Render a single glyph; returns advance width in pixels.
// ============================================================================

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

static float renderGlyph(ImDrawList* dl, Face* face,
                          unsigned int codepoint,
                          float scale, float origin_x, float origin_y,
                          ImU32 col, float thickness)
{
    FT_Face ft = face->ftFace;
    FT_UInt gi = FT_Get_Char_Index(ft, (FT_ULong)codepoint);
    if (gi == 0) return 0.f;

    // FT_LOAD_NO_SCALE: design-unit outlines; variation is already blended by ApplyAxes()
    if (FT_Load_Glyph(ft, gi, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING) != 0)
        return 0.f;

    float extrapX = 1.f, extrapY = 1.f;
    computeExtrapScale(face, &extrapX, &extrapY);

    const float adv = (float)ft->glyph->advance.x * scale * extrapX;

    if (ft->glyph->format != FT_GLYPH_FORMAT_OUTLINE)
        return adv;

    OutlineCtx ctx;
    ctx.dl          = dl;
    ctx.scale       = scale;
    ctx.scaleX      = extrapX;
    ctx.scaleY      = extrapY;
    ctx.originX     = origin_x;
    ctx.originY     = origin_y;
    ctx.col         = col;
    ctx.thickness   = thickness;
    ctx.pathOpen    = false;
    ctx.pathStarted = false;
    ctx.cur         = { origin_x, origin_y };

    FT_Outline_Decompose(&ft->glyph->outline, &kOutlineFuncs, &ctx);
    ctx.flushPath();   // stroke the final contour

    return adv;
}

// ============================================================================
// Public rendering API
// ============================================================================

float AddText(ImDrawList* dl, Face* face,
              float em_px, ImVec2 pos,
              ImU32 col, const char* text,
              float thickness)
{
    if (!dl || !face || !face->ftFace || !text || !*text) return 0.f;

    const FT_Face ft    = face->ftFace;
    const float   scale = (ft->units_per_EM > 0)
                          ? em_px / (float)ft->units_per_EM
                          : 1.f;

    // pos is the top-left corner; shift down by the scaled ascender to reach baseline
    const float ascender = (float)ft->ascender * scale;
    const float line_h   = (ft->height > 0)
                           ? (float)ft->height * scale
                           : em_px * 1.2f;

    float pen_x  = pos.x;
    float base_y = pos.y + ascender;

    const char* p = text;
    while (*p) {
        unsigned int cp = 0;
        int len = utf8_decode(&cp, p);
        if (len == 0) break;
        p += len;

        if (cp == '\n') { base_y += line_h; pen_x = pos.x; continue; }

        pen_x += renderGlyph(dl, face, cp, scale, pen_x, base_y, col, thickness);
    }
    return pen_x - pos.x;
}

float CalcTextWidth(Face* face, float em_px, const char* text) {
    if (!face || !face->ftFace || !text || !*text) return 0.f;

    const FT_Face ft    = face->ftFace;
    const float   scale = (ft->units_per_EM > 0)
                          ? em_px / (float)ft->units_per_EM
                          : 1.f;
    float total = 0.f;
    const char* p = text;
    while (*p) {
        unsigned int cp = 0;
        int len = utf8_decode(&cp, p);
        if (len == 0) break;
        p += len;
        if (cp == '\n') continue;

        FT_UInt gi = FT_Get_Char_Index(ft, (FT_ULong)cp);
        if (gi && FT_Load_Glyph(ft, gi, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING) == 0)
            total += (float)ft->glyph->advance.x * scale;
    }
    return total;
}

float CalcAscenderPx(const Face* face, float em_px) {
    if (!face || !face->ftFace || face->ftFace->units_per_EM == 0) return em_px * 0.8f;
    return (float)face->ftFace->ascender * em_px / (float)face->ftFace->units_per_EM;
}

float CalcDescenderPx(const Face* face, float em_px) {
    if (!face || !face->ftFace || face->ftFace->units_per_EM == 0) return em_px * 0.2f;
    // descender is stored as a negative value in FreeType; return positive depth
    return -(float)face->ftFace->descender * em_px / (float)face->ftFace->units_per_EM;
}

float CalcLineHeightPx(const Face* face, float em_px) {
    if (!face || !face->ftFace) return em_px * 1.2f;
    const FT_Face ft = face->ftFace;
    if (ft->units_per_EM <= 0) return em_px * 1.2f;
    const float scale = em_px / (float)ft->units_per_EM;
    return (ft->height > 0) ? (float)ft->height * scale : em_px * 1.2f;
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
