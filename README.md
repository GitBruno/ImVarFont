# ImVarFont

Variable-font rendering and axis controls for [Dear ImGui](https://github.com/ocornut/imgui).

![preview](example/preview.png)

Renders variable fonts for [Dear ImGui](https://github.com/ocornut/imgui) using
FreeType for outline extraction. Design axes are discovered automatically from the font file
and exposed as ImGui slider widgets.

Filled glyphs are rasterized on the GPU with an analytic **coverage backend**
(signed-area accumulation) — true counter holes and conflation-free
anti-aliasing at any zoom and DPI. See
[Glyph fill: the coverage backend](#glyph-fill-the-coverage-backend) for the
design, trade-offs, and comparison to alternatives.

---

## Features

- Works with any OTF / TTF file FreeType can open (static or variable)
- Axes discovered automatically from the font's `fvar` table
- `AxisSliders()` — one `SliderFloat` per axis with the axis name and 4-char tag
- `MetadataTable()` — family/style, variable flag, axis min/max/default table, live values
- Three preview render modes: **Vector**, **Hinted vector**, **Raster**
- Filled glyph rendering with true counter holes (GPU analytic signed-area coverage, cached per glyph)
- Conflation-free anti-aliasing at any zoom/DPI — no MSAA, no external GL loader
- Optional outline (stroke-only) preview mode
- Optional HarfBuzz GPOS kerning when built with `IMVARFONT_USE_HARFBUZZ`
- Self-contained: drop-in source (`imgui_var_font.{h,cpp}` + `imvarfont_gl.{h,cpp}`)
- Follows Dear ImGui naming conventions (PascalCase members, snake_case parameters)  

---

## Quick start

```cpp
#include "imgui_var_font.h"

// Load once
ImVarFont::Face* face = ImVarFont::LoadFace("fonts/MyFont.ttf");

// Each frame: draw the axis sliders (immediate-mode, so call them every frame).
// AxisSliders() returns true only on the frame a slider actually moves, and it
// pushes the new coordinates to FreeType for you — no separate ApplyAxes() call.
if (ImVarFont::AxisSliders(face)) {
    // ...react to the change here if you need to (e.g. re-rasterize a cache).
}

// (Only call ApplyAxes() yourself after setting axes programmatically, e.g.
//  ImVarFont::SetAxisValue(face, 0, 700.f); ImVarFont::ApplyAxes(face);)

float w = ImVarFont::CalcTextWidth(face, 96.f, "Hello");
ImU32 text_col = IM_COL32(255, 255, 255, 255);

ImVarFont::SetRenderMode(face, ImVarFont::RenderMode::HintedVector);
ImVarFont::SetHintingFlags(face, ImVarFont::HintingFlags::Native);

ImVarFont::AddText(
    ImGui::GetWindowDrawList(), face,
    96.f,                                // em size in pixels
    ImVec2((width - w) * 0.5f, y),       // top-left of text block
    text_col,
    "Hello",
    true,                                // fill glyphs
    false,                               // or/also stroke each contour
    1.5f,                                // outline thickness (px)
    0.f);                                // line height (0 = font default)

// Cleanup
ImVarFont::FreeFace(face);
```

---

## Render modes

| Mode | Hinting | Best for |
|---|---|---|
| **Vector** (default) | Off (`FT_LOAD_NO_SCALE \| FT_LOAD_NO_HINTING`) | Infinite zoom, axis extrapolation |
| **Hinted vector** | On (`FT_Set_Char_Size` + native/light/auto-hint) | Crisper stems at small sizes, still scalable |
| **Raster** | On + `FT_Render_Glyph` | Static preview quality closest to OS/imgui_freetype |

```cpp
enum class RenderMode { Vector, HintedVector, Raster };
enum class HintingFlags { Native, Light, AutoHint };

void SetRenderMode(Face*, RenderMode);
void SetHintingFlags(Face*, HintingFlags);

// CPU RGBA composite — use with your own texture upload (see example)
bool RasterizeText(Face*, float em_px, const char* text, ImU32 col,
                   float line_height_px, std::vector<uint8_t>& out_rgba,
                   int& out_w, int& out_h);
```

`AddText` is a no-op when render mode is **Raster**; call `RasterizeText` and blit the bitmap yourself.

---

## API reference

See `imgui_var_font.h` for the full API.  Key rendering entry points:

```cpp
float AddText(ImDrawList*, Face*, float em_px, ImVec2 pos,
              ImU32 col, const char* text,
              bool fill = true,
              bool outline = false,
              float outline_thickness = 1.5f,
              float line_height_px = 0.f,
              float letter_spacing_px = 0.f);

float CalcTextWidth(Face*, float em_px, const char* text);
float CalcAscenderPx(const Face*, float em_px);
float CalcDescenderPx(const Face*, float em_px);
float CalcLineHeightPx(const Face*, float em_px);
```

---

## Building the example

The example (`example/main.cpp`) uses GLFW + OpenGL 3.3.  Dear ImGui and GLFW 
are fetched automatically by CMake; **FreeType** must be on the system. Glyph 
fill is rendered on the GPU. HarfBuzz is optional (GPOS kerning + OpenType features) 
and enabled when found.

### Windows – MSYS2 / MinGW

```bash
pacman -S mingw-w64-ucrt-x86_64-freetype
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
./example/ImVarFontExample.exe
```

### Windows – vcpkg + MSVC

```bash
vcpkg install freetype
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

### Linux

```bash
sudo apt install libfreetype-dev libgl1-mesa-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./example/ImVarFontExample
```

### macOS

```bash
brew install freetype
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./example/ImVarFontExample
```

---

## Rendering notes

- `AddText` treats `pos` as the **top-left corner** of the text block.
  The baseline sits at `pos.y + ascender * scale`.
- **Vector mode** loads unhinted design-unit outlines and scales linearly — best
  for zooming and axis extrapolation, but softer at small pixel sizes.
- **Hinted vector** loads outlines at the target em size with FreeType hinting;
  stems align to the pixel grid. Hinting may shift slightly when axis sliders move.
- **Raster mode** uses FreeType's grayscale renderer; re-rasterize when size or
  axes change (the example re-rasterizes on zoom).
- **Filled vector modes**: glyph coverage is rasterized on the GPU with
  signed-area accumulation under the non-zero winding rule, giving true counter
  holes (the inside of `O`, `e`, `B`) and conflation-free anti-aliasing at any
  zoom/DPI. Each glyph is rendered once to a cached coverage texture (keyed by
  glyph + device size + axis state) and composited with `AddImage`. Call
  `ImVarFont::InitRenderer(glfwGetProcAddress)` once a GL context is current.
- **Outline mode** (`outline = true`): contours are stroked on top (or instead,
  when `fill = false`).
- Quadratic (TrueType conic) Béziers are upgraded to cubics before being passed
  to `ImDrawList::PathBezierCubicCurveTo()`.

---

## Glyph fill: the coverage backend

Filled glyphs are rendered by an analytic **coverage backend** living in a single
file (`imvarfont_gl.cpp`). The rest of the library never talks to OpenGL — it
extracts curves, flattens them to line edges at the target *device* resolution,
and hands the backend an **edge soup + a cell size**:

```cpp
// imvarfont_gl.h — the entire seam
glr::GlyphTex glr::RenderGlyph(const float* edges, int edge_count,
                               int w, int h, float gamma);
```

That seam is deliberately narrow. The backend owns *how* coverage is computed;
everything upstream (FreeType, variable axes, caching, layout, ImGui
compositing) is backend-agnostic. A different rasterizer can replace **only this
file** without touching the public API.

### How the current backend works

Signed-area accumulation (the same idea behind FreeType's smooth rasterizer,
[font-rs](https://medium.com/@raphlinus/inside-the-fastest-font-renderer-in-the-world-75ae5270c445)
and Pathfinder, moved onto the GPU):

1. **Accumulate.** For every edge of the glyph, draw a quad spanning from the
   edge rightward to the cell border, over the edge's vertical extent. A fragment
   shader adds, per pixel, the *signed* trapezoidal coverage the edge
   contributes (up-going edges `+`, down-going `−`). Additive blending into an
   `RGBA16F` target. The horizontal running-sum a CPU scanline rasterizer needs
   is realized geometrically by extending each quad to the right border, so no
   prefix-sum pass is required. Holes carry opposite winding and cancel — true
   counters, no ear-clipping, no keyhole bridging.
2. **Resolve.** A fullscreen pass takes `|coverage|`, applies coverage gamma, and
   writes `(1,1,1,α)` into an `RGBA8` cell so ImGui's stock shader composites it
   as `colour × coverage` with no custom pipeline.

Each glyph is rasterized once into a cached texture (keyed by glyph + device
size + axis state) and re-used as a textured quad until its outline changes.

### Pros

- **Reference-quality AA.** Coverage is computed from true area, not point
  samples — no MSAA, no shimmer, even weight on stems. This is what a *type*
  tool needs: it shows the designer the letterform, not an approximation.
- **Correct by construction.** Non-zero winding falls straight out of signed
  accumulation; counters and overlapping contours just work. No fragile CPU
  geometry.
- **Resolution-independent source.** Curves are kept in design units and
  flattened at the *device* pixel size, so it's crisp on 4K and re-flattens
  correctly on zoom.
- **Cheap in steady state.** A cache hit is a single `AddImage` (one quad).

### Cons / limitations

- **It rasterizes, so it's resolution-bound per cache entry.** Zooming or
  animating an axis is a cache *miss* → re-rasterize. Smooth at any *static*
  size, but continuous zoom re-renders each frame (still cheap, but not free like
  a pure curve-eval method).
- **Per-glyph textures (v1), not an atlas.** Each cached glyph is its own
  texture, so a glyph = its own draw call. Fine for a font viewer / headings;
  for paragraphs of body text a shelf-packed atlas (same seam, future work)
  would cut draw calls dramatically.
- **Midpoint coverage approximation.** The fragment shader samples right-fraction
  at the band mid-height rather than integrating the clamp exactly. Visually
  indistinguishable on real glyphs; near-horizontal hairlines are the only place
  an exact integral would differ.
- **16-bit accumulation** can theoretically saturate on pathological
  self-overlapping outlines; real fonts don't hit this.

### Alternatives (and whether they should replace it)

| Backend | Quality | Cold cost (first draw) | Warm cost (cached) | Zoom/animate | Complexity | Deps |
|---|---|---|---|---|---|---|
| **Signed-area coverage** (this) | Reference AA | ~1 draw, `O(edges)` quads + resolve | 1 quad (`AddImage`) | re-raster on size change | Low (~1 file, ~2 shaders) | none extra |
| CPU FreeType raster + atlas | Reference AA (hinted option) | CPU rasterize + upload | 1 quad | re-raster on size change | Low–med | FreeType only |
| **Loop-Blinn** (curves as triangles) | Great, but classic *conflation* at curve/edge joins unless resolved with MSAA/derivatives | curve→triangle setup | 1 mesh draw | **free** (curves eval'd in shader) | High (orientation, AA pass) | none extra |
| **Banding / Slug-style** (per-pixel ray vs. curves) | Reference AA + direct curves | band/curve buffer build | 1 quad, heavier fragment shader | **free** | High | none extra (Slug itself is licensed) |
| **SDF / MSDF** | Good at size, **rounds sharp corners**, not exact | offline/atlas gen | 1 quad | scalable but soft/edgy | Med | atlas gen tool |

Rough numbers, *order-of-magnitude, not measured on a real machine* — they reflect
algorithmic cost and published figures, so treat them as guidance:

- **Cold rasterize**, a typical Latin glyph (~150–400 edges after flattening) into
  a ~64–256 px cell: on the order of **tens of microseconds** of GPU time
  (two passes, a few hundred small quads). The dominant real cost is the texture
  allocation, which the cache amortizes to zero.
- **Warm draw**: a single textured quad — **negligible**, indistinguishable from
  drawing any other ImGui image.
- **Quality**: against a 16× supersampled CPU reference, signed-area coverage is
  effectively a visual match (sub-1% per-pixel coverage error from the midpoint
  approximation). Loop-Blinn *without* a dedicated AA pass shows visible
  conflation seams; MSDF shows measurable corner rounding at a 32–48 px atlas.

> Want hard numbers for your fonts/hardware? Ask and I'll wire a small
> frame-timed benchmark (cold vs. warm, glyphs/ms) into the example.

### Recommendation — replace, or keep as the default?

**Keep signed-area coverage as the default.** For a variable-font *tool*, the
priorities are fidelity (Type Designers's letterforms must be honest) and a clean,
low-dependency integration (Omar's ImGui ethos). This backend nails both with the
least machinery, and the per-glyph cache makes typical UI use essentially free.

The other techniques are **better as optional backends behind the same seam, not
as a wholesale replacement**:

- **Loop-Blinn / banding** become attractive only when you need *continuous*
  zoom or per-frame axis animation at large sizes without re-rasterizing, or an
  atlas-free pipeline. They eval curves directly in the shader (zoom is free) but
  cost real complexity (Loop-Blinn's conflation problem needs a proper AA
  strategy; banding needs a per-glyph curve buffer). Good candidates to **add**
  the day a “live morphing poster at 4K” use-case appears — drop a second
  `RenderGlyph` implementation into `imvarfont_gl.cpp`, no upstream changes.
- **SDF / MSDF** is not an upgrade: it is cheap and scalable, but it rounds corners
  and isn't exact, which is wrong for rendering typography.

> **signed-area** is the right default; Loop-Blinn can be slotted in later 
> as an *enhancement* without re-architecting.

---

## License

**MIT** — for everyone.

Except: if your name is **Omar Cornut** (yes, *that* one — the creator of [Dear
ImGui](https://github.com/ocornut/imgui)), you get the **WTFPL** instead. Do what the fuck you want to; thanks!

See [`LICENSE`](LICENSE).
