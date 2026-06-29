// varfont_gl.cpp — GPU analytic glyph coverage (signed-area, Loop-Blinn, Slug, morph recon)
// Renderer layer of the VarFont engine; no Dear ImGui dependency.
//
// Signed-area coverage accumulation:
//   * Pass 1 (accumulate): for every line edge of the glyph we draw a quad that
//     spans from the edge rightward to the cell's right boundary, over the
//     edge's vertical extent. A fragment shader adds, per pixel, the signed
//     trapezoidal coverage the edge contributes (positive for up-going edges,
//     negative for down-going), accumulated additively into an RGBA16F target.
//     The running horizontal sum that a CPU scanline rasterizer needs is here
//     realized geometrically by extending each edge's quad to the right border,
//     so no prefix sum is required. Holes carry opposite winding and cancel.
//   * Pass 2 (resolve): a fullscreen pass takes |coverage|, applies coverage
//     gamma, and writes (1,1,1,alpha) into an RGBA8 cell so ImGui's stock
//     shader composites it as colour*coverage with no custom pipeline.
//
// GL entry points are loaded through a caller-supplied proc loader, so the
// library depends on no GL loader (GLEW/glad) of its own.

#include "varfont_gl.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <cmath>

// Target either desktop OpenGL 3.3 core or OpenGL ES 3.0. ES is selected by the
// same flag ImGui's own GL3 backend uses — IMGUI_IMPL_OPENGL_ES3 — which the
// host already defines for an ES build (e.g. openFrameworks on Raspberry Pi, or
// our CMake -DIMVARFONT_GLES=ON). The signed-area coverage path is identical on
// both; only headers, the entry-point loader, and the GLSL header line differ.
// On ES, RGBA16F is renderable AND blendable via EXT_color_buffer_half_float
// (EXT_float_blend is only needed for 32-bit float), so additive accumulation
// works without it.

#if defined(IMGUI_IMPL_OPENGL_ES3)
  #define IMVARFONT_GLES 1
#else
  #define IMVARFONT_GLES 0
#endif

#if IMVARFONT_GLES
  #include <GLES3/gl3.h>
  #define IMVARFONT_GLSL_HDR \
      "#version 300 es\nprecision highp float;\nprecision highp int;\n"
#else
  #if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
      #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
      #define NOMINMAX
    #endif
    #include <windows.h>
  #endif
  #include <GL/gl.h>
  #include <GL/glext.h>
  #define IMVARFONT_GLSL_HDR "#version 330 core\n"
#endif

namespace ImVarFont {
namespace glr {

#if !IMVARFONT_GLES
// ---------------------------------------------------------------------------
// Desktop: load the non-1.1 GL entry points through the caller's proc loader.
// Declared in this namespace, so unqualified use resolves here; 1.1 functions
// (glTexImage2D, glViewport, glDrawArrays, ...) resolve to the global opengl32
// symbols. On ES all of these are core and linked directly, so this block and
// the loader in Init() are compiled out.
// ---------------------------------------------------------------------------
static PFNGLGENFRAMEBUFFERSPROC        glGenFramebuffers        = nullptr;
static PFNGLDELETEFRAMEBUFFERSPROC     glDeleteFramebuffers     = nullptr;
static PFNGLBINDFRAMEBUFFERPROC        glBindFramebuffer        = nullptr;
static PFNGLFRAMEBUFFERTEXTURE2DPROC   glFramebufferTexture2D   = nullptr;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus = nullptr;

static PFNGLGENVERTEXARRAYSPROC        glGenVertexArrays        = nullptr;
static PFNGLDELETEVERTEXARRAYSPROC     glDeleteVertexArrays     = nullptr;
static PFNGLBINDVERTEXARRAYPROC        glBindVertexArray        = nullptr;

static PFNGLGENBUFFERSPROC             glGenBuffers             = nullptr;
static PFNGLDELETEBUFFERSPROC          glDeleteBuffers          = nullptr;
static PFNGLBINDBUFFERPROC             glBindBuffer             = nullptr;
static PFNGLBUFFERDATAPROC             glBufferData             = nullptr;
static PFNGLVERTEXATTRIBPOINTERPROC    glVertexAttribPointer    = nullptr;
static PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray = nullptr;

static PFNGLCREATESHADERPROC           glCreateShader           = nullptr;
static PFNGLSHADERSOURCEPROC           glShaderSource           = nullptr;
static PFNGLCOMPILESHADERPROC          glCompileShader          = nullptr;
static PFNGLGETSHADERIVPROC            glGetShaderiv            = nullptr;
static PFNGLGETSHADERINFOLOGPROC       glGetShaderInfoLog       = nullptr;
static PFNGLDELETESHADERPROC           glDeleteShader           = nullptr;
static PFNGLCREATEPROGRAMPROC          glCreateProgram          = nullptr;
static PFNGLATTACHSHADERPROC           glAttachShader           = nullptr;
static PFNGLLINKPROGRAMPROC            glLinkProgram            = nullptr;
static PFNGLGETPROGRAMIVPROC           glGetProgramiv           = nullptr;
static PFNGLGETPROGRAMINFOLOGPROC      glGetProgramInfoLog      = nullptr;
static PFNGLUSEPROGRAMPROC             glUseProgram             = nullptr;
static PFNGLDELETEPROGRAMPROC          glDeleteProgram          = nullptr;
static PFNGLGETUNIFORMLOCATIONPROC     glGetUniformLocation     = nullptr;
static PFNGLUNIFORM1FPROC              glUniform1f              = nullptr;
static PFNGLUNIFORM2FPROC              glUniform2f              = nullptr;
static PFNGLUNIFORM1IPROC              glUniform1i              = nullptr;
static PFNGLUNIFORM1FVPROC             glUniform1fv             = nullptr;

static PFNGLACTIVETEXTUREPROC          glActiveTexture          = nullptr;
static PFNGLBLENDEQUATIONPROC          glBlendEquation          = nullptr;
static PFNGLGETSTRINGIPROC             glGetStringi             = nullptr;
#endif // !IMVARFONT_GLES

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static bool   s_ready       = false;   // base: atlas uploads + compositing work
static bool   s_covReady    = false;   // GPU analytic-coverage path available
static bool   s_forceCpu    = false;   // diagnostics: force the CPU fallback
static bool   s_preferSlug  = false;   // route live/morph through the Slug path
static GLuint s_covProg     = 0;   // accumulate program
static GLuint s_resProg     = 0;   // resolve program
static GLint  s_covU_dim    = -1;  // vec2 cell dim (w,h)
static GLint  s_resU_tex    = -1;  // sampler
static GLint  s_resU_gamma  = -1;  // float
static GLint  s_resU_origin = -1;  // vec2 dst viewport origin

static GLuint s_covFbo     = 0;   // accumulate FBO
static GLuint s_covTex     = 0;   // RGBA16F scratch
static int    s_covW       = 0;   // scratch size
static int    s_covH       = 0;

static GLuint s_resFbo     = 0;   // resolve FBO (dst tex attached per call)

static GLuint s_edgeVao    = 0;
static GLuint s_edgeVbo    = 0;
static GLuint s_quadVao    = 0;
static GLuint s_quadVbo    = 0;

// ---- Loop-Blinn analytic fill (quadratics) ----
// s_lbProg accumulates non-zero winding at SS x SS supersamples into the same
// RGBA16F scratch as the signed-area path; s_lbResProg box-downsamples that into
// the RGBA8 atlas cell. Geometry is a chord-triangle fan (anchored at the cell
// origin) plus one analytic curve triangle per quadratic.
static bool   s_lbReady       = false;
static GLuint s_lbProg        = 0;
static GLuint s_lbResProg     = 0;
static GLint  s_lbU_dim       = -1;  // vec2 supersample dim (sw,sh)
static GLint  s_lbResU_tex    = -1;  // sampler
static GLint  s_lbResU_gamma  = -1;  // float
static GLint  s_lbResU_origin = -1;  // vec2 dst viewport origin
static GLint  s_lbResU_ss     = -1;  // int supersample factor
static GLuint s_lbVao         = 0;
static GLuint s_lbVbo         = 0;

// ---- Exact-curve analytic coverage (attribution: see kCurveFS header) ----
// Single-pass GLSL: exact-area coverage per pixel from quadratic Béziers (not
// Eric's dual-ray Slug fill). Curves in an RGBA32F texture (2 texels per quad);
// texelFetch only (GL 3.3 / ES 3.0). No band table: loop every curve in the cell
// with a per-fragment convex-hull cull before integration (ImVarFont contribution:
// morph-friendly substitute for Slug's offline band tables). Toggle uCull /
// SetCullEnabled() for A/B timing.
static bool   s_slugReady    = false;
static GLuint s_slugProg     = 0;
static GLint  s_slugU_origin = -1;  // vec2 dst cell origin (dx,dy) in the FBO
static GLint  s_slugU_count  = -1;  // int   curve count
static GLint  s_slugU_curves = -1;  // sampler2D curve texture
static GLint  s_slugU_gamma  = -1;  // float coverage gamma
static GLint  s_slugU_cull   = -1;  // int   per-fragment convex-hull cull on/off
static GLuint s_slugCurveTex = 0;   // RGBA32F, 2 texels/curve, reuploaded per glyph

// Runtime gate for the per-fragment convex-hull cull in kCurveFS. On by default
// (the cull is exact, so it never changes output). A benchmark can flip it off
// via SetCullEnabled() to measure the cull's speedup as a clean A/B on one build.
static bool   s_cullEnabled  = true;

// ---- GPU morph reconstruction (base + Σ frac·delta on the GPU) ----
// The novel layer: a glyph's morph cell is uploaded ONCE as a static "delta
// buffer" — design-unit base control points plus one delta per correction TERM
// (order-1 per-axis main effects AND order-2 coupled pairs) — into a per-glyph
// RGBA32F texture (width 2*curves, height 1+terms; row 0 = base, rows 1.. = term
// deltas, 2 texels/quad). Each frame a tiny "reconstruct" pass renders a
// 2*curves×1 texture whose every texel is base + Σ weight·delta (weight = uniforms:
// frac_i for an axis term, frac_i·frac_j for a pair term) then applies the cell's
// pixel transform (scale/origin = uniforms). So an axis drag OR a zoom is just a
// uniform change — no CPU outline work, no re-upload — and the reconstructed curves
// feed the unchanged exact-curve coverage shader. GL 3.3 / ES 3.0 friendly: float
// textures + texelFetch, no SSBO/compute.
static const int kReconMaxTerms = 32;  // GPU uniform-array cap; more -> CPU recon
static bool   s_reconReady    = false;
static GLuint s_reconProg     = 0;
static GLint  s_reconU_data   = -1;  // sampler2D delta buffer
static GLint  s_reconU_terms  = -1;  // int   correction-term count
static GLint  s_reconU_weight = -1;  // float[kReconMaxTerms] per-term weights
static GLint  s_reconU_scale  = -1;  // vec2  (sx,sy) design->cell-pixel scale
static GLint  s_reconU_origin = -1;  // vec2  (ox,oy) cell-pixel origin (y-up)
static GLuint s_reconTex      = 0;   // RGBA32F reconstructed curves (2*curves × 1)
static GLuint s_reconFbo      = 0;
static int    s_reconW        = 0;

// ---- Shared glyph atlas (multi-page shelf packer) ----
// Small/medium cells pack into RGBA8 pages so glyphs sharing a page batch into
// one ImGui draw call. Pages are owned here and never freed per-glyph. When the
// current page fills we GROW (add a page) rather than recycle, so a cell drawn
// earlier this frame is never overwritten. Only when the page cap is exceeded do
// we mark a recycle, which is applied at the next frame boundary (BeginFrame) so
// it can never corrupt geometry already emitted this frame; the recycle bumps
// s_atlasGen so stale cache entries re-render. Each page is cleared to
// transparent on allocation, so the gutters between cells never bleed. Cells too
// large for a page get a dedicated texture (rare; only at extreme zoom).
static const int kAtlasSize = 1024;   // page dimension (px); 4 MB RGBA8 each
static const int kAtlasPad  = 2;      // gutter between cells (avoids bleed)
static const int kMaxPages  = 16;     // soft cap (~64 MB) before a recycle

struct AtlasPage {
    GLuint tex    = 0;
    int    shelfX = 0;   // next free x in current shelf
    int    shelfY = 0;   // current shelf top
    int    shelfH = 0;   // current shelf height
};
static std::vector<AtlasPage> s_pages;
static std::vector<GLuint>    s_dedicated;     // oversized single-glyph textures
static unsigned int           s_atlasGen     = 1;
static bool                   s_resetPending = false;

// ---- Live (transient) pages ----
// Loop-Blinn live mode re-renders glyphs every frame instead of caching them, so
// zoom/axis morphing never re-rasterizes on the CPU. Its cells live in a separate
// page set that is rewound (and cleared) once per frame at BeginFrame, so the
// textures are reused without growth and stale gutters never bleed. Page textures
// persist across frames; only the per-frame shelf cursors reset.
static std::vector<AtlasPage> s_livePages;
static std::vector<GLuint>    s_liveDedicated;   // oversized live cells (this frame)
static bool                   s_liveNeedsReset = false;
static size_t                 s_liveCur        = 0;  // page being filled this frame (reset each frame)

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------
static const char* kCovVS =
    IMVARFONT_GLSL_HDR
    "layout(location=0) in vec2 aPos;\n"   // quad corner, cell pixel space (y-up)
    "layout(location=1) in vec4 aEdge;\n"  // edge x0,y0,x1,y1 (cell pixel space)
    "uniform vec2 uDim;\n"
    "flat out vec4 vEdge;\n"
    "void main(){\n"
    "  vEdge = aEdge;\n"
    "  vec2 ndc = aPos / uDim * 2.0 - 1.0;\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "}\n";

// Per-edge signed trapezoidal coverage contribution for this pixel. We work in
// pixel-local coordinates (the unit square of the fragment's pixel) and add the
// signed vertical-overlap times the fraction of the pixel lying to the right of
// the edge, evaluated at the mid-height of the overlap band.
static const char* kCovFS =
    IMVARFONT_GLSL_HDR
    "flat in vec4 vEdge;\n"
    "out vec4 frag;\n"
    "void main(){\n"
    "  vec2 a = vEdge.xy;\n"
    "  vec2 b = vEdge.zw;\n"
    "  float dy = b.y - a.y;\n"
    "  if (abs(dy) < 1e-7) { discard; }\n"
    "  float pxL = floor(gl_FragCoord.x);\n"
    "  float pyB = floor(gl_FragCoord.y);\n"
    // pixel-local edge endpoints
    "  vec2 la = vec2(a.x - pxL, a.y - pyB);\n"
    "  vec2 lb = vec2(b.x - pxL, b.y - pyB);\n"
    // y-overlap of the edge with this pixel band [0,1]
    "  float ylo = max(0.0, min(la.y, lb.y));\n"
    "  float yhi = min(1.0, max(la.y, lb.y));\n"
    "  float ov  = yhi - ylo;\n"
    "  if (ov <= 0.0) { discard; }\n"
    // edge x at mid-height of the overlap band
    "  float ymid = 0.5 * (ylo + yhi);\n"
    "  float t    = (ymid - la.y) / (lb.y - la.y);\n"
    "  float xAt  = la.x + (lb.x - la.x) * t;\n"
    // fraction of the pixel width to the right of the edge
    "  float rightFrac = clamp(1.0 - xAt, 0.0, 1.0);\n"
    "  float sgn = (dy > 0.0) ? 1.0 : -1.0;\n"
    "  float cov = sgn * ov * rightFrac;\n"
    "  frag = vec4(cov, 0.0, 0.0, 0.0);\n"
    "}\n";

static const char* kResVS =
    IMVARFONT_GLSL_HDR
    "layout(location=0) in vec2 aPos;\n"   // clip-space fullscreen triangle/quad
    "void main(){ gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char* kResFS =
    IMVARFONT_GLSL_HDR
    "uniform highp sampler2D uCov;\n"
    "uniform float uGamma;\n"
    "uniform vec2 uDstOrigin;\n"   // viewport origin in the destination texture
    "out vec4 frag;\n"
    "void main(){\n"
    "  ivec2 src = ivec2(gl_FragCoord.xy - uDstOrigin);\n"
    "  float cov = texelFetch(uCov, src, 0).r;\n"
    "  float a = clamp(abs(cov), 0.0, 1.0);\n"
    "  a = pow(a, uGamma);\n"
    "  frag = vec4(1.0, 1.0, 1.0, a);\n"
    "}\n";

// ---- Loop-Blinn analytic fill programs ----
// Accumulate pass: each triangle contributes its orientation sign (+/-1) to the
// winding total at every covered supersample. Solid chord triangles count
// everywhere; quadratic curve triangles count only on the control-point side of
// the parabola (Loop-Blinn implicit u*u - v > 0), carving the exact curve.
static const char* kLbVS =
    IMVARFONT_GLSL_HDR
    "layout(location=0) in vec2  aPos;\n"   // supersample pixel space (y-up)
    "layout(location=1) in vec2  aUV;\n"    // Loop-Blinn (u,v); unused for solids
    "layout(location=2) in float aSign;\n"  // triangle orientation sign (+/-1)
    "layout(location=3) in float aSolid;\n" // 1 = chord triangle, 0 = curve
    "uniform vec2 uDim;\n"
    "out vec2 vUV;\n"
    "flat out float vSign;\n"
    "flat out float vSolid;\n"
    "void main(){\n"
    "  vUV = aUV; vSign = aSign; vSolid = aSolid;\n"
    "  vec2 ndc = aPos / uDim * 2.0 - 1.0;\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "}\n";

static const char* kLbFS =
    IMVARFONT_GLSL_HDR
    "in vec2 vUV;\n"
    "flat in float vSign;\n"
    "flat in float vSolid;\n"
    "out vec4 frag;\n"
    "void main(){\n"
    "  if (vSolid < 0.5) {\n"
    "    float f = vUV.x * vUV.x - vUV.y;\n"  // Loop-Blinn quadratic implicit
    "    if (f <= 0.0) discard;\n"            // keep only the control-point side
    "  }\n"
    "  frag = vec4(vSign, 0.0, 0.0, 0.0);\n"
    "}\n";

// Resolve pass: box-average the SS x SS winding block; a subsample counts as
// covered when its accumulated winding number is non-zero.
static const char* kLbResFS =
    IMVARFONT_GLSL_HDR
    "uniform highp sampler2D uCov;\n"
    "uniform float uGamma;\n"
    "uniform vec2  uDstOrigin;\n"
    "uniform int   uSS;\n"
    "out vec4 frag;\n"
    "void main(){\n"
    "  ivec2 d = ivec2(gl_FragCoord.xy - uDstOrigin);\n"
    "  ivec2 base = d * uSS;\n"
    "  float acc = 0.0;\n"
    "  for (int j = 0; j < uSS; ++j) {\n"
    "    for (int i = 0; i < uSS; ++i) {\n"
    "      float wnd = texelFetch(uCov, base + ivec2(i, j), 0).r;\n"
    "      if (abs(wnd) >= 0.5) acc += 1.0;\n"
    "    }\n"
    "  }\n"
    "  float cov = acc / float(uSS * uSS);\n"
    "  cov = pow(clamp(cov, 0.0, 1.0), uGamma);\n"
    "  frag = vec4(1.0, 1.0, 1.0, cov);\n"
    "}\n";

// ---- Exact-curve coverage program (kCurveFS) ----
//
// Attribution:
//   Eric Lengyel — Slug algorithm and banded reference shader (SlugPixelShader.hlsl,
//   MIT, Copyright 2017; JCGT 2017; patent dedicated to the public domain).
//
//   ImVarFont (Bruno Herfst) — for moving outlines (variable-font morphs), we replace
//   Slug's precomputed band tables with: loop all curves + per-curve convex-hull cull
//   (exact; no band rebuild when control points move).
//
// This shader is not a port of Slug's dual-ray math. It computes exact horizontal
// pixel-area coverage: integrate clamp((px+1)-qx,0,1)*qy'(t) over t breakpoints
// {0,1} ∪ crossings of y=py, y=py+1, x=px, x=px+1. Cell-pixel space (y-up),
// one curve loop, optional uCull. ~5× closer to FreeType exact-area AA than a
// single-sample dual-ray (see morph_probe --quality).
//
// Vertex: kResVS fullscreen triangle; fragment uses gl_FragCoord - uOrigin.
static const char* kCurveFS =
    IMVARFONT_GLSL_HDR
    "uniform sampler2D uCurves;\n"
    "uniform int   uCount;\n"
    "uniform vec2  uOrigin;\n"
    "uniform float uGamma;\n"
    "uniform int   uCull;\n"      // 1 = apply the convex-hull cull, 0 = brute force (A/B)
    "out vec4 frag;\n"
    // Real roots of a t^2 + b t + c in (0,1); absent roots returned as -1. Uses
    // the numerically stable form (q = -(b + sign(b) sqrt(disc))/2) to avoid the
    // catastrophic cancellation of (-b +/- sqrt) for near-linear quadratics.
    "vec2 roots01(float a, float b, float c){\n"
    "  vec2 r = vec2(-1.0);\n"
    "  if (abs(a) < 1e-7){\n"
    "    if (abs(b) > 1e-12){ float t = -c / b; if (t > 0.0 && t < 1.0) r.x = t; }\n"
    "    return r;\n"
    "  }\n"
    "  float disc = b * b - 4.0 * a * c;\n"
    "  if (disc < 0.0) return r;\n"
    "  float sd = sqrt(disc);\n"
    "  float q  = -0.5 * (b + (b >= 0.0 ? sd : -sd));\n"
    "  float t0 = (abs(q) > 1e-20) ? q / a       : -b / (2.0 * a);\n"
    "  float t1 = (abs(q) > 1e-20) ? c / q       : t0;\n"
    "  if (t0 > 0.0 && t0 < 1.0) r.x = t0;\n"
    "  if (t1 > 0.0 && t1 < 1.0) r.y = t1;\n"
    "  return r;\n"
    "}\n"
    "void main(){\n"
    "  vec2 rc = gl_FragCoord.xy - uOrigin;\n"          // cell-local pixel coord (y-up)
    "  float px = floor(rc.x), py = floor(rc.y);\n"
    "  float yb0 = py, yb1 = py + 1.0, K = px + 1.0;\n"
    "  float acc = 0.0;\n"
    "  for (int i = 0; i < uCount; ++i){\n"
    "    vec4 t0 = texelFetch(uCurves, ivec2(2*i,     0), 0);\n"
    "    vec2 t1 = texelFetch(uCurves, ivec2(2*i + 1, 0), 0).xy;\n"
    "    vec2 P0 = t0.xy, P1 = t0.zw, P2 = t1;\n"
    // ImVarFont: convex-hull cull (substitute for Slug band tables on moving outlines).
    // Quadratic ⊆ control-point bbox: skip if bbox misses pixel row [yb0,yb1]
    // or lies fully right of the pixel (min x >= K). Exact; no band rebuild.
    "    if (uCull != 0){\n"
    "      float cyl = min(P0.y, min(P1.y, P2.y));\n"
    "      float cyh = max(P0.y, max(P1.y, P2.y));\n"
    "      if (cyh <= yb0 || cyl >= yb1) continue;\n"
    "      if (min(P0.x, min(P1.x, P2.x)) >= K) continue;\n"
    "    }\n"
    "    vec2 A = P0 - 2.0 * P1 + P2;\n"                 // q(t) = A t^2 + B t + C
    "    vec2 B = 2.0 * (P1 - P0);\n"
    "    vec2 C = P0;\n"
    "    float bp[10];\n"
    "    int n = 0; bp[n++] = 0.0; bp[n++] = 1.0;\n"
    "    vec2 r;\n"
    "    r = roots01(A.y, B.y, C.y - yb0); if (r.x >= 0.0) bp[n++] = r.x; if (r.y >= 0.0) bp[n++] = r.y;\n"
    "    r = roots01(A.y, B.y, C.y - yb1); if (r.x >= 0.0) bp[n++] = r.x; if (r.y >= 0.0) bp[n++] = r.y;\n"
    "    r = roots01(A.x, B.x, C.x - px ); if (r.x >= 0.0) bp[n++] = r.x; if (r.y >= 0.0) bp[n++] = r.y;\n"
    "    r = roots01(A.x, B.x, C.x - K  ); if (r.x >= 0.0) bp[n++] = r.x; if (r.y >= 0.0) bp[n++] = r.y;\n"
    "    for (int ii = 1; ii < n; ++ii){\n"              // insertion sort breakpoints
    "      float key = bp[ii]; int jj = ii - 1;\n"
    "      for (; jj >= 0 && bp[jj] > key; --jj) bp[jj + 1] = bp[jj];\n"
    "      bp[jj + 1] = key;\n"
    "    }\n"
    "    for (int s = 0; s + 1 < n; ++s){\n"
    "      float a = bp[s], b = bp[s + 1];\n"
    "      if (b - a < 1e-7) continue;\n"
    "      float tm = 0.5 * (a + b);\n"
    "      float ym = (A.y * tm + B.y) * tm + C.y;\n"
    "      if (ym <= yb0 || ym >= yb1) continue;\n"      // band-clip in y
    "      float xm = (A.x * tm + B.x) * tm + C.x;\n"
    "      float cl = K - xm;\n"
    "      if (cl <= 0.0) continue;\n"                   // pixel fully left of curve
    "      if (cl >= 1.0){\n"                            // pixel fully right: integral of qy'
    "        acc += ((A.y * b + B.y) * b + C.y) - ((A.y * a + B.y) * a + C.y);\n"
    "      } else {\n"                                   // straddle: integral of (K-qx) qy'
    "        float a2 = -A.x, a1 = -B.x, a0 = K - C.x, b1 = 2.0 * A.y, b0 = B.y;\n"
    "        float c3 = a2 * b1;\n"
    "        float c2 = a2 * b0 + a1 * b1;\n"
    "        float c1 = a1 * b0 + a0 * b1;\n"
    "        float c0 = a0 * b0;\n"
    "        float Fb = (((c3 * 0.25) * b + c2 / 3.0) * b + c1 * 0.5) * b * b + c0 * b;\n"
    "        float Fa = (((c3 * 0.25) * a + c2 / 3.0) * a + c1 * 0.5) * a * a + c0 * a;\n"
    "        acc += Fb - Fa;\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "  if (acc != acc) acc = 0.0;\n"             // neutralize any stray NaN (no white boxes)
    "  float cov = clamp(abs(acc), 0.0, 1.0);\n"
    "  cov = pow(cov, uGamma);\n"
    "  frag = vec4(1.0, 1.0, 1.0, cov);\n"
    "}\n";

// ---- GPU morph reconstruction program ----
// One fragment per output texel of the 2*curves×1 reconstructed-curve texture.
// Reads the static per-glyph delta buffer (row 0 = base, rows 1.. = per-axis
// deltas, all in DESIGN units), forms base + Σ frac·delta, then maps to cell
// pixel space (y-up) with the same affine the CPU path used: cx = ox + sx·x,
// cy = oy + sy·y. The (z,w) of a curve's 2nd texel hold p2.xy; the coverage
// shader ignores the remaining lane, so transforming it is harmless.
static const char* kReconFS =
    IMVARFONT_GLSL_HDR
    "uniform sampler2D uData;\n"
    "uniform int   uTerms;\n"
    "uniform float uWeight[32];\n"
    "uniform vec2  uScale;\n"
    "uniform vec2  uOrigin;\n"
    "out vec4 frag;\n"
    "void main(){\n"
    "  int col = int(gl_FragCoord.x);\n"
    "  vec4 v = texelFetch(uData, ivec2(col, 0), 0);\n"     // base (design units)
    "  for (int t = 0; t < uTerms; ++t)\n"                  // + Σ weight·delta
    "    v += uWeight[t] * texelFetch(uData, ivec2(col, 1 + t), 0);\n"
    "  frag = vec4(uOrigin.x + uScale.x * v.x,\n"           // map to cell pixels (y-up)
    "              uOrigin.y + uScale.y * v.y,\n"
    "              uOrigin.x + uScale.x * v.z,\n"
    "              uOrigin.y + uScale.y * v.w);\n"
    "}\n";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]; log[0] = 0;
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[ImVarFont::glr] shader compile failed: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint linkProgram(const char* vs, const char* fs) {
    GLuint v = compileShader(GL_VERTEX_SHADER, vs);
    GLuint f = compileShader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) { if (v) glDeleteShader(v); if (f) glDeleteShader(f); return 0; }
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024]; log[0] = 0;
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[ImVarFont::glr] program link failed: %s\n", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static bool ensureScratch(int w, int h) {
    if (s_covTex && w <= s_covW && h <= s_covH)
        return true;
    int nw = (w > s_covW) ? w : s_covW;
    int nh = (h > s_covH) ? h : s_covH;
    if (nw < 64) nw = 64;
    if (nh < 64) nh = 64;
    if (!s_covTex) glGenTextures(1, &s_covTex);
    glBindTexture(GL_TEXTURE_2D, s_covTex);
    // RGBA16F is valid with FLOAT or HALF_FLOAT; HALF_FLOAT is the canonical type
    // for a renderable half-float target and the most widely accepted on ES/Mesa.
#if IMVARFONT_GLES
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, nw, nh, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
#else
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, nw, nh, 0, GL_RGBA, GL_FLOAT, nullptr);
#endif
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (!s_covFbo) glGenFramebuffers(1, &s_covFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_covFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_covTex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[ImVarFont::glr] coverage FBO incomplete (0x%x)\n", st);
        return false;
    }
    s_covW = nw; s_covH = nh;
    return true;
}

// Grow the reconstructed-curve target (RGBA32F, w×1) on demand. Holds the
// per-frame output of the reconstruct pass; bound as the coverage shader's input.
static bool ensureReconTex(int w) {
    if (s_reconTex && w <= s_reconW)
        return true;
    int nw = (w > s_reconW) ? w : s_reconW;
    if (nw < 64) nw = 64;
    if (!s_reconTex) glGenTextures(1, &s_reconTex);
    glBindTexture(GL_TEXTURE_2D, s_reconTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, nw, 1, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    // Attach to the dedicated recon FBO once (kept attached across frames) and check
    // completeness HERE — only when the target is (re)allocated, not per glyph — so
    // the synchronous glCheckFramebufferStatus never sits on the hot path.
    GLint prevFbo = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_reconFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_reconTex, 0);
    const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        // RGBA32F not renderable here (e.g. ES without color_buffer_float) — disable.
        s_reconReady = false;
        return false;
    }
    s_reconW = nw;
    return true;
}

static GLuint allocCellTexture(int w, int h) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

// Allocate a fresh page and clear it to transparent (so gutters never bleed).
// Must be called with the renderer's GL state already saved (see RenderGlyph).
static bool allocPage() {
    GLuint t = allocCellTexture(kAtlasSize, kAtlasSize);
    if (!t) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, s_resFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, kAtlasSize, kAtlasSize);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    AtlasPage p; p.tex = t;
    s_pages.push_back(p);
    return true;
}

// Apply a pending recycle at a safe point (frame boundary). Deletes all pages
// and oversized textures and bumps the generation so caches re-render.
static void applyResetIfPending() {
    if (!s_resetPending)
        return;
    for (auto& p : s_pages)
        if (p.tex) glDeleteTextures(1, &p.tex);
    s_pages.clear();
    if (!s_dedicated.empty()) {
        glDeleteTextures((GLsizei)s_dedicated.size(), s_dedicated.data());
        s_dedicated.clear();
    }
    ++s_atlasGen;
    s_resetPending = false;
}

// Multi-page shelf next-fit packer. Returns false only if the cell is larger
// than a page (caller uses a dedicated texture). Grows by adding pages rather
// than recycling mid-frame; flags a recycle for the next frame past the cap.
static bool packCell(int w, int h, GLuint* outTex, int* outX, int* outY) {
    const int aw = w + kAtlasPad, ah = h + kAtlasPad;
    if (aw > kAtlasSize || ah > kAtlasSize)
        return false;
    if (s_pages.empty() && !allocPage())
        return false;

    AtlasPage* p = &s_pages.back();
    // Fits in the current shelf?
    if (p->shelfX + aw <= kAtlasSize && ah <= p->shelfH) {
        *outTex = p->tex; *outX = p->shelfX; *outY = p->shelfY;
        p->shelfX += aw;
        return true;
    }
    // Try a new shelf below the current one.
    const int ny = p->shelfY + p->shelfH;
    if (ny + ah <= kAtlasSize) {
        p->shelfY = ny; p->shelfH = ah; p->shelfX = 0;
        *outTex = p->tex; *outX = 0; *outY = ny;
        p->shelfX = aw;
        return true;
    }
    // Page full -> grow with a new page (never overwrite this frame's cells).
    if (!allocPage())
        return false;
    if ((int)s_pages.size() > kMaxPages)
        s_resetPending = true;     // recycle at the next frame boundary
    p = &s_pages.back();
    p->shelfY = 0; p->shelfH = ah; p->shelfX = aw;
    *outTex = p->tex; *outX = 0; *outY = 0;
    return true;
}

// Allocate a fresh transient (live) page, cleared to transparent. Caller must
// already have saved the GL state we touch (see RenderGlyphCurves).
static bool allocLivePage() {
    GLuint t = allocCellTexture(kAtlasSize, kAtlasSize);
    if (!t) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, s_resFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, kAtlasSize, kAtlasSize);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    AtlasPage p; p.tex = t;
    s_livePages.push_back(p);
    return true;
}

// Rewind the live pages for a new frame: clear each page to transparent and reset
// its shelf cursors so cells are reused in place. Frees this-frame oversized live
// cells. Must run inside a saved-GL-state region.
static void resetLivePagesNow() {
    for (auto& p : s_livePages) {
        if (!p.tex) continue;
        glBindFramebuffer(GL_FRAMEBUFFER, s_resFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, p.tex, 0);
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, kAtlasSize, kAtlasSize);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        p.shelfX = p.shelfY = p.shelfH = 0;
    }
    s_liveCur = 0;   // refill from the first page; see packCellLive()
    if (!s_liveDedicated.empty()) {
        glDeleteTextures((GLsizei)s_liveDedicated.size(), s_liveDedicated.data());
        s_liveDedicated.clear();
    }
}

// Transient shelf packer (live pages). Grows by adding pages within the frame;
// never recycles mid-frame. Returns false only when the cell exceeds a page.
static bool packCellLive(int w, int h, GLuint* outTex, int* outX, int* outY) {
    const int aw = w + kAtlasPad, ah = h + kAtlasPad;
    if (aw > kAtlasSize || ah > kAtlasSize)
        return false;
    if (s_livePages.empty() && !allocLivePage())
        return false;
    if (s_liveCur >= s_livePages.size())
        s_liveCur = s_livePages.size() - 1;

    // Fill pages front-to-back via s_liveCur (reset to 0 each frame by
    // resetLivePagesNow). CRITICAL: a rewind empties EVERY page, so once a page
    // fills we must advance to the next already-allocated page and only allocate
    // a new one when none remain. Starting from .back() instead would ignore the
    // rewound front pages and append a fresh page every frame, growing the live
    // page set without bound (4 MB/page, cleared every frame) until VRAM is
    // exhausted — a slow, non-recovering frame-rate collapse while morphing.
    for (;;) {
        AtlasPage* p = &s_livePages[s_liveCur];
        if (p->shelfX + aw <= kAtlasSize && ah <= p->shelfH) {   // current shelf
            *outTex = p->tex; *outX = p->shelfX; *outY = p->shelfY;
            p->shelfX += aw;
            return true;
        }
        const int ny = p->shelfY + p->shelfH;                    // new shelf below
        if (ny + ah <= kAtlasSize) {
            p->shelfY = ny; p->shelfH = ah; p->shelfX = aw;
            *outTex = p->tex; *outX = 0; *outY = ny;
            return true;
        }
        if (s_liveCur + 1 < s_livePages.size()) {                // reuse next page
            ++s_liveCur;
            continue;
        }
        if (!allocLivePage())                                    // else grow once
            return false;
        ++s_liveCur;
        AtlasPage* np = &s_livePages.back();
        np->shelfY = 0; np->shelfH = ah; np->shelfX = aw;
        *outTex = np->tex; *outX = 0; *outY = 0;
        return true;
    }
}

#if IMVARFONT_GLES
static bool hasExtension(const char* name) {
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i) {
        const char* e = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (e && std::strcmp(e, name) == 0)
            return true;
    }
    return false;
}
#endif

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool Init(GLProc get_proc) {
    if (s_ready) return true;

#if !IMVARFONT_GLES
    if (!get_proc) return false;

    bool ok = true;
    #define LOADP(type, name) do { name = (type)get_proc(#name); if (!name) { \
        std::fprintf(stderr, "[ImVarFont::glr] missing GL func: %s\n", #name); ok = false; } } while (0)
    LOADP(PFNGLGENFRAMEBUFFERSPROC,        glGenFramebuffers);
    LOADP(PFNGLDELETEFRAMEBUFFERSPROC,     glDeleteFramebuffers);
    LOADP(PFNGLBINDFRAMEBUFFERPROC,        glBindFramebuffer);
    LOADP(PFNGLFRAMEBUFFERTEXTURE2DPROC,   glFramebufferTexture2D);
    LOADP(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus);
    LOADP(PFNGLGENVERTEXARRAYSPROC,        glGenVertexArrays);
    LOADP(PFNGLDELETEVERTEXARRAYSPROC,     glDeleteVertexArrays);
    LOADP(PFNGLBINDVERTEXARRAYPROC,        glBindVertexArray);
    LOADP(PFNGLGENBUFFERSPROC,             glGenBuffers);
    LOADP(PFNGLDELETEBUFFERSPROC,          glDeleteBuffers);
    LOADP(PFNGLBINDBUFFERPROC,             glBindBuffer);
    LOADP(PFNGLBUFFERDATAPROC,             glBufferData);
    LOADP(PFNGLVERTEXATTRIBPOINTERPROC,    glVertexAttribPointer);
    LOADP(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray);
    LOADP(PFNGLCREATESHADERPROC,           glCreateShader);
    LOADP(PFNGLSHADERSOURCEPROC,           glShaderSource);
    LOADP(PFNGLCOMPILESHADERPROC,          glCompileShader);
    LOADP(PFNGLGETSHADERIVPROC,            glGetShaderiv);
    LOADP(PFNGLGETSHADERINFOLOGPROC,       glGetShaderInfoLog);
    LOADP(PFNGLDELETESHADERPROC,           glDeleteShader);
    LOADP(PFNGLCREATEPROGRAMPROC,          glCreateProgram);
    LOADP(PFNGLATTACHSHADERPROC,           glAttachShader);
    LOADP(PFNGLLINKPROGRAMPROC,            glLinkProgram);
    LOADP(PFNGLGETPROGRAMIVPROC,           glGetProgramiv);
    LOADP(PFNGLGETPROGRAMINFOLOGPROC,      glGetProgramInfoLog);
    LOADP(PFNGLUSEPROGRAMPROC,             glUseProgram);
    LOADP(PFNGLDELETEPROGRAMPROC,          glDeleteProgram);
    LOADP(PFNGLGETUNIFORMLOCATIONPROC,     glGetUniformLocation);
    LOADP(PFNGLUNIFORM1FPROC,              glUniform1f);
    LOADP(PFNGLUNIFORM2FPROC,              glUniform2f);
    LOADP(PFNGLUNIFORM1IPROC,              glUniform1i);
    LOADP(PFNGLUNIFORM1FVPROC,             glUniform1fv);
    LOADP(PFNGLACTIVETEXTUREPROC,          glActiveTexture);
    LOADP(PFNGLBLENDEQUATIONPROC,          glBlendEquation);
    LOADP(PFNGLGETSTRINGIPROC,             glGetStringi);
    #undef LOADP
    if (!ok) return false;
    bool floatOK = true;   // desktop GL 3.3: RGBA16F is renderable+blendable core
#else // IMVARFONT_GLES
    (void)get_proc;  // ES 3 functions are core and linked directly
    // RGBA16F must be color-renderable AND blendable for additive accumulation.
    // EXT_color_buffer_half_float provides both for 16-bit float on ES; without
    // it we keep the atlas (base) path and rasterize coverage on the CPU.
    const bool floatOK = hasExtension("GL_EXT_color_buffer_half_float") ||
                         hasExtension("GL_EXT_color_buffer_float");
    if (!floatOK)
        std::fprintf(stderr, "[ImVarFont::glr] GLES: no blendable float color "
            "buffer; using CPU raster fallback for glyph fill\n");
#endif

    // Base resources: the resolve FBO clears freshly allocated atlas pages, and
    // the atlas/upload path needs nothing beyond core texture + FBO calls. This
    // is enough for the CPU-raster fallback even when the GPU path is disabled.
    glGenFramebuffers(1, &s_resFbo);
    s_ready = true;            // atlas pages are allocated lazily on first glyph

    // GPU analytic-coverage path: needs a blendable float target plus the
    // coverage/resolve programs and their VAOs. Optional — failure here just
    // leaves CoverageReady() false and the caller uses UploadGlyph().
    if (floatOK) {
        s_covProg = linkProgram(kCovVS, kCovFS);
        s_resProg = linkProgram(kResVS, kResFS);
        if (s_covProg && s_resProg) {
            s_covU_dim    = glGetUniformLocation(s_covProg, "uDim");
            s_resU_tex    = glGetUniformLocation(s_resProg, "uCov");
            s_resU_gamma  = glGetUniformLocation(s_resProg, "uGamma");
            s_resU_origin = glGetUniformLocation(s_resProg, "uDstOrigin");

            glGenVertexArrays(1, &s_edgeVao);
            glGenBuffers(1, &s_edgeVbo);
            glBindVertexArray(s_edgeVao);
            glBindBuffer(GL_ARRAY_BUFFER, s_edgeVbo);
            // layout: vec2 aPos, vec4 aEdge -> 6 floats / vertex
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(2 * sizeof(float)));

            static const float quad[] = {
                -1.f, -1.f,  3.f, -1.f,  -1.f, 3.f   // fullscreen triangle
            };
            glGenVertexArrays(1, &s_quadVao);
            glGenBuffers(1, &s_quadVbo);
            glBindVertexArray(s_quadVao);
            glBindBuffer(GL_ARRAY_BUFFER, s_quadVbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);

            glBindVertexArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            s_covReady = true;
        } else {
            if (s_covProg) { glDeleteProgram(s_covProg); s_covProg = 0; }
            if (s_resProg) { glDeleteProgram(s_resProg); s_resProg = 0; }
            std::fprintf(stderr, "[ImVarFont::glr] coverage programs failed to "
                "link; using CPU raster fallback for glyph fill\n");
        }

        // Loop-Blinn analytic-fill programs + VAO (independent of the signed-area
        // cov path; both share the RGBA16F scratch and the resolve FBO).
        s_lbProg    = linkProgram(kLbVS, kLbFS);
        s_lbResProg = linkProgram(kResVS, kLbResFS);
        if (s_lbProg && s_lbResProg) {
            s_lbU_dim       = glGetUniformLocation(s_lbProg,    "uDim");
            s_lbResU_tex    = glGetUniformLocation(s_lbResProg, "uCov");
            s_lbResU_gamma  = glGetUniformLocation(s_lbResProg, "uGamma");
            s_lbResU_origin = glGetUniformLocation(s_lbResProg, "uDstOrigin");
            s_lbResU_ss     = glGetUniformLocation(s_lbResProg, "uSS");

            glGenVertexArrays(1, &s_lbVao);
            glGenBuffers(1, &s_lbVbo);
            glBindVertexArray(s_lbVao);
            glBindBuffer(GL_ARRAY_BUFFER, s_lbVbo);
            // layout: vec2 aPos, vec2 aUV, float aSign, float aSolid -> 6 floats
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(2 * sizeof(float)));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(4 * sizeof(float)));
            glEnableVertexAttribArray(3);
            glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(5 * sizeof(float)));
            glBindVertexArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            s_lbReady = true;
        } else {
            if (s_lbProg)    { glDeleteProgram(s_lbProg);    s_lbProg = 0; }
            if (s_lbResProg) { glDeleteProgram(s_lbResProg); s_lbResProg = 0; }
            std::fprintf(stderr, "[ImVarFont::glr] Loop-Blinn programs failed to "
                "link; that mode will fall back to signed-area coverage\n");
        }

        // Exact-curve analytic-coverage program (single pass, reuses the resolve
        // VS's fullscreen triangle and the existing s_quadVao). Curves arrive
        // through an RGBA32F texture reuploaded per glyph. Flattening-free and
        // matches FreeType's exact-area AA (see kCurveFS).
        s_slugProg = linkProgram(kResVS, kCurveFS);
        if (s_slugProg && s_quadVao) {
            s_slugU_origin = glGetUniformLocation(s_slugProg, "uOrigin");
            s_slugU_count  = glGetUniformLocation(s_slugProg, "uCount");
            s_slugU_curves = glGetUniformLocation(s_slugProg, "uCurves");
            s_slugU_gamma  = glGetUniformLocation(s_slugProg, "uGamma");
            s_slugU_cull   = glGetUniformLocation(s_slugProg, "uCull");
            glGenTextures(1, &s_slugCurveTex);
            glBindTexture(GL_TEXTURE_2D, s_slugCurveTex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
            s_slugReady = (s_slugCurveTex != 0);
        }
        if (!s_slugReady) {
            if (s_slugProg) { glDeleteProgram(s_slugProg); s_slugProg = 0; }
            std::fprintf(stderr, "[ImVarFont::glr] Slug coverage program "
                "unavailable; morph render falls back to Loop-Blinn/signed-area\n");
        }

        // GPU morph-reconstruction program (reconstruct pass; the coverage pass
        // reuses s_slugProg/kCurveFS). Needs the exact-curve coverage path too.
        if (s_slugReady) {
            s_reconProg = linkProgram(kResVS, kReconFS);
            if (s_reconProg) {
                s_reconU_data   = glGetUniformLocation(s_reconProg, "uData");
                s_reconU_terms  = glGetUniformLocation(s_reconProg, "uTerms");
                s_reconU_weight = glGetUniformLocation(s_reconProg, "uWeight");
                s_reconU_scale  = glGetUniformLocation(s_reconProg, "uScale");
                s_reconU_origin = glGetUniformLocation(s_reconProg, "uOrigin");
                glGenFramebuffers(1, &s_reconFbo);
                s_reconReady = (s_reconFbo != 0);
            }
            if (!s_reconReady) {
                if (s_reconProg) { glDeleteProgram(s_reconProg); s_reconProg = 0; }
                std::fprintf(stderr, "[ImVarFont::glr] morph-reconstruction program "
                    "unavailable; morph render falls back to CPU reconstruction\n");
            }
        }
    }
    return true;
}

void Shutdown() {
    if (s_covProg)  { glDeleteProgram(s_covProg);   s_covProg = 0; }
    if (s_resProg)  { glDeleteProgram(s_resProg);   s_resProg = 0; }
    if (s_lbProg)   { glDeleteProgram(s_lbProg);    s_lbProg = 0; }
    if (s_lbResProg){ glDeleteProgram(s_lbResProg); s_lbResProg = 0; }
    if (s_slugProg) { glDeleteProgram(s_slugProg);  s_slugProg = 0; }
    if (s_slugCurveTex) { glDeleteTextures(1, &s_slugCurveTex); s_slugCurveTex = 0; }
    if (s_reconProg) { glDeleteProgram(s_reconProg); s_reconProg = 0; }
    if (s_reconTex)  { glDeleteTextures(1, &s_reconTex); s_reconTex = 0; }
    if (s_reconFbo)  { glDeleteFramebuffers(1, &s_reconFbo); s_reconFbo = 0; }
    if (s_edgeVbo)  { glDeleteBuffers(1, &s_edgeVbo); s_edgeVbo = 0; }
    if (s_quadVbo)  { glDeleteBuffers(1, &s_quadVbo); s_quadVbo = 0; }
    if (s_lbVbo)    { glDeleteBuffers(1, &s_lbVbo);   s_lbVbo = 0; }
    if (s_edgeVao)  { glDeleteVertexArrays(1, &s_edgeVao); s_edgeVao = 0; }
    if (s_quadVao)  { glDeleteVertexArrays(1, &s_quadVao); s_quadVao = 0; }
    if (s_lbVao)    { glDeleteVertexArrays(1, &s_lbVao);   s_lbVao = 0; }
    if (s_covFbo)   { glDeleteFramebuffers(1, &s_covFbo);  s_covFbo = 0; }
    if (s_resFbo)   { glDeleteFramebuffers(1, &s_resFbo);  s_resFbo = 0; }
    if (s_covTex)   { glDeleteTextures(1, &s_covTex); s_covTex = 0; }
    for (auto& p : s_pages)
        if (p.tex) glDeleteTextures(1, &p.tex);
    s_pages.clear();
    if (!s_dedicated.empty()) {
        glDeleteTextures((GLsizei)s_dedicated.size(), s_dedicated.data());
        s_dedicated.clear();
    }
    for (auto& p : s_livePages)
        if (p.tex) glDeleteTextures(1, &p.tex);
    s_livePages.clear();
    s_liveCur = 0;
    if (!s_liveDedicated.empty()) {
        glDeleteTextures((GLsizei)s_liveDedicated.size(), s_liveDedicated.data());
        s_liveDedicated.clear();
    }
    s_liveNeedsReset = false;
    s_resetPending = false;
    ++s_atlasGen;
    s_covW = s_covH = 0;
    s_reconW   = 0;
    s_ready    = false;
    s_covReady = false;
    s_lbReady  = false;
    s_slugReady = false;
    s_reconReady = false;
}

void SetForceCpuFallback(bool enable) {
    if (enable == s_forceCpu)
        return;
    s_forceCpu = enable;
    // Invalidate cached cells so glyphs re-render through the now-active path
    // (GPU<->CPU) on the next frame. Cheap: only the user toggling the option.
    ++s_atlasGen;
}
void SetPreferSlug(bool enable) {
    if (enable == s_preferSlug)
        return;
    s_preferSlug = enable;
    ++s_atlasGen;   // re-render cached cells through the now-active path
}
bool PreferSlug() { return s_preferSlug && s_slugReady && !s_forceCpu; }

void SetCullEnabled(bool enable) { s_cullEnabled = enable; }
bool CullEnabled() { return s_cullEnabled; }
int  LivePageCount() { return (int)s_livePages.size(); }

bool Ready() { return s_ready; }
bool CoverageReady() { return s_covReady && !s_forceCpu; }
bool LoopBlinnReady() { return s_lbReady && !s_forceCpu; }
bool SlugReady() { return s_slugReady && !s_forceCpu; }
bool MorphReady() { return s_reconReady && s_slugReady && !s_forceCpu; }
int  MorphMaxTerms() { return kReconMaxTerms; }
unsigned int AtlasGen() { return s_atlasGen; }

void BeginFrame() {
    applyResetIfPending();
    // Flag live pages for a rewind; the actual clear runs inside the next live
    // RenderGlyphCurves call, where GL state is already saved/restored.
    s_liveNeedsReset = true;
}

GlyphTex RenderGlyph(const float* edges, int edge_count, int w, int h, float gamma,
                     bool live) {
    GlyphTex out;
    if (!s_covReady || !edges || edge_count <= 0 || w <= 0 || h <= 0)
        return out;
    if (!ensureScratch(w, h))
        return out;

    // ----- save GL state we touch -----
    GLint   prevFbo = 0;   glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    GLint   prevVp[4];     glGetIntegerv(GL_VIEWPORT, prevVp);
    GLint   prevScis[4];   glGetIntegerv(GL_SCISSOR_BOX, prevScis);
    GLboolean prevScisOn = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean prevBlend  = glIsEnabled(GL_BLEND);
    GLint   prevBlendSrc = 0; glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevBlendSrc);
    GLint   prevBlendDst = 0; glGetIntegerv(GL_BLEND_DST_ALPHA, &prevBlendDst);
    GLint   prevProg = 0;  glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    GLint   prevVao  = 0;  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    GLint   prevActive = GL_TEXTURE0; glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);

    // Live pages are rewound here (state already saved), once per frame.
    if (live && s_liveNeedsReset) {
        resetLivePagesNow();
        s_liveNeedsReset = false;
    }

    // ----- build edge geometry (cell pixel space, y flipped to bottom-up) -----
    std::vector<float> verts;
    verts.reserve((size_t)edge_count * 6 * 6);
    const float fw = (float)w, fh = (float)h;
    for (int e = 0; e < edge_count; ++e) {
        float x0 = edges[e * 4 + 0];
        float y0 = fh - edges[e * 4 + 1];   // y-down -> bottom-up
        float x1 = edges[e * 4 + 2];
        float y1 = fh - edges[e * 4 + 3];
        if (std::fabs(y1 - y0) < 1e-6f) continue;  // horizontal: no contribution

        float qx0 = std::floor((x0 < x1 ? x0 : x1));
        if (qx0 < 0.f) qx0 = 0.f;
        float qx1 = fw;                              // extend to right border
        float qy0 = std::floor((y0 < y1 ? y0 : y1));
        float qy1 = std::ceil ((y0 > y1 ? y0 : y1));
        if (qy0 < 0.f) qy0 = 0.f;
        if (qy1 > fh)  qy1 = fh;
        if (qx1 <= qx0 || qy1 <= qy0) continue;

        const float E0 = x0, E1 = y0, E2 = x1, E3 = y1;
        const float corners[4][2] = {
            { qx0, qy0 }, { qx1, qy0 }, { qx1, qy1 }, { qx0, qy1 }
        };
        const int idx[6] = { 0, 1, 2, 0, 2, 3 };
        for (int k = 0; k < 6; ++k) {
            verts.push_back(corners[idx[k]][0]);
            verts.push_back(corners[idx[k]][1]);
            verts.push_back(E0); verts.push_back(E1);
            verts.push_back(E2); verts.push_back(E3);
        }
    }
    if (verts.empty())
        return out;

    // ===== Pass 1: accumulate signed coverage into the scratch buffer =====
    glBindFramebuffer(GL_FRAMEBUFFER, s_covFbo);
    glViewport(0, 0, w, h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, w, h);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);

    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE);

    glUseProgram(s_covProg);
    glUniform2f(s_covU_dim, fw, fh);
    glBindVertexArray(s_edgeVao);
    glBindBuffer(GL_ARRAY_BUFFER, s_edgeVbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.size() * sizeof(float)),
                 verts.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(verts.size() / 6));

    // ----- pick a destination: (live or persistent) atlas sub-rect, else a
    //       dedicated texture -----
    GLuint dstTex = 0;
    int    dx = 0, dy = 0, dstW = w, dstH = h;
    const bool packed = live ? packCellLive(w, h, &dstTex, &dx, &dy)
                             : packCell(w, h, &dstTex, &dx, &dy);
    if (packed) {
        dstW = dstH = kAtlasSize;
    } else {
        dstTex = allocCellTexture(w, h);          // oversized cell (big zoom)
        (live ? s_liveDedicated : s_dedicated).push_back(dstTex);
        dx = dy = 0;
    }
    if (!dstTex)
        return out;

    // ===== Pass 2: resolve |coverage| -> RGBA8 destination sub-rect =====
    glBindFramebuffer(GL_FRAMEBUFFER, s_resFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dstTex, 0);
    glViewport(dx, dy, w, h);                      // viewport (not scissor) confines the write
    glDisable(GL_BLEND);
    glUseProgram(s_resProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_covTex);
    glUniform1i(s_resU_tex, 0);
    glUniform1f(s_resU_gamma, (gamma > 0.f) ? gamma : 1.f);
    glUniform2f(s_resU_origin, (float)dx, (float)dy);
    glBindVertexArray(s_quadVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

    // ----- restore GL state -----
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
    glScissor(prevScis[0], prevScis[1], prevScis[2], prevScis[3]);
    if (prevScisOn) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (prevBlend)  glEnable(GL_BLEND);        else glDisable(GL_BLEND);
    glBlendFunc(prevBlendSrc, prevBlendDst);
    glUseProgram((GLuint)prevProg);
    glBindVertexArray((GLuint)prevVao);
    glActiveTexture((GLenum)prevActive);

    // UVs: coverage is written bottom-up, so the glyph TOP sits at the higher
    // atlas row (dy + h). (u0,v0) -> screen top-left, (u1,v1) -> bottom-right.
    const float fW = (float)dstW, fH = (float)dstH;
    out.tex   = dstTex;
    out.u0    = (float)dx / fW;
    out.v0    = (float)(dy + h) / fH;
    out.u1    = (float)(dx + w) / fW;
    out.v1    = (float)dy / fH;
    out.w     = w;
    out.h     = h;
    out.gen   = s_atlasGen;
    out.valid = true;
    return out;
}

GlyphTex RenderGlyphCurves(const Curve* curves, int count, int w, int h, float gamma, bool live) {
    GlyphTex out;
    if (!s_lbReady || !curves || count <= 0 || w <= 0 || h <= 0)
        return out;

    // Supersample factor for edge AA; clamp so the scratch stays within limits.
    int ss = 4;
    while (ss > 1 && (w * ss > 4096 || h * ss > 4096)) --ss;
    const int sw = w * ss, sh = h * ss;
    if (!ensureScratch(sw, sh))
        return out;

    // ----- save GL state we touch (same set as RenderGlyph) -----
    GLint   prevFbo = 0;   glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    GLint   prevVp[4];     glGetIntegerv(GL_VIEWPORT, prevVp);
    GLint   prevScis[4];   glGetIntegerv(GL_SCISSOR_BOX, prevScis);
    GLboolean prevScisOn = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean prevBlend  = glIsEnabled(GL_BLEND);
    GLint   prevBlendSrc = 0; glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevBlendSrc);
    GLint   prevBlendDst = 0; glGetIntegerv(GL_BLEND_DST_ALPHA, &prevBlendDst);
    GLint   prevProg = 0;  glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    GLint   prevVao  = 0;  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    GLint   prevActive = GL_TEXTURE0; glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);

    // Live pages are rewound here (state is saved), once per frame.
    if (live && s_liveNeedsReset) {
        resetLivePagesNow();
        s_liveNeedsReset = false;
    }

    // ----- build geometry (supersample pixel space, y flipped to bottom-up) -----
    // Winding fan anchored at the cell origin: each segment contributes a chord
    // triangle (O, A, B); quadratics add one analytic curve triangle (A, K, B).
    // Per-triangle orientation sign turns the fan into the non-zero winding number.
    std::vector<float> verts;
    verts.reserve((size_t)count * 6 * 6);
    const float fsw = (float)sw, fsh = (float)sh;
    const float O[2] = { 0.f, 0.f };
    auto area2 = [](const float* a, const float* b, const float* c) {
        return (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
    };
    auto emitTri = [&](const float* a, const float* b, const float* c,
                       float ua, float va, float ub, float vb, float uc, float vc,
                       float sgn, float solid) {
        const float P[3][2] = { { a[0], a[1] }, { b[0], b[1] }, { c[0], c[1] } };
        const float UV[3][2] = { { ua, va }, { ub, vb }, { uc, vc } };
        for (int k = 0; k < 3; ++k) {
            verts.push_back(P[k][0]); verts.push_back(P[k][1]);
            verts.push_back(UV[k][0]); verts.push_back(UV[k][1]);
            verts.push_back(sgn); verts.push_back(solid);
        }
    };
    for (int i = 0; i < count; ++i) {
        const Curve& c = curves[i];
        const bool quad = (c.type == 2);
        const float A[2] = { c.p[0] * ss, fsh - c.p[1] * ss };
        const float B[2] = { (quad ? c.p[4] : c.p[2]) * ss,
                             fsh - (quad ? c.p[5] : c.p[3]) * ss };
        const float s1 = area2(O, A, B);
        if (s1 != 0.f)
            emitTri(O, A, B, 0,0, 0,0, 0,0, (s1 > 0.f) ? 1.f : -1.f, 1.f);
        if (quad) {
            const float K[2] = { c.p[2] * ss, fsh - c.p[3] * ss };
            const float s2 = area2(A, K, B);
            if (s2 != 0.f)
                emitTri(A, K, B, 0.f,0.f, 0.5f,0.f, 1.f,1.f,
                        (s2 > 0.f) ? 1.f : -1.f, 0.f);
        }
    }
    if (verts.empty())
        return out;

    // ===== Pass 1: accumulate winding into the supersample scratch =====
    glBindFramebuffer(GL_FRAMEBUFFER, s_covFbo);
    glViewport(0, 0, sw, sh);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, sw, sh);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);

    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE);

    glUseProgram(s_lbProg);
    glUniform2f(s_lbU_dim, fsw, fsh);
    glBindVertexArray(s_lbVao);
    glBindBuffer(GL_ARRAY_BUFFER, s_lbVbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.size() * sizeof(float)),
                 verts.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(verts.size() / 6));

    // ----- pick a destination: (live or persistent) atlas sub-rect, else a
    //       dedicated texture -----
    GLuint dstTex = 0;
    int    dx = 0, dy = 0, dstW = w, dstH = h;
    const bool packed = live ? packCellLive(w, h, &dstTex, &dx, &dy)
                             : packCell(w, h, &dstTex, &dx, &dy);
    if (packed) {
        dstW = dstH = kAtlasSize;
    } else {
        dstTex = allocCellTexture(w, h);
        (live ? s_liveDedicated : s_dedicated).push_back(dstTex);
        dx = dy = 0;
    }
    if (!dstTex)
        return out;

    // ===== Pass 2: box-downsample winding -> RGBA8 destination sub-rect =====
    glBindFramebuffer(GL_FRAMEBUFFER, s_resFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dstTex, 0);
    glViewport(dx, dy, w, h);
    glDisable(GL_BLEND);
    glUseProgram(s_lbResProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_covTex);
    glUniform1i(s_lbResU_tex, 0);
    glUniform1f(s_lbResU_gamma, (gamma > 0.f) ? gamma : 1.f);
    glUniform2f(s_lbResU_origin, (float)dx, (float)dy);
    glUniform1i(s_lbResU_ss, ss);
    glBindVertexArray(s_quadVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

    // ----- restore GL state -----
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
    glScissor(prevScis[0], prevScis[1], prevScis[2], prevScis[3]);
    if (prevScisOn) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (prevBlend)  glEnable(GL_BLEND);        else glDisable(GL_BLEND);
    glBlendFunc(prevBlendSrc, prevBlendDst);
    glUseProgram((GLuint)prevProg);
    glBindVertexArray((GLuint)prevVao);
    glActiveTexture((GLenum)prevActive);

    // UVs: written bottom-up (glyph TOP at the higher atlas row), matching
    // RenderGlyph so the cache/placement path is identical.
    const float fW = (float)dstW, fH = (float)dstH;
    out.tex   = dstTex;
    out.u0    = (float)dx / fW;
    out.v0    = (float)(dy + h) / fH;
    out.u1    = (float)(dx + w) / fW;
    out.v1    = (float)dy / fH;
    out.w     = w;
    out.h     = h;
    out.gen   = s_atlasGen;
    out.valid = true;
    return out;
}

GlyphTex RenderGlyphSlug(const Curve* curves, int count, int w, int h, float gamma, bool live) {
    GlyphTex out;
    if (!s_slugReady || !curves || count <= 0 || w <= 0 || h <= 0)
        return out;

    // Pack curves into the RGBA32F curve texture: 2 texels per quad,
    //   texel 2i   = (p1.x, p1.y, p2.x, p2.y)
    //   texel 2i+1 = (p3.x, p3.y, 0, 0)
    // Input curves are CELL PIXEL space, y-down; we flip to the FBO's y-up space
    // (y' = h - y) so they share the fragment's coordinate frame. A line is a
    // degenerate quadratic with its control point at the segment midpoint.
    std::vector<float> buf((size_t)count * 8, 0.f);
    for (int i = 0; i < count; ++i) {
        const Curve& c = curves[i];
        const float fh = (float)h;
        const float x0 = c.p[0],          y0 = fh - c.p[1];
        float x1, y1, x2, y2;
        if (c.type == 2) {
            x1 = c.p[2];                  y1 = fh - c.p[3];
            x2 = c.p[4];                  y2 = fh - c.p[5];
        } else {
            x2 = c.p[2];                  y2 = fh - c.p[3];
            x1 = 0.5f * (x0 + x2);        y1 = 0.5f * (y0 + y2);
        }
        float* t = &buf[(size_t)i * 8];
        t[0] = x0; t[1] = y0; t[2] = x1; t[3] = y1;
        t[4] = x2; t[5] = y2; t[6] = 0.f; t[7] = 0.f;
    }

    // ----- save GL state we touch (same set as RenderGlyphCurves, + bound tex) -----
    GLint   prevFbo = 0;   glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    GLint   prevTex = 0;   glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    GLint   prevVp[4];     glGetIntegerv(GL_VIEWPORT, prevVp);
    GLint   prevScis[4];   glGetIntegerv(GL_SCISSOR_BOX, prevScis);
    GLboolean prevScisOn = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean prevBlend  = glIsEnabled(GL_BLEND);
    GLint   prevProg = 0;  glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    GLint   prevVao  = 0;  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    GLint   prevActive = GL_TEXTURE0; glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);

    // Live pages are rewound here (state is saved), once per frame.
    if (live && s_liveNeedsReset) {
        resetLivePagesNow();
        s_liveNeedsReset = false;
    }

    // Upload the curve texture (2 texels per curve, single row).
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_slugCurveTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, count * 2, 1, 0, GL_RGBA, GL_FLOAT, buf.data());

    // ----- pick a destination cell -----
    GLuint dstTex = 0;
    int    dx = 0, dy = 0, dstW = w, dstH = h;
    const bool packed = live ? packCellLive(w, h, &dstTex, &dx, &dy)
                             : packCell(w, h, &dstTex, &dx, &dy);
    if (packed) {
        dstW = dstH = kAtlasSize;
    } else {
        dstTex = allocCellTexture(w, h);
        (live ? s_liveDedicated : s_dedicated).push_back(dstTex);
        dx = dy = 0;
    }
    if (!dstTex)
        return out;

    // ----- single analytic pass straight into the destination cell sub-rect -----
    glBindFramebuffer(GL_FRAMEBUFFER, s_resFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dstTex, 0);
    glViewport(dx, dy, w, h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glUseProgram(s_slugProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_slugCurveTex);
    glUniform1i(s_slugU_curves, 0);
    glUniform1i(s_slugU_count, count);
    glUniform2f(s_slugU_origin, (float)dx, (float)dy);
    glUniform1f(s_slugU_gamma, (gamma > 0.f) ? gamma : 1.f);
    glUniform1i(s_slugU_cull, s_cullEnabled ? 1 : 0);
    glBindVertexArray(s_quadVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

    // ----- restore GL state -----
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
    glScissor(prevScis[0], prevScis[1], prevScis[2], prevScis[3]);
    if (prevScisOn) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (prevBlend)  glEnable(GL_BLEND);        else glDisable(GL_BLEND);
    glUseProgram((GLuint)prevProg);
    glBindVertexArray((GLuint)prevVao);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glActiveTexture((GLenum)prevActive);

    // UVs: glyph TOP at the higher atlas row, matching RenderGlyph(Curves).
    const float fW = (float)dstW, fH = (float)dstH;
    out.tex   = dstTex;
    out.u0    = (float)dx / fW;
    out.v0    = (float)(dy + h) / fH;
    out.u1    = (float)(dx + w) / fW;
    out.v1    = (float)dy / fH;
    out.w     = w;
    out.h     = h;
    out.gen   = s_atlasGen;
    out.valid = true;
    return out;
}

void DeleteMorphCurves(unsigned int* tex) {
    if (tex && *tex) { glDeleteTextures(1, (const GLuint*)tex); *tex = 0; }
}

bool UpdateMorphCurves(unsigned int* tex, const Curve* base, const Curve* deltas,
                       int count, int termCount) {
    if (!s_reconReady || !tex || !base || count <= 0 ||
        termCount < 0 || termCount > kReconMaxTerms)
        return false;

    // Delta buffer: width 2*count (2 texels per quad), height 1+termCount.
    //   row 0           : base control points (design units)
    //   row 1 + t       : delta of correction term t (design units)
    // texel 2i = (p0x,p0y,p1x,p1y), texel 2i+1 = (p2x,p2y,0,0).
    const int rows = 1 + termCount;
    std::vector<float> buf((size_t)(2 * count) * rows * 4, 0.f);
    auto put = [&](int row, const Curve* src) {
        for (int i = 0; i < count; ++i) {
            float* t = &buf[((size_t)row * (2 * count) + (size_t)2 * i) * 4];
            t[0] = src[i].p[0]; t[1] = src[i].p[1];   // p0
            t[2] = src[i].p[2]; t[3] = src[i].p[3];   // p1 (line: midpoint set by caller)
            t[4] = src[i].p[4]; t[5] = src[i].p[5];   // p2
            t[6] = 0.f;         t[7] = 0.f;
        }
    };
    put(0, base);
    for (int t = 0; t < termCount; ++t)
        put(1 + t, deltas + (size_t)t * count);

    GLint prevTex = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    if (*tex == 0) glGenTextures(1, (GLuint*)tex);
    glBindTexture(GL_TEXTURE_2D, (GLuint)*tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 2 * count, rows, 0, GL_RGBA, GL_FLOAT, buf.data());
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    return true;
}

GlyphTex RenderMorphGlyph(unsigned int dataTex, int count, int termCount,
                          const float* weights, float originX, float originY,
                          float scaleX, float scaleY, int w, int h,
                          float gamma, bool live) {
    GlyphTex out;
    if (!s_reconReady || dataTex == 0 || count <= 0 || w <= 0 || h <= 0 ||
        termCount < 0 || termCount > kReconMaxTerms || (weights == nullptr && termCount > 0))
        return out;
    if (!ensureReconTex(2 * count))
        return out;

    // ----- save GL state we touch -----
    GLint   prevFbo = 0;   glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    GLint   prevTex = 0;   glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    GLint   prevVp[4];     glGetIntegerv(GL_VIEWPORT, prevVp);
    GLint   prevScis[4];   glGetIntegerv(GL_SCISSOR_BOX, prevScis);
    GLboolean prevScisOn = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean prevBlend  = glIsEnabled(GL_BLEND);
    GLint   prevProg = 0;  glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    GLint   prevVao  = 0;  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    GLint   prevActive = GL_TEXTURE0; glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);

    if (live && s_liveNeedsReset) {
        resetLivePagesNow();
        s_liveNeedsReset = false;
    }

    // ===== Pass 1: reconstruct base + Σ weight·delta into s_reconTex (design->cell) =====
    // s_reconTex is already attached to s_reconFbo (in ensureReconTex), so no
    // per-glyph attach/detach or completeness query here.
    glBindFramebuffer(GL_FRAMEBUFFER, s_reconFbo);
    glViewport(0, 0, 2 * count, 1);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glUseProgram(s_reconProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)dataTex);
    glUniform1i(s_reconU_data, 0);
    glUniform1i(s_reconU_terms, termCount);
    if (termCount > 0) glUniform1fv(s_reconU_weight, termCount, weights);
    glUniform2f(s_reconU_scale, scaleX, scaleY);
    glUniform2f(s_reconU_origin, originX, originY);
    glBindVertexArray(s_quadVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // ----- pick a destination cell -----
    GLuint dstTex = 0;
    int    dx = 0, dy = 0, dstW = w, dstH = h;
    const bool packed = live ? packCellLive(w, h, &dstTex, &dx, &dy)
                             : packCell(w, h, &dstTex, &dx, &dy);
    if (packed) {
        dstW = dstH = kAtlasSize;
    } else {
        dstTex = allocCellTexture(w, h);
        (live ? s_liveDedicated : s_dedicated).push_back(dstTex);
        dx = dy = 0;
    }
    if (!dstTex)
        return out;

    // ===== Pass 2: exact-curve coverage of the reconstructed curves -> cell =====
    glBindFramebuffer(GL_FRAMEBUFFER, s_resFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dstTex, 0);
    glViewport(dx, dy, w, h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glUseProgram(s_slugProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_reconTex);
    glUniform1i(s_slugU_curves, 0);
    glUniform1i(s_slugU_count, count);
    glUniform2f(s_slugU_origin, (float)dx, (float)dy);
    glUniform1f(s_slugU_gamma, (gamma > 0.f) ? gamma : 1.f);
    glUniform1i(s_slugU_cull, s_cullEnabled ? 1 : 0);
    glBindVertexArray(s_quadVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

    // ----- restore GL state -----
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
    glScissor(prevScis[0], prevScis[1], prevScis[2], prevScis[3]);
    if (prevScisOn) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (prevBlend)  glEnable(GL_BLEND);        else glDisable(GL_BLEND);
    glUseProgram((GLuint)prevProg);
    glBindVertexArray((GLuint)prevVao);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glActiveTexture((GLenum)prevActive);

    const float fW = (float)dstW, fH = (float)dstH;
    out.tex   = dstTex;
    out.u0    = (float)dx / fW;
    out.v0    = (float)(dy + h) / fH;
    out.u1    = (float)(dx + w) / fW;
    out.v1    = (float)dy / fH;
    out.w     = w;
    out.h     = h;
    out.gen   = s_atlasGen;
    out.valid = true;
    return out;
}

GlyphTex UploadGlyph(const unsigned char* a8, int w, int h, bool live) {
    GlyphTex out;
    if (!s_ready || !a8 || w <= 0 || h <= 0)
        return out;

    // Expand single-channel coverage to (255,255,255,coverage) so the cell
    // composites as colour*coverage, exactly like the resolved GPU cell.
    std::vector<unsigned char> rgba((size_t)w * h * 4);
    for (int i = 0; i < w * h; ++i) {
        rgba[i * 4 + 0] = 255;
        rgba[i * 4 + 1] = 255;
        rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = a8[i];
    }

    // packCell()/allocPage() touch the FBO binding, viewport, scissor and clear
    // colour without restoring; save everything we (or they) disturb.
    GLint     prevFbo    = 0;          glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    GLint     prevTex    = 0;          glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    GLint     prevUnpack = 4;          glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevUnpack);
    GLint     prevVp[4];               glGetIntegerv(GL_VIEWPORT, prevVp);
    GLint     prevScis[4];             glGetIntegerv(GL_SCISSOR_BOX, prevScis);
    GLboolean prevScisOn = glIsEnabled(GL_SCISSOR_TEST);
    GLfloat   prevClear[4];            glGetFloatv(GL_COLOR_CLEAR_VALUE, prevClear);

    GLuint dstTex = 0;
    int    dx = 0, dy = 0, dstW = w, dstH = h;
    const bool atlas = live ? packCellLive(w, h, &dstTex, &dx, &dy)
                            : packCell(w, h, &dstTex, &dx, &dy);
    if (atlas) {
        dstW = dstH = kAtlasSize;
    } else {
        dstTex = allocCellTexture(w, h);          // oversized cell (big zoom)
        if (dstTex) (live ? s_liveDedicated : s_dedicated).push_back(dstTex);
        dx = dy = 0;
    }

    if (dstTex) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);    // RGBA rows are 4-byte aligned
        glBindTexture(GL_TEXTURE_2D, dstTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, dx, dy, w, h,
                        GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    }

    // ----- restore GL state -----
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prevUnpack);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
    glScissor(prevScis[0], prevScis[1], prevScis[2], prevScis[3]);
    if (prevScisOn) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    glClearColor(prevClear[0], prevClear[1], prevClear[2], prevClear[3]);

    if (!dstTex)
        return out;

    // Coverage is uploaded top-down (row 0 = glyph top), so the top maps to the
    // lower V. (u0,v0) -> screen top-left, (u1,v1) -> bottom-right.
    const float fW = (float)dstW, fH = (float)dstH;
    out.tex   = dstTex;
    out.u0    = (float)dx / fW;
    out.v0    = (float)dy / fH;
    out.u1    = (float)(dx + w) / fW;
    out.v1    = (float)(dy + h) / fH;
    out.w     = w;
    out.h     = h;
    out.gen   = s_atlasGen;
    out.valid = true;
    return out;
}

} // namespace glr
} // namespace ImVarFont
