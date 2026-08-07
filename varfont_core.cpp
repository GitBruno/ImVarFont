// varfont_core.cpp — VarFont engine (Face / morph / layout / host emit)

#include "varfont_core.h"

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
#include <cstdlib>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <limits>
#include <utility>
#include <unordered_map>

#ifdef IMVARFONT_USE_HARFBUZZ
#include <hb.h>
#include <hb-ft.h>
#include <hb-ot.h>
#endif

#include "varfont_gl.h"
#include "varfont_core_detail.h"

namespace VarFont {

using detail::valueToFixed;
using detail::extractCurves;
using detail::pushEdge;
using detail::flatQuad;
using detail::flatCubic;
using detail::rasterOutlineCPU;
using detail::setFaceVarToCurrent;
using detail::morphBlendGlyph;
using detail::resolveVMetrics;

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

            dst.Tag     = (uint32_t)src.tag;
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
                "VarFont warning: \"%s\" has no kern table and no GPOS data - "
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
    face->glyphTexCache.clear();  // atlas textures are owned by the GL renderer
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

// ----------------------------------------------------------------------------
// OpenType features
// ----------------------------------------------------------------------------

static uint32_t tagFromString(const char* s) {
    char t[4] = { ' ', ' ', ' ', ' ' };
    for (int i = 0; i < 4 && s && s[i]; ++i) t[i] = s[i];
    return MakeTag(t[0], t[1], t[2], t[3]);
}

void SetFeatureRange(Face* f, const char* tag, uint32_t value,
                     uint32_t start, uint32_t end) {
    if (!f || !tag) return;
    const uint32_t t = tagFromString(tag);
    for (auto& fs : f->features) {
        if (fs.Tag == t) { fs.Value = value; fs.Start = start; fs.End = end; return; }
    }
    f->features.push_back(FeatureSetting{ t, value, start, end });
}

void SetFeature(Face* f, const char* tag, uint32_t value) {
    SetFeatureRange(f, tag, value, 0u, 0xFFFFFFFFu);
}

void ClearFeature(Face* f, const char* tag) {
    if (!f || !tag) return;
    const uint32_t t = tagFromString(tag);
    f->features.erase(
        std::remove_if(f->features.begin(), f->features.end(),
                       [t](const FeatureSetting& fs) { return fs.Tag == t; }),
        f->features.end());
}

void ClearAllFeatures(Face* f) { if (f) f->features.clear(); }

int GetFeatureCount(const Face* f) { return f ? (int)f->features.size() : 0; }

const FeatureSetting* GetFeatures(const Face* f) {
    return (f && !f->features.empty()) ? f->features.data() : nullptr;
}

bool GetFeatureValue(const Face* f, const char* tag, uint32_t* out_value) {
    if (!f || !tag) return false;
    const uint32_t t = tagFromString(tag);
    for (const auto& fs : f->features) {
        if (fs.Tag == t) { if (out_value) *out_value = fs.Value; return true; }
    }
    return false;
}

int SetFeaturesString(Face* f, const char* s) {
    if (!f) return 0;
    f->features.clear();
    if (!s) return 0;
    int n = 0;
    const char* p = s;
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\t') ++p;
        if (!*p) break;
        uint32_t value = 1;
        if (*p == '+') { value = 1; ++p; }
        else if (*p == '-') { value = 0; ++p; }
        char tag[5] = {0};
        int ti = 0;
        while (*p && *p != ',' && *p != ' ' && *p != '=' && *p != '\t') {
            if (ti < 4) tag[ti++] = *p;
            ++p;
        }
        if (*p == '=') {
            ++p;
            value = (uint32_t)strtoul(p, nullptr, 10);
            while (*p && *p != ',' && *p != ' ' && *p != '\t') ++p;
        }
        if (ti > 0) { SetFeature(f, tag, value); ++n; }
    }
    return n;
}

#ifdef IMVARFONT_USE_HARFBUZZ
// Convert the stored feature settings (plus the kerning master switch) into the
// hb_feature_t array passed to hb_shape().
static void buildHbFeatures(const Face* face, std::vector<hb_feature_t>& out) {
    out.clear();
    out.reserve(face->features.size() + 1);
    for (const auto& fs : face->features) {
        char t[5]; TagToStr(fs.Tag, t);
        hb_feature_t hf;
        hf.tag   = HB_TAG(t[0], t[1], t[2], t[3]);
        hf.value = fs.Value;
        hf.start = fs.Start;
        hf.end   = fs.End;
        out.push_back(hf);
    }
    // Respect the kerning master switch by explicitly disabling GPOS 'kern'.
    if (!face->useKerning) {
        hb_feature_t kf;
        kf.tag = HB_TAG('k','e','r','n');
        kf.value = 0;
        kf.start = 0;
        kf.end   = 0xFFFFFFFFu;
        out.push_back(kf);
    }
}
#endif

RenderMode GetRenderMode(const Face* face) {
    return face ? face->renderMode : RenderMode::Vector;
}

HintingFlags GetHintingFlags(const Face* face) {
    return face ? face->hintingFlags : HintingFlags::Native;
}

const char* GetRenderModeLabel(RenderMode mode) {
    switch (mode) {
    case RenderMode::HintedVector:  return "Hinted vector";
    case RenderMode::Raster:        return "Raster";
    case RenderMode::LoopBlinn:     return "Loop-Blinn";
    case RenderMode::LoopBlinnLive: return "Loop-Blinn (live)";
    default:                        return "Vector";
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

namespace detail {
FT_Fixed valueToFixed(float v) {
    return (FT_Fixed)(v * 65536.f + (v >= 0.f ? 0.5f : -0.5f));
}
} // namespace detail

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
    ++f->outlineGen;   // outline shape changed → invalidate glyph fill cache

#ifdef IMVARFONT_USE_HARFBUZZ
    if (f->hbFont)
        hb_ft_font_changed(f->hbFont);
#endif
}

// Resolve the face's vertical metrics (design units) once, at the font's DEFAULT
// instance, and cache them for the lifetime of the face. Vertical metrics are a
// font-level property; reading them at the live instance makes them track the
// MVAR table, so dragging an axis would continuously shift ascender/descender/
// height — reflowing the preview text and churning the metadata numbers. Freezing
// them at the default keeps the layout stable across a morph and the displayed
// metrics steady, which is the expected behaviour for a preview/morph tool. The
// user's instance is restored before returning so nothing else is disturbed.
namespace detail {

void resolveVMetrics(Face* f) {
    if (!f || !f->ftFace || f->metricsValid) return;
    const int n = (int)f->axes.size();
    if (n > 0 && n <= kMorphMaxAxes) {
        FT_Fixed def[kMorphMaxAxes], cur[kMorphMaxAxes];
        for (int i = 0; i < n; ++i) {
            def[i] = valueToFixed(f->axes[i].Default);
            cur[i] = valueToFixed(std::clamp(f->axes[i].Value,
                                             f->axes[i].Min, f->axes[i].Max));
        }
        FT_Set_Var_Design_Coordinates(f->ftFace, (FT_UInt)n, def);
        f->metricAsc     = (int)f->ftFace->ascender;
        f->metricDesc    = (int)f->ftFace->descender;
        f->metricHeight  = (int)f->ftFace->height;
        f->metricBbox[0] = (long)f->ftFace->bbox.xMin;
        f->metricBbox[1] = (long)f->ftFace->bbox.yMin;
        f->metricBbox[2] = (long)f->ftFace->bbox.xMax;
        f->metricBbox[3] = (long)f->ftFace->bbox.yMax;
        FT_Set_Var_Design_Coordinates(f->ftFace, (FT_UInt)n, cur);   // restore user instance
    } else {
        f->metricAsc     = (int)f->ftFace->ascender;
        f->metricDesc    = (int)f->ftFace->descender;
        f->metricHeight  = (int)f->ftFace->height;
        f->metricBbox[0] = (long)f->ftFace->bbox.xMin;
        f->metricBbox[1] = (long)f->ftFace->bbox.yMin;
        f->metricBbox[2] = (long)f->ftFace->bbox.xMax;
        f->metricBbox[3] = (long)f->ftFace->bbox.yMax;
    }
    f->metricsValid = true;
}

} // namespace detail

// ============================================================================
// Outline decomposition → screen space
// ============================================================================

static Vec2 outlineToScreen(float originX, float originY,
                              float scale, float scaleX, float scaleY,
                              const FT_Vector& v) {
    const float px = originX + (float)v.x * scale;
    const float py = originY - (float)v.y * scale;  // FT y-up → screen y-down
    return { originX + (px - originX) * scaleX,
             originY + (py - originY) * scaleY };
}


// ============================================================================
// Analytic GPU glyph fill (signed-area coverage)
//
// Curve-preserving extraction (design units, cached per glyph for the vector
// pipeline) is flattened to line edges at the target DEVICE resolution and
// handed to the GL coverage backend, which accumulates winding-correct,
// conflation-free coverage and resolves it to an RGBA8 cell. The cell leaves as
// a GlyphQuad for the host to composite tinted by the text colour, so counters,
// curves and stems stay faithful at any zoom and DPI.
// ============================================================================
namespace {

struct CurveExtractCtx {
    GlyphCurves* g     = nullptr;
    Vec2       cur   {};
    Vec2       start {};
    bool         open  = false;

    void ext(const Vec2& p) {
        if (p.x < g->bboxMin.x) g->bboxMin.x = p.x;
        if (p.y < g->bboxMin.y) g->bboxMin.y = p.y;
        if (p.x > g->bboxMax.x) g->bboxMax.x = p.x;
        if (p.y > g->bboxMax.y) g->bboxMax.y = p.y;
    }
    void closeContour() {
        if (!open) return;
        if (cur.x != start.x || cur.y != start.y) {
            GlyphSeg s; s.type = SegType::Line; s.p[0] = cur; s.p[1] = start;
            g->segs.push_back(s);
        }
        open = false;
    }
};

static inline Vec2 ftVec(const FT_Vector* v) { return Vec2((float)v->x, (float)v->y); }

static int cx_moveto(const FT_Vector* to, void* u) {
    auto& c = *static_cast<CurveExtractCtx*>(u);
    c.closeContour();
    c.cur = c.start = ftVec(to);
    c.ext(c.cur);
    c.open = true;
    return 0;
}
static int cx_lineto(const FT_Vector* to, void* u) {
    auto& c = *static_cast<CurveExtractCtx*>(u);
    if (!c.open) return 0;
    const Vec2 p = ftVec(to);
    GlyphSeg s; s.type = SegType::Line; s.p[0] = c.cur; s.p[1] = p;
    c.g->segs.push_back(s);
    c.cur = p; c.ext(p);
    return 0;
}
static int cx_conicto(const FT_Vector* ctrl, const FT_Vector* to, void* u) {
    auto& c = *static_cast<CurveExtractCtx*>(u);
    if (!c.open) return 0;
    const Vec2 k = ftVec(ctrl), p = ftVec(to);
    GlyphSeg s; s.type = SegType::Quad; s.p[0] = c.cur; s.p[1] = k; s.p[2] = p;
    c.g->segs.push_back(s);
    c.cur = p; c.ext(k); c.ext(p);
    return 0;
}
static int cx_cubicto(const FT_Vector* c1, const FT_Vector* c2,
                      const FT_Vector* to, void* u) {
    auto& c = *static_cast<CurveExtractCtx*>(u);
    if (!c.open) return 0;
    const Vec2 a = ftVec(c1), b = ftVec(c2), p = ftVec(to);
    GlyphSeg s; s.type = SegType::Cubic; s.p[0] = c.cur; s.p[1] = a; s.p[2] = b; s.p[3] = p;
    c.g->segs.push_back(s);
    c.cur = p; c.ext(a); c.ext(b); c.ext(p);
    return 0;
}
static const FT_Outline_Funcs kCurveExtractFuncs = {
    cx_moveto, cx_lineto, cx_conicto, cx_cubicto, 0, 0
};

} // anonymous namespace (curve extract helpers)

namespace detail {

void extractCurves(const FT_Outline* ol, GlyphCurves& out) {
    out.segs.clear();
    out.bboxMin = Vec2( 1e30f,  1e30f);
    out.bboxMax = Vec2(-1e30f, -1e30f);
    CurveExtractCtx ctx; ctx.g = &out;
    FT_Outline_Decompose(const_cast<FT_Outline*>(ol), &kCurveExtractFuncs, &ctx);
    ctx.closeContour();
}

} // namespace detail

// Cached design-unit curves (vector pipeline; size-independent). Invalidated by
// outlineGen. Hinted outlines are size-specific and are extracted fresh instead.
static const GlyphCurves& getCurvesCached(Face* face, FT_UInt gi, const FT_Outline* ol) {
    if (face->curveCacheGen != face->outlineGen) {
        face->curveCache.clear();
        face->curveCacheGen = face->outlineGen;
    }
    auto it = face->curveCache.find(gi);
    if (it != face->curveCache.end())
        return it->second;
    GlyphCurves gc;
    extractCurves(ol, gc);
    return face->curveCache.emplace(gi, std::move(gc)).first->second;
}

static inline Vec2 segMid(const Vec2& a, const Vec2& b) {
    return Vec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
}

namespace detail {

void pushEdge(std::vector<float>& E, const Vec2& a, const Vec2& b) {
    E.push_back(a.x); E.push_back(a.y); E.push_back(b.x); E.push_back(b.y);
}

// Adaptive flattening in cell-pixel space (tolSq = chord-deviation² in px²).
void flatQuad(std::vector<float>& E, Vec2 p0, Vec2 p1, Vec2 p2,
                     float tolSq, int depth) {
    const float dx = p2.x - p0.x, dy = p2.y - p0.y;
    const float cross = (p1.x - p2.x) * dy - (p1.y - p2.y) * dx;
    const float len2  = dx * dx + dy * dy;
    if (depth >= 18 || (len2 > 1e-12f ? (cross * cross <= tolSq * len2)
                                      : (((p1.x - p0.x) * (p1.x - p0.x) +
                                          (p1.y - p0.y) * (p1.y - p0.y)) <= tolSq))) {
        pushEdge(E, p0, p2);
        return;
    }
    const Vec2 p01 = segMid(p0, p1), p12 = segMid(p1, p2), p012 = segMid(p01, p12);
    flatQuad(E, p0, p01, p012, tolSq, depth + 1);
    flatQuad(E, p012, p12, p2, tolSq, depth + 1);
}
void flatCubic(std::vector<float>& E, Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3,
                      float tolSq, int depth) {
    const float dx = p3.x - p0.x, dy = p3.y - p0.y;
    const float d1 = std::fabs((p1.x - p3.x) * dy - (p1.y - p3.y) * dx);
    const float d2 = std::fabs((p2.x - p3.x) * dy - (p2.y - p3.y) * dx);
    const float len2 = dx * dx + dy * dy;
    const float dd = d1 + d2;
    if (depth >= 18 || (len2 > 1e-12f ? (dd * dd <= tolSq * len2)
                                      : (((p1.x - p0.x) * (p1.x - p0.x) +
                                          (p1.y - p0.y) * (p1.y - p0.y)) <= tolSq))) {
        pushEdge(E, p0, p3);
        return;
    }
    const Vec2 p01 = segMid(p0, p1), p12 = segMid(p1, p2), p23 = segMid(p2, p3);
    const Vec2 p012 = segMid(p01, p12), p123 = segMid(p12, p23);
    const Vec2 p0123 = segMid(p012, p123);
    flatCubic(E, p0, p01, p012, p0123, tolSq, depth + 1);
    flatCubic(E, p0123, p123, p23, p3, tolSq, depth + 1);
}

} // namespace detail

// Approximate a cubic Bezier with analytic quadratics for the Loop-Blinn path
// (smooth at any zoom, unlike line flattening). Adaptive by device-pixel
// tolerance via the |third difference| error bound; each piece uses the standard
// single-quad control point. Subdivision is at t=0.5, so every output control
// point is a fixed linear combination of the input ones — this keeps the result
// linear in the cubic's control points, which the axis-morph blend relies on
// (provided the subdivision count is held fixed; see lattice sampling).
static void cubicToQuads(std::vector<glr::Curve>& out,
                         Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3,
                         float tolPx, int depth) {
    const float dx = p0.x - 3.f * p1.x + 3.f * p2.x - p3.x;
    const float dy = p0.y - 3.f * p1.y + 3.f * p2.y - p3.y;
    const float err = 0.0481125f * std::sqrt(dx * dx + dy * dy);   // sqrt(3)/36
    if (depth >= 12 || err <= tolPx) {
        glr::Curve c; c.type = 2;
        c.p[0] = p0.x; c.p[1] = p0.y;
        c.p[2] = (3.f * (p1.x + p2.x) - (p0.x + p3.x)) * 0.25f;
        c.p[3] = (3.f * (p1.y + p2.y) - (p0.y + p3.y)) * 0.25f;
        c.p[4] = p3.x; c.p[5] = p3.y;
        out.push_back(c);
        return;
    }
    const Vec2 p01 = segMid(p0, p1), p12 = segMid(p1, p2), p23 = segMid(p2, p3);
    const Vec2 p012 = segMid(p01, p12), p123 = segMid(p12, p23);
    const Vec2 p0123 = segMid(p012, p123);
    cubicToQuads(out, p0, p01, p012, p0123, tolPx, depth + 1);
    cubicToQuads(out, p0123, p123, p23, p3, tolPx, depth + 1);
}

namespace detail {

// CPU coverage fallback (used when the GPU analytic path is unavailable, e.g.
// OpenGL ES 2 / WebGL1). Rasterizes the outline with FreeType's smooth renderer
// into a w*h, top-down, 8-bit coverage bitmap, transformed into the SAME device
// cell the GPU path uses (pad, y-flip, scale), so the composited result matches.
bool rasterOutlineCPU(FT_Library lib, const FT_Outline* ol,
                             int w, int h, float sx, float sy,
                             float bx0, float by1, int pad,
                             std::vector<unsigned char>& a8) {
    if (!lib || !ol || ol->n_points <= 0 || ol->n_contours <= 0)
        return false;

    // design point (X,Y) -> FT 26.6: x = X*sx + tx, y = Y*sy + ty (y-up; FT fills
    // the bitmap top-down so the glyph top lands on row ~pad, matching the GPU cell).
    const float tx = (float)pad - bx0 * sx;
    const float ty = (float)h - (float)pad - by1 * sy;

    std::vector<FT_Vector> pts((size_t)ol->n_points);
    for (int i = 0; i < ol->n_points; ++i) {
        const float X = (float)ol->points[i].x;
        const float Y = (float)ol->points[i].y;
        pts[i].x = (FT_Pos)std::lround((X * sx + tx) * 64.0f);
        pts[i].y = (FT_Pos)std::lround((Y * sy + ty) * 64.0f);
    }

    FT_Outline oc;
    oc.n_contours = ol->n_contours;
    oc.n_points   = ol->n_points;
    oc.points     = pts.data();
    oc.tags       = ol->tags;       // fill flags + on/off-curve tags are unchanged
    oc.contours   = ol->contours;
    oc.flags      = ol->flags;      // carries the (non-zero / even-odd) fill rule

    a8.assign((size_t)w * h, 0);
    FT_Bitmap bmp;
    std::memset(&bmp, 0, sizeof(bmp));
    bmp.rows       = (unsigned)h;
    bmp.width      = (unsigned)w;
    bmp.pitch      = w;             // positive pitch -> rows stored top-down
    bmp.num_grays  = 256;
    bmp.pixel_mode = FT_PIXEL_MODE_GRAY;
    bmp.buffer     = a8.data();
    return FT_Outline_Get_Bitmap(lib, &oc, &bmp) == 0;
}

} // namespace detail

// ============================================================================
// Knot-lattice axis morphing (O(N) main-effects form)
//
// Within an axis "cell" free of variation knots, every OpenType region scalar is
// linear, so the summed outline is multilinear there. The full multilinear blend
// needs 2^N corners; instead we sample the base + N single-axis "main-effect"
// deltas (O(N)), reconstruct additively, and bound the residual cross-axis
// coupling by adaptive cell refinement (FreeType is the authoritative reference). This scales to
// high-axis parametric fonts (Roboto Flex, 13 axes) where 2^N is hopeless.
// Dragging an axis is then a cheap blend of cached control points; we re-sample
// only when the axis vector leaves the cell.
//
// We work in FreeType's normalized BLEND space (post-avar), so the avar remap is
// absorbed at sample time. To stay exact even for fonts with INTERIOR masters
// (region peaks at non-±1 coords), the cell is refined adaptively: starting from
// the {-1,0}/{0,1} bracket we bisect toward the current point until a center probe
// matches FreeType within tolerance — i.e. until the cell is knot-free. FreeType
// is the authoritative reference, so this is font-agnostic and cannot silently drift.
// ============================================================================

// Fill norm[] with the current axis values mapped to FreeType's normalized BLEND
// coordinates ([-1,1], post-avar). We do the cell/blend math in this space (not
// design units) so the avar remap is absorbed by FreeType at sample time and the
// blend stays linear where the region scalars are — eliminating avar drift.
static void currentBlendCoords(Face* f, float* norm) {
    const int n = (int)f->axes.size();
    std::vector<FT_Fixed> d((size_t)n), b((size_t)n);
    for (int i = 0; i < n; ++i)
        d[i] = valueToFixed(std::clamp(f->axes[i].Value, f->axes[i].Min, f->axes[i].Max));
    FT_Set_Var_Design_Coordinates(f->ftFace, (FT_UInt)n, d.data());
    FT_Get_Var_Blend_Coordinates(f->ftFace, (FT_UInt)n, b.data());
    for (int i = 0; i < n; ++i)
        norm[i] = (float)b[i] / 65536.f;
}

static inline int segPts(SegType t) {
    return (t == SegType::Line) ? 2 : (t == SegType::Quad) ? 3 : 4;
}

// Sample a glyph outline + advance at one blend-space coordinate vector.
// This is the only FreeType ground-truth sample call; everything else is a CPU blend.
static bool sampleOutlineAt(Face* f, FT_UInt gi, int n, const float* coord,
                            GlyphCurves& out, float& adv) {
    ++f->morphFtSamples;
    std::vector<FT_Fixed> c((size_t)n);
    for (int i = 0; i < n; ++i) c[i] = valueToFixed(coord[i]);
    FT_Set_Var_Blend_Coordinates(f->ftFace, (FT_UInt)n, c.data());
    if (FT_Load_Glyph(f->ftFace, gi, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING) != 0)
        return false;
    adv = (float)f->ftFace->glyph->advance.x;
    out.segs.clear();
    if (f->ftFace->glyph->format == FT_GLYPH_FORMAT_OUTLINE)
        extractCurves(&f->ftFace->glyph->outline, out);
    return true;
}

// Sample base (all axes at lo) + each axis's main-effect delta (that axis at hi,
// the rest at lo) for the bracket [lo,hi]. Axes whose delta is below the noise
// floor are marked inactive so they cost nothing and never trigger a re-sample.
// O(active axes + 1) FreeType loads instead of the 2^n full corner lattice.
static bool sampleLinearCell(Face* f, FT_UInt gi, int n,
                             const float* lo, const float* hi,
                             MorphGlyphCell& cell, float activeEps,
                             bool refineMask = false) {
    // On refinement re-samples we already know which axes the glyph responds to
    // in this region: an axis whose main-effect delta is below the noise floor
    // over the full bracket stays inert in any sub-bracket (the response is
    // linear within a knot-free cell, so halving the bracket halves the delta),
    // and reconstructBlend skips inactive axes outright. So we sample only the
    // known-active axes' deltas and skip a FreeType load for each inert axis —
    // the dominant cost on high-axis parametric fonts where most axes don't move
    // a given glyph.
    bool wasActive[kMorphMaxAxes];
    if (refineMask)
        for (int i = 0; i < n; ++i) wasActive[i] = cell.active[i];

    float coord[kMorphMaxAxes];
    for (int i = 0; i < n; ++i) coord[i] = lo[i];
    if (!sampleOutlineAt(f, gi, n, coord, cell.base, cell.baseAdv)) return false;

    cell.delta.assign((size_t)n, GlyphCurves{});
    cell.deltaAdv.assign((size_t)n, 0.f);
    for (int i = 0; i < n; ++i) cell.active[i] = false;
    if (cell.base.segs.empty()) return true;   // empty glyph (e.g. space)

    for (int i = 0; i < n; ++i) {
        if (hi[i] == lo[i]) continue;          // zero-width axis: inert here
        if (refineMask && !wasActive[i]) continue;   // known inert -> stays inert (frac=0)
        coord[i] = hi[i];
        GlyphCurves pi; float adv = 0.f;
        const bool okSample = sampleOutlineAt(f, gi, n, coord, pi, adv);
        coord[i] = lo[i];
        if (!okSample) return false;
        if (pi.segs.size() != cell.base.segs.size()) return false;  // topology guard

        GlyphCurves& d = cell.delta[i];
        d.segs.assign(cell.base.segs.size(), GlyphSeg{});
        float mx = 0.f;
        for (size_t s = 0; s < d.segs.size(); ++s) {
            if (pi.segs[s].type != cell.base.segs[s].type) return false;
            d.segs[s].type = cell.base.segs[s].type;
            const int np = (d.segs[s].type == SegType::Line) ? 2
                         : (d.segs[s].type == SegType::Quad) ? 3 : 4;
            for (int j = 0; j < np; ++j) {
                const float dx = pi.segs[s].p[j].x - cell.base.segs[s].p[j].x;
                const float dy = pi.segs[s].p[j].y - cell.base.segs[s].p[j].y;
                d.segs[s].p[j] = Vec2(dx, dy);
                mx = std::max(mx, std::max(std::fabs(dx), std::fabs(dy)));
            }
        }
        cell.deltaAdv[i] = adv - cell.baseAdv;
        // An axis found relevant at the wide bracket must stay active through
        // refinement: halving shrinks its delta, so it can dip below activeEps, but
        // declassifying it to inert sets its reuse bracket to the full axis and the
        // cell would then be reused (frozen at base) while that axis is dragged.
        cell.active[i]   = (mx > activeEps) || (refineMask && wasActive[i]);
    }
    return true;
}

// Sample the kept order-2 pair corners for the bracket [lo,hi] and form the
// interaction deltas Δ_ij = P(e_i+e_j) - P(e_i) - P(e_j) + base (the mixed second
// difference). Requires base + per-axis deltas already sampled. A pair whose
// endpoints are degenerate falls back to a zero term (harmless).
static bool sampleCellPairs(Face* f, FT_UInt gi, int n,
                            const float* lo, const float* hi, MorphGlyphCell& cell) {
    const size_t np = cell.pairs.size();
    cell.pairDelta.assign(np, GlyphCurves{});
    cell.pairAdv.assign(np, 0.f);
    const size_t segN = cell.base.segs.size();
    if (segN == 0) return true;

    float coord[kMorphMaxAxes];
    for (int i = 0; i < n; ++i) coord[i] = lo[i];
    for (size_t k = 0; k < np; ++k) {
        const int i = cell.pairs[k].i, j = cell.pairs[k].j;
        GlyphCurves& d = cell.pairDelta[k];
        d.segs.assign(segN, GlyphSeg{});
        for (size_t s = 0; s < segN; ++s) d.segs[s].type = cell.base.segs[s].type;

        const GlyphCurves& di = cell.delta[i];
        const GlyphCurves& dj = cell.delta[j];
        if (di.segs.size() != segN || dj.segs.size() != segN)
            continue;                               // axis degenerate here -> zero term

        coord[i] = hi[i]; coord[j] = hi[j];
        GlyphCurves pij; float adv = 0.f;
        const bool ok = sampleOutlineAt(f, gi, n, coord, pij, adv);
        coord[i] = lo[i]; coord[j] = lo[j];
        if (!ok || pij.segs.size() != segN) return false;

        for (size_t s = 0; s < segN; ++s) {
            if (pij.segs[s].type != cell.base.segs[s].type) return false;
            const int q = segPts(d.segs[s].type);
            for (int p = 0; p < q; ++p) {
                d.segs[s].p[p].x = pij.segs[s].p[p].x - cell.base.segs[s].p[p].x
                                 - di.segs[s].p[p].x - dj.segs[s].p[p].x;
                d.segs[s].p[p].y = pij.segs[s].p[p].y - cell.base.segs[s].p[p].y
                                 - di.segs[s].p[p].y - dj.segs[s].p[p].y;
            }
        }
        cell.pairAdv[k] = adv - cell.baseAdv - cell.deltaAdv[i] - cell.deltaAdv[j];
    }
    return true;
}

// Largest |component| of an interaction delta (font units) — used to prune pairs.
static float curvesMaxAbs(const GlyphCurves& g) {
    float mx = 0.f;
    for (const GlyphSeg& sg : g.segs) {
        const int q = segPts(sg.type);
        for (int p = 0; p < q; ++p)
            mx = std::max(mx, std::max(std::fabs(sg.p[p].x), std::fabs(sg.p[p].y)));
    }
    return mx;
}

static void recomputeBbox(GlyphCurves& g) {
    g.bboxMin = Vec2( 1e30f,  1e30f);
    g.bboxMax = Vec2(-1e30f, -1e30f);
    for (const GlyphSeg& sg : g.segs) {
        const int np = (sg.type == SegType::Line) ? 2 : (sg.type == SegType::Quad) ? 3 : 4;
        for (int j = 0; j < np; ++j) {
            g.bboxMin.x = std::min(g.bboxMin.x, sg.p[j].x);
            g.bboxMin.y = std::min(g.bboxMin.y, sg.p[j].y);
            g.bboxMax.x = std::max(g.bboxMax.x, sg.p[j].x);
            g.bboxMax.y = std::max(g.bboxMax.y, sg.p[j].y);
        }
    }
}

// out = base + Σ_i frac_i·Δ_i + Σ_pairs frac_i·frac_j·Δ_ij. The order-1
// main-effects part is O(active axes); the order-2 part adds the few kept coupled
// pairs (usually empty). Cross-axis coupling beyond the kept pairs and any knots
// are handled by adaptive cell refinement.
static void reconstructBlend(const MorphGlyphCell& cell, int n,
                             const float* frac, GlyphCurves& out, float* advOut) {
    out.segs = cell.base.segs;
    float adv = cell.baseAdv;
    for (int i = 0; i < n; ++i) {
        if (!cell.active[i]) continue;
        const float fr = frac[i];
        if (fr == 0.f) continue;
        adv += fr * cell.deltaAdv[i];
        const GlyphCurves& d = cell.delta[i];
        for (size_t s = 0; s < out.segs.size(); ++s) {
            const int q = segPts(out.segs[s].type);
            for (int j = 0; j < q; ++j) {
                out.segs[s].p[j].x += fr * d.segs[s].p[j].x;
                out.segs[s].p[j].y += fr * d.segs[s].p[j].y;
            }
        }
    }
    for (size_t k = 0; k < cell.pairs.size(); ++k) {
        const float w = frac[cell.pairs[k].i] * frac[cell.pairs[k].j];
        if (w == 0.f) continue;
        adv += w * cell.pairAdv[k];
        const GlyphCurves& d = cell.pairDelta[k];
        if (d.segs.size() != out.segs.size()) continue;
        for (size_t s = 0; s < out.segs.size(); ++s) {
            const int q = segPts(out.segs[s].type);
            for (int j = 0; j < q; ++j) {
                out.segs[s].p[j].x += w * d.segs[s].p[j].x;
                out.segs[s].p[j].y += w * d.segs[s].p[j].y;
            }
        }
    }
    recomputeBbox(out);
    if (advOut) *advOut = adv;
}

// Bbox + advance ONLY, without materializing the blended outline. Used when the
// GPU reconstructs base + Σ weight·delta itself: the CPU just needs the cell's
// tight design-unit bounds (for cell size/placement) and the blended advance (for
// layout), so we accumulate each control point's blended position into a reused
// scratch buffer (axis-outer, like reconstructBlend, for cache-friendly delta
// access) and min/max it — skipping the per-frame GlyphCurves seg copy/maintenance
// the full blend would do. Bounds match reconstructBlend exactly (same weights),
// so the GPU geometry lands in exactly this cell.
static void reconstructBboxAdv(const MorphGlyphCell& cell, int n, const float* frac,
                               Vec2& bbMin, Vec2& bbMax, float* advOut) {
    static std::vector<Vec2> scratch;   // reused across calls (single-threaded path)
    const auto& bsegs = cell.base.segs;

    // Flatten base control points into the scratch buffer.
    size_t total = 0;
    for (const GlyphSeg& s : bsegs) total += (size_t)segPts(s.type);
    scratch.resize(total);
    {
        size_t o = 0;
        for (const GlyphSeg& s : bsegs) {
            const int q = segPts(s.type);
            for (int j = 0; j < q; ++j) scratch[o++] = s.p[j];
        }
    }

    float adv = cell.baseAdv;
    for (int i = 0; i < n; ++i) {
        if (!cell.active[i]) continue;
        const float fr = frac[i];
        if (fr == 0.f) continue;
        adv += fr * cell.deltaAdv[i];
        const GlyphCurves& d = cell.delta[i];
        if (d.segs.size() != bsegs.size()) continue;
        size_t o = 0;
        for (size_t s = 0; s < bsegs.size(); ++s) {
            const int q = segPts(bsegs[s].type);
            for (int j = 0; j < q; ++j, ++o) {
                scratch[o].x += fr * d.segs[s].p[j].x;
                scratch[o].y += fr * d.segs[s].p[j].y;
            }
        }
    }
    for (size_t k = 0; k < cell.pairs.size(); ++k) {
        const float w = frac[cell.pairs[k].i] * frac[cell.pairs[k].j];
        if (w == 0.f) continue;
        adv += w * cell.pairAdv[k];
        const GlyphCurves& d = cell.pairDelta[k];
        if (d.segs.size() != bsegs.size()) continue;
        size_t o = 0;
        for (size_t s = 0; s < bsegs.size(); ++s) {
            const int q = segPts(bsegs[s].type);
            for (int j = 0; j < q; ++j, ++o) {
                scratch[o].x += w * d.segs[s].p[j].x;
                scratch[o].y += w * d.segs[s].p[j].y;
            }
        }
    }

    bbMin = Vec2( 1e30f,  1e30f);
    bbMax = Vec2(-1e30f, -1e30f);
    for (const Vec2& p : scratch) {
        bbMin.x = std::min(bbMin.x, p.x); bbMin.y = std::min(bbMin.y, p.y);
        bbMax.x = std::max(bbMax.x, p.x); bbMax.y = std::max(bbMax.y, p.y);
    }
    if (advOut) *advOut = adv;
}

// Incrementally move the live blend 'g' from frac 'oldFrac' to 'newFrac' by adding
// (newFrac_i - oldFrac_i)·Δ_i for the axes that changed, plus the change in each
// kept pair's bilinear weight·Δ_ij. Exactly preserves g = reconstructBlend(frac)
// (modulo float rounding) but only touches the axes that actually moved -> a
// single-axis drag is O(points + pairs-with-that-axis), independent of axis count.
static void applyFracDelta(const MorphGlyphCell& cell, int n, GlyphCurves& g,
                           const float* oldFrac, const float* newFrac,
                           float& adv) {
    bool moved = false;
    for (int i = 0; i < n; ++i) {
        if (!cell.active[i]) continue;
        const float df = newFrac[i] - oldFrac[i];
        if (df == 0.f) continue;
        moved = true;
        adv += df * cell.deltaAdv[i];
        const GlyphCurves& d = cell.delta[i];
        for (size_t s = 0; s < g.segs.size(); ++s) {
            const int q = segPts(g.segs[s].type);
            for (int j = 0; j < q; ++j) {
                g.segs[s].p[j].x += df * d.segs[s].p[j].x;
                g.segs[s].p[j].y += df * d.segs[s].p[j].y;
            }
        }
    }
    for (size_t k = 0; k < cell.pairs.size(); ++k) {
        const int i = cell.pairs[k].i, j = cell.pairs[k].j;
        const float dw = newFrac[i] * newFrac[j] - oldFrac[i] * oldFrac[j];
        if (dw == 0.f) continue;
        const GlyphCurves& d = cell.pairDelta[k];
        if (d.segs.size() != g.segs.size()) continue;
        moved = true;
        adv += dw * cell.pairAdv[k];
        for (size_t s = 0; s < g.segs.size(); ++s) {
            const int q = segPts(g.segs[s].type);
            for (int p = 0; p < q; ++p) {
                g.segs[s].p[p].x += dw * d.segs[s].p[p].x;
                g.segs[s].p[p].y += dw * d.segs[s].p[p].y;
            }
        }
    }
    if (moved) recomputeBbox(g);
}

static float curvesMaxErr(const GlyphCurves& a, const GlyphCurves& b) {
    if (a.segs.size() != b.segs.size()) return 1e30f;
    float mx = 0.f;
    for (size_t s = 0; s < a.segs.size(); ++s) {
        if (a.segs[s].type != b.segs[s].type) return 1e30f;
        const int np = (a.segs[s].type == SegType::Line) ? 2
                     : (a.segs[s].type == SegType::Quad) ? 3 : 4;
        for (int j = 0; j < np; ++j) {
            const float dx = a.segs[s].p[j].x - b.segs[s].p[j].x;
            const float dy = a.segs[s].p[j].y - b.segs[s].p[j].y;
            mx = std::max(mx, std::sqrt(dx * dx + dy * dy));
        }
    }
    return mx;
}

static void restoreUserInstance(Face* f) {
    const int n = (int)f->axes.size();
    std::vector<FT_Fixed> d((size_t)n);
    for (int i = 0; i < n; ++i)
        d[i] = valueToFixed(std::clamp(f->axes[i].Value, f->axes[i].Min, f->axes[i].Max));
    FT_Set_Var_Design_Coordinates(f->ftFace, (FT_UInt)n, d.data());
}

// Normalized blend coords are identical for every glyph at a given axis vector, so
// resolve them through FreeType once and cache on the face; only the first glyph
// after an axis change pays the FT round-trip.
static const float* cachedBlendCoords(Face* f, int n) {
    if (f->morphNormValid && f->morphNormN == n) {
        bool same = true;
        for (int i = 0; i < n; ++i)
            if (f->axes[i].Value != f->morphNormVals[i]) { same = false; break; }
        if (same) return f->morphNorm;
    }
    currentBlendCoords(f, f->morphNorm);
    for (int i = 0; i < n; ++i) f->morphNormVals[i] = f->axes[i].Value;
    f->morphNormN = n;
    f->morphNormValid = true;
    return f->morphNorm;
}

// ── Per-frame render profiler ─────────────────────────────────────────────────
// Lightweight CPU timers so the example can attribute a frame's cost to the morph
// outline blend vs. the rasterization submission, and infer "GPU/other" as the
// remainder of the wall-clock frame. The accumulators reset on the first glyph of
// each host frame; GetRenderProfile() returns the last completed frame's totals.
// These measure CPU time only (the GPU runs the coverage/recon draws asynchronously),
// which is exactly what we need to separate a CPU-bound frame from a GPU-bound one.
namespace {
    using ProfClock = std::chrono::high_resolution_clock;
    struct ProfAccum { double blendMs = 0.0, rasterMs = 0.0; int glyphs = 0, rebuilds = 0; };
    ProfAccum s_prof, s_profLast;
    int s_profFrame = -1;
    int s_profSeq = 0;
    inline void profFrameTick() {
        // ensureAtlasFrame() bumps s_profSeq via noteProfFrame()
        const int fr = s_profSeq;
        if (fr != s_profFrame) { s_profLast = s_prof; s_prof = ProfAccum{}; s_profFrame = fr; }
    }
    inline void noteProfFrame(int frame) { s_profSeq = frame; }
    inline double profMsSince(ProfClock::time_point t) {
        return std::chrono::duration<double, std::milli>(ProfClock::now() - t).count();
    }
}

// Build / reuse a knot-free cell for this glyph, then blend it at the current axis
// vector and return a pointer to the cached blended outline (nullptr on failure).
// Re-samples (and adaptively refines) only when the vector leaves the cached cell;
// within the cell, dragging is a pure CPU blend (no FreeType calls) and updates
// only the axes that changed since the previous frame.
// Opt-in: reconstruct morph glyphs on the GPU (base + Σ frac·delta) instead of
// CPU-blending + re-uploading curves each frame. Default off so the proven path is
// the baseline; the example exposes a toggle for A/B.
static bool s_preferGpuMorph = false;

// Grid-fit (small-size hinting) state. See PreferGridFit() in the header. When enabled,
// glyphs below the pixel-size cutoff are rendered through FreeType's own autohinter +
// grayscale raster (GridFitMode::FreeType); at/above the cutoff it is a no-op. The cutoff
// is caller-tunable so an app can match its smallest text size.
static bool        s_gridFit       = false;
static GridFitMode s_gridFitMode   = GridFitMode::FreeType;  // mode when enabled
static float       s_gridFitMaxEmPx = 28.f;   // logical px/em; at/above this -> no-op

namespace detail {

const GlyphCurves* morphBlendGlyph(Face* f, FT_UInt gi, float* advOut,
                                          MorphGlyphCell** outCell) {
    const int n = (int)f->axes.size();
    if (n <= 0 || n > kMorphMaxAxes) return nullptr;

    const float* norm = cachedBlendCoords(f, n);

    bool extrapActive = false;
    if (f->morphExtrap)
        for (int i = 0; i < n; ++i) {
            const Axis& ax = f->axes[i];
            if (ax.Value > ax.Max + 1e-6f || ax.Value < ax.Min - 1e-6f) { extrapActive = true; break; }
        }

    MorphGlyphCell& cell = f->morphCache[gi];
    if (outCell) *outCell = &cell;

    bool reuse = cell.ok && cell.n == n && cell.builtExtrap == extrapActive;
    if (reuse)
        for (int i = 0; i < n; ++i) {
            // Brackets are signed (base==lo is the build vector, hi the far end), so
            // the reuse region is [min(lo,hi), max(lo,hi)].
            const float a = std::min(cell.lo[i], cell.hi[i]), b = std::max(cell.lo[i], cell.hi[i]);
            if (norm[i] < a - 1e-4f || norm[i] > b + 1e-4f) { reuse = false; break; }
        }

    if (!reuse) {
        ++f->morphCellBuilds;
        ++s_prof.rebuilds;
        const int   upm = f->ftFace->units_per_EM ? (int)f->ftFace->units_per_EM : 1000;
        const float tol = 0.001f * (float)upm;   // ~0.1% em; FT rounding is the floor
        const float activeEps = 0.25f;           // font units; below FT rounding -> inert axis
        const int   maxDepth = 12;

        // Anchor the base at the CURRENT vector (lo = norm) and extend the bracket
        // toward the farther adjacent master (+1 or -1). This makes the reused cell a
        // local linearization around where the user is: a single-axis drag moves only
        // that axis off frac=0 while the others stay at the base (frac=0), so the
        // bilinear cross-axis coupling — which previously dominated drags because the
        // non-dragged axes sat at a bracket corner (frac=1) — drops out entirely.
        // (The old master-anchored half-axis cell validated only its center, so reuse
        // toward the corners drifted hundreds of units and glyphs visibly lagged.)
        float lo[kMorphMaxAxes], hi[kMorphMaxAxes];
        for (int i = 0; i < n; ++i) {
            if (extrapActive) {   // master-anchored half-axis (extrap continuation needs it)
                if (norm[i] <= 0.f) { lo[i] = -1.f; hi[i] = 0.f; }
                else                { lo[i] =  0.f; hi[i] = 1.f; }
            } else {
                lo[i] = norm[i];                                       // base = query (frac 0)
                hi[i] = (1.f - norm[i] >= norm[i] + 1.f) ? 1.f : -1.f; // toward farther master
            }
        }
        cell.pairs.clear(); cell.pairDelta.clear(); cell.pairAdv.clear();
        if (!sampleLinearCell(f, gi, n, lo, hi, cell, activeEps)) {
            restoreUserInstance(f); cell.ok = false; return nullptr;
        }

        if (!extrapActive && !cell.base.segs.empty()) {
            const size_t segN = cell.base.segs.size();

            // (1) Per-axis (anisotropic) refinement. With the base anchored at the
            // current vector, a single-axis drag moves only that axis off frac=0 while
            // the others stay at the base — so each axis's reuse accuracy depends
            // solely on ITS OWN 1D nonlinearity over its bracket, with no cross-axis
            // coupling to confound a per-axis criterion (the very coupling that forced
            // the old master-anchored cell to refine every axis together). We shrink
            // each active axis's bracket toward the base only until its midpoint probe
            // (that axis at frac 0.5, others at base) is sub-pixel, then re-sample just
            // that axis's delta. This keeps every bracket as wide as its own curvature
            // allows -> the fewest cell rebuilds while dragging, still sub-pixel exact.
            auto resampleDelta = [&](int i) -> bool {
                float coord[kMorphMaxAxes];
                for (int k = 0; k < n; ++k) coord[k] = lo[k];
                coord[i] = hi[i];
                GlyphCurves pi; float adv = 0.f;
                if (!sampleOutlineAt(f, gi, n, coord, pi, adv) || pi.segs.size() != segN)
        return false;
                GlyphCurves& d = cell.delta[i];
                for (size_t s = 0; s < segN; ++s) {
                    if (pi.segs[s].type != cell.base.segs[s].type) return false;
                    const int q = segPts(cell.base.segs[s].type);
                    for (int j = 0; j < q; ++j) {
                        d.segs[s].p[j].x = pi.segs[s].p[j].x - cell.base.segs[s].p[j].x;
                        d.segs[s].p[j].y = pi.segs[s].p[j].y - cell.base.segs[s].p[j].y;
                    }
                }
                cell.deltaAdv[i] = adv - cell.baseAdv;
                return true;
            };
            // The chord fit is exact at the endpoints (base and the re-sampled hi), so
            // the midpoint is the natural single probe for the bracket's nonlinearity;
            // a knot inside the bracket bends the response away from the chord there and
            // is bisected out. (A multi-fraction probe set is supported but measured no
            // better on Roboto Flex while costing proportionally more FreeType loads.)
            static const float kProbeFr[] = { 0.5f };
            for (int i = 0; i < n; ++i) {
                if (!cell.active[i]) continue;
                for (int depth = 0; depth < maxDepth; ++depth) {
                    const GlyphCurves& d = cell.delta[i];
                    float err = 0.f;
                    for (float pf : kProbeFr) {
                        float coord[kMorphMaxAxes];
                        for (int k = 0; k < n; ++k) coord[k] = lo[k];
                        coord[i] = lo[i] + pf * (hi[i] - lo[i]);   // axis i probe, others at base
                        GlyphCurves tC; float tadv = 0.f;
                        if (!sampleOutlineAt(f, gi, n, coord, tC, tadv) || tC.segs.size() != segN) {
                            err = 0.f; break;
                        }
                        for (size_t s = 0; s < segN; ++s) {
                            const int q = segPts(cell.base.segs[s].type);
                            for (int j = 0; j < q; ++j) {
                                const float rx = cell.base.segs[s].p[j].x + pf * d.segs[s].p[j].x;
                                const float ry = cell.base.segs[s].p[j].y + pf * d.segs[s].p[j].y;
                                err = std::max(err, std::max(std::fabs(rx - tC.segs[s].p[j].x),
                                                       std::fabs(ry - tC.segs[s].p[j].y)));
                            }
                        }
                    }
                    if (err <= tol) break;
                    hi[i] = 0.5f * (lo[i] + hi[i]);
                    ++f->morphBisectSteps;
                    if (!resampleDelta(i)) { restoreUserInstance(f); cell.ok = false; return nullptr; }
                }
            }

            // (2) Order-2 detection on the refined brackets. Single-axis drags are now
            // exact; this captures the few dominant coupled pairs for a simultaneous
            // multi-axis move within the cell, but only if they bring the all-axes
            // center probe within tolerance. Sampled at the FINAL brackets so the pair
            // deltas stay consistent with reconstruction. Usually empty.
            float ctr[kMorphMaxAxes], frc[kMorphMaxAxes];
            bool any = false;
            for (int i = 0; i < n; ++i) {
                ctr[i] = 0.5f * (lo[i] + hi[i]);
                frc[i] = cell.active[i] ? 0.5f : 0.f;
                any |= cell.active[i];
            }
            if (any) {
                GlyphCurves bC, tC; float tadv = 0.f;
                reconstructBlend(cell, n, frc, bC, nullptr);
                if (sampleOutlineAt(f, gi, n, ctr, tC, tadv) && curvesMaxErr(bC, tC) > tol) {
                    int idx[kMorphMaxAxes], m = 0;
                    for (int i = 0; i < n; ++i) if (cell.active[i]) idx[m++] = i;
                    auto mag = [&](int a) { return curvesMaxAbs(cell.delta[a]); };
                    for (int a = 0; a < m; ++a)
                        for (int b = a + 1; b < m; ++b)
                            if (mag(idx[b]) > mag(idx[a])) std::swap(idx[a], idx[b]);
                    const int K = std::min(m, kMorphOrder2TopK);
                    for (int a = 0; a < K; ++a)
                        for (int b = a + 1; b < K; ++b)
                            cell.pairs.push_back(MorphPair{ std::min(idx[a], idx[b]), std::max(idx[a], idx[b]) });

                    bool keep = false;
                    if (!cell.pairs.empty() && sampleCellPairs(f, gi, n, lo, hi, cell)) {
                        std::vector<MorphPair> kp; std::vector<GlyphCurves> kd; std::vector<float> ka;
                        for (size_t k = 0; k < cell.pairs.size(); ++k)
                            if (curvesMaxAbs(cell.pairDelta[k]) > activeEps) {
                                kp.push_back(cell.pairs[k]);
                                kd.push_back(cell.pairDelta[k]);
                                ka.push_back(cell.pairAdv[k]);
                            }
                        cell.pairs = kp; cell.pairDelta = kd; cell.pairAdv = ka;
                        if (!cell.pairs.empty()) {
                            GlyphCurves bC2; reconstructBlend(cell, n, frc, bC2, nullptr);
                            keep = (curvesMaxErr(bC2, tC) <= tol);
                        }
                    }
                    if (!keep) { cell.pairs.clear(); cell.pairDelta.clear(); cell.pairAdv.clear(); }
                }
            }
        }
        if (!cell.pairs.empty()) f->morphPairTerms += (long long)cell.pairs.size();
        // Store the build bracket for EVERY axis. An inert axis was verified inert
        // only over its bracket [lo,hi] (base==lo, hi toward the farther master) — NOT
        // over the whole axis. Giving it the full [-1,1] (the old "never re-sample"
        // shortcut) lets the cell be reused while that axis is dragged into its
        // unverified half, where it may well move the glyph: the axis then appears
        // frozen and, since base==outline(lo)≠outline(-1), reconstruction is corrupted.
        // Bounding inert reuse to the verified half forces a rebuild + re-classification
        // when the axis is dragged out of it.
        for (int i = 0; i < n; ++i) { cell.lo[i] = lo[i]; cell.hi[i] = hi[i]; }
        cell.n = n; cell.builtExtrap = extrapActive; cell.ok = true;
        cell.curValid = false;        // base/deltas changed -> live blend is stale
        cell.gpuDirty = true;         // GPU delta buffer must be re-uploaded
        restoreUserInstance(f);
    }

    float frac[kMorphMaxAxes];
    for (int i = 0; i < n; ++i) {
        if (!cell.active[i]) { frac[i] = 0.f; continue; }
        const float wdt = cell.hi[i] - cell.lo[i];   // signed (hi may be below lo)
        float fr = (std::fabs(wdt) > 1e-9f) ? (norm[i] - cell.lo[i]) / wdt : 0.f;
        fr = std::clamp(fr, 0.f, 1.f);
        if (extrapActive) {   // linear continuation beyond outer master (wdt == 1)
            const Axis& ax = f->axes[i];
            if (ax.Value > ax.Max && ax.Max > ax.Default + 1e-6f && cell.hi[i] >= 1.f - 1e-4f)
                fr += (ax.Value - ax.Max) / (ax.Max - ax.Default);
            else if (ax.Value < ax.Min && ax.Default > ax.Min + 1e-6f && cell.lo[i] <= -1.f + 1e-4f)
                fr -= (ax.Min - ax.Value) / (ax.Default - ax.Min);
            fr = std::clamp(fr, -6.f, 6.f);
        }
        frac[i] = fr;
    }

    // When the GPU reconstructs this glyph (opt-in GPU morph, glyph eligible), the
    // CPU outline blend is redundant: the shader rebuilds base + Σ weight·delta from
    // the static delta buffer using these same fractions. So we skip the full
    // per-frame seg blend and compute only the cell bbox + advance (which the CPU
    // still needs to size/place the cell and lay out the line). gpuOk is set by the
    // first buildMorphGpuData; on the rebuild frame it is still false, so that frame
    // pays a full reconstruct (harmless — the base sample just happened anyway) and
    // every reused frame after takes the cheap path. curFrac is still updated below
    // because the GPU render path reads it to form the per-term weights.
    const bool gpuReconstruct =
        s_preferGpuMorph && glr::MorphReady() && !extrapActive && cell.gpuOk;

    if (gpuReconstruct) {
        reconstructBboxAdv(cell, n, frac, cell.cur.bboxMin, cell.cur.bboxMax, &cell.curAdv);
        cell.curBboxOnly = true;
        cell.curValid    = false;   // cur.segs intentionally not maintained
        cell.curSteps    = 0;
    } else {
        // Update the live blend. Count how many active axes actually moved: a fresh
        // cell, a big jump (most axes changed, e.g. the validation harness), or a long
        // incremental run all trigger a full reconstruct (which also resets float
        // drift); a typical single-axis drag updates just the moved axis in O(points).
        int changed = 0, activeN = 0;
        if (cell.curValid && !cell.curBboxOnly)
            for (int i = 0; i < n; ++i) {
                if (!cell.active[i]) continue;
                ++activeN;
                if (frac[i] != cell.curFrac[i]) ++changed;
            }
        const bool fullRebuild = !cell.curValid || cell.curBboxOnly ||
                                 changed * 2 > activeN || cell.curSteps >= 256;
        if (fullRebuild) {
            reconstructBlend(cell, n, frac, cell.cur, &cell.curAdv);
            cell.curSteps = 0;
        } else {
            applyFracDelta(cell, n, cell.cur, cell.curFrac, frac, cell.curAdv);
            ++cell.curSteps;
        }
        cell.curBboxOnly = false;
        cell.curValid    = true;
    }
    for (int i = 0; i < n; ++i) cell.curFrac[i] = frac[i];

    if (advOut) *advOut = cell.curAdv;
    return &cell.cur;       // advance is valid even for empty (space) glyphs
}

} // namespace detail

static bool morphIsActive(const Face* f, bool hinted) {
    return f && f->morphEnabled && f->isVariable && !hinted &&
           !f->axes.empty() && (int)f->axes.size() <= kMorphMaxAxes &&
           glr::LoopBlinnReady();
}

// Build and upload a (re)built cell's static GPU delta buffer (design-unit base +
// active-axis deltas as quads) plus its conservative bbox. Quad-only: lines become
// degenerate quads (control = midpoint); a cubic makes the glyph GPU-ineligible
// (rendered via CPU reconstruction). Returns cell.gpuOk. Must run with a GL context.
static bool buildMorphGpuData(MorphGlyphCell& cell, int n) {
    cell.gpuOk = false;
    if (cell.base.segs.empty()) return false;
    // Extrapolation cells can drive fractions outside [0,1], which breaks the
    // conservative-bbox assumption below (and is a rare edge case), so leave them on
    // the CPU path.
    if (cell.builtExtrap) return false;
    const int maxTerms = std::min(glr::MorphMaxTerms(), kMorphMaxGpuTerms);

    const size_t segN = cell.base.segs.size();
    auto toQuad = [](const GlyphSeg& s, glr::Curve& c) -> bool {
        c.type = 2;
        if (s.type == SegType::Line) {
            c.p[0] = s.p[0].x;                  c.p[1] = s.p[0].y;
            c.p[2] = 0.5f * (s.p[0].x + s.p[1].x);
            c.p[3] = 0.5f * (s.p[0].y + s.p[1].y);
            c.p[4] = s.p[1].x;                  c.p[5] = s.p[1].y;
            return true;
        }
        if (s.type == SegType::Quad) {
            c.p[0] = s.p[0].x; c.p[1] = s.p[0].y;
            c.p[2] = s.p[1].x; c.p[3] = s.p[1].y;
            c.p[4] = s.p[2].x; c.p[5] = s.p[2].y;
            return true;
        }
        return false;                                  // cubic -> ineligible
    };

    std::vector<glr::Curve> baseQ(segN);
    for (size_t s = 0; s < segN; ++s)
        if (!toQuad(cell.base.segs[s], baseQ[s])) return false;

    // Assemble correction terms: order-1 active-axis main effects, then order-2 kept
    // pairs. Each term's delta is converted to quad space the SAME way as the base
    // (the line->midpoint map is linear, so quote(base+Δ)-quote(base) is exact), and
    // gpuTermA/B record which fractions form its weight at render time (axis: frac[a];
    // pair: frac[a]·frac[b]).
    int termN = 0, termA[kMorphMaxGpuTerms], termB[kMorphMaxGpuTerms];
    std::vector<glr::Curve> deltaQ;
    deltaQ.reserve((size_t)maxTerms * segN);

    auto addTerm = [&](const GlyphCurves& d, int a, int b) -> bool {
        if (termN >= maxTerms) return false;           // too many terms -> CPU path
        if (d.segs.size() != segN) return false;
        for (size_t s = 0; s < segN; ++s) {
            GlyphSeg sum = cell.base.segs[s];
            const int q = (sum.type == SegType::Line) ? 2
                        : (sum.type == SegType::Quad) ? 3 : 4;
            for (int k = 0; k < q; ++k) {
                sum.p[k].x += d.segs[s].p[k].x;
                sum.p[k].y += d.segs[s].p[k].y;
            }
            glr::Curve cs;
            if (!toQuad(sum, cs)) return false;
            glr::Curve dc; dc.type = 2;
            for (int j = 0; j < 6; ++j) dc.p[j] = cs.p[j] - baseQ[s].p[j];
            deltaQ.push_back(dc);
        }
        termA[termN] = a; termB[termN] = b; ++termN;
        return true;
    };

    for (int i = 0; i < n; ++i)
        if (cell.active[i] && !addTerm(cell.delta[i], i, -1))
            return false;
    for (size_t k = 0; k < cell.pairs.size(); ++k)
        if (!addTerm(cell.pairDelta[k], cell.pairs[k].i, cell.pairs[k].j))
            return false;

    if (!glr::UpdateMorphCurves(&cell.gpuTex, baseQ.data(),
                                termN ? deltaQ.data() : nullptr, (int)segN, termN))
        return false;
    cell.gpuCount = (int)segN;
    cell.gpuTermN = termN;
    for (int t = 0; t < termN; ++t) { cell.gpuTermA[t] = termA[t]; cell.gpuTermB[t] = termB[t]; }
    cell.gpuOk = true;
    return true;
}

namespace detail {

// Set the FreeType face to the current (live) variation instance, so a glyph loaded
// through FreeType (e.g. the autohinter raster path) matches the morph being shown.
void setFaceVarToCurrent(Face* f) {
    const int n = (int)f->axes.size();
    if (n <= 0 || n > kMorphMaxAxes) return;
    FT_Fixed cur[kMorphMaxAxes];
    for (int i = 0; i < n; ++i)
        cur[i] = valueToFixed(std::clamp(f->axes[i].Value, f->axes[i].Min, f->axes[i].Max));
    FT_Set_Var_Design_Coordinates(f->ftFace, (FT_UInt)n, cur);
}

} // namespace detail

// Host compositing: filled quads via emitter; outlines via optional PathEmitter.
static int              s_atlasFrame       = -1;
static int              s_hostFrame        = -1;
static bool             s_useHostFrame     = false;
static float            s_fbScaleOverride  = 0.f;
static EmitGlyphQuadFn  s_emitGlyphQuad    = nullptr;
static void*            s_emitGlyphUser    = nullptr;
static PathEmitter      s_pathEmitterStore {};
static const PathEmitter* s_pathEmitter    = nullptr;
static int              s_autoFrame        = 0;

void BeginHostFrame(int frame_index, float framebuffer_scale) {
    s_useHostFrame = true;
    s_hostFrame    = frame_index;
    if (framebuffer_scale > 0.f)
        s_fbScaleOverride = framebuffer_scale;
    if (frame_index != s_atlasFrame) {
        glr::BeginFrame();
        s_atlasFrame = frame_index;
    }
    noteProfFrame(frame_index);
}

void SetGlyphQuadEmitter(EmitGlyphQuadFn fn, void* user) {
    s_emitGlyphQuad = fn;
    s_emitGlyphUser = user;
}

void SetPathEmitter(const PathEmitter* emitter) {
    if (emitter) {
        s_pathEmitterStore = *emitter;
        s_pathEmitter = &s_pathEmitterStore;
    } else {
        s_pathEmitter = nullptr;
    }
}

EmitGlyphQuadFn GetGlyphQuadEmitter(void** out_user) {
    if (out_user)
        *out_user = s_emitGlyphUser;
    return s_emitGlyphQuad;
}

const PathEmitter* GetPathEmitter() {
    return s_pathEmitter;
}

// Apply any pending atlas recycle exactly once per frame, before drawing —
// keeps recycling off the hot path and safe w.r.t. already-emitted quads.
static void syncAtlasFrame() {
    const int frame = s_useHostFrame ? s_hostFrame : s_autoFrame;
    if (frame != s_atlasFrame) {
        glr::BeginFrame();
        s_atlasFrame = frame;
    }
    noteProfFrame(frame);
}

// Start of a DrawString batch. A host that never calls BeginHostFrame gets one
// synthetic frame per batch, so live atlas pages still recycle.
static void ensureAtlasFrame() {
    if (!s_useHostFrame)
        ++s_autoFrame;
    syncAtlasFrame();
}

static void fillGlyphAnalytic(Face* face, FT_UInt gi,
                              const FT_Outline* ol, bool hinted, float em_px,
                              float originX, float originY,
                              float scale, float extrapX, float extrapY, uint32_t col,
                              const GlyphCurves* morphGc = nullptr,
                              MorphGlyphCell* morphCell = nullptr) {
    if (!glr::Ready())
        return;
    if (!s_emitGlyphQuad)
        return;

    syncAtlasFrame();

    float fbScale = (s_fbScaleOverride > 0.f) ? s_fbScaleOverride : 1.f;

    GlyphCurves tmp;
    const GlyphCurves& gc = morphGc ? *morphGc
                          : hinted  ? (extractCurves(ol, tmp), tmp)
                                    : getCurvesCached(face, gi, ol);
    if (gc.empty())
        return;

    // GPU morph reconstruction: refresh the cell's static delta buffer on a rebuild.
    // We still size/place the cell with the live blended bbox (gc) — the same tight
    // bbox the CPU path uses — since the GPU reconstructs exactly that geometry; this
    // keeps cells tight (no fill inflation) and tracking the morph. Falls back
    // transparently if the glyph is ineligible.
    bool useGpuMorph = false;
    if (morphCell && morphGc && s_preferGpuMorph && glr::MorphReady()) {
        if (morphCell->gpuDirty) {
            buildMorphGpuData(*morphCell, morphCell->n);
            morphCell->gpuDirty = false;
        }
        useGpuMorph = morphCell->gpuOk;
    }

    const float bx0 = gc.bboxMin.x, by0 = gc.bboxMin.y;
    const float bx1 = gc.bboxMax.x, by1 = gc.bboxMax.y;
    if (bx1 <= bx0 || by1 <= by0)
        return;

    // device-pixel transform (cell space, y-down): cx = pad + (x-bx0)*sx,
    // cy = pad + (by1-y)*sy.
    const float sx = scale * extrapX * fbScale;
    const float sy = scale * extrapY * fbScale;
    if (sx <= 0.f || sy <= 0.f)
        return;

    const int   pad = 2;

    // ---- small-size hinting (FreeType raster) ---------------------------------
    // Below the cutoff, mode FreeType renders the glyph through FreeType's own
    // autohinter + grayscale raster straight into the glyph cache — literally
    // FreeType, no shape distortion. It runs both static and under a live morph (the
    // glyph is re-rastered at the current instance each frame; at these sizes that is
    // microseconds per glyph). Above the cutoff, or in mode Off, the analytic outline /
    // GPU morph path is used unchanged.
    const bool ftRaster = em_px < s_gridFitMaxEmPx &&
                          s_gridFitMode == GridFitMode::FreeType &&
                          face->ftFace != nullptr;

    const int   w    = (int)std::ceil((bx1 - bx0) * sx) + 2 * pad;
    const int   h    = (int)std::ceil((float)pad + (by1 - by0) * sy) + pad;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
        return;

    const float gammaFit = 1.f;   // coverage gamma (1 = linear; no stem darkening)

    // The atlas owns all glyph textures; invalidating just drops cache entries
    // (no texture deletion). Stale entries are also caught by the atlas-gen check
    // below, in case the atlas recycled since they were rendered.
    if (face->glyphTexCacheGen != face->outlineGen) {
        face->glyphTexCache.clear();
        face->glyphTexCacheGen = face->outlineGen;
    }

    const uint32_t emQ = (uint32_t)(em_px * fbScale * 4.f + 0.5f);
    const uint32_t exQ = (uint32_t)(extrapX * 256.f + 0.5f);
    const uint32_t eyQ = (uint32_t)(extrapY * 256.f + 0.5f);
    // A FreeType-rastered cell depends on the variation instance (the autohinter
    // bakes the resolved instance), so it must not alias an unhinted cell or a cell
    // from another instance. gfQ folds a quantized instance hash; it is 0 otherwise.
    uint32_t gfQ = 0;
    if (ftRaster) {
        uint32_t ih = 2166136261u;
        for (const Axis& a : face->axes) {
            const uint32_t q = (uint32_t)(int)std::lround(a.Value * 16.f);
            ih = (ih ^ q) * 16777619u;
        }
        gfQ = 0x80u | ((ih & 0x3FFFFFu) << 8);
    }
    const uint64_t key = ((uint64_t)gi << 32) ^ ((uint64_t)emQ << 8) ^
                         (uint64_t)(exQ * 2654435761u ^ (eyQ * 40503u)) ^
                         ((uint64_t)gfQ * 0x9E3779B97F4A7C15ull);

    const bool morph  = (morphGc != nullptr);
    const bool live   = morph || (face->renderMode == RenderMode::LoopBlinnLive);
    // Live/morph glyphs are re-rendered every frame. Prefer the analytic signed-
    // area coverage path: it is deterministic and threshold-free (FreeType-quality
    // AA), unlike the supersampled Loop-Blinn live path whose winding-threshold
    // resolve sparkles/flickers frame to frame. Static Loop-Blinn mode keeps LB.
    // Exact-curve analytic coverage (single pass, quadratics fed directly — no
    // flattening) takes priority for live/morph glyphs when the user has opted in
    // and it is available; otherwise the signed-area coverage path handles live.
    const bool slugLive = live && glr::PreferSlug();
    const bool covLive = live && !slugLive && glr::CoverageReady();
    const bool lbMode  = !slugLive && !covLive &&
                         (morph || face->renderMode == RenderMode::LoopBlinn ||
                          face->renderMode == RenderMode::LoopBlinnLive) &&
                         glr::LoopBlinnReady();

    glr::GlyphTex tex;
    bool cached = false;
    int  ftW = 0, ftH = 0, ftLeft = 0, ftTop = 0;   // FreeType-raster cell (mode FreeType)
    if (!live) {
        auto it = face->glyphTexCache.find(key);
        if (it != face->glyphTexCache.end() && it->second.valid &&
            it->second.gen == glr::AtlasGen())
            { tex = it->second; cached = true; }
    }
    if (ftRaster) {
        // FreeType's own autohinter + grayscale raster. We use LIGHT hinting: it snaps
        // only on the Y axis (baseline / x-height / caps -> crisp), and does NO horizontal
        // hinting, so glyph widths, side bearings and advances are left exactly as designed
        // (the autohinter does not widen the 'e' relative to the 'a'). That also keeps the
        // advance equal to the unhinted width our layout already uses, so the rastered cells
        // do not collide. Sync FT to the live variation instance, raster, then reuse the
        // cached cell (static) or upload a fresh one. Under a live morph this runs every
        // frame into the transient page; at these sizes it is cheap.
        setFaceVarToCurrent(face);
        face->syncedEmPx = -1.f;   // we drive FT_Set_Pixel_Sizes directly here
        const int ppi = (std::max)(1, (int)std::lround(em_px * fbScale));
        if (FT_Set_Pixel_Sizes(face->ftFace, 0, (FT_UInt)ppi) == 0 &&
            FT_Load_Glyph(face->ftFace, gi, FT_LOAD_FORCE_AUTOHINT | FT_LOAD_TARGET_LIGHT) == 0 &&
            FT_Render_Glyph(face->ftFace->glyph, FT_RENDER_MODE_NORMAL) == 0) {
            const FT_Bitmap& bm = face->ftFace->glyph->bitmap;
            ftW    = (int)bm.width;
            ftH    = (int)bm.rows;
            ftLeft = face->ftFace->glyph->bitmap_left;
            ftTop  = face->ftFace->glyph->bitmap_top;
            if (cached) {
                // reuse cached texture; geometry recovered above for placement
            } else if (ftW > 0 && ftH > 0 && bm.buffer && bm.pixel_mode == FT_PIXEL_MODE_GRAY) {
                std::vector<unsigned char> a8((size_t)ftW * ftH, 0);
                const int pitch = bm.pitch;
                for (int r = 0; r < ftH; ++r) {
                    const unsigned char* srow = bm.buffer + (ptrdiff_t)r * pitch;
                    std::memcpy(&a8[(size_t)r * ftW], srow, (size_t)ftW);
                }
                tex = glr::UploadGlyph(a8.data(), ftW, ftH, /*live=*/live);
                if (tex.valid && !live) face->glyphTexCache[key] = tex;
            }
        }
        if (!tex.valid)
            return;
    } else if (cached) {
        // cached analytic / Loop-Blinn cell reused as-is
    } else if (useGpuMorph) {
        // GPU morph reconstruction: control points are rebuilt on the GPU from the
        // static delta buffer with the current axis fractions as uniforms — no CPU
        // outline blend, no per-frame curve upload. Just gather the active-axis
        // fractions (already computed by morphBlendGlyph) and the cell transform.
        float weight[kMorphMaxGpuTerms];
        for (int t = 0; t < morphCell->gpuTermN; ++t) {
            const int a = morphCell->gpuTermA[t], b = morphCell->gpuTermB[t];
            weight[t] = (b < 0) ? morphCell->curFrac[a]
                                : morphCell->curFrac[a] * morphCell->curFrac[b];
        }
        // Cell-pixel affine, y-up: cx = oxu + sx·x, cy = oyu + sy·y.
        const float oxu = (float)pad - bx0 * sx;
        const float oyu = (float)h - (float)pad - by1 * sy;
        tex = glr::RenderMorphGlyph(morphCell->gpuTex, morphCell->gpuCount,
                                    morphCell->gpuTermN, weight, oxu, oyu, sx, sy,
                                    w, h, gammaFit, /*live=*/true);
        if (!tex.valid && morphGc) {
            // Rare GPU-path failure: fall back to CPU-blended curves -> Slug coverage.
            // The blend ran bbox-only (cur.segs is stale), so reconstruct the real
            // outline once here from the same fractions before flattening.
            GlyphCurves fbTmp;
            const std::vector<GlyphSeg>* srcSegs = &gc.segs;
            if (morphCell->curBboxOnly) {
                reconstructBlend(*morphCell, (int)face->axes.size(),
                                 morphCell->curFrac, fbTmp, nullptr);
                srcSegs = &fbTmp.segs;
            }
            std::vector<glr::Curve> C; C.reserve(srcSegs->size());
            for (const GlyphSeg& s : *srcSegs) {
                Vec2 P[4];
                const int nn = (s.type == SegType::Line) ? 2
                             : (s.type == SegType::Quad) ? 3 : 4;
                for (int i = 0; i < nn; ++i) {
                    P[i].x = (float)pad + (s.p[i].x - bx0) * sx;
                    P[i].y = (float)pad + (by1 - s.p[i].y) * sy;
                }
                if (s.type == SegType::Line) {
                    glr::Curve cc; cc.type = 1;
                    cc.p[0]=P[0].x; cc.p[1]=P[0].y; cc.p[2]=P[1].x; cc.p[3]=P[1].y;
                    C.push_back(cc);
                } else if (s.type == SegType::Quad) {
                    glr::Curve cc; cc.type = 2;
                    cc.p[0]=P[0].x; cc.p[1]=P[0].y; cc.p[2]=P[1].x; cc.p[3]=P[1].y;
                    cc.p[4]=P[2].x; cc.p[5]=P[2].y;
                    C.push_back(cc);
                } else {
                    cubicToQuads(C, P[0], P[1], P[2], P[3], 0.2f, 0);
                }
            }
            if (!C.empty())
                tex = glr::RenderGlyphSlug(C.data(), (int)C.size(), w, h, gammaFit, true);
        }
    } else if (slugLive) {
        // Exact-curve analytic coverage: hand lines/quadratics straight to the GPU
        // as curves (cubics converted to quadratics); the fragment shader computes
        // exact-area coverage in one supersample-free pass. Transient live cell,
        // re-rendered every frame from the (CPU-blended) morph output.
        std::vector<glr::Curve> C;
        C.reserve(gc.segs.size());
        for (const GlyphSeg& s : gc.segs) {
            Vec2 P[4];
            const int n = (s.type == SegType::Line) ? 2
                        : (s.type == SegType::Quad) ? 3 : 4;
            for (int i = 0; i < n; ++i) {
                P[i].x = (float)pad + (s.p[i].x - bx0) * sx;
                P[i].y = (float)pad + (by1 - s.p[i].y) * sy;
            }
            if (s.type == SegType::Line) {
                glr::Curve cc; cc.type = 1;
                cc.p[0] = P[0].x; cc.p[1] = P[0].y;
                cc.p[2] = P[1].x; cc.p[3] = P[1].y;
                C.push_back(cc);
            } else if (s.type == SegType::Quad) {
                glr::Curve cc; cc.type = 2;
                cc.p[0] = P[0].x; cc.p[1] = P[0].y;
                cc.p[2] = P[1].x; cc.p[3] = P[1].y;
                cc.p[4] = P[2].x; cc.p[5] = P[2].y;
                C.push_back(cc);
            } else {
                cubicToQuads(C, P[0], P[1], P[2], P[3], 0.2f, 0);
            }
        }
        if (C.empty())
            return;
        tex = glr::RenderGlyphSlug(C.data(), (int)C.size(), w, h, gammaFit, live);
    } else if (covLive) {
        // Analytic signed-area coverage into a transient (live) cell: re-rendered
        // every frame for morph/animation, deterministic and flicker-free. Curves
        // are flattened to edges (cubics via quads) in cell space.
        std::vector<float> E;
        E.reserve(gc.segs.size() * 4);
        const float tolSq = 0.18f * 0.18f;
        for (const GlyphSeg& s : gc.segs) {
            Vec2 P[4];
            const int n = (s.type == SegType::Line) ? 2
                        : (s.type == SegType::Quad) ? 3 : 4;
            for (int i = 0; i < n; ++i) {
                P[i].x = (float)pad + (s.p[i].x - bx0) * sx;
                P[i].y = (float)pad + (by1 - s.p[i].y) * sy;
            }
            if (s.type == SegType::Line)      pushEdge(E, P[0], P[1]);
            else if (s.type == SegType::Quad) flatQuad(E, P[0], P[1], P[2], tolSq, 0);
            else                              flatCubic(E, P[0], P[1], P[2], P[3], tolSq, 0);
        }
        if (E.empty())
            return;
        tex = glr::RenderGlyph(E.data(), (int)(E.size() / 4), w, h, gammaFit, /*live=*/true);
    } else if (lbMode) {
        // Loop-Blinn path: hand lines and quadratics to the GPU as analytic
        // curves; cubics (CFF/OTF) are converted to analytic quadratics. In live
        // mode the cell is transient (re-rendered every frame, never cached).
        std::vector<glr::Curve> C;
        C.reserve(gc.segs.size());
        for (const GlyphSeg& s : gc.segs) {
            Vec2 P[4];
            const int n = (s.type == SegType::Line) ? 2
                        : (s.type == SegType::Quad) ? 3 : 4;
            for (int i = 0; i < n; ++i) {
                P[i].x = (float)pad + (s.p[i].x - bx0) * sx;
                P[i].y = (float)pad + (by1 - s.p[i].y) * sy;
            }
            if (s.type == SegType::Line) {
                glr::Curve cc; cc.type = 1;
                cc.p[0] = P[0].x; cc.p[1] = P[0].y;
                cc.p[2] = P[1].x; cc.p[3] = P[1].y;
                C.push_back(cc);
            } else if (s.type == SegType::Quad) {
                glr::Curve cc; cc.type = 2;
                cc.p[0] = P[0].x; cc.p[1] = P[0].y;
                cc.p[2] = P[1].x; cc.p[3] = P[1].y;
                cc.p[4] = P[2].x; cc.p[5] = P[2].y;
                C.push_back(cc);
            } else {
                cubicToQuads(C, P[0], P[1], P[2], P[3], 0.2f, 0);
            }
        }
        if (C.empty())
            return;
        tex = glr::RenderGlyphCurves(C.data(), (int)C.size(), w, h, gammaFit, live);
        if (!live)
            face->glyphTexCache[key] = tex;
    } else if (glr::CoverageReady()) {
        // GPU path: flatten curves to edges and accumulate signed coverage.
        std::vector<float> E;
        E.reserve(gc.segs.size() * 4);
        const float tolSq = 0.18f * 0.18f;
        for (const GlyphSeg& s : gc.segs) {
            Vec2 P[4];
            const int n = (s.type == SegType::Line) ? 2
                        : (s.type == SegType::Quad) ? 3 : 4;
            for (int i = 0; i < n; ++i) {
                P[i].x = (float)pad + (s.p[i].x - bx0) * sx;
                P[i].y = (float)pad + (by1 - s.p[i].y) * sy;
            }
            if (s.type == SegType::Line)      pushEdge(E, P[0], P[1]);
            else if (s.type == SegType::Quad) flatQuad(E, P[0], P[1], P[2], tolSq, 0);
            else                              flatCubic(E, P[0], P[1], P[2], P[3], tolSq, 0);
        }
        if (E.empty())
            return;
        tex = glr::RenderGlyph(E.data(), (int)(E.size() / 4), w, h, gammaFit);
        face->glyphTexCache[key] = tex;
    } else if (!morph) {
        // CPU fallback (ES2 / WebGL1 / no blendable float): FreeType rasterizes
        // the outline into the same device cell, uploaded into the atlas. Only
        // reached when not morphing and GPU coverage is unavailable.
        std::vector<unsigned char> a8;
        if (!rasterOutlineCPU(face->library, ol, w, h, sx, sy, bx0, by1, pad, a8))
            return;
        tex = glr::UploadGlyph(a8.data(), w, h);
        face->glyphTexCache[key] = tex;
    }
    if (!tex.valid)
        return;

    // Place the device cell back into logical screen space. A design point
    // (nx,ny) maps to screen (originX + nx*scale*extrapX, originY - ny*scale*extrapY).
    const float sxl = scale * extrapX;

    float        pminX, pminY;
    int          cellW = w, cellH = h;
    if (ftRaster) {
        // FreeType places the bitmap by integer pen bearings on a snapped baseline.
        const float bDev = std::round(originY * fbScale);
        pminX = originX + (float)ftLeft / fbScale;
        pminY = (bDev - (float)ftTop) / fbScale;
        cellW = ftW; cellH = ftH;
    } else {
        const float syl = scale * extrapY;
        pminX = (originX + bx0 * sxl) - (float)pad / fbScale;
        pminY = (originY - by1 * syl) - (float)pad / fbScale;
    }
    const Vec2 pmin(pminX, pminY);
    const Vec2 pmax(pmin.x + (float)cellW / fbScale, pmin.y + (float)cellH / fbScale);

    // GlyphTex UVs are pre-oriented: (u0,v0)->pmin (top-left), (u1,v1)->pmax.
    {
        GlyphQuad q;
        q.tex = tex.tex;
        q.x0  = pmin.x;
        q.y0  = pmin.y;
        q.x1  = pmax.x;
        q.y1  = pmax.y;
        q.u0  = tex.u0;
        q.v0  = tex.v0;
        q.u1  = tex.u1;
        q.v1  = tex.v1;
        q.col = (uint32_t)col;
        s_emitGlyphQuad(q, s_emitGlyphUser);
    }
}

bool InitRenderer(void* (*gl_get_proc_address)(const char*)) {
    return glr::Init((glr::GLProc)gl_get_proc_address);
}
void ShutdownRenderer() {
    glr::Shutdown();
}
bool RendererReady() {
    return glr::Ready();
}
void ForceCpuFallback(bool enable) {
    glr::SetForceCpuFallback(enable);
}
void PreferSlugRenderer(bool enable) {
    glr::SetPreferSlug(enable);
}
bool PreferSlugRenderer() {
    return glr::PreferSlug();
}
bool SlugRendererAvailable() {
    return glr::SlugReady();
}

// Release every cell's GPU delta-buffer texture before dropping the cells. Safe to
// call with no GL context (handles are 0 when the GPU morph path was never used).
static void freeMorphCacheGpu(Face* face) {
    for (auto& kv : face->morphCache)
        glr::DeleteMorphCurves(&kv.second.gpuTex);
}

void PreferGpuMorphRenderer(bool enable) {
    if (s_preferGpuMorph == enable) return;
    s_preferGpuMorph = enable;
}
bool PreferGpuMorphRenderer() {
    return s_preferGpuMorph;
}
bool GpuMorphAvailable() {
    return glr::MorphReady();
}

void PreferGridFit(bool enable) {
    s_gridFit = enable;
    s_gridFitMode = enable ? GridFitMode::FreeType : GridFitMode::Off;
}
bool PreferGridFit() {
    return s_gridFit;
}

void PreferGridFitMode(GridFitMode mode) {
    s_gridFitMode = mode;
    s_gridFit = (mode != GridFitMode::Off);
}
GridFitMode PreferGridFitMode() {
    return s_gridFitMode;
}

void PreferGridFitMaxPx(float px) {
    s_gridFitMaxEmPx = std::clamp(px, 6.f, 96.f);
}
float PreferGridFitMaxPx() {
    return s_gridFitMaxEmPx;
}

void EnableMorph(Face* face, bool enable, bool allow_extrapolation) {
    if (!face) return;
    // Only discard the cached cells when the morph *mode* actually changes.
    // Callers commonly invoke this once per frame to sync UI state; wiping the
    // cache unconditionally would force a full FreeType re-sample + adaptive
    // cell rebuild for every glyph every frame, which is both wasteful and a
    // source of frame-to-frame instability (borderline refinement decisions can
    // flip between rebuilds). Axis-value changes are handled downstream by the
    // blend-coord cache and the per-cell reuse test, so they need no reset here.
    if (face->morphEnabled == enable && face->morphExtrap == allow_extrapolation)
        return;
    face->morphEnabled = enable;
    face->morphExtrap  = allow_extrapolation;
    freeMorphCacheGpu(face);
    face->morphCache.clear();   // re-sample base + deltas on next use
    face->morphNormValid = false;
}

bool MorphEnabled(const Face* face) {
    return face && face->morphEnabled;
}


struct StrokeOutlineCtx {
    float       scale       = 1.f;
    float       scaleX      = 1.f;
    float       scaleY      = 1.f;
    float       originX     = 0.f;
    float       originY     = 0.f;
    Vec2        cur         {};
    Vec2        pathFirst   {};
    uint32_t    col         = 0;
    float       thickness   = 1.f;
    bool        pathOpen    = false;
    bool        pathStarted = false;

    Vec2 toScreen(const FT_Vector& v) const {
        return outlineToScreen(originX, originY, scale, scaleX, scaleY, v);
    }

    void pathClear() {
        if (s_pathEmitter && s_pathEmitter->begin)
            s_pathEmitter->begin(s_pathEmitter->user);
    }
    void pathLineTo(const Vec2& p) {
        if (s_pathEmitter && s_pathEmitter->line_to)
            s_pathEmitter->line_to(p.x, p.y, s_pathEmitter->user);
    }
    void pathCubicTo(const Vec2& a, const Vec2& b, const Vec2& c) {
        if (s_pathEmitter && s_pathEmitter->cubic_to)
            s_pathEmitter->cubic_to(a.x, a.y, b.x, b.y, c.x, c.y, s_pathEmitter->user);
    }
    void pathStroke(bool closed) {
        if (s_pathEmitter && s_pathEmitter->stroke)
            s_pathEmitter->stroke(col, thickness, closed, s_pathEmitter->user);
    }

    void flushPath() {
        if (!pathOpen)
            return;
        if (pathStarted) {
            const float dx = pathFirst.x - cur.x;
            const float dy = pathFirst.y - cur.y;
            if (dx * dx + dy * dy > 0.25f)
                pathLineTo(pathFirst);
            pathStroke(true);
        } else {
            pathClear();
        }
        pathOpen    = false;
        pathStarted = false;
    }

    void ensurePathStarted() {
        if (!pathStarted) {
            pathLineTo(cur);
            pathFirst   = cur;
            pathStarted = true;
        }
    }
};

static int stroke_moveto(const FT_Vector* to, void* user) {
    auto& c = *static_cast<StrokeOutlineCtx*>(user);
    c.flushPath();
    c.cur         = c.toScreen(*to);
    c.pathClear();
    c.pathOpen    = true;
    c.pathStarted = false;
    return 0;
}

static int stroke_lineto(const FT_Vector* to, void* user) {
    auto& c = *static_cast<StrokeOutlineCtx*>(user);
    c.ensurePathStarted();
    c.cur = c.toScreen(*to);
    c.pathLineTo(c.cur);
    return 0;
}

static int stroke_conicto(const FT_Vector* ctrl, const FT_Vector* to, void* user) {
    auto& c = *static_cast<StrokeOutlineCtx*>(user);
    c.ensurePathStarted();
    const Vec2 p1 = c.toScreen(*ctrl);
    const Vec2 p2 = c.toScreen(*to);
    const float  t  = 2.f / 3.f;
    const Vec2 cp1 = { c.cur.x + t * (p1.x - c.cur.x),
                       c.cur.y + t * (p1.y - c.cur.y) };
    const Vec2 cp2 = { p2.x    + t * (p1.x - p2.x),
                       p2.y    + t * (p1.y - p2.y) };
    c.pathCubicTo(cp1, cp2, p2);
    c.cur = p2;
    return 0;
}

static int stroke_cubicto(const FT_Vector* c1, const FT_Vector* c2,
                          const FT_Vector* to, void* user) {
    auto& c = *static_cast<StrokeOutlineCtx*>(user);
    c.ensurePathStarted();
    const Vec2 p2 = c.toScreen(*to);
    c.pathCubicTo(c.toScreen(*c1), c.toScreen(*c2), p2);
    c.cur = p2;
    return 0;
}

static const FT_Outline_Funcs kStrokeOutlineFuncs = {
    stroke_moveto, stroke_lineto, stroke_conicto, stroke_cubicto, 0, 0
};

static void decomposeStrokeOutline(StrokeOutlineCtx& ctx, const FT_Outline* ol) {
    ctx.pathOpen    = false;
    ctx.pathStarted = false;
    FT_Outline_Decompose(const_cast<FT_Outline*>(ol), &kStrokeOutlineFuncs, &ctx);
    ctx.flushPath();
}

// ============================================================================
// Glyph load / metrics (vector vs hinted pipeline)
// ============================================================================

static bool usesHintedPipeline(const Face* face) {
    // Vector and Loop-Blinn both consume unhinted, size-independent design-unit
    // outlines; only HintedVector and Raster drive FreeType's hinted pipeline.
    return face && (face->renderMode == RenderMode::HintedVector ||
                    face->renderMode == RenderMode::Raster);
}

static FT_Int32 buildLoadFlags(const Face* face) {
    if (!face || !usesHintedPipeline(face))
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
    if (!face || !usesHintedPipeline(face))
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
    if (face->renderMode == mode)
        return;
    face->renderMode = mode;
    face->syncedEmPx = -1.f;
    ++face->outlineGen;
#ifdef IMVARFONT_USE_HARFBUZZ
    if (face->hbFont)
        hb_ft_font_set_load_flags(face->hbFont, buildHbLoadFlags(face));
#endif
}

void SetHintingFlags(Face* face, HintingFlags flags) {
    if (!face) return;
    if (face->hintingFlags == flags)
        return;
    face->hintingFlags = flags;
    face->syncedEmPx = -1.f;
    ++face->outlineGen;
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

    if (!usesHintedPipeline(face)) {
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
        resolveVMetrics(face);   // canonical, instance-independent design metrics
        const float scale = (ft->units_per_EM > 0)
                            ? em_px / (float)ft->units_per_EM : 1.f;
        if (ascender)    *ascender    = (float)face->metricAsc * scale;
        if (descender)   *descender   = -(float)face->metricDesc * scale;
        if (line_height) *line_height = (face->metricHeight > 0)
                                        ? (float)face->metricHeight * scale : em_px * 1.2f;
    }
}

static void computeExtrapScale(const Face* face, float* out_sx, float* out_sy) {
    float sx = 1.f, sy = 1.f;
    if (face && !face->axisExtrap.empty()) {
        for (size_t i = 0; i < face->axes.size(); ++i) {
            const float e = face->axisExtrap[i];
            if (std::fabs(e) < 1e-6f)
                continue;
            const uint32_t tag = face->axes[i].Tag;
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

static float renderGlyphByIndex(Face* face, FT_UInt gi,
                                const GlyphMetricsCtx& gctx,
                                float origin_x, float origin_y,
                                uint32_t col,
                                bool filled, bool strokeOutline,
                                float thickness)
{
    FT_Face ft = face->ftFace;
    if (gi == 0) return 0.f;

    // Morph path: blend cached master corners instead of re-instancing FreeType.
    // Advance and outline both come from the blend; no per-frame FT_Load_Glyph.
    if (morphIsActive(face, gctx.hinted)) {
        profFrameTick();
        float advDesign = 0.f;
        MorphGlyphCell* mcell = nullptr;
        const auto tBlend = ProfClock::now();
        const GlyphCurves* mg = morphBlendGlyph(face, gi, &advDesign, &mcell);
        s_prof.blendMs += profMsSince(tBlend);
        if (mg) {
            const float adv = advDesign * gctx.outline_scale;
            if (s_emitGlyphQuad && filled && !mg->empty()) {
                const auto tRaster = ProfClock::now();
                fillGlyphAnalytic(face, gi, nullptr, false, gctx.em_px,
                                  origin_x, origin_y, gctx.outline_scale,
                                  1.f, 1.f, col, mg, mcell);
                s_prof.rasterMs += profMsSince(tRaster);
                ++s_prof.glyphs;
            }
            return adv;   // outline stroking is not morphed in this version
        }
        // Blend unavailable (e.g. topology mismatch): fall through to FreeType.
    }

    if (FT_Load_Glyph(ft, gi, gctx.load_flags) != 0)
        return 0.f;

    float extrapX = 1.f, extrapY = 1.f;
    computeExtrapScale(face, &extrapX, &extrapY);

    const float adv = gctx.hinted
                      ? (float)ft->glyph->advance.x / 64.f * extrapX
                      : (float)ft->glyph->advance.x * gctx.outline_scale * extrapX;

    if (ft->glyph->format != FT_GLYPH_FORMAT_OUTLINE)
        return adv;

    if ((!s_emitGlyphQuad && !s_pathEmitter) || (!filled && !strokeOutline))
        return adv;

    const FT_Outline* ol = &ft->glyph->outline;

    // Glyph fill uses the analytic GPU coverage renderer (signed-area, non-zero
    // winding) for faithful counters and conflation-free anti-aliasing. Requires
    // InitRenderer() to have succeeded; otherwise filled text is skipped.
    if (filled) {
        profFrameTick();
        const auto tRaster = ProfClock::now();
        fillGlyphAnalytic(face, gi, ol, gctx.hinted, gctx.em_px,
                          origin_x, origin_y,
                          gctx.outline_scale, extrapX, extrapY, col);
        s_prof.rasterMs += profMsSince(tRaster);
        ++s_prof.glyphs;
    }

    if (strokeOutline && s_pathEmitter) {
        StrokeOutlineCtx ctx;
        ctx.scale       = gctx.outline_scale;
        ctx.scaleX      = extrapX;
        ctx.scaleY      = extrapY;
        ctx.originX     = origin_x;
        ctx.originY     = origin_y;
        ctx.col         = col;
        ctx.thickness   = thickness;
        decomposeStrokeOutline(ctx, ol);
    }

    return adv;
}

static float renderGlyph(Face* face,
                          unsigned int codepoint,
                          const GlyphMetricsCtx& gctx,
                          float origin_x, float origin_y,
                          uint32_t col,
                          bool filled, bool strokeOutline,
                          float thickness)
{
    FT_Face ft = face->ftFace;
    const FT_UInt gi = FT_Get_Char_Index(ft, (FT_ULong)codepoint);
    return renderGlyphByIndex(face, gi, gctx, origin_x, origin_y, col,
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
                          int x, int y, uint8_t cov, uint32_t col) {
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
                               uint32_t col, std::vector<uint8_t>& rgba,
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


static int utf8Next(unsigned int* out_cp, const char* p, const char* end) {
    if (!p || p >= end) { *out_cp = 0; return 0; }
    unsigned char c = (unsigned char)*p;
    if (c < 0x80) { *out_cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && p + 1 < end) {
        *out_cp = ((c & 0x1F) << 6) | ((unsigned char)p[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && p + 2 < end) {
        *out_cp = ((c & 0x0F) << 12) | (((unsigned char)p[1] & 0x3F) << 6) | ((unsigned char)p[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && p + 3 < end) {
        *out_cp = ((c & 0x07) << 18) | (((unsigned char)p[1] & 0x3F) << 12) | (((unsigned char)p[2] & 0x3F) << 6) | ((unsigned char)p[3] & 0x3F);
        return 4;
    }
    *out_cp = 0xFFFD; return 1;
}
static int utf8Append(char* buf, unsigned int cp) {
    if (cp < 0x80) { buf[0] = (char)cp; return 1; }
    if (cp < 0x800) { buf[0] = (char)(0xC0 | (cp >> 6)); buf[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    buf[0] = (char)(0xF0 | (cp >> 18));
    buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

// Draw, measure, or rasterize one line.
static float drawTextLine(Face* face, const char* line, int line_len,
                          float em_px, float pen_x, float base_y,
                          uint32_t col,
                          bool filled, bool strokeOutline, float thickness,
                          float letter_spacing_px,
                          std::vector<PlacedGlyph>* layout_out = nullptr,
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
    // Shape with HarfBuzz when it can contribute: GPOS positioning (kerning) or
    // any active OpenType feature (which may substitute glyphs via GSUB).
    const bool hbForPositioning = face->useKerning && face->useHarfBuzz && face->hasGpos;
    const bool hbForFeatures    = !face->features.empty();
    if ((hbForPositioning || hbForFeatures) && face->hbFont && face->hbBuf) {
        syncHbFontSize(face, em_px);

        hb_buffer_t* buf = face->hbBuf;
        hb_buffer_clear_contents(buf);
        hb_buffer_add_utf8(buf, line, line_len, 0, line_len);
        hb_buffer_guess_segment_properties(buf);
        std::vector<hb_feature_t> feats;
        buildHbFeatures(face, feats);
        hb_shape(face->hbFont, buf, feats.empty() ? nullptr : feats.data(),
                 (unsigned)feats.size());

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

                    if (layout_out)
                        layout_out->push_back({ gid, gx, gy });
                    else if (raster)
                        renderGlyphBitmap(face, gid, gctx, gx, gy, col,
                                          *raster_rgba, raster_w, raster_h);
                    else if (s_emitGlyphQuad || s_pathEmitter)
                        renderGlyphByIndex(face, gid, gctx, gx, gy, col,
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
        const int len = utf8Next(&cp, p, line_end);
        if (len == 0) break;
        p += len;

        const FT_UInt gi = glyphIndex(ft, cp);
        if (prev_gi != 0 && face->hasKerningTable && face->useKerning && face->useKernTable)
            pen_x += kerningPx(face, ft, prev_gi, gi, gctx);

        if (layout_out)
            layout_out->push_back({ gi, pen_x, base_y });

        if (raster)
            pen_x += renderGlyphBitmap(face, gi, gctx, pen_x, base_y, col,
                                       *raster_rgba, raster_w, raster_h);
        else if (s_emitGlyphQuad || s_pathEmitter)
            pen_x += renderGlyphByIndex(face, gi, gctx, pen_x, base_y, col,
                                        filled, strokeOutline, thickness);
        else
            pen_x += glyphAdvancePx(face, ft, gi, gctx);

        if (letter_spacing_px != 0.f && p < line_end)
            pen_x += letter_spacing_px;

        prev_gi = gi;
    }
    return pen_x - start_x;
}

static float addTextLayout(Face* face,
                           float em_px, Vec2 pos,
                           uint32_t col, const char* text,
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
                                            pos.x, base_y, col,
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

void LayoutGlyphs(Face* face, const char* text, float em_px,
                  float line_height_px, float letter_spacing_px,
                  std::vector<PlacedGlyph>& out)
{
    out.clear();
    if (!face || !face->ftFace || !text || !*text || em_px <= 0.f)
        return;

    float ascender = 0.f, descender = 0.f, default_line_h = 0.f;
    getVerticalMetrics(face, em_px, &ascender, &descender, &default_line_h);
    (void)descender;
    const float line_h = (line_height_px > 0.f) ? line_height_px : default_line_h;

    float base_y     = ascender;
    const char* line_start = text;

    for (const char* p = text; ; ++p) {
        if (*p == '\n' || *p == '\0') {
            const int line_len = (int)(p - line_start);
            if (line_len > 0) {
                drawTextLine(face, line_start, line_len, em_px,
                             0.f, base_y, 0,
                             false, false, 0.f, letter_spacing_px,
                             &out);
            }
            if (*p == '\0') break;
            base_y += line_h;
            line_start = p + 1;
        }
    }
}

void* GetFtFace(Face* face) {
    return (face && face->ftFace) ? (void*)face->ftFace : nullptr;
}

RenderProfile GetRenderProfile() {
    RenderProfile p;
    p.blendMs  = (float)s_profLast.blendMs;
    p.rasterMs = (float)s_profLast.rasterMs;
    p.glyphs   = s_profLast.glyphs;
    p.rebuilds = s_profLast.rebuilds;
    return p;
}

// ============================================================================
// Public rendering API
// ============================================================================

float DrawString(Face* face, float em_px, Vec2 pos, uint32_t col, const char* text)
{
    if ((!s_emitGlyphQuad && !s_pathEmitter) || !face || !face->ftFace || !text || !*text)
        return 0.f;
    if (face->renderMode == RenderMode::Raster) return 0.f;
    ensureAtlasFrame();
    const TextStyle st{};
    return addTextLayout(face, em_px, pos, col, text,
                         st.fill, st.outline, st.outline_thickness,
                         st.line_height_px, st.letter_spacing_px);
}

float DrawString(Face* face, float em_px, Vec2 pos, uint32_t col, const char* text,
               const TextStyle& style)
{
    if ((!s_emitGlyphQuad && !s_pathEmitter) || !face || !face->ftFace || !text || !*text)
        return 0.f;
    if (face->renderMode == RenderMode::Raster) return 0.f;
    ensureAtlasFrame();
    return addTextLayout(face, em_px, pos, col, text,
                         style.fill, style.outline, style.outline_thickness,
                         style.line_height_px, style.letter_spacing_px);
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
                                             0.f, 0.f, 0,
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

bool RasterizeText(Face* face, float em_px, const char* text, uint32_t col,
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
                             (float)pad, base_y, col,
                             false, false, 0.f, letter_spacing_px,
                             nullptr, &out_rgba, out_w, out_h);
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
    p += utf8Append(p, (int)cp_left);
    p += utf8Append(p, (int)cp_right);
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

} // namespace VarFont
