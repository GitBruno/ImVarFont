// ImVarFont Example  –  Variable font viewer (dockable)
//
//   Dock Controls │ Preview │ Metadata  – drag tabs to rearrange
//
// Build: see ../CMakeLists.txt
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_var_font.h"

#include <nfd.h>
#if defined(IMGUI_IMPL_OPENGL_ES3)
#include <GLES3/gl3.h>
#endif
#include <GLFW/glfw3.h>
#if defined(IMGUI_IMPL_OPENGL_ES3)
// GLES headers already included above.
#elif defined(_WIN32)
#include <GL/gl.h>
#elif defined(__APPLE__)
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>


// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------

static ImVarFont::Face* g_face          = nullptr;
static char             g_fontPath[512] = "";
static char             g_loadError[256] = "";

static char             g_text[4096]  = "The quick brown fox\njumps over the lazy dog";
static float            g_emPx           = 140.f;
static float            g_lineHeightMult = 1.f;
static float            g_letterSpacingEm = 0.f;
static float            g_thickness      = 1.5f;
static bool             g_outline        = false;
static ImVec4           g_textColor   = { 1.00f, 1.00f, 1.00f, 1.00f };
static ImVec4           g_bgColor     = { 0.04f, 0.04f, 0.07f, 1.00f };

static bool             g_extrapolate      = false;
static bool             g_morph            = false;  // re-raster-free axis morph
static bool             g_vsync            = true;   // swap interval; off = uncapped (benchmarking)
static float            g_dpi_scale        = 1.0f;  // set from glfwGetWindowContentScale

static bool             g_useFontForUi     = false;
static float            g_uiFontSize       = 15.f;
static bool             g_uiFontDirty      = true;
static ImFont*          g_uiFont           = nullptr;
static bool             g_dockLayoutBuilt  = false;

static ImVec2           g_previewPan       = { 0.f, 0.f };
static float            g_previewZoom      = 1.f;

static int              g_renderModeIdx    = 0;
static int              g_hintingIdx       = 0;
static bool             g_forceCpuFill     = false;   // test the CPU raster fallback
static bool             g_rasterDirty      = true;
static unsigned int     g_rasterTex        = 0;
static int              g_rasterTexW       = 0;
static int              g_rasterTexH       = 0;
static std::vector<uint8_t> g_rasterPixels;

// Offscreen frame dump: --capture <dir> <WxH> <frames>
static bool             g_captureMode     = false;
static char             g_captureDir[512] = "";
static int              g_captureW        = 600;
static int              g_captureH        = 600;
static int              g_captureFrames   = 64;
static int              g_captureIndex    = 0;
static int              g_captureWarmup   = 4;   // settle GPU / ImGui before dumping
static int              g_wghtAxis        = -1;
static int              g_wdthAxis        = -1;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Normalise path separators so FreeType accepts Windows paths on all platforms
static void normalisePath(char* path) {
    for (char* c = path; *c; ++c)
        if (*c == '\\') *c = '/';
}

static void tryLoadFont() {
    normalisePath(g_fontPath);
    ImVarFont::FreeFace(g_face);
    g_loadError[0] = '\0';
    g_face = ImVarFont::LoadFace(g_fontPath, g_loadError, (int)sizeof(g_loadError));
    if (g_face)
        ImVarFont::ApplyAxes(g_face, g_extrapolate);
    g_uiFontDirty = true;
    g_rasterDirty = true;
}

static void applyUiFontSize() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontSizeBase = g_uiFontSize;
    style._NextFrameFontSizeBase = g_uiFontSize;
    if (ImGui::GetCurrentContext())
        ImGui::UpdateCurrentFontSize(0.0f);
}

static void rebuildUiFont() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->ClearFonts();

    if (g_useFontForUi && g_face) {
        g_uiFont = ImVarFont::SetImGuiFont(io.Fonts, g_face, g_uiFontSize);
    } else {
        ImFontConfig cfg;
        cfg.SizePixels = g_uiFontSize;
        g_uiFont = io.Fonts->AddFontDefault(&cfg);
    }

    if (g_uiFont)
        io.FontDefault = g_uiFont;

    applyUiFontSize();
    g_uiFontDirty = false;
}

// Standard type specimens for previewing fonts
static const char* kSampleTexts[] = {
    "The quick brown fox jumps over the lazy dog",
    "Sphinx of black quartz, judge my vow",
    "Pack my box with five dozen liquor jugs",
    "How vexingly quick daft zebras jump!",
    "The quick brown fox\njumps over the lazy dog",
    "Variable Fonts\nWeight  Width  Slant  Custom axes",
    "ABCDEFGHIJKLM\nNOPQRSTUVWXYZ\nabcdefghijklm\nnopqrstuvwxyz",
    "0123456789\n!@#$%^&*()_+-=[]{}",
    "Hello World",
    nullptr
};

static float calcLineWidth(ImVarFont::Face* face, float emPx, float letterSpacingPx,
                           const char* start, const char* end) {
    if (start >= end) return 0.f;
    std::string line(start, (size_t)(end - start));
    return ImVarFont::CalcTextWidth(face, emPx, line.c_str(), letterSpacingPx);
}

// Measure widest line and total block height for multiline preview centring
static void calcTextBlock(ImVarFont::Face* face, float emPx, float lineHeightPx,
                          float letterSpacingPx, const char* text,
                          float* outW, float* outH) {
    const float asc   = ImVarFont::CalcAscenderPx(face, emPx);
    const float desc  = ImVarFont::CalcDescenderPx(face, emPx);
    const float lineH = lineHeightPx;

    float maxW = 0.f;
    int   lines = 1;
    const char* lineStart = text;
    for (const char* p = text; ; ++p) {
        if (*p == '\n' || *p == '\0') {
            maxW = std::max(maxW, calcLineWidth(face, emPx, letterSpacingPx, lineStart, p));
            if (*p == '\0') break;
            ++lines;
            lineStart = p + 1;
        }
    }

    *outW = maxW;
    *outH = asc + desc + (lines - 1) * lineH;
}

static void syncRenderSettings() {
    if (!g_face) return;
    ImVarFont::SetRenderMode(g_face, (ImVarFont::RenderMode)g_renderModeIdx);
    ImVarFont::SetHintingFlags(g_face, (ImVarFont::HintingFlags)g_hintingIdx);
    ImVarFont::EnableMorph(g_face, g_morph, g_extrapolate);
}

static void uploadRasterTexture(const std::vector<uint8_t>& px, int w, int h) {
    if (g_rasterTex == 0)
        glGenTextures(1, &g_rasterTex);
    glBindTexture(GL_TEXTURE_2D, g_rasterTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (w != g_rasterTexW || h != g_rasterTexH) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        g_rasterTexW = w;
        g_rasterTexH = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    }
}

static void freeRasterTexture() {
    if (g_rasterTex != 0) {
        glDeleteTextures(1, &g_rasterTex);
        g_rasterTex = 0;
    }
    g_rasterTexW = g_rasterTexH = 0;
    g_rasterPixels.clear();
}

// Binary PPM (P6) — Pillow reads it; avoids pulling in an image encoder.
static bool saveFramePpm(const char* path, int w, int h, const uint8_t* rgba) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> row(w * 3);
    // GL origin is bottom-left; flip vertically while writing.
    for (int y = h - 1; y >= 0; --y) {
        const uint8_t* src = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
    return true;
}

static void findWghtWdthAxes() {
    g_wghtAxis = g_wdthAxis = -1;
    if (!g_face) return;
    const int n = ImVarFont::GetAxisCount(g_face);
    ImVarFont::Axis* axes = ImVarFont::GetAxes(g_face);
    const uint32_t wght = ImVarFont::MakeTag('w', 'g', 'h', 't');
    const uint32_t wdth = ImVarFont::MakeTag('w', 'd', 't', 'h');
    for (int i = 0; i < n; ++i) {
        if (axes[i].Tag == wght) g_wghtAxis = i;
        if (axes[i].Tag == wdth) g_wdthAxis = i;
    }
}

// Same cos/sin drive as VarFont research/tools/make_readme_gif.py
static void applyCaptureAxisFrame(int fi, int n) {
    if (!g_face || n <= 0) return;
    ImVarFont::Axis* axes = ImVarFont::GetAxes(g_face);
    const float t = 2.f * 3.14159265f * (float)fi / (float)n;
    if (g_wghtAxis >= 0) {
        const auto& a = axes[g_wghtAxis];
        float v = a.Default + 0.34f * (a.Max - a.Default) * std::cos(t);
        ImVarFont::SetAxisValue(g_face, g_wghtAxis, v, true);
    }
    if (g_wdthAxis >= 0) {
        const auto& a = axes[g_wdthAxis];
        float v = a.Default + 0.26f * (a.Max - a.Default) * std::sin(t + 0.55f);
        ImVarFont::SetAxisValue(g_face, g_wdthAxis, v, true);
    }
}

static bool parseSizeWxH(const char* s, int* w, int* h) {
    int ww = 0, hh = 0;
    if (sscanf(s, "%dx%d", &ww, &hh) != 2 || ww < 64 || hh < 64) return false;
    *w = ww; *h = hh;
    return true;
}

// GLFW drag-and-drop: accept the first dropped file as the font path
static void dropCallback(GLFWwindow*, int count, const char** paths) {
    if (count < 1) return;
    strncpy(g_fontPath, paths[0], sizeof(g_fontPath) - 1);
    g_fontPath[sizeof(g_fontPath) - 1] = '\0';
    tryLoadFont();
}

// ---------------------------------------------------------------------------
// Docking
// ---------------------------------------------------------------------------

static void SetupDockLayout(ImGuiID dockspace_id) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->WorkSize);

    ImGuiID dock_main  = dockspace_id;
    ImGuiID dock_left  = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Left,  0.22f, nullptr, &dock_main);
    ImGuiID dock_right = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right, 0.26f, nullptr, &dock_main);
    ImGuiID dock_bottom = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Down, 0.28f, nullptr, &dock_main);
    // Performance gets its own fixed pane at the top of the left column so the
    // metrics stay visible while the axis sliders scroll in Controls below.
    ImGuiID dock_left_top = ImGui::DockBuilderSplitNode(dock_left, ImGuiDir_Up, 0.30f, nullptr, &dock_left);

    ImGui::DockBuilderDockWindow("Performance", dock_left_top);
    ImGui::DockBuilderDockWindow("Controls",  dock_left);
    ImGui::DockBuilderDockWindow("Preview",   dock_main);
    ImGui::DockBuilderDockWindow("Metadata",  dock_right);
    ImGui::DockBuilderDockWindow("Kern table", dock_bottom);
    ImGui::DockBuilderFinish(dockspace_id);
}

static void DrawDockSpace() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::Begin("DockSpaceHost", nullptr, flags);
    ImGui::PopStyleVar(2);

    ImGuiID dockspace_id = ImGui::GetID("ImVarFontDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.f, 0.f), ImGuiDockNodeFlags_PassthruCentralNode);

    if (!g_dockLayoutBuilt) {
        g_dockLayoutBuilt = true;
        ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace_id);
        if (!node || node->IsEmpty())
            SetupDockLayout(dockspace_id);
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Controls window  (left panel)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Performance window  (own dockable pane so metrics stay put while the axis
// sliders scroll in the Controls panel)
// ---------------------------------------------------------------------------

static void DrawPerformance() {
    ImGui::Begin("Performance");

    // ImGui keeps a smoothed framerate; show frame time + FPS so the render cost is
    // visible while panning/morphing. The path line reports which morph rasteriser
    // is live so it's clear what the numbers reflect.
    const float fps = ImGui::GetIO().Framerate;
    const float frameMs = fps > 0.f ? 1000.f / fps : 0.f;
    ImGui::Text("%.2f ms/frame  (%.0f FPS)", frameMs, fps);
    const char* path =
        (g_morph && ImVarFont::PreferGpuMorphRenderer() && ImVarFont::GpuMorphAvailable())
            ? "morph: GPU reconstruction"
        : (g_morph && ImVarFont::PreferSlugRenderer())
            ? "morph: CPU blend -> exact-curve coverage"
        : g_morph ? "morph: CPU blend -> signed-area coverage"
                  : "static glyph cache";
    ImGui::TextDisabled("%s", path);

    // Per-phase CPU breakdown (last completed frame). "GPU/other" is the frame
    // remainder: when it dominates we are fill/vsync-bound; when blend or raster
    // dominates we are CPU-bound and know exactly which phase to attack.
    const ImVarFont::RenderProfile rp = ImVarFont::GetRenderProfile();
    const float other = frameMs - rp.blendMs - rp.rasterMs;
    ImGui::Text("  blend (CPU)   %6.2f ms", rp.blendMs);
    ImGui::Text("  raster submit %6.2f ms", rp.rasterMs);
    ImGui::Text("  GPU / other   %6.2f ms", other > 0.f ? other : 0.f);
    ImGui::Text("  glyphs %d   rebuilds %d", rp.glyphs, rp.rebuilds);
    if (rp.glyphs > 0)
        ImGui::TextDisabled("  %.1f us/glyph CPU",
                            1000.f * (rp.blendMs + rp.rasterMs) / (float)rp.glyphs);
    ImGui::Checkbox("VSync", &g_vsync);
    ImGui::SameLine();
    ImGui::TextDisabled("(off = uncapped, for benchmarking)");

    ImGui::End();
}

static void DrawControls() {
    ImGui::Begin("Controls");

    // ── Font ────────────────────────────────────────────────────────────────
    ImGui::SeparatorText("Font");

    // Browse button → native OS file dialog
    if (ImGui::Button("Browse...", { -1.f, 0.f })) {
        nfdchar_t* out_path = nullptr;
        nfdfilteritem_t filters[] = { { "Font files", "ttf,otf,woff2" } };
        nfdresult_t res = NFD_OpenDialog(&out_path, filters, 1, nullptr);
        if (res == NFD_OKAY && out_path) {
            strncpy(g_fontPath, out_path, sizeof(g_fontPath) - 1);
            g_fontPath[sizeof(g_fontPath) - 1] = '\0';
            NFD_FreePath(out_path);
            tryLoadFont();
        } else if (res == NFD_ERROR) {
            snprintf(g_loadError, sizeof(g_loadError), "Dialog: %s", NFD_GetError());
        }
    }

    // Path input as a fallback (Enter or drag-and-drop)
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::InputText("##path", g_fontPath, sizeof(g_fontPath),
                         ImGuiInputTextFlags_EnterReturnsTrue))
        tryLoadFont();

    if (g_loadError[0])
        ImGui::TextColored({ 1.f, 0.4f, 0.4f, 1.f }, "%s", g_loadError);
    else if (g_face)
        ImGui::TextColored({ 0.4f, 0.9f, 0.5f, 1.f }, "%s  %s",
                           ImVarFont::GetFamilyName(g_face),
                           ImVarFont::GetStyleName(g_face));

    ImGui::Spacing();
    if (ImGui::Checkbox("Use loaded font for UI", &g_useFontForUi))
        g_uiFontDirty = true;
    if (g_useFontForUi && !g_face)
        ImGui::TextDisabled("Load a font to style the UI");
    else if (g_useFontForUi && g_face && ImVarFont::IsVariable(g_face))
        ImGui::TextDisabled("Axis changes update UI text live");

    ImGui::Text("UI size (px)");
    ImGui::SetNextItemWidth(-1.f);
    ImGui::SliderFloat("##uiPx", &g_uiFontSize, 10.f, 28.f, "%.0f px");
    if (ImGui::IsItemEdited())
        applyUiFontSize();

    // ── Text / appearance ────────────────────────────────────────────────────
    ImGui::SeparatorText("Text");

    // Sample phrase picker
    if (ImGui::BeginCombo("Sample", "Choose specimen…")) {
        for (int i = 0; kSampleTexts[i]; ++i) {
            const bool selected = (strcmp(g_text, kSampleTexts[i]) == 0);
            if (ImGui::Selectable(kSampleTexts[i], selected)) {
                strncpy(g_text, kSampleTexts[i], sizeof(g_text) - 1);
                g_text[sizeof(g_text) - 1] = '\0';
                g_rasterDirty = true;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::InputTextMultiline("##text", g_text, sizeof(g_text),
                              ImVec2(-1.f, 90.f * g_dpi_scale)))
        g_rasterDirty = true;

    ImGui::Spacing();
    ImGui::Text("Size (px)");
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::SliderFloat("##emPx", &g_emPx, 12.f, 600.f, "%.0f px"))
        g_rasterDirty = true;

    ImGui::Text("Line height");
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::SliderFloat("##lineH", &g_lineHeightMult, 0.5f, 3.f, "%.2f×"))
        g_rasterDirty = true;

    ImGui::Text("Letter spacing");
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::SliderFloat("##letterSp", &g_letterSpacingEm, -0.2f, 0.5f, "%.3f em"))
        g_rasterDirty = true;

    ImGui::Text("Render mode");
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::Combo("##renderMode", &g_renderModeIdx,
                     "Vector\0Hinted vector\0Raster\0Loop-Blinn\0Loop-Blinn (live)\0")) {
        syncRenderSettings();
        g_rasterDirty = true;
    }

    if (g_renderModeIdx == (int)ImVarFont::RenderMode::HintedVector ||
        g_renderModeIdx == (int)ImVarFont::RenderMode::Raster) {
        ImGui::Text("Hinting");
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::Combo("##hinting", &g_hintingIdx,
                         "Native\0Light\0Auto-hint\0")) {
            syncRenderSettings();
            g_rasterDirty = true;
        }
    }

    // Filled vector glyphs normally use the GPU coverage path; this forces the
    // CPU FreeType fallback (the path used on OpenGL ES 2 / WebGL1) so it can be
    // compared on a desktop GL build. No effect in Raster mode.
    if (g_renderModeIdx != (int)ImVarFont::RenderMode::Raster) {
        if (ImGui::Checkbox("Force CPU fallback (test)", &g_forceCpuFill))
            ImVarFont::ForceCpuFallback(g_forceCpuFill);
        ImGui::SameLine();
        ImGui::TextDisabled(ImVarFont::RendererReady() ? "(GPU ready)" : "(no GPU)");
    }

    if (g_renderModeIdx == 2 && g_outline)
        ImGui::TextDisabled("Outline applies to vector modes only");
    if (g_outline) {
        ImGui::Text("Outline thickness");
        ImGui::SetNextItemWidth(-1.f);
        ImGui::SliderFloat("##thick", &g_thickness, 0.5f, 8.f, "%.1f px");
    }

    if (g_face) {
        ImGui::SeparatorText("Kerning");

        bool kern = ImVarFont::GetUseKerning(g_face);
        if (ImGui::Checkbox("Kerning", &kern))
            ImVarFont::SetUseKerning(g_face, kern);

        ImGui::TextDisabled("Engine: %s", ImVarFont::GetKerningEngineLabel(g_face));

        if (ImVarFont::UsesHarfBuzz(g_face)) {
            bool use_hb = ImVarFont::GetUseHarfBuzz(g_face);
            if (ImGui::Checkbox("Use HarfBuzz", &use_hb))
                ImVarFont::SetUseHarfBuzz(g_face, use_hb);
            if (!ImVarFont::HasGpos(g_face))
                ImGui::TextDisabled("(font has no GPOS table)");
        } else {
            bool use_hb = false;
            ImGui::BeginDisabled();
            ImGui::Checkbox("Use HarfBuzz", &use_hb);
            ImGui::EndDisabled();
            ImGui::TextDisabled("(not built with HarfBuzz)");
        }

        if (ImVarFont::HasKernTable(g_face)) {
            bool use_kern = ImVarFont::GetUseKernTable(g_face);
            if (ImGui::Checkbox("Use kern table", &use_kern))
                ImVarFont::SetUseKernTable(g_face, use_kern);
        } else {
            bool use_kern = false;
            ImGui::BeginDisabled();
            ImGui::Checkbox("Use kern table", &use_kern);
            ImGui::EndDisabled();
            ImGui::TextDisabled("(font has no kern table)");
        }
    }

    ImGui::Spacing();
    ImGui::ColorEdit3("Text",  (float*)&g_textColor, ImGuiColorEditFlags_NoInputs);
    if (ImGui::IsItemEdited()) g_rasterDirty = true;
    ImGui::SameLine();
    ImGui::ColorEdit3("BG",    (float*)&g_bgColor,   ImGuiColorEditFlags_NoInputs);

    // ── Axes ─────────────────────────────────────────────────────────────────
    if (g_face && ImVarFont::IsVariable(g_face)) {
        ImGui::SeparatorText("Axes");
        if (ImVarFont::AxisSliders(g_face, "##axes", g_extrapolate)) {
            if (g_useFontForUi) g_uiFontDirty = true;
            g_rasterDirty = true;
        }
        ImGui::Checkbox("Extrapolate beyond limits", &g_extrapolate);
        if (ImGui::IsItemEdited() && g_face) {
            ImVarFont::ApplyAxes(g_face, g_extrapolate);
            if (g_morph) ImVarFont::EnableMorph(g_face, true, g_extrapolate);
            g_rasterDirty = true;
        }

        // GPU-style morphing — axis drags blend a cached base outline plus
        // per-axis deltas instead of re-instancing FreeType (re-rasterization-free).
        if (ImGui::Checkbox("Morph axes (no re-raster)", &g_morph)) {
            ImVarFont::EnableMorph(g_face, g_morph, g_extrapolate);
            if (!g_morph) ImVarFont::ApplyAxes(g_face, g_extrapolate);  // resync FT
            g_rasterDirty = true;
        }
        if (g_morph)
            ImGui::TextDisabled(ImVarFont::RendererReady()
                                ? "Knot-lattice blend (live GPU coverage)"
                                : "Needs GPU renderer; inactive on this backend");

        // Exact-curve analytic-coverage rasterizer. Feeds the morph output's
        // quadratics straight to the GPU; single-pass, flattening-free, exact area.
        if (ImVarFont::SlugRendererAvailable()) {
            bool slug = ImVarFont::PreferSlugRenderer();
            if (ImGui::Checkbox("Exact-curve coverage (analytic GPU)", &slug))
                ImVarFont::PreferSlugRenderer(slug);
            if (slug)
                ImGui::TextDisabled("Exact-area coverage on quadratics (no flattening)");
        } else {
            ImGui::TextDisabled("Exact-curve coverage: unavailable on this backend");
        }

        // GPU morph reconstruction: rebuild control points on the GPU from a static
        // base+delta buffer (axis fractions = uniforms). An axis drag/zoom is then a
        // uniform update with no per-glyph CPU outline blend or per-frame upload.
        if (g_morph && ImVarFont::GpuMorphAvailable()) {
            bool gpum = ImVarFont::PreferGpuMorphRenderer();
            if (ImGui::Checkbox("GPU morph reconstruction (base + frac*delta)", &gpum))
                ImVarFont::PreferGpuMorphRenderer(gpum);
            if (gpum)
                ImGui::TextDisabled("Axis/zoom = uniform update; control points rebuilt on GPU");
        } else if (g_morph) {
            ImGui::TextDisabled("GPU morph reconstruction: unavailable on this backend");
        }

        // Grid-fit small sizes: render small text through FreeType's own autohinter +
        // raster (guaranteed FreeType parity, no shape distortion). Runs static and
        // under a live morph; above the size cap it is a no-op (analytic / GPU morph).
        bool gf = ImVarFont::PreferGridFit();
        if (ImGui::Checkbox("FreeType-hint small text", &gf)) {
            ImVarFont::PreferGridFit(gf);
            g_rasterDirty = true;
        }
        if (gf) {
            float maxPx = ImVarFont::PreferGridFitMaxPx();
            if (ImGui::SliderFloat("Max px/em", &maxPx, 8.f, 48.f, "%.0f")) {
                ImVarFont::PreferGridFitMaxPx(maxPx);
                g_rasterDirty = true;
            }
            ImGui::TextDisabled("FreeType raster below %.0f px/em; analytic above", maxPx);
        }
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Preview window  (centre – large stroked outlines)
// ---------------------------------------------------------------------------

static void DrawPreview() {
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          ImGui::ColorConvertFloat4ToU32(g_bgColor));
    ImGui::Begin("Preview", nullptr, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();

    const ImVec2 canvasPos  = ImGui::GetCursorScreenPos();
    const ImVec2 canvasSize = ImGui::GetContentRegionAvail();

    ImGui::InvisibleButton("##preview_canvas", canvasSize,
                           ImGuiButtonFlags_MouseButtonLeft);
    const bool canvasHovered = ImGui::IsItemHovered();
    const bool canvasActive  = ImGui::IsItemActive();

    ImGuiIO& io = ImGui::GetIO();
    if (canvasHovered && io.MouseWheel != 0.f) {
        const ImVec2 center = {
            canvasPos.x + canvasSize.x * 0.5f,
            canvasPos.y + canvasSize.y * 0.5f
        };
        const ImVec2 world = {
            (io.MousePos.x - center.x - g_previewPan.x) / g_previewZoom,
            (io.MousePos.y - center.y - g_previewPan.y) / g_previewZoom
        };
        g_previewZoom *= (1.f + io.MouseWheel * 0.12f);
        g_previewZoom = std::clamp(g_previewZoom, 0.05f, 20.f);
        g_previewPan.x = io.MousePos.x - center.x - world.x * g_previewZoom;
        g_previewPan.y = io.MousePos.y - center.y - world.y * g_previewZoom;
        g_rasterDirty = true;
    }
    if (canvasActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        g_previewPan = { g_previewPan.x + io.MouseDelta.x,
                         g_previewPan.y + io.MouseDelta.y };
    if (canvasHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        g_previewPan  = { 0.f, 0.f };
        g_previewZoom = 1.f;
        g_rasterDirty = true;
    }

    if (g_face) {
        syncRenderSettings();
        const float emPx = g_emPx * g_previewZoom;
        const float lineH = ImVarFont::CalcLineHeightPx(g_face, emPx) * g_lineHeightMult;
        const float letterSp = g_letterSpacingEm * emPx;
        float textW = 0.f, textH = 0.f;
        calcTextBlock(g_face, emPx, lineH, letterSp, g_text, &textW, &textH);

        const ImVec2 center = {
            canvasPos.x + canvasSize.x * 0.5f,
            canvasPos.y + canvasSize.y * 0.5f
        };
        const float posX = center.x - textW * 0.5f + g_previewPan.x;
        const float posY = center.y - textH * 0.5f + g_previewPan.y;

        ImU32 col = ImGui::ColorConvertFloat4ToU32(g_textColor);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        if (g_renderModeIdx == (int)ImVarFont::RenderMode::Raster) {
            if (g_rasterDirty) {
                int rw = 0, rh = 0;
                if (ImVarFont::RasterizeText(g_face, emPx, g_text, col, lineH, letterSp,
                                             g_rasterPixels, rw, rh)) {
                    uploadRasterTexture(g_rasterPixels, rw, rh);
                }
                g_rasterDirty = false;
            }
            if (g_rasterTex != 0 && g_rasterTexW > 0 && g_rasterTexH > 0) {
                const float drawX = center.x - g_rasterTexW * 0.5f + g_previewPan.x;
                const float drawY = center.y - g_rasterTexH * 0.5f + g_previewPan.y;
                dl->AddImage((ImTextureID)(intptr_t)g_rasterTex,
                             { drawX, drawY },
                             { drawX + (float)g_rasterTexW, drawY + (float)g_rasterTexH });
            }
        } else {
            ImVarFont::TextStyle st;
            st.outline           = g_outline;
            st.outline_thickness = g_thickness * g_previewZoom;
            st.line_height_px    = lineH;
            st.letter_spacing_px = letterSp;
            ImVarFont::AddText(dl, g_face, emPx, { posX, posY }, col, g_text, st);
        }
    } else {
        const char* hint = "Drop a .ttf / .otf file here,  or enter its path and press Load";
        ImVec2 ts    = ImGui::CalcTextSize(hint);
        ImVec2 cpos  = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos({ cpos.x + (canvasSize.x - ts.x) * 0.5f,
                                    cpos.y + (canvasSize.y - ts.y) * 0.5f });
        ImGui::TextDisabled("%s", hint);
    }

    ImGui::End();
}


// ---------------------------------------------------------------------------
// Metadata window  (right panel)
// ---------------------------------------------------------------------------

static void DrawMetadata() {
    ImGui::Begin("Metadata");

    ImVarFont::MetadataTable(g_face);

    ImGui::End();
}

static void DrawKernTable() {
    ImGui::Begin("Kern table");
    ImVarFont::KernTableUi(g_face, g_emPx);
    ImGui::End();
}

// Compact single-window layout for --capture (square / banner GIFs).
static void DrawCaptureScene() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertFloat4ToU32(g_bgColor));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.f, 12.f));
    ImGui::Begin("##capture", nullptr, flags);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();

    ImGui::TextDisabled("ImVarFont");
    ImGui::SameLine();
    ImGui::TextDisabled("· variable-font morph");

    const float sliderH = (g_face && ImVarFont::IsVariable(g_face)) ? 110.f : 0.f;
    const ImVec2 canvasPos  = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    canvasSize.y = std::max(40.f, canvasSize.y - sliderH);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(canvasPos,
                      { canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y },
                      ImGui::ColorConvertFloat4ToU32(g_bgColor));

    if (g_face) {
        syncRenderSettings();
        const float emPx = g_emPx;
        const float lineH = ImVarFont::CalcLineHeightPx(g_face, emPx) * g_lineHeightMult;
        const float letterSp = g_letterSpacingEm * emPx;
        float textW = 0.f, textH = 0.f;
        calcTextBlock(g_face, emPx, lineH, letterSp, g_text, &textW, &textH);

        const ImVec2 center = {
            canvasPos.x + canvasSize.x * 0.5f,
            canvasPos.y + canvasSize.y * 0.5f
        };
        const float posX = center.x - textW * 0.5f;
        const float posY = center.y - textH * 0.5f;
        ImU32 col = ImGui::ColorConvertFloat4ToU32(g_textColor);

        ImVarFont::TextStyle st;
        st.line_height_px    = lineH;
        st.letter_spacing_px = letterSp;
        ImVarFont::AddText(dl, g_face, emPx, { posX, posY }, col, g_text, st);
    }

    ImGui::Dummy(canvasSize);

    if (g_face && ImVarFont::IsVariable(g_face)) {
        ImGui::Separator();
        // Show only weight + width so the square preview stays readable.
        ImVarFont::Axis* axes = ImVarFont::GetAxes(g_face);
        if (g_wghtAxis >= 0) {
            auto& a = axes[g_wghtAxis];
            ImGui::TextUnformatted(a.Name[0] ? a.Name : "wght");
            ImGui::SetNextItemWidth(-1.f);
            ImGui::SliderFloat("##cap_wght", &a.Value, a.Min, a.Max, "%.0f");
        }
        if (g_wdthAxis >= 0) {
            auto& a = axes[g_wdthAxis];
            ImGui::TextUnformatted(a.Name[0] ? a.Name : "wdth");
            ImGui::SetNextItemWidth(-1.f);
            ImGui::SliderFloat("##cap_wdth", &a.Value, a.Min, a.Max, "%.0f");
        }
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// GLFW / OpenGL boilerplate
// ---------------------------------------------------------------------------

static void glfwErrorCb(int err, const char* msg) {
    fprintf(stderr, "GLFW error %d: %s\n", err, msg);
}



int main(int argc, char** argv) {
    NFD_Init();

    // Optional: pass a .ttf/.otf path as the first non-option argument.
    // Otherwise drop a font on the window or use Load in Controls.
    // Capture: --capture <dir> <WxH> <frames>
    const char* cliFont = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--capture") == 0) {
            if (i + 3 >= argc) {
                fprintf(stderr, "usage: --capture <dir> <WxH> <frames>\n");
                NFD_Quit();
                return 1;
            }
            g_captureMode = true;
            strncpy(g_captureDir, argv[++i], sizeof(g_captureDir) - 1);
            if (!parseSizeWxH(argv[++i], &g_captureW, &g_captureH)) {
                fprintf(stderr, "bad size (expected e.g. 600x600)\n");
                NFD_Quit();
                return 1;
            }
            g_captureFrames = atoi(argv[++i]);
            if (g_captureFrames < 1) g_captureFrames = 1;
        } else if (argv[i][0] != '-') {
            cliFont = argv[i];
        }
    }

    glfwSetErrorCallback(glfwErrorCb);
    if (!glfwInit()) { NFD_Quit(); return 1; }

#if defined(IMGUI_IMPL_OPENGL_ES3)
    // OpenGL ES 3.0 context (Raspberry Pi / embedded). Requires a GLFW built
    // with EGL + GLES support (the default on Pi OS / Wayland / X11).
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
#endif

    const int W = g_captureMode ? g_captureW : 1920;
    const int H = g_captureMode ? g_captureH : 1080;
    GLFWwindow* window = glfwCreateWindow(W, H,
                                           g_captureMode
                                               ? "ImVarFont  –  capture"
                                               : "ImVarFont  –  Variable Font Viewer",
                                           nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(g_captureMode ? 0 : 1);

    // ── DPI scale (HiDPI / 4K support) ───────────────────────────────────────
    glfwGetWindowContentScale(window, &g_dpi_scale, nullptr);
    if (g_dpi_scale < 1.0f) g_dpi_scale = 1.0f;  // sanity clamp
    if (g_captureMode)
        g_dpi_scale = 1.0f;  // keep capture pixels predictable

    // ── Drag-and-drop ─────────────────────────────────────────────────────────
    glfwSetDropCallback(window, dropCallback);

    // ── Dear ImGui ────────────────────────────────────────────────────────────
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    if (!g_captureMode)
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Style  (scale all sizes by DPI so padding / borders feel right on 4K)
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.f;
    style.FrameRounding  = 3.f;
    style.ItemSpacing    = { 8.f, 6.f };
    style.Colors[ImGuiCol_WindowBg]      = { 0.10f, 0.10f, 0.13f, 1.00f };
    style.Colors[ImGuiCol_TitleBg]       = { 0.08f, 0.08f, 0.10f, 1.00f };
    style.Colors[ImGuiCol_TitleBgActive] = { 0.13f, 0.13f, 0.18f, 1.00f };
    style.ScaleAllSizes(g_dpi_scale);   // scale padding, borders, scrollbars

    g_uiFontSize = 15.f * g_dpi_scale;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
#if defined(IMGUI_IMPL_OPENGL_ES3)
    ImGui_ImplOpenGL3_Init("#version 300 es");
#else
    ImGui_ImplOpenGL3_Init("#version 330 core");
#endif

    // Analytic GPU glyph renderer (signed-area + exact-curve backends). Uses GLFW's GL loader.
    // The "Force CPU fallback" checkbox in the UI toggles ImVarFont::ForceCpuFallback
    // at runtime to exercise the ES2 / WebGL1 path on desktop.
    if (!ImVarFont::InitRenderer((void* (*)(const char*))glfwGetProcAddress))
        std::fprintf(stderr, "ImVarFont: GPU renderer init failed; using CPU fallback\n");

    if (cliFont) {
        strncpy(g_fontPath, cliFont, sizeof(g_fontPath) - 1);
        g_fontPath[sizeof(g_fontPath) - 1] = '\0';
        tryLoadFont();
    }

    if (g_captureMode) {
        if (!g_face) {
            fprintf(stderr, "capture requires a font path argument\n");
            ImVarFont::ShutdownRenderer();
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
            glfwDestroyWindow(window);
            glfwTerminate();
            NFD_Quit();
            return 1;
        }
        std::error_code ec;
        std::filesystem::create_directories(g_captureDir, ec);
        if (ec) {
            fprintf(stderr, "cannot create capture dir %s: %s\n",
                    g_captureDir, ec.message().c_str());
            return 1;
        }
        // Demo defaults tuned for a readable square / banner loop.
        strncpy(g_text, "ImVarFont", sizeof(g_text) - 1);
        g_emPx = (g_captureH <= 500) ? 72.f : 96.f;
        g_morph = true;
        g_bgColor = { 0.06f, 0.07f, 0.10f, 1.00f };
        g_textColor = { 1.00f, 1.00f, 1.00f, 1.00f };
        findWghtWdthAxes();
        ImVarFont::EnableMorph(g_face, true, false);
        if (ImVarFont::SlugRendererAvailable())
            ImVarFont::PreferSlugRenderer(true);
        applyCaptureAxisFrame(0, g_captureFrames);
        fprintf(stderr, "capture → %s  %dx%d  %d frames\n",
                g_captureDir, g_captureW, g_captureH, g_captureFrames);
    }

    rebuildUiFont();


    // ── Main loop ────────────────────────────────────────────────────────────
    int appliedSwap = g_captureMode ? 0 : 1;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        const int wantSwap = g_vsync ? 1 : 0;
        if (!g_captureMode && wantSwap != appliedSwap) {
            glfwSwapInterval(wantSwap);
            appliedSwap = wantSwap;
        }

        if (g_captureMode && g_captureWarmup <= 0) {
            const int fi = std::min(g_captureIndex, g_captureFrames - 1);
            applyCaptureAxisFrame(fi, g_captureFrames);
        }

        if (g_uiFontDirty)
            rebuildUiFont();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (g_captureMode) {
            DrawCaptureScene();
        } else {
            DrawDockSpace();
            DrawPerformance();
            DrawControls();
            DrawPreview();
            DrawMetadata();
            DrawKernTable();
        }

        // ── Render ──────────────────────────────────────────────────────────
        ImGui::Render();
        int fbW = 0, fbH = 0;
        glfwGetFramebufferSize(window, &fbW, &fbH);
        glViewport(0, 0, fbW, fbH);
        glClearColor(g_bgColor.x, g_bgColor.y, g_bgColor.z, g_bgColor.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (g_captureMode) {
            if (g_captureWarmup > 0) {
                --g_captureWarmup;
            } else if (g_captureIndex < g_captureFrames) {
                std::vector<uint8_t> px((size_t)fbW * fbH * 4);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, fbW, fbH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                char path[768];
                snprintf(path, sizeof(path), "%s/frame_%04d.ppm",
                         g_captureDir, g_captureIndex);
                if (!saveFramePpm(path, fbW, fbH, px.data()))
                    fprintf(stderr, "failed to write %s\n", path);
                ++g_captureIndex;
                if (g_captureIndex >= g_captureFrames)
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
        }

        glfwSwapBuffers(window);
    }

    // ── Cleanup ──────────────────────────────────────────────────────────────
    ImVarFont::ShutdownRenderer();
    ImVarFont::FreeFace(g_face);
    freeRasterTexture();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    NFD_Quit();
}
