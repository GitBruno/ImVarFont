# ImVarFont

**v1.0.0** · Variable-font rendering and axis controls for [Dear ImGui](https://github.com/ocornut/imgui).

![preview](example/preview.png)

Renders variable fonts for [Dear ImGui](https://github.com/ocornut/imgui) using
FreeType for outline extraction. Design axes are discovered automatically from the font file
and exposed as ImGui slider widgets.

Filled glyphs are rasterized on the GPU with an analytic **coverage backend**
(signed-area accumulation) — true counter holes and conflation-free
anti-aliasing at any zoom and DPI.

---

## Features

- Works with any OTF / TTF file FreeType can open (static or variable)
- Axes discovered automatically from the font's `fvar` table
- `AxisSliders()` — one `SliderFloat` per axis with the axis name and 4-char tag
- `MetadataTable()` — family/style, variable flag, axis min/max/default table, live values
- Three preview render modes: **Vector**, **Hinted vector**, **Raster**
- Filled glyphs with true counter holes via GPU signed-area coverage
- Runs on desktop GL 3.3 and GLES 3; CPU FreeType fallback on ES2 / WebGL1
- Optional outline (stroke-only) mode
- Optional HarfBuzz GPOS kerning when built with `IMVARFONT_USE_HARFBUZZ`
- Self-contained: drop-in source (`imgui_var_font.{h,cpp}` + `imvarfont_gl.{h,cpp}`)

---

## Platform support

Filled glyphs use the GPU coverage path where a **blendable `RGBA16F`** target is
available, and transparently fall back to **FreeType CPU rasterization** (into the
same atlas) where it isn't — so filled text renders anywhere Dear ImGui's
OpenGL3 backend runs:

| Target | Requirement | Glyph fill path |
|---|---|---|
| Desktop **OpenGL 3.3+** | core | GPU coverage |
| **OpenGL ES 3.0** (Raspberry Pi 4/5, …) | `GL_EXT_color_buffer_half_float` | GPU coverage |
| **OpenGL ES 2 / WebGL1**, or any device without a blendable float target | — | CPU fallback |

> This choice is automatic at `InitRenderer()` time

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

// Simple (fill on, no outline, font-default line height):
ImVarFont::AddText(ImGui::GetWindowDrawList(), face,
                   96.f, ImVec2((width - w) * 0.5f, y), text_col, "Hello");

// With styling (named fields — set only what you need):
ImVarFont::TextStyle st;
st.outline           = true;
st.outline_thickness = 2.f;
st.line_height_px    = 110.f;
ImVarFont::AddText(ImGui::GetWindowDrawList(), face,
                   96.f, ImVec2(x, y), text_col, "Hello", st);

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
// Styling options — all fields have defaults, set only what you need:
struct TextStyle {
    bool  fill              = true;
    bool  outline           = false;
    float outline_thickness = 1.5f;
    float line_height_px    = 0.f;   // 0 = font default
    float letter_spacing_px = 0.f;
};

// Short form (fill=true, no outline, font-default line height):
float AddText(ImDrawList*, Face*, float em_px, ImVec2 pos, ImU32 col, const char* text);

// Full control via named fields:
float AddText(ImDrawList*, Face*, float em_px, ImVec2 pos, ImU32 col, const char* text,
              const TextStyle& style);

float CalcTextWidth(Face*, float em_px, const char* text);
float CalcAscenderPx(const Face*, float em_px);
float CalcDescenderPx(const Face*, float em_px);
float CalcLineHeightPx(const Face*, float em_px);
```

---

## Building the example

The example (`example/main.cpp`) uses GLFW + OpenGL 3.3 (or OpenGL ES 3.0, see
below).  Dear ImGui and GLFW are fetched automatically by CMake; **FreeType**
must be on the system. Glyph fill is rendered on the GPU. HarfBuzz is optional
(GPOS kerning + OpenType features) and enabled when found.

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

### Raspberry Pi / OpenGL ES 3.0

The renderer also targets **OpenGL ES 3.0**, so it runs in embedded/ES
environments (Raspberry Pi 4/5, etc.). Configure with `-DIMVARFONT_GLES=ON`:

```bash
sudo apt install libfreetype-dev libgles2-mesa-dev libegl1-mesa-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release -DIMVARFONT_GLES=ON
cmake --build build
./example/ImVarFontExample
```

This selects ImGui's ES backend, creates a GLES 3.0 context, and compiles the
`#version 300 es` shaders. The GPU path needs a blendable `RGBA16F` target
(`GL_EXT_color_buffer_half_float`); on ES2 / WebGL1 or a device lacking it, the
renderer falls back to FreeType CPU rasterization (see
[Portability & CPU fallback](#portability--cpu-fallback)).

> ES is selected by ImGui's own `IMGUI_IMPL_OPENGL_ES3` flag — no ImVarFont
> define needed. Hosts that build ImGui's ES backend (e.g. openFrameworks on the
> Pi) already define it; the CMake option propagates it to `imvarfont` via
> `imgui_impl`'s `PUBLIC` usage requirement.

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
- **Filled vector modes**: glyph coverage is rasterized with signed-area
  accumulation (non-zero winding) for true counter holes and conflation-free AA,
  cached per glyph and composited with `AddImage`. Call
  `ImVarFont::InitRenderer(glfwGetProcAddress)` once a GL context is current; see
  [the coverage backend](#glyph-fill-the-coverage-backend) for details and the
  CPU fallback.
- **Outline mode** (`outline = true`): contours are stroked on top (or instead,
  when `fill = false`).
- Quadratic (TrueType conic) Béziers are upgraded to cubics before being passed
  to `ImDrawList::PathBezierCubicCurveTo()`.

---

### How it works

Signed-area accumulation (FreeType's smooth rasterizer /
[font-rs](https://medium.com/@raphlinus/inside-the-fastest-font-renderer-in-the-world-75ae5270c445)
/ Pathfinder, on the GPU): for each edge, a quad spanning to the cell's right
border adds *signed* trapezoidal coverage (up-going `+`, down-going `−`) into an
`RGBA16F` target with additive blending — the scanline running-sum done
geometrically, no prefix pass. Holes carry opposite winding and cancel, so
counters are exact with no ear-clipping or keyhole bridging. A resolve pass writes
`(1,1,1,|coverage|^gamma)` into an `RGBA8` atlas cell, composited by ImGui's stock
shader as `colour × coverage`.

Each glyph is rasterized once into a shared, shelf-packed atlas (keyed by glyph +
device size + axis state) and reused as a textured quad; glyphs sharing a page
batch into one draw call. The atlas grows by adding pages and recycles (bumping a
generation that invalidates stale entries) only past a soft cap; oversized cells
(extreme zoom) use a dedicated texture.

### TODO

Replace `imvarfont_gl.cpp` without touching the public API.

| Backend | Zoom / animate | Notes |
|---|---|---|
| **Signed-area** (current) | re-raster on size change | reference AA, ~1 file + 2 shaders, no extra deps |
| CPU FreeType raster | re-raster on size change | the built-in fallback; FreeType only |
| Loop-Blinn (future) | **free** (curves in shader) | high complexity; for continuous zoom / live morph |

---

## Changelog

### 1.0.0

- GPU **analytic coverage** glyph fill (signed-area accumulation): true counter
  holes, conflation-free anti-aliasing at any zoom/DPI.
- **Shared shelf-packed atlas** — glyphs batch into few draw calls.
- New API: `InitRenderer()` / `ShutdownRenderer()` / `RendererReady()`.
- **OpenGL ES 3.0 support** (Raspberry Pi / embedded): build with
  `-DIMVARFONT_GLES=ON`. ES is selected via ImGui's own `IMGUI_IMPL_OPENGL_ES3`
  flag (no extra ImVarFont define). Uses `GL_EXT_color_buffer_half_float` for
  blendable RGBA16F accumulation.
- **CPU raster fallback**: when no blendable float target is available (ES2 /
  WebGL1 / older drivers) the renderer rasterizes coverage with FreeType and
  uploads it into the same atlas, so filled text renders everywhere ImGui runs.

---

## License

**MIT** — for everyone.

Except: if your name is **Omar Cornut** (yes, *that* one — the creator of [Dear
ImGui](https://github.com/ocornut/imgui)), you get the **WTFPL** instead. Do what the fuck you want to; thanks!

See [`LICENSE`](LICENSE).
