# ImVarFont

**v1.1.0** · Variable-font **engine** (`VarFont`: Face / morph / layout / GPU coverage)
with a thin [Dear ImGui](https://github.com/ocornut/imgui) adapter.

![preview](example/preview.png)

FreeType extracts outlines; design axes become `SliderFloat`s in the ImGui
adapter. Filled glyphs use GPU analytic coverage (signed-area / exact-curve)
with a CPU FreeType fallback on ES2 / WebGL1.

```
Hosts (composite atlas quads / optional outline paths)
  • ImGui              — imgui_var_font AddText → ImDrawList::AddImage
  • openFrameworks     — ofxVariableFont::drawStringGpu
  • RigKit / others    — BeginHostFrame + SetGlyphQuadEmitter + DrawString
        │
        ▼
This repo
  Face / morph / layout   varfont_core.*   (no imgui.h)
  GPU coverage atlas      varfont_gl.*     (no imgui.h)
  Dear ImGui adapter      imgui_var_font.* (widgets + AddText)
```

ImGui is one **host**, not the engine. `varfont_core` + `varfont_gl` have no
`imgui.h` dependency. Outline stroking uses `SetPathEmitter` (the ImGui adapter
registers `ImDrawList` Path*).

Academic manuscript, validation harness, and benchmarks (not the runtime atlas):
[**VarFont**](https://github.com/GitBruno/VarFont).

---

## Files

| File | Role |
|---|---|
| `varfont_core.h` / `.cpp` | Engine: Face / morph / layout / `DrawString` / host emitters |
| `varfont_core_detail.h` | Internal (do not include from apps) |
| `varfont_gl.h` / `.cpp` | GPU coverage atlas / backends (ImGui-free) |
| `imgui_var_font.h` / `.cpp` | Dear ImGui adapter: widgets + `AddText(ImDrawList*)` |
| `example/` | Interactive demo (`ImVarFontExample`) |

CMake targets: `varfont_gl` → `varfont_core` → `imvarfont` (optional adapter).

---

## Quick start (ImGui)

```cpp
#include "imgui_var_font.h"

ImVarFont::Face* face = ImVarFont::LoadFace("MyFont.ttf");
ImVarFont::InitRenderer((void*(*)(const char*))glfwGetProcAddress);

if (ImVarFont::AxisSliders(face)) { /* axes applied */ }
ImVarFont::EnableMorph(face, true);

ImVarFont::AddText(ImGui::GetWindowDrawList(), face,
                   96.f, ImVec2(x, y), IM_COL32_WHITE, "Hello");

ImVarFont::FreeFace(face);
ImVarFont::ShutdownRenderer();
```

## Quick start (non-ImGui host)

```cpp
#include "varfont_core.h"

VarFont::Face* face = VarFont::LoadFace("MyFont.ttf");
VarFont::InitRenderer((void*(*)(const char*))glfwGetProcAddress);
VarFont::SetGlyphQuadEmitter(myEmit, user);

// each frame:
VarFont::BeginHostFrame(frameIndex, framebufferScale);
VarFont::DrawString(face, 96.f, {x, y}, VARFONT_COL32_WHITE, "Hello");
```

See `varfont_core.h` / `imgui_var_font.h` for the full API.

---

## Build the example

Needs **FreeType** on the system. Dear ImGui, GLFW, and NFD are fetched by CMake.
HarfBuzz is optional (GPOS kerning) when found via pkg-config.

```bash
# Linux
sudo apt install libfreetype-dev libgl1-mesa-dev libharfbuzz-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./example/ImVarFontExample /path/to/VariableFont.ttf

# macOS
brew install freetype harfbuzz
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build

# Windows (MSYS2 UCRT64)
pacman -S mingw-w64-ucrt-x86_64-freetype mingw-w64-ucrt-x86_64-harfbuzz
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Raspberry Pi / GLES 3
cmake -B build -DCMAKE_BUILD_TYPE=Release -DIMVARFONT_GLES=ON
```

Link `varfont_core` (+ `varfont_gl`, FreeType, GL) without ImGui for a non-ImGui
host. Link `imvarfont` when you want the Dear ImGui adapter.

---

## Features

- Any OTF/TTF FreeType can open (static or variable)
- ImGui widgets: `AxisSliders()` / `MetadataTable()` / `KernTableUi()`
- Vector, hinted-vector, and raster preview modes
- GPU analytic coverage; ES2 / WebGL1 CPU fallback
- Host-neutral filled text: `BeginHostFrame`, `SetGlyphQuadEmitter`, `DrawString`
- Re-rasterization-free axis morphing (`EnableMorph`)
- Optional GPU reconstruction (`PreferGpuMorphRenderer`)
- Optional HarfBuzz GPOS kerning (`IMVARFONT_USE_HARFBUZZ`)

---

## License

**MIT** for everyone — except **Omar Cornut**, who gets the **WTFPL**.

Omar: do what the fuck you want to. Thanks for Dear ImGui.

See [`LICENSE`](LICENSE).
