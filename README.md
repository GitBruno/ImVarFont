# ImVarFont

**v1.1.0** · Variable-font rendering and axis controls for [Dear ImGui](https://github.com/ocornut/imgui).

![preview](example/preview.png)

Drop-in sources for Dear ImGui. FreeType extracts outlines; design axes become
`SliderFloat`s. Filled glyphs use GPU analytic coverage (signed-area / exact-curve)
with a CPU FreeType fallback on ES2 / WebGL1.

Paper, validation harness, and benchmarks:
[**VarFont**](https://github.com/GitBruno/VarFont).

---

## Files

| File | Role |
|---|---|
| `imgui_var_font.h` / `.cpp` | Public API + morph / ImGui integration |
| `imgui_var_font_detail.h` | Internal (do not include from apps) |
| `varfont_gl.h` / `.cpp` | GPU coverage backend (ImGui-free) |
| `example/` | Interactive demo (`ImVarFontExample`) |

---

## Quick start

```cpp
#include "imgui_var_font.h"

ImVarFont::Face* face = ImVarFont::LoadFace("MyFont.ttf");

// After you have a GL context:
ImVarFont::InitRenderer((void*(*)(const char*))glfwGetProcAddress);

if (ImVarFont::AxisSliders(face)) { /* axis moved */ }

ImVarFont::EnableMorph(face, true);   // optional: re-raster-free axis drag

ImVarFont::AddText(ImGui::GetWindowDrawList(), face,
                   96.f, ImVec2(x, y), IM_COL32_WHITE, "Hello");

ImVarFont::FreeFace(face);
ImVarFont::ShutdownRenderer();
```

See `imgui_var_font.h` for the full API (`TextStyle`, render modes, HarfBuzz
kerning, GPU morph reconstruction, …).

---

## Build the example

Needs **FreeType** on the system. Dear ImGui, GLFW, and NFD are fetched by CMake.
HarfBuzz is optional (GPOS kerning) when found via pkg-config.

```bash
# Linux
sudo apt install libfreetype-dev libgl1-mesa-dev libharfbuzz-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./example/ImVarFontExample /path/to/VariableFont.ttf   # or drop a font on the window

# macOS
brew install freetype harfbuzz
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build

# Windows (MSYS2 UCRT64)
pacman -S mingw-w64-ucrt-x86_64-freetype mingw-w64-ucrt-x86_64-harfbuzz
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./example/ImVarFontExample.exe C:/Fonts/SomeVar.ttf

# Raspberry Pi / GLES 3
cmake -B build -DCMAKE_BUILD_TYPE=Release -DIMVARFONT_GLES=ON
```

Or add the four library sources to your own ImGui project and link FreeType
(+ OpenGL / GLES).

---

## Features

- Any OTF/TTF FreeType can open (static or variable)
- `AxisSliders()` / `MetadataTable()` widgets
- Vector, hinted-vector, and raster preview modes
- GPU analytic coverage; ES2 / WebGL1 CPU fallback
- Re-rasterization-free axis morphing (`EnableMorph`)
- Optional GPU reconstruction (`PreferGpuMorphRenderer`)
- Optional HarfBuzz GPOS kerning (`IMVARFONT_USE_HARFBUZZ`)

---

## License

**MIT** for everyone — except **Omar Cornut**, who gets the **WTFPL**.

Omar: do what the fuck you want to. Thanks for Dear ImGui.

See [`LICENSE`](LICENSE).
