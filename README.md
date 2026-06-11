# ImVarFont

Variable-font rendering and axis controls for [Dear ImGui](https://github.com/ocornut/imgui).

![preview](example/preview.png)

Renders glyph outlines directly into an `ImDrawList` using FreeType for outline
extraction.  Variable-font design axes (weight, width, custom axes like scanline
or horizontal bleed) are automatically discovered from the font file and exposed
as ImGui slider widgets.

No texture atlas is required for vector modes — outlines are tessellated on the
fly via `ImDrawList::PathBezierCubicCurveTo`.

---

## Features

- Works with any OTF / TTF file FreeType can open (static or variable)
- Axes discovered automatically from the font's `fvar` table
- `AxisSliders()` — one `SliderFloat` per axis with the axis name and 4-char tag
- `MetadataTable()` — family/style, variable flag, axis min/max/default table, live values
- Three preview render modes: **Vector**, **Hinted vector**, **Raster**
- Filled glyph rendering with correct counters (holes punched via background colour in vector modes)
- Optional outline (stroke-only) preview mode
- Optional HarfBuzz GPOS kerning when built with `IMVARFONT_USE_HARFBUZZ`
- Self-contained: two files (`imgui_var_font.h` + `imgui_var_font.cpp`)
- Follows Dear ImGui naming conventions (PascalCase members, snake_case parameters)

---

## Quick start

```cpp
#include "imgui_var_font.h"

// Load once
ImVarFont::Face* face = ImVarFont::LoadFace("fonts/MyFont.ttf");

// Each frame
if (ImVarFont::AxisSliders(face))
    ImVarFont::ApplyAxes(face);          // push new axis values to FreeType

float w = ImVarFont::CalcTextWidth(face, 96.f, "Hello");
ImU32 text_col = IM_COL32(255, 255, 255, 255);
ImU32 bg_col   = IM_COL32(10, 10, 18, 255);   // hole punch in vector modes

ImVarFont::SetRenderMode(face, ImVarFont::RenderMode::HintedVector);
ImVarFont::SetHintingFlags(face, ImVarFont::HintingFlags::Native);

ImVarFont::AddText(
    ImGui::GetWindowDrawList(), face,
    96.f,                                // em size in pixels
    ImVec2((width - w) * 0.5f, y),       // top-left of text block
    text_col,
    "Hello",
    false,                               // outline mode (stroke only)
    1.5f,                                // outline thickness (px)
    0.f,                                 // line height (0 = font default)
    bg_col);                             // hole fill colour (0 = skip punch)

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
              bool outline = false,
              float outline_thickness = 1.5f,
              float line_height_px = 0.f,
              ImU32 hole_col = 0);

float CalcTextWidth(Face*, float em_px, const char* text);
float CalcAscenderPx(const Face*, float em_px);
float CalcDescenderPx(const Face*, float em_px);
float CalcLineHeightPx(const Face*, float em_px);
```

---

## Building the example

The example (`example/main.cpp`) uses GLFW + OpenGL 3.3.  Dear ImGui and GLFW
are fetched automatically by CMake; only FreeType needs to be on the system.

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
- **Filled vector modes**: outer contours are filled with `col`.  Counter holes
  are punched by filling hole contours with `hole_col` — pass the canvas
  background colour for correct results.
- **Outline mode** (`outline = true`): contours are stroked only, no fill.
- Quadratic (TrueType conic) Béziers are upgraded to cubics before being passed
  to `ImDrawList::PathBezierCubicCurveTo()`.

---

## License

MIT
