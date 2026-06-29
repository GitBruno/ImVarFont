// imvarfont_gl.cpp  -  GPU analytic-coverage glyph rasterizer (signed-area)
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

#include "imvarfont_gl.h"

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
    }
    return true;
}

void Shutdown() {
    if (s_covProg)  { glDeleteProgram(s_covProg);   s_covProg = 0; }
    if (s_resProg)  { glDeleteProgram(s_resProg);   s_resProg = 0; }
    if (s_edgeVbo)  { glDeleteBuffers(1, &s_edgeVbo); s_edgeVbo = 0; }
    if (s_quadVbo)  { glDeleteBuffers(1, &s_quadVbo); s_quadVbo = 0; }
    if (s_edgeVao)  { glDeleteVertexArrays(1, &s_edgeVao); s_edgeVao = 0; }
    if (s_quadVao)  { glDeleteVertexArrays(1, &s_quadVao); s_quadVao = 0; }
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
    s_resetPending = false;
    ++s_atlasGen;
    s_covW = s_covH = 0;
    s_ready    = false;
    s_covReady = false;
}

void SetForceCpuFallback(bool enable) {
    if (enable == s_forceCpu)
        return;
    s_forceCpu = enable;
    // Invalidate cached cells so glyphs re-render through the now-active path
    // (GPU<->CPU) on the next frame. Cheap: only the user toggling the option.
    ++s_atlasGen;
}
bool Ready() { return s_ready; }
bool CoverageReady() { return s_covReady && !s_forceCpu; }
unsigned int AtlasGen() { return s_atlasGen; }

void BeginFrame() { applyResetIfPending(); }

GlyphTex RenderGlyph(const float* edges, int edge_count, int w, int h, float gamma) {
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

    // ----- pick a destination: atlas sub-rect, else a dedicated texture -----
    GLuint dstTex = 0;
    int    dx = 0, dy = 0, dstW = w, dstH = h;
    if (packCell(w, h, &dstTex, &dx, &dy)) {
        dstW = dstH = kAtlasSize;
    } else {
        dstTex = allocCellTexture(w, h);          // oversized cell (big zoom)
        s_dedicated.push_back(dstTex);
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

GlyphTex UploadGlyph(const unsigned char* a8, int w, int h) {
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
    const bool atlas = packCell(w, h, &dstTex, &dx, &dy);
    if (atlas) {
        dstW = dstH = kAtlasSize;
    } else {
        dstTex = allocCellTexture(w, h);          // oversized cell (big zoom)
        if (dstTex) s_dedicated.push_back(dstTex);
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
