// imvarfont_gl.h  -  GPU analytic-coverage glyph rasterizer (internal)
//
// Backend-agnostic, dependency-free GL helper used by the analytic glyph
// renderer. It knows nothing about FreeType or fonts: it takes a soup of line
// edges (in cell pixel space) and produces a single-channel coverage texture
// via signed-area accumulation (font-rs / Pathfinder style), with exact,
// conflation-free anti-aliasing and no MSAA.
//
// GL entry points are loaded through a caller-provided proc loader (e.g.
// glfwGetProcAddress) so the library pulls in no GL loader of its own.
//
// The seam (RenderGlyph taking an edge soup + cell size) is deliberately narrow
// so a future Loop-Blinn or per-pixel banding backend can replace only this file.

#pragma once

namespace ImVarFont {
namespace glr {

// Proc loader signature, e.g. (GLProc)glfwGetProcAddress.
typedef void* (*GLProc)(const char*);

// One rasterized glyph as its own coverage texture (v1 uses one texture per
// cached glyph cell; a shelf-packed atlas can replace this later behind the
// same seam). The texture is RGBA8 holding (1,1,1,coverage) so ImGui's default
// shader composites it as colour*coverage with no custom shader.
struct GlyphTex {
    unsigned int tex   = 0;
    int          w     = 0;
    int          h     = 0;
    bool         valid = false;
};

// Initialize GL resources (shaders, FBOs, VAO/VBO). Safe to call once a GL
// context is current. Returns false if GL entry points or programs failed.
bool Init(GLProc get_proc);

// Release all GL resources. Safe to call with no context (best-effort).
void Shutdown();

// True when Init succeeded and the renderer is usable this frame.
bool Ready();

// Rasterize the given line edges into a new coverage texture of size (w,h).
//   edges       : flat array [x0,y0,x1,y1, ...] in CELL PIXEL space, y-down,
//                 already offset so the glyph sits inside [0,w] x [0,h].
//   edge_count  : number of edges (so the array has edge_count*4 floats).
//   w,h         : texture/cell size in pixels (> 0).
//   gamma       : coverage gamma applied at resolve (e.g. 1/1.4 for text);
//                 pass 1.0 for linear coverage.
// Returns an invalid GlyphTex on failure. Caller owns the returned texture and
// must free it with Delete().
GlyphTex RenderGlyph(const float* edges, int edge_count, int w, int h, float gamma);

// Delete a texture returned by RenderGlyph.
void Delete(unsigned int tex);

} // namespace glr
} // namespace ImVarFont
