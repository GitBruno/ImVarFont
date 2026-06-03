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
#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------

static ImVarFont::Face* g_face          = nullptr;
static char             g_fontPath[512] = "";
static char             g_loadError[256] = "";

static char             g_text[4096]  = "The quick brown fox\njumps over the lazy dog";
static float            g_emPx        = 140.f;
static float            g_thickness   = 1.5f;
static ImVec4           g_textColor   = { 1.00f, 1.00f, 1.00f, 1.00f };
static ImVec4           g_bgColor     = { 0.04f, 0.04f, 0.07f, 1.00f };

static bool             g_extrapolate      = false;
static float            g_dpi_scale        = 1.0f;  // set from glfwGetWindowContentScale

static bool             g_useFontForUi     = false;
static float            g_uiFontSize       = 15.f;
static bool             g_uiFontDirty      = true;
static ImFont*          g_uiFont           = nullptr;
static bool             g_dockLayoutBuilt  = false;

static ImVec2           g_previewPan       = { 0.f, 0.f };
static float            g_previewZoom      = 1.f;

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
}

static ImFont* loadSystemUiFont(ImFontAtlas* atlas, float size_px) {
#if defined(_WIN32)
    const char* ui_font_paths[] = {
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/calibri.ttf",
        nullptr
    };
#elif defined(__APPLE__)
    const char* ui_font_paths[] = {
        "/System/Library/Fonts/SFNS.ttf",
        "/Library/Fonts/Arial.ttf",
        nullptr
    };
#else
    const char* ui_font_paths[] = {
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        nullptr
    };
#endif
    for (int i = 0; ui_font_paths[i]; ++i) {
        if (ImFont* font = atlas->AddFontFromFileTTF(ui_font_paths[i], size_px))
            return font;
    }
    ImFontConfig cfg;
    cfg.SizePixels = size_px;
    return atlas->AddFontDefault(&cfg);
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
        g_uiFont = loadSystemUiFont(io.Fonts, g_uiFontSize);
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

static float calcLineWidth(ImVarFont::Face* face, float emPx,
                           const char* start, const char* end) {
    if (start >= end) return 0.f;
    std::string line(start, (size_t)(end - start));
    return ImVarFont::CalcTextWidth(face, emPx, line.c_str());
}

// Measure widest line and total block height for multiline preview centring
static void calcTextBlock(ImVarFont::Face* face, float emPx, const char* text,
                          float* outW, float* outH) {
    const float asc   = ImVarFont::CalcAscenderPx(face, emPx);
    const float desc  = ImVarFont::CalcDescenderPx(face, emPx);
    const float lineH = ImVarFont::CalcLineHeightPx(face, emPx);

    float maxW = 0.f;
    int   lines = 1;
    const char* lineStart = text;
    for (const char* p = text; ; ++p) {
        if (*p == '\n' || *p == '\0') {
            maxW = std::max(maxW, calcLineWidth(face, emPx, lineStart, p));
            if (*p == '\0') break;
            ++lines;
            lineStart = p + 1;
        }
    }

    *outW = maxW;
    *outH = asc + desc + (lines - 1) * lineH;
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

    ImGui::DockBuilderDockWindow("Controls", dock_left);
    ImGui::DockBuilderDockWindow("Preview",  dock_main);
    ImGui::DockBuilderDockWindow("Metadata", dock_right);
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
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextMultiline("##text", g_text, sizeof(g_text),
                              ImVec2(-1.f, 90.f * g_dpi_scale));

    ImGui::Spacing();
    ImGui::Text("Size (px)");
    ImGui::SetNextItemWidth(-1.f);
    ImGui::SliderFloat("##emPx", &g_emPx, 12.f, 600.f, "%.0f px");

    ImGui::Text("Stroke thickness");
    ImGui::SetNextItemWidth(-1.f);
    ImGui::SliderFloat("##thick", &g_thickness, 0.5f, 8.f, "%.1f px");

    ImGui::Spacing();
    ImGui::ColorEdit3("Text",  (float*)&g_textColor, ImGuiColorEditFlags_NoInputs);
    ImGui::SameLine();
    ImGui::ColorEdit3("BG",    (float*)&g_bgColor,   ImGuiColorEditFlags_NoInputs);

    // ── Axes ─────────────────────────────────────────────────────────────────
    if (g_face && ImVarFont::IsVariable(g_face)) {
        ImGui::SeparatorText("Axes");
        if (ImVarFont::AxisSliders(g_face, "##axes", g_extrapolate) && g_useFontForUi)
            g_uiFontDirty = true;
        ImGui::Checkbox("Extrapolate beyond limits", &g_extrapolate);
        if (ImGui::IsItemEdited() && g_face)
            ImVarFont::ApplyAxes(g_face, g_extrapolate);
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
    }
    if (canvasActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        g_previewPan = { g_previewPan.x + io.MouseDelta.x,
                         g_previewPan.y + io.MouseDelta.y };
    if (canvasHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        g_previewPan  = { 0.f, 0.f };
        g_previewZoom = 1.f;
    }

    if (g_face) {
        const float emPx = g_emPx * g_previewZoom;
        float textW = 0.f, textH = 0.f;
        calcTextBlock(g_face, emPx, g_text, &textW, &textH);

        const ImVec2 center = {
            canvasPos.x + canvasSize.x * 0.5f,
            canvasPos.y + canvasSize.y * 0.5f
        };
        const float posX = center.x - textW * 0.5f + g_previewPan.x;
        const float posY = center.y - textH * 0.5f + g_previewPan.y;

        ImU32 col = ImGui::ColorConvertFloat4ToU32(g_textColor);
        ImVarFont::AddText(ImGui::GetWindowDrawList(), g_face,
                           emPx, { posX, posY },
                           col, g_text, g_thickness * g_previewZoom);

        if (canvasHovered) {
            ImGui::SetTooltip("Scroll: zoom  ·  Drag: pan  ·  Double-click: reset");
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

// ---------------------------------------------------------------------------
// GLFW / OpenGL boilerplate
// ---------------------------------------------------------------------------

static void glfwErrorCb(int err, const char* msg) {
    fprintf(stderr, "GLFW error %d: %s\n", err, msg);
}

int main(int /*argc*/, char** /*argv*/) {
    NFD_Init();

    glfwSetErrorCallback(glfwErrorCb);
    if (!glfwInit()) { NFD_Quit(); return 1; }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    constexpr int W = 1400, H = 900;
    GLFWwindow* window = glfwCreateWindow(W, H,
                                           "ImVarFont  –  Variable Font Viewer",
                                           nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    // ── DPI scale (HiDPI / 4K support) ───────────────────────────────────────
    glfwGetWindowContentScale(window, &g_dpi_scale, nullptr);
    if (g_dpi_scale < 1.0f) g_dpi_scale = 1.0f;  // sanity clamp

    // ── Drag-and-drop ─────────────────────────────────────────────────────────
    glfwSetDropCallback(window, dropCallback);

    // ── Dear ImGui ────────────────────────────────────────────────────────────
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
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
    ImGui_ImplOpenGL3_Init("#version 330 core");

    rebuildUiFont();

    // ── Main loop ────────────────────────────────────────────────────────────
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        if (g_uiFontDirty)
            rebuildUiFont();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        DrawDockSpace();
        DrawControls();
        DrawPreview();
        DrawMetadata();

        // ── Render ──────────────────────────────────────────────────────────
        ImGui::Render();
        int fbW = 0, fbH = 0;
        glfwGetFramebufferSize(window, &fbW, &fbH);
        glViewport(0, 0, fbW, fbH);
        glClearColor(0.04f, 0.04f, 0.07f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // ── Cleanup ──────────────────────────────────────────────────────────────
    ImVarFont::FreeFace(g_face);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    NFD_Quit();
    return 0;
}
