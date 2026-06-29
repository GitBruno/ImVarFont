// imgui_var_font_detail.h — internal types for imgui_var_font.cpp.
// Not part of the public API; do not include from application code.

#pragma once

#include "imgui_var_font.h"
#include "varfont_gl.h"
#include "imgui.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef IMVARFONT_USE_HARFBUZZ
#include <hb.h>
#endif

namespace ImVarFont {

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

enum class SegType : uint8_t { Line, Quad, Cubic };

struct GlyphSeg {
    SegType type;
    ImVec2  p[4];
};

struct GlyphCurves {
    std::vector<GlyphSeg> segs;
    ImVec2 bboxMin {  1e30f,  1e30f };
    ImVec2 bboxMax { -1e30f, -1e30f };
    bool   empty() const { return segs.empty(); }
};

inline constexpr int kMorphMaxAxes    = 24;
inline constexpr int kMorphOrder2TopK = 4;
inline constexpr int kMorphMaxGpuTerms = 32;

struct MorphPair { int i = 0, j = 0; };

struct MorphGlyphCell {
    int                      n = 0;
    bool                     active[kMorphMaxAxes];
    float                    lo[kMorphMaxAxes];
    float                    hi[kMorphMaxAxes];
    GlyphCurves              base;
    float                    baseAdv = 0.f;
    std::vector<GlyphCurves> delta;
    std::vector<float>       deltaAdv;
    std::vector<MorphPair>   pairs;
    std::vector<GlyphCurves> pairDelta;
    std::vector<float>       pairAdv;
    bool                     builtExtrap = false;
    bool                     ok = false;
    GlyphCurves              cur;
    float                    curAdv  = 0.f;
    float                    curFrac[kMorphMaxAxes];
    bool                     curValid = false;
    int                      curSteps = 0;
    bool                     curBboxOnly = false;
    unsigned int             gpuTex   = 0;
    bool                     gpuDirty = true;
    bool                     gpuOk    = false;
    int                      gpuCount = 0;
    int                      gpuTermN = 0;
    int                      gpuTermA[kMorphMaxGpuTerms];
    int                      gpuTermB[kMorphMaxGpuTerms];
};

struct Face {
    FT_Library         library    = nullptr;
    FT_Face            ftFace     = nullptr;
    std::string        filePath;
    std::string        familyName;
    std::string        styleName;
    bool               isVariable = false;
    std::vector<Axis>  axes;
    std::vector<float> axisExtrap;
    FontMetadata       metadata;
    bool               hasKerningTable = false;
    bool               hasGpos         = false;
    bool               useKerning      = true;
    bool               useHarfBuzz     = true;
    bool               useKernTable    = true;
    std::vector<FeatureSetting> features;
    RenderMode         renderMode      = RenderMode::Vector;
    HintingFlags       hintingFlags    = HintingFlags::Native;
    float              syncedEmPx      = -1.f;
    uint64_t                                outlineGen        = 0;
    uint64_t                                curveCacheGen     = (uint64_t)-1;
    std::unordered_map<FT_UInt, GlyphCurves> curveCache;
    uint64_t                                glyphTexCacheGen  = (uint64_t)-1;
    std::unordered_map<uint64_t, glr::GlyphTex> glyphTexCache;
    bool                                    morphEnabled  = false;
    bool                                    morphExtrap   = false;
    std::unordered_map<FT_UInt, MorphGlyphCell> morphCache;
    float                                   morphNorm[kMorphMaxAxes];
    float                                   morphNormVals[kMorphMaxAxes];
    int                                     morphNormN     = 0;
    bool                                    morphNormValid = false;
    int                                     metricAsc      = 0;
    int                                     metricDesc     = 0;
    int                                     metricHeight   = 0;
    long                                    metricBbox[4]  = { 0, 0, 0, 0 };
    bool                                    metricsValid   = false;
    long long                               morphCellBuilds  = 0;
    long long                               morphBisectSteps = 0;
    long long                               morphFtSamples   = 0;
    long long                               morphPairTerms   = 0;
#ifdef IMVARFONT_USE_HARFBUZZ
    hb_font_t*         hbFont          = nullptr;
    hb_buffer_t*       hbBuf           = nullptr;
#endif
};

namespace detail {

FT_Fixed valueToFixed(float v);
void extractCurves(const FT_Outline* ol, GlyphCurves& out);
void pushEdge(std::vector<float>& E, const ImVec2& a, const ImVec2& b);
void flatQuad(std::vector<float>& E, ImVec2 p0, ImVec2 p1, ImVec2 p2, float tolSq, int depth);
void flatCubic(std::vector<float>& E, ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, float tolSq, int depth);
bool rasterOutlineCPU(FT_Library lib, const FT_Outline* ol, int w, int h, float sx, float sy,
                      float bx0, float by1, int pad, std::vector<unsigned char>& a8);
void setFaceVarToCurrent(Face* f);
const GlyphCurves* morphBlendGlyph(Face* f, FT_UInt gi, float* advOut,
                                   MorphGlyphCell** outCell = nullptr);

} // namespace detail

} // namespace ImVarFont
