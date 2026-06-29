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

namespace ImVarFont {
namespace glr {

// ---------------------------------------------------------------------------
// Loaded GL entry points (only the non-1.1 functions we use). Declared in this
// namespace, so unqualified use resolves here; 1.1 functions (glTexImage2D,
// glViewport, glDrawArrays, ...) are used unqualified and resolve to the global
// opengl32 symbols.
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

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static bool   s_ready      = false;
static GLuint s_covProg    = 0;   // accumulate program
static GLuint s_resProg    = 0;   // resolve program
static GLint  s_covU_dim   = -1;  // vec2 cell dim (w,h)
static GLint  s_resU_tex   = -1;  // sampler
static GLint  s_resU_gamma = -1;  // float

static GLuint s_covFbo     = 0;   // accumulate FBO
static GLuint s_covTex     = 0;   // RGBA16F scratch
static int    s_covW       = 0;   // scratch size
static int    s_covH       = 0;

static GLuint s_resFbo     = 0;   // resolve FBO (output tex attached per call)

static GLuint s_edgeVao    = 0;
static GLuint s_edgeVbo    = 0;
static GLuint s_quadVao    = 0;
static GLuint s_quadVbo    = 0;

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------
static const char* kCovVS =
    "#version 330 core\n"
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
    "#version 330 core\n"
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
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"   // clip-space fullscreen triangle/quad
    "void main(){ gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char* kResFS =
    "#version 330 core\n"
    "uniform sampler2D uCov;\n"
    "uniform float uGamma;\n"
    "out vec4 frag;\n"
    "void main(){\n"
    "  float cov = texelFetch(uCov, ivec2(gl_FragCoord.xy), 0).r;\n"
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, nw, nh, 0, GL_RGBA, GL_FLOAT, nullptr);
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

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool Init(GLProc get_proc) {
    if (s_ready) return true;
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
    #undef LOADP
    if (!ok) return false;

    s_covProg = linkProgram(kCovVS, kCovFS);
    s_resProg = linkProgram(kResVS, kResFS);
    if (!s_covProg || !s_resProg) { Shutdown(); return false; }

    s_covU_dim   = glGetUniformLocation(s_covProg, "uDim");
    s_resU_tex   = glGetUniformLocation(s_resProg, "uCov");
    s_resU_gamma = glGetUniformLocation(s_resProg, "uGamma");

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

    glGenFramebuffers(1, &s_resFbo);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    s_ready = true;
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
    s_covW = s_covH = 0;
    s_ready = false;
}

bool Ready() { return s_ready; }

GlyphTex RenderGlyph(const float* edges, int edge_count, int w, int h, float gamma) {
    GlyphTex out;
    if (!s_ready || !edges || edge_count <= 0 || w <= 0 || h <= 0)
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

    // ----- create output texture -----
    GLuint outTex = 0;
    glGenTextures(1, &outTex);
    glBindTexture(GL_TEXTURE_2D, outTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // ===== Pass 1: accumulate signed coverage =====
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

    // ===== Pass 2: resolve |coverage| -> RGBA8 cell =====
    glBindFramebuffer(GL_FRAMEBUFFER, s_resFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outTex, 0);
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glUseProgram(s_resProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_covTex);
    glUniform1i(s_resU_tex, 0);
    glUniform1f(s_resU_gamma, (gamma > 0.f) ? gamma : 1.f);
    glBindVertexArray(s_quadVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    // detach so the output texture isn't left bound to the FBO
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

    out.tex   = outTex;
    out.w     = w;
    out.h     = h;
    out.valid = true;
    return out;
}

void Delete(unsigned int tex) {
    if (tex) { GLuint t = tex; glDeleteTextures(1, &t); }
}

} // namespace glr
} // namespace ImVarFont
