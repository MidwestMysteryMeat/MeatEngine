#include "editor/RoomEditor.h"

#include "engine/core/Log.h"
#include "engine/core/ViewMath.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <ImGuizmo.h> // must follow imgui.h — its header uses ImGui types
#include <imnodes.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <utility>

namespace meat {

namespace {

constexpr float kMaxPitch = glm::radians(89.0f);
constexpr float kPickDistance = 100.0f;
constexpr int kMaxBatchOps = 4096;   // sanity cap: a runaway drag must not flood the server
constexpr int kMaxPreviewLights = 16;

// Preview markers are point lights, not sprites: submitSprite needs a
// TextureHandle the editor doesn't own, while lights are visible, free,
// and already capped by the renderer's per-frame budget.
constexpr glm::vec3 kBuildPreview{0.3f, 0.9f, 0.3f};
constexpr glm::vec3 kErasePreview{0.9f, 0.3f, 0.3f};
constexpr glm::vec3 kVolumePreview{0.15f, 0.2f, 0.6f};

const char* const kToolNames[] = {"Place",   "Erase",   "Wall",  "Floor",
                                  "Platform", "Doorway", "Light", "Seed Volume"};

// Content-browser group labels, indexed by RoomEditor::AssetKind (minus Count).
const char* const kAssetKindNames[] = {"Models", "Textures", "Scripts", "Shaders", "Other"};

// Short tile tag drawn on a content-browser placeholder square, indexed by kind.
const char* const kAssetKindTags[] = {"MDL", "TEX", "LUA", "SHD", "FILE"};

// Distinct restrained accent per asset kind for the placeholder tile — colours
// read as a family (muted, not neon) so the grid scans by type at a glance.
ImVec4 kindTileColor(int kind) {
    switch (kind) {
    case 0: return ImVec4(0.22f, 0.42f, 0.70f, 1.00f); // Models  — blue
    case 1: return ImVec4(0.28f, 0.55f, 0.42f, 1.00f); // Textures— green
    case 2: return ImVec4(0.62f, 0.45f, 0.24f, 1.00f); // Scripts — amber
    case 3: return ImVec4(0.50f, 0.33f, 0.62f, 1.00f); // Shaders — violet
    default: return ImVec4(0.34f, 0.35f, 0.38f, 1.00f); // Other  — grey
    }
}
// Folder tiles get their own warm-grey so they never look like an asset kind.
const ImVec4 kFolderTileColor(0.55f, 0.48f, 0.28f, 1.00f);

// Truncate label to fit within maxW pixels, appending an ellipsis when clipped,
// so tile captions stay on one line under a fixed-width square.
std::string fitLabel(const std::string& s, float maxW) {
    if (ImGui::CalcTextSize(s.c_str()).x <= maxW) return s;
    std::string out = s;
    while (!out.empty() && ImGui::CalcTextSize((out + "...").c_str()).x > maxW)
        out.pop_back();
    return out + "...";
}

// Human-readable byte count for the content browser's size column.
std::string humanSize(std::uintmax_t bytes) {
    const char* const units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

std::string toLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

int snapDown(int v, int step) {
    // floor-division snap so negative coords snap toward -inf, not toward 0
    const int r = v % step;
    return v - (r < 0 ? r + step : r);
}

glm::vec3 voxelCenter(glm::ivec3 v) {
    return (glm::vec3(v) + 0.5f) * kVoxelSize;
}

bool worldClickable(bool flying) {
    if (!ImGui::IsMouseClicked(0)) return false;
    if (ImGuizmo::IsOver() || ImGuizmo::IsUsing()) return false;
    // While flying the cursor is captured and frozen — ImGui's hover state is
    // stale, so only respect WantCaptureMouse with a free cursor.
    return flying || !ImGui::GetIO().WantCaptureMouse;
}

} // namespace

void RoomEditor::update(EditorContext& ctx, float dt) {
    if (!m_themed) { // one-shot: restyle ImGui the first frame the editor runs
        applyEditorTheme();
        m_themed = true;
    }
    if (!m_editorBootLogged) {
        m_editorBootLogged = true;
        log::info("editor: Room Designer ready — Output Log, Node Graph, Details available");
    }
    m_previewLights = 0;
    ctx.buildBlock = m_buildBlock; // ctx is rebuilt per frame; the editor owns persistence
    if (m_statusTtl > 0.0f && (m_statusTtl -= dt) <= 0.0f) m_status.clear();
    if (m_codeStatusTtl > 0.0f && (m_codeStatusTtl -= dt) <= 0.0f) m_codeStatus.clear();
    if (m_importStatusTtl > 0.0f && (m_importStatusTtl -= dt) <= 0.0f) m_importStatus.clear();
    if (m_graphStatusTtl > 0.0f && (m_graphStatusTtl -= dt) <= 0.0f) m_graphStatus.clear();

    // OS drag-drop: any files dropped on the window this frame get imported, so a
    // dev can drop an .fbx straight onto the viewport instead of typing a path.
    for (const std::string& dropped : ctx.input.drainDroppedPaths()) doImport(ctx, dropped);

    updateFlyCamera(ctx, dt);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float aspect = vp->Size.y > 0.0f ? vp->Size.x / vp->Size.y : 16.0f / 9.0f;
    const glm::mat4 view = ctx.camera.view();
    const glm::mat4 proj = ctx.camera.proj(aspect);

    drawTopBar(ctx);
    drawToolbar();
    drawOutliner(ctx);
    drawAssetBrowser(ctx);
    drawContentBrowser(ctx);
    drawCodeEditor(ctx);
    drawNodeGraph(ctx);
    drawNodeGraphDetails(ctx);
    drawOutputLog();
    drawDetailsPanel(ctx);
    updateObjectHighlight(ctx);
    if (!m_flying) drawGizmo(ctx, view, proj);

    // Picking ray: camera center while flying, otherwise unproject the cursor.
    glm::vec3 rayDir = ctx.camera.forward();
    if (!m_flying) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const float nx = 2.0f * (mouse.x - vp->Pos.x) / vp->Size.x - 1.0f;
        const float ny = 1.0f - 2.0f * (mouse.y - vp->Pos.y) / vp->Size.y;
        const glm::mat4 invVp = glm::inverse(proj * view);
        glm::vec4 pNear = invVp * glm::vec4(nx, ny, -1.0f, 1.0f);
        glm::vec4 pFar = invVp * glm::vec4(nx, ny, 1.0f, 1.0f);
        pNear /= pNear.w;
        pFar /= pFar.w;
        rayDir = glm::normalize(glm::vec3(pFar) - glm::vec3(pNear));
    }
    handleTool(ctx, ctx.camera.pos, rayDir);

    // Visualize the selected seed volume with dim corner lights.
    if (m_selKind == Selection::Volume && m_selIndex >= 0 &&
        m_selIndex < static_cast<int>(ctx.seedVolumes.size())) {
        const SeedVolume& sv = ctx.seedVolumes[static_cast<std::size_t>(m_selIndex)];
        previewBox(ctx, sv.min, sv.max, kVolumePreview, false);
    }

    if (ctx.input.pressed(GLFW_KEY_ESCAPE)) m_anchor.reset();

    // Delete removes the selected prop (matching the Outliner's Delete button).
    // Server-authoritative: request remove; PropRemoved echo updates ctx.props.
    if (m_selKind == Selection::Prop && ctx.input.pressed(GLFW_KEY_DELETE) && m_selIndex >= 0 &&
        m_selIndex < static_cast<int>(ctx.props.size())) {
        const EditorProp& prop = ctx.props[static_cast<std::size_t>(m_selIndex)];
        if (prop.id != 0 && ctx.requestRemoveProp) ctx.requestRemoveProp(prop.id);
        m_selKind = Selection::None;
        m_selIndex = -1;
        m_propGizmoDirty = false;
    }
}

void RoomEditor::updateFlyCamera(EditorContext& ctx, float dt) {
    const bool rmb = ctx.input.down(GLFW_MOUSE_BUTTON_RIGHT);
    if (!m_flying && rmb && !ImGui::GetIO().WantCaptureMouse && !ImGuizmo::IsUsing()) {
        m_flying = true;
        ctx.setRelativeMouse(true);
        ctx.input.consumeScrollSteps(); // discard notches accumulated while free,
                                        // else the speed jumps on fly start
    } else if (m_flying && !rmb) {
        m_flying = false;
        ctx.setRelativeMouse(false);
    }
    if (!m_flying) return;

    const glm::vec2 look = ctx.input.mouseDelta() * ctx.input.sensitivity;
    ctx.camera.yaw -= look.x; // matches the game: mouse right turns right (yaw decreases)
    ctx.camera.pitch = glm::clamp(ctx.camera.pitch - look.y, -kMaxPitch, kMaxPitch);

    if (const int steps = ctx.input.consumeScrollSteps(); steps != 0)
        m_flySpeed = glm::clamp(m_flySpeed + 2.0f * static_cast<float>(steps), 2.0f, 64.0f);

    const glm::vec3 fwd = ctx.camera.forward();
    const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0.0f, 1.0f, 0.0f)));
    glm::vec3 move{0.0f};
    if (ctx.input.down(GLFW_KEY_W)) move += fwd;
    if (ctx.input.down(GLFW_KEY_S)) move -= fwd;
    if (ctx.input.down(GLFW_KEY_D)) move += right;
    if (ctx.input.down(GLFW_KEY_A)) move -= right;
    if (ctx.input.down(GLFW_KEY_E)) move.y += 1.0f;
    if (ctx.input.down(GLFW_KEY_Q)) move.y -= 1.0f;
    if (glm::dot(move, move) > 0.0f) {
        const float speed = m_flySpeed * (ctx.input.down(GLFW_KEY_LEFT_SHIFT) ? 3.0f : 1.0f);
        ctx.camera.pos += glm::normalize(move) * speed * dt;
    }
}

// UE5 Slate-inspired dark tool theme: near-black panels, cool greys, selection
// blue close to UE's "Details" accent. Only editor windows use this style.
void RoomEditor::applyEditorTheme() {
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding = 2.0f;
    s.ChildRounding = 2.0f;
    s.FrameRounding = 2.0f;
    s.PopupRounding = 2.0f;
    s.GrabRounding = 2.0f;
    s.TabRounding = 2.0f;
    s.ScrollbarRounding = 2.0f;
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.PopupBorderSize = 1.0f;
    s.WindowPadding = ImVec2(8.0f, 8.0f);
    s.FramePadding = ImVec2(6.0f, 3.0f);
    s.ItemSpacing = ImVec2(6.0f, 4.0f);
    s.ItemInnerSpacing = ImVec2(4.0f, 3.0f);
    s.ScrollbarSize = 11.0f;
    s.GrabMinSize = 8.0f;
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);

    // Approximate Unreal Slate: GraphEditor / Details panel greys.
    const ImVec4 accent(0.0f, 0.47f, 0.83f, 1.00f);       // UE select blue
    const ImVec4 accentDim(0.0f, 0.47f, 0.83f, 0.45f);
    const ImVec4 accentHover(0.12f, 0.56f, 0.92f, 1.00f);
    const ImVec4 bg(0.02f, 0.02f, 0.02f, 1.00f);          // #050505
    const ImVec4 panel(0.09f, 0.09f, 0.09f, 1.00f);       // #171717
    const ImVec4 panelHover(0.14f, 0.14f, 0.14f, 1.00f);
    const ImVec4 header(0.12f, 0.12f, 0.12f, 1.00f);
    const ImVec4 border(0.22f, 0.22f, 0.22f, 0.90f);
    const ImVec4 text(0.86f, 0.86f, 0.86f, 1.00f);
    const ImVec4 textDim(0.45f, 0.45f, 0.45f, 1.00f);
    const ImVec4 titleBg(0.04f, 0.04f, 0.04f, 1.00f);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = textDim;
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = ImVec4(0.05f, 0.05f, 0.05f, 1.00f);
    c[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.07f, 0.07f, 0.98f);
    c[ImGuiCol_Border] = border;
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg] = panel;
    c[ImGuiCol_FrameBgHovered] = panelHover;
    c[ImGuiCol_FrameBgActive] = accentDim;
    c[ImGuiCol_TitleBg] = titleBg;
    c[ImGuiCol_TitleBgActive] = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed] = titleBg;
    c[ImGuiCol_MenuBarBg] = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.03f, 0.03f, 0.03f, 0.60f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive] = accent;
    c[ImGuiCol_CheckMark] = accentHover;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accentHover;
    c[ImGuiCol_Button] = panel;
    c[ImGuiCol_ButtonHovered] = panelHover;
    c[ImGuiCol_ButtonActive] = accentDim;
    c[ImGuiCol_Header] = header;
    c[ImGuiCol_HeaderHovered] = ImVec4(0.0f, 0.47f, 0.83f, 0.35f);
    c[ImGuiCol_HeaderActive] = accentDim;
    c[ImGuiCol_Separator] = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    c[ImGuiCol_SeparatorHovered] = accentDim;
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = ImVec4(0.20f, 0.20f, 0.20f, 0.60f);
    c[ImGuiCol_ResizeGripHovered] = accentDim;
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_Tab] = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    c[ImGuiCol_TabHovered] = accentDim;
    c[ImGuiCol_TabActive] = ImVec4(0.12f, 0.22f, 0.35f, 1.00f);
    c[ImGuiCol_TabUnfocused] = ImVec4(0.05f, 0.05f, 0.05f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive] = ImVec4(0.08f, 0.12f, 0.18f, 1.00f);
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_TextSelectedBg] = accentDim;
    c[ImGuiCol_NavHighlight] = accent;
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    c[ImGuiCol_TableBorderStrong] = border;
    c[ImGuiCol_TableBorderLight] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.02f);
}

void RoomEditor::applyNodeGraphStyle() {
    // Node graph canvas: dark grid, red events, blue actions, pure teal/green.
    ImNodesStyle& ns = ImNodes::GetStyle();
    ns.NodeCornerRounding = 3.0f;
    ns.NodePadding = ImVec2(8.0f, 6.0f);
    ns.NodeBorderThickness = 1.0f;
    ns.LinkThickness = 2.5f;
    ns.PinCircleRadius = 4.5f;
    ns.PinHoverRadius = 8.0f;
    ns.GridSpacing = 24.0f;
    ns.Flags = ImNodesStyleFlags_NodeOutline | ImNodesStyleFlags_GridLines |
               ImNodesStyleFlags_GridLinesPrimary;
    // Colors as ABGR packed for imnodes (ImU32).
    auto col = [](int r, int g, int b, int a = 255) -> unsigned int {
        return IM_COL32(r, g, b, a);
    };
    ns.Colors[ImNodesCol_NodeBackground] = col(24, 24, 24);
    ns.Colors[ImNodesCol_NodeBackgroundHovered] = col(32, 32, 32);
    ns.Colors[ImNodesCol_NodeBackgroundSelected] = col(28, 40, 55);
    ns.Colors[ImNodesCol_NodeOutline] = col(10, 10, 10);
    ns.Colors[ImNodesCol_TitleBar] = col(50, 50, 50);
    ns.Colors[ImNodesCol_TitleBarHovered] = col(60, 60, 60);
    ns.Colors[ImNodesCol_TitleBarSelected] = col(0, 100, 170);
    ns.Colors[ImNodesCol_Link] = col(220, 220, 220);
    ns.Colors[ImNodesCol_LinkHovered] = col(255, 255, 255);
    ns.Colors[ImNodesCol_LinkSelected] = col(0, 160, 255);
    ns.Colors[ImNodesCol_Pin] = col(200, 200, 200);
    ns.Colors[ImNodesCol_PinHovered] = col(255, 255, 255);
    ns.Colors[ImNodesCol_BoxSelector] = col(0, 120, 215, 40);
    ns.Colors[ImNodesCol_BoxSelectorOutline] = col(0, 120, 215, 180);
    ns.Colors[ImNodesCol_GridBackground] = col(18, 18, 18);
    ns.Colors[ImNodesCol_GridLine] = col(32, 32, 32);
    ns.Colors[ImNodesCol_GridLinePrimary] = col(48, 48, 48);
    ns.Colors[ImNodesCol_MiniMapBackground] = col(12, 12, 12, 200);
    ns.Colors[ImNodesCol_MiniMapBackgroundHovered] = col(16, 16, 16, 220);
    ns.Colors[ImNodesCol_MiniMapOutline] = col(60, 60, 60);
    ns.Colors[ImNodesCol_MiniMapOutlineHovered] = col(0, 120, 215);
    ns.Colors[ImNodesCol_MiniMapNodeBackground] = col(40, 40, 40);
    ns.Colors[ImNodesCol_MiniMapNodeBackgroundHovered] = col(70, 70, 70);
    ns.Colors[ImNodesCol_MiniMapNodeBackgroundSelected] = col(0, 100, 170);
    ns.Colors[ImNodesCol_MiniMapNodeOutline] = col(20, 20, 20);
    ns.Colors[ImNodesCol_MiniMapLink] = col(160, 160, 160);
    ns.Colors[ImNodesCol_MiniMapLinkSelected] = col(0, 160, 255);
    ns.Colors[ImNodesCol_MiniMapCanvas] = col(0, 0, 0, 0);
    ns.Colors[ImNodesCol_MiniMapCanvasOutline] = col(80, 80, 80);
}

void RoomEditor::drawTopBar(EditorContext& ctx) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({vp->Pos.x + 12.0f, vp->Pos.y + 12.0f}, ImGuiCond_FirstUseEver);
    ImGui::Begin("Room Designer", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("tool: %s", kToolNames[static_cast<int>(m_tool)]);

    int block = static_cast<int>(m_buildBlock);
    ImGui::RadioButton("stone", &block, 1);
    ImGui::SameLine();
    ImGui::RadioButton("dirt", &block, 2);
    ImGui::SameLine();
    ImGui::RadioButton("grass", &block, 3);
    m_buildBlock = static_cast<BlockId>(block);
    ctx.buildBlock = m_buildBlock;

    if (ImGui::Button("Save World")) {
        ctx.requestWorldSave();
        setStatus("world save requested");
    }
    ImGui::SameLine();
    if (ImGui::Button("New Map...")) {
        m_newMapOpen = true;
        m_newMapGenre = ctx.currentGameTemplate;
        m_newMapTerrain = static_cast<int>(ctx.currentTerrain);
        m_newMapEnvironment = static_cast<int>(ctx.currentEnvironment);
        m_newMapSeed = static_cast<int>(ctx.currentSeed);
    }
    ImGui::SameLine();
    if (ImGui::Button(m_nodeGraphOpen ? "Node Graph##hide" : "Node Graph##show"))
        m_nodeGraphOpen = !m_nodeGraphOpen;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("C6 visual scripting — node graph compiles to Lua (not UE Blueprints)");
    ImGui::SameLine();
    if (ImGui::Button(m_outputLogOpen ? "Output Log##hide" : "Output Log##show"))
        m_outputLogOpen = !m_outputLogOpen;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("C9 — engine / script / graph messages (warnings & errors)");
    ImGui::SameLine();
    if (ImGui::Button(m_detailsOpen ? "Details##hide" : "Details##show"))
        m_detailsOpen = !m_detailsOpen;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("C5 — world settings + selection inspector");
    if (ctx.setHemisphereAmbient) {
        bool hemi = ctx.hemisphereAmbient;
        if (ImGui::Checkbox("hemisphere ambient", &hemi)) ctx.setHemisphereAmbient(hemi);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A3 sky/ground fill (not gated by torches). F7 also toggles.");
    }
    ImGui::TextDisabled("RMB fly (WASD/QE, wheel = speed) | LMB apply | Esc cancel | F1 exit");
    if (!m_status.empty())
        ImGui::TextColored({1.0f, 0.85f, 0.3f, 1.0f}, "%s", m_status.c_str());
    ImGui::End();

    if (m_newMapOpen) drawNewMapDialog(ctx);
}

void RoomEditor::drawNewMapDialog(EditorContext& ctx) {
    ImGui::OpenPopup("New Map");
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f},
                            ImGuiCond_Appearing, {0.5f, 0.5f});
    if (!ImGui::BeginPopupModal("New Map", &m_newMapOpen, ImGuiWindowFlags_AlwaysAutoResize)) {
        m_newMapOpen = false;
        return;
    }

    ImGui::TextUnformatted("Create a fresh world (host / single-player).");
    ImGui::Separator();

    ImGui::TextUnformatted("Game template");
    if (ImGui::RadioButton("FPS (on-foot)", &m_newMapGenre, 0)) {
        /* keep terrain/env as chosen */
    }
    if (ImGui::RadioButton("TPS (third-person)", &m_newMapGenre, 1)) {
        /* camera only — terrain free */
    }
    if (ImGui::RadioButton("Space ship", &m_newMapGenre, 2)) {
        m_newMapTerrain = 2;     // Void
        m_newMapEnvironment = 2; // Space
    }
    if (ImGui::RadioButton("Racer", &m_newMapGenre, 3)) {
        m_newMapTerrain = 1;     // Superflat
        m_newMapEnvironment = 0; // Surface
    }
    if (m_newMapGenre == 2)
        ImGui::TextDisabled("Space presets Void + Space env + ships.");
    if (m_newMapGenre == 3)
        ImGui::TextDisabled("Racer presets Superflat + surface car (E board, chase cam).");

    ImGui::Separator();
    ImGui::TextUnformatted("World template");
    ImGui::RadioButton("Landscape (Normal)", &m_newMapTerrain, 0);
    ImGui::RadioButton("Superflat", &m_newMapTerrain, 1);
    ImGui::RadioButton("Blank (Void)", &m_newMapTerrain, 2);

    ImGui::Separator();
    ImGui::TextUnformatted("Environment");
    ImGui::RadioButton("Surface", &m_newMapEnvironment, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Underwater", &m_newMapEnvironment, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Space", &m_newMapEnvironment, 2);

    ImGui::Separator();
    ImGui::InputInt("Seed", &m_newMapSeed);
    if (ImGui::Button("Randomize seed")) m_newMapSeed = static_cast<int>(std::rand());

    ImGui::Separator();
    ImGui::TextDisabled("Clears voxel edits and world props. Players respawn on the pad.");

    if (ImGui::Button("Create", {120, 0})) {
        const auto seed = static_cast<std::uint32_t>(m_newMapSeed < 0 ? 0 : m_newMapSeed);
        // Ordinals match GameRules enums (engine stays game-free).
        if (ctx.requestNewMap &&
            ctx.requestNewMap(m_newMapTerrain, m_newMapEnvironment, m_newMapGenre, seed)) {
            setStatus("New Map created");
            m_newMapOpen = false;
            ImGui::CloseCurrentPopup();
        } else {
            setStatus("New Map failed (host/SP only)");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", {120, 0})) {
        m_newMapOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void RoomEditor::drawToolbar() {
    ImGui::Begin("Tools", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    int tool = static_cast<int>(m_tool);
    for (int i = 0; i < static_cast<int>(std::size(kToolNames)); ++i)
        ImGui::RadioButton(kToolNames[i], &tool, i);
    if (tool != static_cast<int>(m_tool)) {
        m_tool = static_cast<Tool>(tool);
        m_anchor.reset();
    }

    ImGui::Separator();
    if (m_tool == Tool::Wall)
        ImGui::SliderInt("height (vox)", &m_wallHeight, 1, 16);
    if (m_tool == Tool::Platform)
        ImGui::SliderInt("offset (vox)", &m_platformOffset, 1, 32);
    if (m_tool == Tool::SeedVolume) {
        int seed = static_cast<int>(m_nextSeed);
        if (ImGui::InputInt("seed", &seed)) m_nextSeed = static_cast<std::uint32_t>(seed);
    }

    ImGui::Text("snap");
    ImGui::SameLine();
    ImGui::RadioButton("off", &m_snap, 1);
    ImGui::SameLine();
    ImGui::RadioButton("2", &m_snap, 2);
    ImGui::SameLine();
    ImGui::RadioButton("4", &m_snap, 4);

    if (m_anchor)
        ImGui::Text("anchor %d %d %d", m_anchor->x, m_anchor->y, m_anchor->z);
    ImGui::End();
}

void RoomEditor::drawOutliner(EditorContext& ctx) {
    ImGui::Begin("Outliner", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    for (int i = 0; i < static_cast<int>(ctx.lights.size()); ++i) {
        const EditorLight& light = ctx.lights[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        char label[32];
        std::snprintf(label, sizeof(label), "light %d (%s)", i,
                      light.type == 0 ? "point" : "spot");
        if (ImGui::Selectable(label, m_selKind == Selection::Light && m_selIndex == i)) {
            m_selKind = Selection::Light;
            m_selIndex = i;
        }
        ImGui::PopID();
    }
    for (int i = 0; i < static_cast<int>(ctx.seedVolumes.size()); ++i) {
        const SeedVolume& sv = ctx.seedVolumes[static_cast<std::size_t>(i)];
        ImGui::PushID(1000 + i);
        char label[40];
        std::snprintf(label, sizeof(label), "volume %d [seed %u]", i, sv.seed);
        if (ImGui::Selectable(label, m_selKind == Selection::Volume && m_selIndex == i)) {
            m_selKind = Selection::Volume;
            m_selIndex = i;
        }
        ImGui::PopID();
    }
    for (int i = 0; i < static_cast<int>(ctx.props.size()); ++i) {
        const EditorProp& p = ctx.props[static_cast<std::size_t>(i)];
        ImGui::PushID(2000 + i);
        const std::size_t slash = p.assetPath.find_last_of('/');
        const std::string leaf =
            slash == std::string::npos ? p.assetPath : p.assetPath.substr(slash + 1);
        char label[80];
        std::snprintf(label, sizeof(label), "prop %d (%s)", i, leaf.c_str());
        if (ImGui::Selectable(label, m_selKind == Selection::Prop && m_selIndex == i)) {
            m_selKind = Selection::Prop;
            m_selIndex = i;
        }
        ImGui::PopID();
    }
    if (ctx.lights.empty() && ctx.seedVolumes.empty() && ctx.props.empty())
        ImGui::TextDisabled("(empty)");
    ImGui::TextDisabled("Selection properties → Details panel");
    ImGui::End();
}

void RoomEditor::drawGizmo(EditorContext& ctx, const glm::mat4& view, const glm::mat4& proj) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetRect(vp->Pos.x, vp->Pos.y, vp->Size.x, vp->Size.y);

    if (m_selKind == Selection::Light && m_selIndex >= 0 &&
        m_selIndex < static_cast<int>(ctx.lights.size())) {
        // Lights are a point: translate only.
        EditorLight& light = ctx.lights[static_cast<std::size_t>(m_selIndex)];
        glm::mat4 model = glm::translate(glm::mat4(1.0f), light.pos);
        if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), ImGuizmo::TRANSLATE,
                                 ImGuizmo::WORLD, glm::value_ptr(model)))
            light.pos = glm::vec3(model[3]);
    } else if (m_selKind == Selection::Prop && m_selIndex >= 0 &&
               m_selIndex < static_cast<int>(ctx.props.size())) {
        // Props carry a full TRS: move/rotate/scale, manipulated in place. Scale
        // reads more predictably in local space; move/rotate use world axes.
        // Commit to the server on gizmo release so the collider + peers update.
        EditorProp& prop = ctx.props[static_cast<std::size_t>(m_selIndex)];
        const ImGuizmo::OPERATION op = m_propGizmoOp == 1   ? ImGuizmo::ROTATE
                                       : m_propGizmoOp == 2 ? ImGuizmo::SCALE
                                                            : ImGuizmo::TRANSLATE;
        const ImGuizmo::MODE mode = op == ImGuizmo::SCALE ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
        if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), op, mode,
                                 glm::value_ptr(prop.transform))) {
            m_propGizmoDirty = true;
        }
        if (m_propGizmoDirty && !ImGuizmo::IsUsing() && prop.id != 0 && ctx.requestMoveProp) {
            ctx.requestMoveProp(prop.id, prop.transform);
            m_propGizmoDirty = false;
        }
    }
}

void RoomEditor::handleTool(EditorContext& ctx, glm::vec3 rayOrigin, glm::vec3 rayDir) {
    const auto hit = ctx.voxels.raycast(rayOrigin, rayDir, kPickDistance);
    if (!hit) return;
    const bool click = worldClickable(m_flying);

    switch (m_tool) {
    case Tool::Place: {
        const glm::ivec3 target = hit->voxel + hit->normal;
        submitPreviewLight(ctx, voxelCenter(target), kBuildPreview, 2.0f);
        if (click) ctx.requestVoxelOp(target, ctx.buildBlock);
        break;
    }
    case Tool::Erase: {
        submitPreviewLight(ctx, voxelCenter(hit->voxel), kErasePreview, 2.0f);
        if (click) ctx.requestVoxelOp(hit->voxel, 0);
        break;
    }
    case Tool::Wall:
    case Tool::Floor:
    case Tool::Platform: {
        const glm::ivec3 target = snapVoxel(hit->voxel + hit->normal);
        if (!m_anchor) {
            submitPreviewLight(ctx, voxelCenter(target), kBuildPreview, 2.0f);
            if (click) m_anchor = target;
        } else {
            glm::ivec3 lo, hi;
            if (toolBox(*m_anchor, target, lo, hi)) {
                previewBox(ctx, lo, hi, kBuildPreview, true);
                if (click) {
                    fillBox(ctx, lo, hi, ctx.buildBlock);
                    m_anchor.reset();
                }
            }
        }
        break;
    }
    case Tool::Doorway: {
        submitPreviewLight(ctx, voxelCenter(hit->voxel), kErasePreview, 2.0f);
        if (click) carveDoorway(ctx, hit->voxel, hit->normal);
        break;
    }
    case Tool::Light: {
        const glm::vec3 point =
            rayOrigin + rayDir * hit->t + glm::vec3(hit->normal) * 0.25f;
        submitPreviewLight(ctx, point, kBuildPreview, 2.0f);
        if (click) {
            EditorLight light;
            light.pos = point;
            light.color = {1.0f, 0.9f, 0.75f}; // warm white
            light.radius = 8.0f;
            light.dir = ctx.camera.forward(); // MVP spot dir = placing view direction
            ctx.lights.push_back(light);
            m_selKind = Selection::Light;
            m_selIndex = static_cast<int>(ctx.lights.size()) - 1;
        }
        break;
    }
    case Tool::SeedVolume: {
        const glm::ivec3 target = snapVoxel(hit->voxel + hit->normal);
        if (!m_anchor) {
            submitPreviewLight(ctx, voxelCenter(target), kVolumePreview, 2.0f);
            if (click) m_anchor = target;
        } else {
            const glm::ivec3 lo = glm::min(*m_anchor, target);
            const glm::ivec3 hi = glm::max(*m_anchor, target);
            previewBox(ctx, lo, hi, kVolumePreview, true);
            if (click) {
                ctx.seedVolumes.push_back(SeedVolume{lo, hi, m_nextSeed});
                m_selKind = Selection::Volume;
                m_selIndex = static_cast<int>(ctx.seedVolumes.size()) - 1;
                m_anchor.reset();
            }
        }
        break;
    }
    }
}

// Region between two anchors for the box brushes. Walls collapse to 1 voxel
// thick along whichever horizontal axis moved least.
bool RoomEditor::toolBox(glm::ivec3 a, glm::ivec3 b, glm::ivec3& lo, glm::ivec3& hi) const {
    switch (m_tool) {
    case Tool::Wall: {
        const int y0 = std::min(a.y, b.y);
        if (std::abs(b.x - a.x) >= std::abs(b.z - a.z)) {
            lo = {std::min(a.x, b.x), y0, a.z};
            hi = {std::max(a.x, b.x), y0 + m_wallHeight - 1, a.z};
        } else {
            lo = {a.x, y0, std::min(a.z, b.z)};
            hi = {a.x, y0 + m_wallHeight - 1, std::max(a.z, b.z)};
        }
        return true;
    }
    case Tool::Floor:
    case Tool::Platform: {
        const int y = a.y + (m_tool == Tool::Platform ? m_platformOffset : 0);
        lo = {std::min(a.x, b.x), y, std::min(a.z, b.z)};
        hi = {std::max(a.x, b.x), y, std::max(a.z, b.z)};
        return true;
    }
    default:
        return false;
    }
}

void RoomEditor::fillBox(EditorContext& ctx, glm::ivec3 lo, glm::ivec3 hi, BlockId block) {
    const glm::ivec3 d = hi - lo + 1;
    const long long count =
        static_cast<long long>(d.x) * static_cast<long long>(d.y) * static_cast<long long>(d.z);
    if (count > kMaxBatchOps) {
        setStatus("region too large (" + std::to_string(count) + " voxels, cap " +
                  std::to_string(kMaxBatchOps) + ")");
        return;
    }
    for (int y = lo.y; y <= hi.y; ++y)
        for (int z = lo.z; z <= hi.z; ++z)
            for (int x = lo.x; x <= hi.x; ++x)
                ctx.requestVoxelOp({x, y, z}, block);
}

void RoomEditor::carveDoorway(EditorContext& ctx, glm::ivec3 voxel, glm::ivec3 normal) {
    // 2 wide x 4 high (1 m x 2 m), depth 1 along the face normal's axis. An even
    // width can't center exactly on a voxel: the clicked column is the left/lower
    // of the pair; vertically the click sits one voxel above the opening's base.
    const glm::ivec3 wAxis = (normal.x != 0) ? glm::ivec3(0, 0, 1) : glm::ivec3(1, 0, 0);
    for (int h = -1; h <= 2; ++h)
        for (int w = 0; w <= 1; ++w)
            ctx.requestVoxelOp(voxel + wAxis * w + glm::ivec3(0, h, 0), 0);
}

glm::ivec3 RoomEditor::snapVoxel(glm::ivec3 v) const {
    if (m_snap <= 1) return v;
    return {snapDown(v.x, m_snap), snapDown(v.y, m_snap), snapDown(v.z, m_snap)};
}

void RoomEditor::submitPreviewLight(EditorContext& ctx, glm::vec3 pos, glm::vec3 color,
                                    float radius) {
    if (m_previewLights >= kMaxPreviewLights) return;
    ++m_previewLights;
    ctx.renderer.submitPointLight(pos, color, radius);
}

void RoomEditor::previewBox(EditorContext& ctx, glm::ivec3 lo, glm::ivec3 hi, glm::vec3 color,
                            bool withCenter) {
    const glm::vec3 wLo = glm::vec3(lo) * kVoxelSize;
    const glm::vec3 wHi = glm::vec3(hi + 1) * kVoxelSize;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner{(i & 1) ? wHi.x : wLo.x, (i & 2) ? wHi.y : wLo.y,
                               (i & 4) ? wHi.z : wLo.z};
        submitPreviewLight(ctx, corner, color, 2.0f);
    }
    if (withCenter) submitPreviewLight(ctx, (wLo + wHi) * 0.5f, color, 2.0f);
}

void RoomEditor::setStatus(std::string text) {
    m_status = std::move(text);
    m_statusTtl = 4.0f;
}

// ===== IDE panels ==========================================================

// Cached listing: fetch from ctx.listFiles once per dir, then serve from the
// cache until Refresh clears it — so an open tree doesn't re-list every frame.
const std::vector<std::string>& RoomEditor::listDir(EditorContext& ctx,
                                                    const std::string& dir) {
    auto it = m_dirCache.find(dir);
    if (it == m_dirCache.end()) {
        std::vector<std::string> entries;
        if (ctx.listFiles) entries = ctx.listFiles(dir);
        it = m_dirCache.emplace(dir, std::move(entries)).first;
    }
    return it->second;
}

void RoomEditor::drawDirTree(EditorContext& ctx, const std::string& dir) {
    // listDir is cached, so recursing here only calls ctx.listFiles the first
    // time a node is expanded (or after Refresh) — not every frame.
    for (const std::string& name : listDir(ctx, dir)) {
        if (name.empty()) continue;
        const bool isDir = name.back() == '/';
        if (isDir) {
            const std::string leaf = name.substr(0, name.size() - 1);
            const std::string child = dir + "/" + leaf;
            // str_id = full path (unique), label = leaf name.
            if (ImGui::TreeNode(child.c_str(), "%s", leaf.c_str())) {
                drawDirTree(ctx, child);
                ImGui::TreePop();
            }
            continue;
        }

        // File leaf. Show its extension as the type tag. TODO: preview textures/
        // models/shaders in-panel (thumbnail via TextureHandle / SceneCapture);
        // for the MVP we only list non-script files by extension.
        const std::string path = dir + "/" + name;
        const char* dot = std::strrchr(name.c_str(), '.');
        const char* ext = dot ? dot + 1 : "?";
        const bool isLua = dot && std::strcmp(dot, ".lua") == 0;

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf |
                                   ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                   ImGuiTreeNodeFlags_SpanAvailWidth;
        if (m_selectedAsset == path) flags |= ImGuiTreeNodeFlags_Selected;
        ImGui::TreeNodeEx(path.c_str(), flags, "%s  [%s]", name.c_str(), ext);
        if (ImGui::IsItemClicked()) m_selectedAsset = path;
        // Double-click a .lua to open it; a selected .lua also gets an "open"
        // button so the action is discoverable without a double-click.
        if (isLua && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
            openLuaFile(ctx, path);
        if (isLua && m_selectedAsset == path) {
            ImGui::SameLine();
            ImGui::PushID(path.c_str());
            if (ImGui::SmallButton("open")) openLuaFile(ctx, path);
            ImGui::PopID();
        }
    }
}

void RoomEditor::drawAssetBrowser(EditorContext& ctx) {
    ImGui::Begin("Assets", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    if (ImGui::Button("Refresh")) m_dirCache.clear(); // drop cache → re-list on expand
    ImGui::SameLine();
    if (m_selectedAsset.empty())
        ImGui::TextDisabled("(no selection)");
    else
        ImGui::Text("sel: %s", m_selectedAsset.c_str());
    ImGui::TextDisabled("double-click a .lua to edit it below");

    // Import row: paste/type a source path + Import, or drag a file onto the
    // window (handled in update). MVP has no native file-open dialog.
    ImGui::SetNextItemWidth(320.0f);
    const bool entered = ImGui::InputText("##importpath", m_importPath, sizeof(m_importPath),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Import...") || entered) doImport(ctx, m_importPath);
    ImGui::TextDisabled("paste a path to an .fbx/.obj/.glb/.png/.jpg (or drag a file onto the window)");
    if (!m_importStatus.empty()) {
        const bool ok = m_importStatus.rfind("imported", 0) == 0;
        ImGui::TextColored(ok ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f) : ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                           "%s", m_importStatus.c_str());
    }
    ImGui::Separator();

    if (!ctx.listFiles) {
        ImGui::TextDisabled("(file listing unavailable)");
        ImGui::End();
        return;
    }
    // Rooted at "assets"; listFiles surfaces scripts/models/textures/shaders as
    // subdirs (trailing "/"), which expand on demand via drawDirTree.
    drawDirTree(ctx, "assets");
    ImGui::End();
}

// Route a source path through ctx.importAsset (the engine validates + copies) and
// surface the result. A successful import invalidates the dir cache so the tree
// re-lists and the new file appears without a manual Refresh.
void RoomEditor::doImport(EditorContext& ctx, const std::string& sourcePath) {
    if (sourcePath.empty()) return;
    if (!ctx.importAsset) {
        setImportStatus("import unavailable");
        log::error("import: importAsset callback unavailable");
        m_outputLogOpen = true;
        return;
    }
    const std::string result = ctx.importAsset(sourcePath);
    if (result.empty()) return; // not attempted
    setImportStatus(result);
    if (result.rfind("imported", 0) == 0) {
        m_dirCache.clear();
        m_contentScanned = false;
        log::info("import: {}", result);
    } else {
        log::warn("import: {}", result);
        m_outputLogOpen = true;
    }
}

void RoomEditor::setImportStatus(std::string text) {
    m_importStatus = std::move(text);
    m_importStatusTtl = 6.0f;
}

void RoomEditor::openLuaFile(EditorContext& ctx, const std::string& path) {
    if (!ctx.readFile) {
        setCodeStatus("readFile unavailable");
        return;
    }
    m_codeText = ctx.readFile(path);
    m_codePath = path;
    m_codeDirty = false;
    setCodeStatus("opened " + path);
}

// Grows m_codeText as the user types past its current capacity. UserData is the
// std::string itself; keep data->Buf pointed at its storage after the resize.
int RoomEditor::codeResizeCb(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* str = static_cast<std::string*>(data->UserData);
        str->resize(static_cast<std::size_t>(data->BufTextLen));
        data->Buf = str->data();
    }
    return 0;
}

void RoomEditor::drawCodeEditor(EditorContext& ctx) {
    ImGui::Begin("Code");
    if (m_codePath.empty()) {
        ImGui::TextDisabled("Open a .lua file from the Assets panel.");
        ImGui::End();
        return;
    }

    // Header shows the open file; '*' marks unsaved edits.
    ImGui::Text("%s%s", m_codePath.c_str(), m_codeDirty ? " *" : "");

    // Saving a script under scripts/ hot-reloads it into the running server so
    // gameplay changes apply live: writeFile → (if under scripts/) reloadScripts.
    const bool underScripts = m_codePath.find("scripts/") != std::string::npos;
    if (ImGui::Button("Save")) {
        if (ctx.writeFile && ctx.writeFile(m_codePath, m_codeText)) {
            m_codeDirty = false;
            if (underScripts && ctx.reloadScripts) {
                const bool ok = ctx.reloadScripts();
                setCodeStatus(ok ? "saved + reloaded" : "saved (reload FAILED)");
            } else {
                setCodeStatus("saved");
            }
        } else {
            setCodeStatus("save FAILED");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload from disk")) { // re-read, discarding local edits
        if (ctx.readFile) {
            m_codeText = ctx.readFile(m_codePath);
            m_codeDirty = false;
            setCodeStatus("reloaded from disk");
        } else {
            setCodeStatus("readFile unavailable");
        }
    }
    if (!m_codeStatus.empty()) {
        ImGui::SameLine();
        ImGui::TextColored({0.4f, 0.9f, 0.4f, 1.0f}, "%s", m_codeStatus.c_str());
    }

    // Plain editable text — no syntax highlighting for the MVP.
    // TODO: swap for ImGuiColorTextEdit (MIT, ImGui-native) to get Lua colouring.
    const ImGuiInputTextFlags flags =
        ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize;
    if (ImGui::InputTextMultiline("##code", m_codeText.data(), m_codeText.capacity() + 1,
                                  ImVec2(-FLT_MIN, -FLT_MIN), flags,
                                  &RoomEditor::codeResizeCb, &m_codeText)) {
        m_codeDirty = true;
    }
    ImGui::End();
}

void RoomEditor::setCodeStatus(std::string text) {
    m_codeStatus = std::move(text);
    m_codeStatusTtl = 4.0f;
}

// ===== Content browser =====================================================

RoomEditor::AssetKind RoomEditor::classifyExt(const std::string& ext) {
    if (ext == ".fbx" || ext == ".obj" || ext == ".glb" || ext == ".gltf")
        return AssetKind::Model;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp")
        return AssetKind::Texture;
    if (ext == ".lua") return AssetKind::Script;
    if (ext == ".vert" || ext == ".frag" || ext == ".comp" || ext == ".glsl")
        return AssetKind::Shader;
    return AssetKind::Other;
}

// Walk assets/ once and cache a flat, type-sorted listing with per-file sizes.
// The editor context doesn't surface --project's dir, so the scan is rooted at
// the runtime assets/ dir — the same root the Assets tree lists. All filesystem
// work is error_code-based so a missing/locked dir can't throw across the UI.
void RoomEditor::rescanContent() {
    namespace fs = std::filesystem;
    m_content.clear();
    m_contentScanned = true;

    std::error_code ec;
    const fs::path root = "assets";
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) return;

    const fs::recursive_directory_iterator end;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code fe;
        if (!it->is_regular_file(fe) || fe) continue;

        ContentEntry entry;
        entry.name = it->path().filename().string();
        entry.path = fs::relative(it->path(), root, fe).generic_string();
        if (fe || entry.path.empty()) entry.path = it->path().generic_string();
        entry.size = it->file_size(fe);
        if (fe) entry.size = 0;
        entry.kind = classifyExt(toLowerAscii(it->path().extension().string()));
        m_content.push_back(std::move(entry));
    }

    std::sort(m_content.begin(), m_content.end(),
              [](const ContentEntry& a, const ContentEntry& b) {
                  if (a.kind != b.kind) return a.kind < b.kind;
                  return a.name < b.name;
              });
}

void RoomEditor::drawContentBrowser(EditorContext& ctx) {
    // Open roomy and clear of the other default-cascaded panels so the tile grid
    // has space; the dev can still move/resize it (FirstUseEver only seeds it).
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({vp->Pos.x + vp->Size.x - 452.0f, vp->Pos.y + 40.0f},
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({440.0f, 480.0f}, ImGuiCond_FirstUseEver);
    ImGui::Begin("Content Browser");
    if (!m_contentScanned) rescanContent(); // first open: scan once, not per frame

    if (ImGui::Button("Refresh")) rescanContent();
    ImGui::SameLine();
    ImGui::Text("%d assets under assets/", static_cast<int>(m_content.size()));

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##contentfilter", "filter by name...", m_contentFilter,
                             sizeof(m_contentFilter));
    const std::string needle = toLowerAscii(m_contentFilter);
    const auto matches = [&](const std::string& name) {
        return needle.empty() || toLowerAscii(name).find(needle) != std::string::npos;
    };

    // Breadcrumb: "assets" root plus each segment of the current sub-folder, each
    // clickable to jump back up. The scan is flat + recursive; m_contentDir just
    // scopes which tiles show. Guard against a stale dir the last scan dropped.
    const std::string prefix = m_contentDir.empty() ? std::string() : m_contentDir + "/";
    ImGui::Separator();
    if (ImGui::SmallButton("assets")) m_contentDir.clear();
    {
        std::string acc;
        std::size_t start = 0;
        while (start <= m_contentDir.size()) {
            const std::size_t slash = m_contentDir.find('/', start);
            const std::size_t end = slash == std::string::npos ? m_contentDir.size() : slash;
            if (end == start) break; // trailing/empty segment
            const std::string seg = m_contentDir.substr(start, end - start);
            acc = acc.empty() ? seg : acc + "/" + seg;
            ImGui::SameLine();
            ImGui::TextDisabled("/");
            ImGui::SameLine();
            ImGui::PushID(static_cast<int>(start));
            if (ImGui::SmallButton(seg.c_str())) m_contentDir = acc;
            ImGui::PopID();
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
    }

    // Split the flat scan into this folder's immediate sub-folders + files. A path
    // like "anim_packs/Pack/idle.fbx" under dir "anim_packs" contributes folder
    // "Pack"; with no further slash it is a file that lives in the current dir.
    std::vector<std::string> folders;      // immediate child folder leaf names
    std::vector<const ContentEntry*> files; // files directly in m_contentDir
    for (const ContentEntry& e : m_content) {
        if (e.path.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string rest = e.path.substr(prefix.size());
        const std::size_t slash = rest.find('/');
        if (slash == std::string::npos) {
            if (matches(e.name)) files.push_back(&e);
        } else {
            const std::string leaf = rest.substr(0, slash);
            if (std::find(folders.begin(), folders.end(), leaf) == folders.end())
                folders.push_back(leaf);
        }
    }
    std::sort(folders.begin(), folders.end());

    // Tile grid: folders first, then files (already kind-then-name sorted by the
    // scan). Each tile is a colour-coded placeholder square + truncated caption;
    // a per-row counter wraps to the panel width. Real rendered previews are out
    // of scope — the square's colour + tag stands in for a thumbnail.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float tile = 84.0f;
    const float availW = ImGui::GetContentRegionAvail().x;
    const int perRow =
        std::max(1, static_cast<int>((availW + style.ItemSpacing.x) / (tile + style.ItemSpacing.x)));

    ImGui::BeginChild("##contentgrid", ImVec2(0.0f, 300.0f), ImGuiChildFlags_Borders);
    int cell = 0;
    const auto nextCell = [&]() {
        if (++cell % perRow != 0) ImGui::SameLine();
    };

    for (const std::string& leaf : folders) {
        ImGui::PushID(("dir/" + leaf).c_str());
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Button, kFolderTileColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(kFolderTileColor.x + 0.12f, kFolderTileColor.y + 0.12f,
                                     kFolderTileColor.z + 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kFolderTileColor);
        const bool enter = ImGui::Button("DIR", ImVec2(tile, tile));
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s  (folder)", leaf.c_str());
        if (enter) m_contentDir = prefix + leaf;
        ImGui::TextUnformatted(fitLabel(leaf, tile).c_str());
        ImGui::EndGroup();
        ImGui::PopID();
        nextCell();
    }

    for (const ContentEntry* e : files) {
        ImGui::PushID(e->path.c_str());
        const int k = static_cast<int>(e->kind);
        const bool selected = m_contentSelected == e->path;
        ImVec4 col = kindTileColor(k);
        if (selected) { // brighten the selected tile so it reads as active
            col.x = std::min(1.0f, col.x + 0.18f);
            col.y = std::min(1.0f, col.y + 0.18f);
            col.z = std::min(1.0f, col.z + 0.18f);
        }
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Button, col);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(std::min(1.0f, col.x + 0.12f), std::min(1.0f, col.y + 0.12f),
                                     std::min(1.0f, col.z + 0.12f), 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, col);
        const bool clicked = ImGui::Button(kAssetKindTags[k], ImVec2(tile, tile));
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s\n%s  |  %s", e->name.c_str(),
                              kAssetKindNames[k], humanSize(e->size).c_str());
            // Double-click a model tile to drop it into the world (mirrors the
            // Place button + the Assets panel's double-click-to-open convention).
            if (e->kind == AssetKind::Model && ImGui::IsMouseDoubleClicked(0)) {
                m_contentSelected = e->path;
                placeSelectedProp(ctx);
            }
        }
        if (clicked) m_contentSelected = e->path;
        ImGui::TextUnformatted(fitLabel(e->name, tile).c_str());
        ImGui::EndGroup();
        ImGui::PopID();
        nextCell();
    }

    if (folders.empty() && files.empty())
        ImGui::TextDisabled(m_content.empty() ? "(no assets under assets/)" : "(empty folder)");
    ImGui::EndChild();

    // Details for the current selection.
    const ContentEntry* sel = nullptr;
    for (const ContentEntry& e : m_content)
        if (e.path == m_contentSelected) {
            sel = &e;
            break;
        }
    ImGui::Separator();
    if (sel) {
        ImGui::Text("%s", sel->name.c_str());
        ImGui::TextDisabled("assets/%s", sel->path.c_str());
        ImGui::TextDisabled("%s  |  %s", kAssetKindNames[static_cast<int>(sel->kind)],
                            humanSize(sel->size).c_str());
        // A model can be dropped into the world: it spawns an EditorProp where the
        // camera is looking (raycast pick), then gets a transform gizmo + Outliner
        // entry. Meshes only — textures/scripts/shaders aren't placeable.
        if (sel->kind == AssetKind::Model) {
            if (ImGui::Button("Place in world")) placeSelectedProp(ctx);
            ImGui::SameLine();
            ImGui::TextDisabled("drops where the camera looks");
        }
    } else {
        ImGui::TextDisabled("(no selection)");
    }
    ImGui::End();
}

void RoomEditor::placeSelectedProp(EditorContext& ctx) {
    if (m_contentSelected.empty()) return;
    // Content paths are relative to the assets/ scan root; props store a
    // project-relative path, which is what the engine's loader expects.
    const std::string assetPath = "assets/" + m_contentSelected;

    // Raycast the camera forward ray against the voxel world and seat the prop on
    // the hit surface; with nothing in view, drop it a few metres ahead so the
    // action never silently no-ops.
    const glm::vec3 origin = ctx.camera.pos;
    const glm::vec3 dir = ctx.camera.forward();
    const auto hit = ctx.voxels.raycast(origin, dir, kPickDistance);
    const glm::vec3 point = hit ? origin + dir * hit->t : origin + dir * 5.0f;

    // Server intent: the prop becomes a shared, collidable, saved world object and
    // comes back via PropAddedMsg into ctx.props (with a real id + collider) — no
    // local push. Selection follows once it lands (the Outliner lists synced props).
    if (ctx.requestPlaceProp)
        ctx.requestPlaceProp(assetPath, glm::translate(glm::mat4(1.0f), point));
    setStatus("placed " + assetPath);
}

// ===== Node Graph (C6 visual scripting — not "Blueprints"; UE trademark) ====

namespace {
constexpr const char* kGraphPath = "scripts/graphs/main.graph.json";
// Legacy path still accepted on load if the new one is missing.
constexpr const char* kGraphPathLegacy = "scripts/blueprints/main.graph.json";
// Load last alphabetically so hand-written example.lua does not stomp it.
constexpr const char* kEmitLuaPath = "scripts/zz_nodegraph.lua";
constexpr const char* kEmitLuaPathLegacy = "scripts/zz_blueprint.lua";

const NodeKind kPaletteKinds[] = {
    NodeKind::EventOnInit,       NodeKind::EventOnTick,       NodeKind::EventOnPlayerJoin,
    NodeKind::EventOnPlayerDeath, NodeKind::ActionLog,        NodeKind::ActionSetBlock,
    NodeKind::ActionSpawnPickup, NodeKind::GetPlayerCount,    NodeKind::GetItemId,
    NodeKind::Randi,             NodeKind::ConstInt,          NodeKind::ConstFloat,
    NodeKind::ConstString,       NodeKind::Branch,            NodeKind::Sequence,
    NodeKind::MathAdd,           NodeKind::MathSubtract,      NodeKind::MathMultiply,
    NodeKind::MathGreater,       NodeKind::MathEqual,         NodeKind::GetWorldObject,
    NodeKind::GetPropPosition,   NodeKind::HighlightObject,   NodeKind::PrintObject,
    NodeKind::GetBlock,
};
constexpr int kPaletteCount = static_cast<int>(sizeof(kPaletteKinds) / sizeof(kPaletteKinds[0]));

// Node title bar hues by category (packed ImU32).
ImU32 categoryTitleColor(const char* cat) {
    if (std::strcmp(cat, "Event") == 0) return IM_COL32(160, 28, 28, 255);   // red
    if (std::strcmp(cat, "Action") == 0) return IM_COL32(28, 72, 140, 255);  // blue function
    if (std::strcmp(cat, "Flow") == 0) return IM_COL32(120, 90, 20, 255);    // gold
    if (std::strcmp(cat, "Math") == 0) return IM_COL32(28, 100, 72, 255);    // pure green
    if (std::strcmp(cat, "Object") == 0) return IM_COL32(90, 40, 120, 255);  // purple actor
    return IM_COL32(36, 90, 70, 255); // Data / pure
}

ImU32 pinColor(PinKind k) {
    switch (k) {
    case PinKind::Exec: return IM_COL32(240, 240, 240, 255); // white exec
    case PinKind::Int: return IM_COL32(70, 200, 255, 255);   // cyan
    case PinKind::Float: return IM_COL32(90, 220, 130, 255);
    case PinKind::String: return IM_COL32(255, 80, 180, 255);
    case PinKind::Bool: return IM_COL32(220, 60, 60, 255);
    case PinKind::Object: return IM_COL32(90, 160, 255, 255); // soft object blue
    }
    return IM_COL32(200, 200, 200, 255);
}

bool paletteMatchesSearch(NodeKind kind, const char* search) {
    if (!search || search[0] == '\0') return true;
    std::string q = search;
    for (char& c : q) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto has = [&](const char* s) {
        std::string t = s;
        for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return t.find(q) != std::string::npos;
    };
    return has(nodeKindName(kind)) || has(nodeKindCategory(kind));
}
} // namespace

void RoomEditor::ensureNodeGraphContext() {
    if (m_imnodes) return;
    ImNodes::SetImGuiContext(ImGui::GetCurrentContext());
    m_imnodes = ImNodes::CreateContext();
    applyNodeGraphStyle();
    ImNodes::GetIO().LinkDetachWithModifierClick.Modifier = &ImGui::GetIO().KeyCtrl;
    ImNodes::GetIO().EmulateThreeButtonMouse.Modifier = &ImGui::GetIO().KeyAlt;
}

void RoomEditor::loadOrSeedNodeGraph(EditorContext& ctx) {
    if (m_nodeGraphLoaded) return;
    m_nodeGraphLoaded = true;
    bool ok = false;
    std::string loadedFrom;
    if (ctx.readFile) {
        std::string text = ctx.readFile(kGraphPath);
        if (!text.empty()) {
            ok = loadGraphJson(m_nodeGraph, text);
            if (ok) loadedFrom = kGraphPath;
        }
        if (!ok) {
            // Pre-rename projects used scripts/blueprints/ (avoid "Blueprints" branding).
            text = ctx.readFile(kGraphPathLegacy);
            if (!text.empty()) {
                ok = loadGraphJson(m_nodeGraph, text);
                if (ok) loadedFrom = kGraphPathLegacy;
            }
        }
    }
    m_graphPlacedIds.clear();
    if (!ok) {
        m_nodeGraph = NodeGraph::makeExample();
        m_nodeGraphDirty = true;
        m_graphStatus = "seeded example node graph";
        m_graphStatusTtl = 4.0f;
    } else {
        m_graphStatus = "loaded " + loadedFrom;
        m_graphStatusTtl = 3.0f;
        if (loadedFrom == kGraphPathLegacy) m_nodeGraphDirty = true; // re-save under new path
    }
    (void)kEmitLuaPathLegacy;
}

bool RoomEditor::saveAndCompileNodeGraph(EditorContext& ctx) {
    if (!ctx.writeFile) {
        m_graphStatus = "writeFile unavailable";
        m_graphStatusTtl = 4.0f;
        log::error("node graph: writeFile unavailable (cannot compile)");
        return false;
    }
    // Sync node positions from imnodes before save.
    for (GraphNode& n : m_nodeGraph.nodes) {
        const ImVec2 p = ImNodes::GetNodeGridSpacePos(n.id);
        n.posX = p.x;
        n.posY = p.y;
    }
    const std::string json = saveGraphJson(m_nodeGraph);
    if (!ctx.writeFile(kGraphPath, json)) {
        m_graphStatus = "failed to write graph JSON";
        m_graphStatusTtl = 4.0f;
        log::error("node graph: failed to write {}", kGraphPath);
        m_outputLogOpen = true;
        return false;
    }
    const std::string lua = emitGraphLua(m_nodeGraph);
    if (!ctx.writeFile(kEmitLuaPath, lua)) {
        m_graphStatus = "failed to write generated Lua";
        m_graphStatusTtl = 4.0f;
        log::error("node graph: failed to write {}", kEmitLuaPath);
        m_outputLogOpen = true;
        return false;
    }
    m_nodeGraphDirty = false;
    bool reloaded = false;
    if (ctx.reloadScripts) reloaded = ctx.reloadScripts();
    m_graphStatus = reloaded ? "saved + compiled + scripts reloaded"
                          : "saved + compiled (reload skipped / host only)";
    m_graphStatusTtl = 5.0f;
    if (reloaded)
        log::info("node graph: compiled {} ({} nodes) + scripts reloaded", kEmitLuaPath,
                  m_nodeGraph.nodes.size());
    else
        log::warn("node graph: compiled {} but script reload skipped (host/SP only)",
                  kEmitLuaPath);
    return true;
}

void RoomEditor::openGraphNode(int nodeId) {
    if (!m_nodeGraph.findNode(nodeId)) return;
    ensureNodeGraphContext();
    ImNodes::SetCurrentContext(m_imnodes);
    m_graphOpenNodeId = nodeId;
    m_nodeGraphOpen = true;
    // Selecting in imnodes so the graph shows focus.
    ImNodes::ClearNodeSelection();
    ImNodes::SelectNode(nodeId);
    focusGraphNode(nodeId);
}

void RoomEditor::focusGraphNode(int nodeId) {
    if (!m_imnodes) return;
    ImNodes::SetCurrentContext(m_imnodes);
    ImNodes::EditorContextMoveToNode(nodeId);
}

void RoomEditor::createObjectNodeFromSelection(EditorContext& ctx) {
    if (m_selKind != Selection::Prop || m_selIndex < 0 ||
        m_selIndex >= static_cast<int>(ctx.props.size()))
        return;
    ensureNodeGraphContext();
    loadOrSeedNodeGraph(ctx);
    ImNodes::SetCurrentContext(m_imnodes);
    const EditorProp& prop = ctx.props[static_cast<std::size_t>(m_selIndex)];
    const ImVec2 pan = ImNodes::EditorContextGetPanning();
    const int id = m_nodeGraph.addNode(NodeKind::GetWorldObject, 120.0f - pan.x, 120.0f - pan.y);
    if (GraphNode* n = m_nodeGraph.findNode(id)) {
        n->intA = static_cast<int>(prop.id);
        const std::size_t slash = prop.assetPath.find_last_of('/');
        n->strA = slash == std::string::npos ? prop.assetPath : prop.assetPath.substr(slash + 1);
    }
    m_nodeGraphDirty = true;
    m_graphHighlightPropId = prop.id;
    m_graphHighlightPulse = 2.0f;
    openGraphNode(id);
    m_graphStatus = "created Get World Object for prop " + std::to_string(prop.id);
    m_graphStatusTtl = 4.0f;
}

void RoomEditor::updateObjectHighlight(EditorContext& ctx) {
    // Pulse highlight: bright point lights around the referenced prop (UE select silhouette feel).
    if (m_graphHighlightPulse > 0.0f) m_graphHighlightPulse -= 1.0f / 60.0f;

    // Prefer open node / selected GetWorldObject for live highlight.
    auto resolvePropId = [&](const GraphNode& n) -> std::uint32_t {
        if (n.kind == NodeKind::GetWorldObject || n.kind == NodeKind::HighlightObject ||
            n.kind == NodeKind::PrintObject)
            return static_cast<std::uint32_t>(n.intA);
        return 0;
    };
    if (m_graphOpenNodeId != 0) {
        if (const GraphNode* n = m_nodeGraph.findNode(m_graphOpenNodeId)) {
            if (const std::uint32_t pid = resolvePropId(*n); pid != 0)
                m_graphHighlightPropId = pid;
        }
    } else if (m_imnodes) {
        ImNodes::SetCurrentContext(m_imnodes);
        if (ImNodes::NumSelectedNodes() == 1) {
            int id = 0;
            ImNodes::GetSelectedNodes(&id);
            if (const GraphNode* n = m_nodeGraph.findNode(id)) {
                if (const std::uint32_t pid = resolvePropId(*n); pid != 0)
                    m_graphHighlightPropId = pid;
            }
        }
    }

    if (m_graphHighlightPropId == 0) return;
    const EditorProp* prop = nullptr;
    int propIndex = -1;
    for (int i = 0; i < static_cast<int>(ctx.props.size()); ++i) {
        if (ctx.props[static_cast<std::size_t>(i)].id == m_graphHighlightPropId) {
            prop = &ctx.props[static_cast<std::size_t>(i)];
            propIndex = i;
            break;
        }
    }
    if (!prop) return;

    // Keep Outliner in sync when focusing a graph object node (highlight selection).
    if (m_graphOpenNodeId != 0 || m_graphHighlightPulse > 0.0f) {
        m_selKind = Selection::Prop;
        m_selIndex = propIndex;
    }

    const glm::vec3 center = glm::vec3(prop->transform[3]);
    const float pulse = 0.55f + 0.45f * std::abs(std::sin(m_graphHighlightPulse * 8.0f));
    const glm::vec3 col(0.15f * pulse, 0.65f * pulse, 1.0f * pulse);
    // Corner markers around the prop origin.
    constexpr float r = 1.2f;
    submitPreviewLight(ctx, center + glm::vec3(r, 0.5f, r), col, 4.0f);
    submitPreviewLight(ctx, center + glm::vec3(-r, 0.5f, r), col, 4.0f);
    submitPreviewLight(ctx, center + glm::vec3(r, 0.5f, -r), col, 4.0f);
    submitPreviewLight(ctx, center + glm::vec3(-r, 0.5f, -r), col, 4.0f);
    submitPreviewLight(ctx, center + glm::vec3(0.0f, 1.5f, 0.0f), col * 1.2f, 5.0f);
}

void RoomEditor::drawNodeGraphContextMenu(EditorContext& ctx) {
    (void)ctx;
    if (!m_graphContextOpen) return;
    ImGui::OpenPopup("##graph_place_node");
    m_graphContextOpen = false;
    if (!ImGui::BeginPopup("##graph_place_node")) return;

    ImGui::TextDisabled("Place a new node");
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##graphsearch", "Search nodes...", m_graphSearch, sizeof(m_graphSearch));
    ImGui::Separator();

    // Grouped like UE's right-click palette.
    const char* lastCat = "";
    for (int i = 0; i < kPaletteCount; ++i) {
        const NodeKind kind = kPaletteKinds[i];
        if (!paletteMatchesSearch(kind, m_graphSearch)) continue;
        const char* cat = nodeKindCategory(kind);
        if (std::strcmp(cat, lastCat) != 0) {
            if (lastCat[0] != '\0') ImGui::Separator();
            ImGui::TextColored(ImVec4(0.0f, 0.65f, 1.0f, 1.0f), "%s", cat);
            lastCat = cat;
        }
        if (ImGui::MenuItem(nodeKindName(kind))) {
            const int id =
                m_nodeGraph.addNode(kind, m_graphContextGridX, m_graphContextGridY);
            m_nodeGraphDirty = true;
            openGraphNode(id);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndPopup();
}

void RoomEditor::drawNodeGraphDetails(EditorContext& ctx) {
    if (m_graphOpenNodeId == 0) return;
    GraphNode* n = m_nodeGraph.findNode(m_graphOpenNodeId);
    if (!n) {
        m_graphOpenNodeId = 0;
        return;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({vp->Pos.x + vp->Size.x - 340.0f, vp->Pos.y + 48.0f},
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({320.0f, 360.0f}, ImGuiCond_FirstUseEver);
    bool open = true;
    if (!ImGui::Begin("Node Details", &open, ImGuiWindowFlags_None)) {
        ImGui::End();
        if (!open) m_graphOpenNodeId = 0;
        return;
    }
    if (!open) {
        m_graphOpenNodeId = 0;
        ImGui::End();
        return;
    }

    ImGui::TextColored(ImVec4(0.0f, 0.65f, 1.0f, 1.0f), "%s", nodeKindCategory(n->kind));
    ImGui::Text("%s", nodeKindName(n->kind));
    ImGui::TextDisabled("Node Id  %d", n->id);
    ImGui::Separator();

    // Category-specific property editors (UE Details panel feel).
    if (n->kind == NodeKind::ActionLog || n->kind == NodeKind::ConstString ||
        n->kind == NodeKind::GetItemId || n->kind == NodeKind::GetWorldObject) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s", n->strA.c_str());
        if (ImGui::InputText("String", buf, sizeof(buf))) {
            n->strA = buf;
            m_nodeGraphDirty = true;
        }
    }
    if (n->kind == NodeKind::ConstInt || n->kind == NodeKind::GetWorldObject ||
        n->kind == NodeKind::Randi || n->kind == NodeKind::ActionSetBlock ||
        n->kind == NodeKind::ActionSpawnPickup || n->kind == NodeKind::HighlightObject ||
        n->kind == NodeKind::PrintObject) {
        if (ImGui::InputInt("Int A / Object Id", &n->intA)) m_nodeGraphDirty = true;
        if (n->kind == NodeKind::Randi || n->kind == NodeKind::ActionSetBlock)
            if (ImGui::InputInt("Int B", &n->intB)) m_nodeGraphDirty = true;
        if (n->kind == NodeKind::ActionSetBlock) {
            if (ImGui::InputInt("Int C", &n->intC)) m_nodeGraphDirty = true;
            if (ImGui::InputInt("Block", &n->intD)) m_nodeGraphDirty = true;
        }
        if (n->kind == NodeKind::ActionSpawnPickup)
            if (ImGui::InputInt("Count", &n->intD)) m_nodeGraphDirty = true;
    }
    if (n->kind == NodeKind::ConstFloat || n->kind == NodeKind::ActionSpawnPickup ||
        n->kind == NodeKind::MathAdd || n->kind == NodeKind::HighlightObject) {
        const char* flabel =
            n->kind == NodeKind::HighlightObject ? "Duration (s)" : "Float";
        if (ImGui::InputFloat(flabel, &n->floatA, 0.1f, 1.0f, "%.2f")) m_nodeGraphDirty = true;
    }

    if (n->kind == NodeKind::GetWorldObject) {
        ImGui::Separator();
        ImGui::TextUnformatted("World Object");
        // Dropdown of live props for pick-list assignment.
        if (ImGui::BeginCombo("Prop", n->strA.empty() ? "(none)" : n->strA.c_str())) {
            for (const EditorProp& p : ctx.props) {
                const std::size_t slash = p.assetPath.find_last_of('/');
                const std::string leaf =
                    slash == std::string::npos ? p.assetPath : p.assetPath.substr(slash + 1);
                char lab[96];
                std::snprintf(lab, sizeof(lab), "%u  %s", p.id, leaf.c_str());
                const bool sel = static_cast<int>(p.id) == n->intA;
                if (ImGui::Selectable(lab, sel)) {
                    n->intA = static_cast<int>(p.id);
                    n->strA = leaf;
                    m_nodeGraphDirty = true;
                    m_graphHighlightPropId = p.id;
                    m_graphHighlightPulse = 1.5f;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button("Select in Outliner / Viewport")) {
            m_graphHighlightPropId = static_cast<std::uint32_t>(n->intA);
            m_graphHighlightPulse = 2.5f;
            for (int i = 0; i < static_cast<int>(ctx.props.size()); ++i) {
                if (ctx.props[static_cast<std::size_t>(i)].id == m_graphHighlightPropId) {
                    m_selKind = Selection::Prop;
                    m_selIndex = i;
                    break;
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Focus Graph")) focusGraphNode(n->id);
    }

    ImGui::Separator();
    if (ImGui::Button("Close")) m_graphOpenNodeId = 0;
    ImGui::SameLine();
    if (ImGui::Button("Delete Node")) {
        m_nodeGraph.removeNode(n->id);
        m_graphOpenNodeId = 0;
        m_nodeGraphDirty = true;
    }
    ImGui::End();
}

void RoomEditor::drawNodeGraph(EditorContext& ctx) {
    if (!m_nodeGraphOpen) return;
    ensureNodeGraphContext();
    loadOrSeedNodeGraph(ctx);
    ImNodes::SetCurrentContext(m_imnodes);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({vp->Pos.x + 300.0f, vp->Pos.y + 40.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({780.0f, 520.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Node Graph", &m_nodeGraphOpen)) {
        ImGui::End();
        return;
    }

    // Toolbar strip (UE Graph editor style).
    if (ImGui::Button("Compile")) saveAndCompileNodeGraph(ctx);
    ImGui::SameLine();
    if (ImGui::Button("Save")) saveAndCompileNodeGraph(ctx);
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) m_graphContextOpen = true;
    ImGui::SameLine();
    if (ImGui::Button("Open Node") && ImNodes::NumSelectedNodes() == 1) {
        int id = 0;
        ImNodes::GetSelectedNodes(&id);
        openGraphNode(id);
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete") && ImNodes::NumSelectedNodes() > 0) {
        std::vector<int> ids(static_cast<std::size_t>(ImNodes::NumSelectedNodes()));
        ImNodes::GetSelectedNodes(ids.data());
        for (int id : ids) {
            if (id == m_graphOpenNodeId) m_graphOpenNodeId = 0;
            m_nodeGraph.removeNode(id);
        }
        ImNodes::ClearNodeSelection();
        m_nodeGraphDirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset Graph")) {
        m_nodeGraph = NodeGraph::makeExample();
        m_graphPlacedIds.clear();
        m_graphOpenNodeId = 0;
        m_nodeGraphDirty = true;
    }
    if (m_nodeGraphDirty) {
        ImGui::SameLine();
        ImGui::TextColored({1.0f, 0.75f, 0.2f, 1.0f}, "* dirty");
    }
    if (!m_graphStatus.empty()) {
        ImGui::SameLine();
        ImGui::TextColored({0.4f, 0.85f, 0.55f, 1.0f}, "%s", m_graphStatus.c_str());
    }
    ImGui::TextDisabled("RMB empty graph: place node  |  Double-click node: open Details  |  "
                        "Ctrl+drag link detach  |  Outliner: Create Graph Node");

    ImNodes::BeginNodeEditor();
    for (GraphNode& n : m_nodeGraph.nodes) {
        if (std::find(m_graphPlacedIds.begin(), m_graphPlacedIds.end(), n.id) == m_graphPlacedIds.end()) {
            ImNodes::SetNodeGridSpacePos(n.id, ImVec2(n.posX, n.posY));
            m_graphPlacedIds.push_back(n.id);
        }
        const bool isOpen = n.id == m_graphOpenNodeId;
        ImNodes::PushColorStyle(ImNodesCol_TitleBar, categoryTitleColor(nodeKindCategory(n.kind)));
        ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered,
                                categoryTitleColor(nodeKindCategory(n.kind)));
        ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, IM_COL32(0, 120, 200, 255));
        if (isOpen)
            ImNodes::PushColorStyle(ImNodesCol_NodeOutline, IM_COL32(0, 180, 255, 255));
        ImNodes::BeginNode(n.id);
        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted(nodeKindName(n.kind));
        if (n.kind == NodeKind::GetWorldObject && !n.strA.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", n.strA.c_str());
        }
        ImNodes::EndNodeTitleBar();

        int pinCount = 0;
        const GraphPinDesc* pins = nodePinLayout(n.kind, pinCount);
        for (int p = 0; p < pinCount; ++p) {
            const int attr = pinAttrId(n.id, p);
            ImNodes::PushColorStyle(ImNodesCol_Pin, pinColor(pins[p].kind));
            ImNodes::PushColorStyle(ImNodesCol_PinHovered, IM_COL32(255, 255, 255, 255));
            const ImNodesPinShape shape =
                pins[p].kind == PinKind::Exec ? ImNodesPinShape_TriangleFilled
                                              : ImNodesPinShape_CircleFilled;
            if (pins[p].isInput) {
                ImNodes::BeginInputAttribute(attr, shape);
                ImGui::Text("%s", pins[p].name);
                if (n.kind == NodeKind::ActionLog && p == 2) {
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), "%s", n.strA.c_str());
                    ImGui::SetNextItemWidth(120.0f);
                    if (ImGui::InputText("##msg", buf, sizeof(buf))) {
                        n.strA = buf;
                        m_nodeGraphDirty = true;
                    }
                }
                ImNodes::EndInputAttribute();
            } else {
                ImNodes::BeginOutputAttribute(attr, shape);
                if (n.kind == NodeKind::ConstInt) {
                    ImGui::SetNextItemWidth(64.0f);
                    if (ImGui::InputInt("##ci", &n.intA, 0, 0)) m_nodeGraphDirty = true;
                } else if (n.kind == NodeKind::ConstFloat) {
                    ImGui::SetNextItemWidth(64.0f);
                    if (ImGui::InputFloat("##cf", &n.floatA, 0, 0, "%.2f"))
                        m_nodeGraphDirty = true;
                } else if (n.kind == NodeKind::ConstString) {
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), "%s", n.strA.c_str());
                    ImGui::SetNextItemWidth(100.0f);
                    if (ImGui::InputText("##cs", buf, sizeof(buf))) {
                        n.strA = buf;
                        m_nodeGraphDirty = true;
                    }
                } else if (n.kind == NodeKind::GetWorldObject && p == 0) {
                    ImGui::Text("%s #%d", pins[p].name, n.intA);
                } else {
                    ImGui::Text("%s", pins[p].name);
                }
                ImNodes::EndOutputAttribute();
            }
            ImNodes::PopColorStyle();
            ImNodes::PopColorStyle();
        }
        ImNodes::EndNode();
        if (isOpen) ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
    }

    for (const GraphLink& L : m_nodeGraph.links) {
        // Color exec links white, data links by approximate type.
        ImNodes::PushColorStyle(ImNodesCol_Link, IM_COL32(200, 200, 200, 255));
        ImNodes::Link(L.id, pinAttrId(L.fromNode, L.fromPin), pinAttrId(L.toNode, L.toPin));
        ImNodes::PopColorStyle();
    }

    ImNodes::MiniMap(0.16f, ImNodesMiniMapLocation_BottomRight);
    const bool editorHovered = ImNodes::IsEditorHovered();
    ImNodes::EndNodeEditor();

    // Double-click node → Open Details (UE: open node).
    if (editorHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        int hovered = -1;
        if (ImNodes::IsNodeHovered(&hovered) && hovered > 0) openGraphNode(hovered);
    }
    // Single click still selects; if selection changes and user presses F, open.
    if (ImGui::IsKeyPressed(ImGuiKey_F) && ImNodes::NumSelectedNodes() == 1) {
        int id = 0;
        ImNodes::GetSelectedNodes(&id);
        openGraphNode(id);
    }
    // Delete key
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) && ImNodes::NumSelectedNodes() > 0 &&
        !ImGui::GetIO().WantTextInput) {
        std::vector<int> ids(static_cast<std::size_t>(ImNodes::NumSelectedNodes()));
        ImNodes::GetSelectedNodes(ids.data());
        for (int id : ids) {
            if (id == m_graphOpenNodeId) m_graphOpenNodeId = 0;
            m_nodeGraph.removeNode(id);
        }
        ImNodes::ClearNodeSelection();
        m_nodeGraphDirty = true;
    }

    // Right-click empty canvas → place node menu (UE Graph action menu).
    if (editorHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        int nodeH = -1, linkH = -1, pinH = -1;
        const bool onNode = ImNodes::IsNodeHovered(&nodeH);
        const bool onLink = ImNodes::IsLinkHovered(&linkH);
        const bool onPin = ImNodes::IsPinHovered(&pinH);
        if (!onNode && !onLink && !onPin) {
            // Approximate grid spawn under cursor using panning (good enough for place).
            const ImVec2 pan = ImNodes::EditorContextGetPanning();
            const ImVec2 mouse = ImGui::GetMousePos();
            const ImVec2 win = ImGui::GetWindowPos();
            m_graphContextGridX = mouse.x - win.x - pan.x;
            m_graphContextGridY = mouse.y - win.y - pan.y - 40.0f;
            m_graphContextOpen = true;
            m_graphSearch[0] = '\0';
        } else if (onNode && nodeH > 0) {
            // Right-click node → open details.
            openGraphNode(nodeH);
        }
    }
    drawNodeGraphContextMenu(ctx);

    // Link create / destroy
    int startAttr = 0, endAttr = 0;
    if (ImNodes::IsLinkCreated(&startAttr, &endAttr)) {
        int sn = pinNodeId(startAttr), sp = pinIndexOf(startAttr);
        int en = pinNodeId(endAttr), ep = pinIndexOf(endAttr);
        const GraphNode* a = m_nodeGraph.findNode(sn);
        const GraphNode* b = m_nodeGraph.findNode(en);
        if (a && b) {
            int ca = 0, cb = 0;
            const GraphPinDesc* pa = nodePinLayout(a->kind, ca);
            const GraphPinDesc* pb = nodePinLayout(b->kind, cb);
            if (sp < ca && ep < cb) {
                if (pa[sp].isInput && !pb[ep].isInput) std::swap(sn, en), std::swap(sp, ep);
                if (m_nodeGraph.addLink(sn, sp, en, ep)) m_nodeGraphDirty = true;
            }
        }
    }
    int destroyed = 0;
    if (ImNodes::IsLinkDestroyed(&destroyed)) {
        m_nodeGraph.removeLink(destroyed);
        m_nodeGraphDirty = true;
    }

    ImGui::End();
}

// ===== Details (C5 lite) ===================================================

void RoomEditor::drawDetailsPanel(EditorContext& ctx) {
    if (!m_detailsOpen) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({vp->Pos.x + vp->Size.x - 340.0f, vp->Pos.y + 420.0f},
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({320.0f, 320.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Details", &m_detailsOpen)) {
        ImGui::End();
        return;
    }

    // --- World (read-only snapshot of host rules; change via New Map) --------
    if (ImGui::CollapsingHeader("World", ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* kTerrain[] = {"Normal", "Superflat", "Void"};
        static const char* kEnv[] = {"Surface", "Underwater", "Space"};
        static const char* kGenre[] = {"FPS", "TPS", "Space", "Racer"};
        const int t = std::clamp(ctx.currentTerrain, 0, 2);
        const int e = std::clamp(ctx.currentEnvironment, 0, 2);
        const int g = std::clamp(ctx.currentGameTemplate, 0, 3);
        ImGui::Text("Terrain");
        ImGui::SameLine(110);
        ImGui::TextDisabled("%s", kTerrain[t]);
        ImGui::Text("Environment");
        ImGui::SameLine(110);
        ImGui::TextDisabled("%s", kEnv[e]);
        ImGui::Text("Template");
        ImGui::SameLine(110);
        ImGui::TextDisabled("%s", kGenre[g]);
        ImGui::Text("Seed");
        ImGui::SameLine(110);
        ImGui::TextDisabled("%u", ctx.currentSeed);
        ImGui::Text("Hemi ambient");
        ImGui::SameLine(110);
        ImGui::TextDisabled("%s", ctx.hemisphereAmbient ? "on" : "off");
        if (ImGui::SmallButton("New Map...")) {
            m_newMapOpen = true;
            m_newMapGenre = ctx.currentGameTemplate;
            m_newMapTerrain = ctx.currentTerrain;
            m_newMapEnvironment = ctx.currentEnvironment;
            m_newMapSeed = static_cast<int>(ctx.currentSeed);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("host/SP only");
    }

    // --- Selection ----------------------------------------------------------
    if (ImGui::CollapsingHeader("Selection", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (m_selKind == Selection::Light && m_selIndex >= 0 &&
            m_selIndex < static_cast<int>(ctx.lights.size())) {
            EditorLight& light = ctx.lights[static_cast<std::size_t>(m_selIndex)];
            ImGui::TextUnformatted("Light");
            const char* const types[] = {"point", "spot"};
            ImGui::Combo("type", &light.type, types, 2);
            ImGui::ColorEdit3("color", glm::value_ptr(light.color));
            ImGui::DragFloat("radius", &light.radius, 0.1f, 0.5f, 64.0f);
            if (light.type == 1) {
                ImGui::SliderFloat("angle", &light.angle, 0.05f, 1.4f, "%.2f rad");
                if (ImGui::Button("dir = camera fwd")) light.dir = ctx.camera.forward();
            }
            if (ImGui::Button("Delete Light")) {
                ctx.lights.erase(ctx.lights.begin() + m_selIndex);
                m_selKind = Selection::None;
                m_selIndex = -1;
            }
        } else if (m_selKind == Selection::Volume && m_selIndex >= 0 &&
                   m_selIndex < static_cast<int>(ctx.seedVolumes.size())) {
            SeedVolume& sv = ctx.seedVolumes[static_cast<std::size_t>(m_selIndex)];
            ImGui::TextUnformatted("Seed Volume");
            int seed = static_cast<int>(sv.seed);
            if (ImGui::InputInt("seed", &seed)) sv.seed = static_cast<std::uint32_t>(seed);
            ImGui::Text("min %d %d %d", sv.min.x, sv.min.y, sv.min.z);
            ImGui::Text("max %d %d %d", sv.max.x, sv.max.y, sv.max.z);
            if (ImGui::Button("Delete Volume")) {
                ctx.seedVolumes.erase(ctx.seedVolumes.begin() + m_selIndex);
                m_selKind = Selection::None;
                m_selIndex = -1;
            }
        } else if (m_selKind == Selection::Prop && m_selIndex >= 0 &&
                   m_selIndex < static_cast<int>(ctx.props.size())) {
            EditorProp& prop = ctx.props[static_cast<std::size_t>(m_selIndex)];
            ImGui::TextUnformatted("Prop");
            ImGui::TextDisabled("%s", prop.assetPath.c_str());
            ImGui::Text("Id");
            ImGui::SameLine(80);
            ImGui::Text("%u", prop.id);

            glm::vec3 pos = glm::vec3(prop.transform[3]);
            if (ImGui::DragFloat3("Location", &pos.x, 0.05f)) {
                prop.transform[3] = glm::vec4(pos, 1.0f);
                m_propGizmoDirty = true;
                if (prop.id != 0 && ctx.requestMoveProp)
                    ctx.requestMoveProp(prop.id, prop.transform);
            }
            ImGui::Text("Gizmo");
            ImGui::SameLine();
            ImGui::RadioButton("Move", &m_propGizmoOp, 0);
            ImGui::SameLine();
            ImGui::RadioButton("Rotate", &m_propGizmoOp, 1);
            ImGui::SameLine();
            ImGui::RadioButton("Scale", &m_propGizmoOp, 2);

            if (ImGui::Button("Create Graph Node")) {
                m_nodeGraphOpen = true;
                createObjectNodeFromSelection(ctx);
            }
            ImGui::SameLine();
            if (ImGui::Button("Highlight")) {
                m_graphHighlightPropId = prop.id;
                m_graphHighlightPulse = 2.5f;
                log::info("details: highlight prop {} ({})", prop.id, prop.assetPath);
            }
            if (ImGui::Button("Delete Prop")) {
                if (prop.id != 0 && ctx.requestRemoveProp) ctx.requestRemoveProp(prop.id);
                m_selKind = Selection::None;
                m_selIndex = -1;
                m_propGizmoDirty = false;
            }
        } else {
            ImGui::TextDisabled("Nothing selected — pick a prop, light, or volume in the Outliner.");
        }
    }

    // --- Import shortcut ----------------------------------------------------
    if (ImGui::CollapsingHeader("Import Asset")) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##import", "paste full path to .fbx/.obj/.png …", m_importPath,
                                 sizeof(m_importPath));
        if (ImGui::Button("Import")) doImport(ctx, m_importPath);
        ImGui::SameLine();
        ImGui::TextDisabled("or drop file on window");
        if (!m_importStatus.empty())
            ImGui::TextWrapped("%s", m_importStatus.c_str());
    }

    ImGui::End();
}

// ===== Output Log (C9) =====================================================

void RoomEditor::drawOutputLog() {
    if (!m_outputLogOpen) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({vp->Pos.x + 12.0f, vp->Pos.y + vp->Size.y - 280.0f},
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({720.0f, 240.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Output Log", &m_outputLogOpen)) {
        ImGui::End();
        return;
    }

    // Toolbar — mirrors UE Output Log filters.
    ImGui::RadioButton("All", &m_logFilter, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Messages", &m_logFilter, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Warnings", &m_logFilter, 2);
    ImGui::SameLine();
    ImGui::RadioButton("Errors", &m_logFilter, 3);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputTextWithHint("##logsearch", "Filter...", m_logSearch, sizeof(m_logSearch));
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &m_logAutoScroll);
    ImGui::SameLine();
    if (ImGui::Button("Clear")) log::clearHistory();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu lines", log::historySize());

    ImGui::Separator();
    ImGui::BeginChild("##logscroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

    const std::vector<log::Entry> lines = log::snapshotHistory();
    const std::string search = m_logSearch;
    auto searchLower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string q = searchLower(search);

    int shown = 0;
    for (const log::Entry& e : lines) {
        if (m_logFilter == 1 && e.level != log::Level::Info) continue;
        if (m_logFilter == 2 && e.level != log::Level::Warn) continue;
        if (m_logFilter == 3 && e.level != log::Level::Error) continue;
        if (!q.empty()) {
            if (searchLower(e.message).find(q) == std::string::npos) continue;
        }

        ImVec4 col(0.80f, 0.80f, 0.80f, 1.0f);
        if (e.level == log::Level::Warn) col = ImVec4(1.0f, 0.78f, 0.20f, 1.0f);
        if (e.level == log::Level::Error) col = ImVec4(1.0f, 0.35f, 0.30f, 1.0f);

        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(e.timeWall);
        ImGui::SameLine();
        ImGui::Text("[%s]", log::levelTag(e.level));
        ImGui::SameLine();
        // Selectable so Ctrl+C can copy (UE-like pick lines).
        ImGui::PushID(shown);
        if (ImGui::Selectable(e.message.c_str(), false,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            if (ImGui::IsMouseDoubleClicked(0)) {
                ImGui::SetClipboardText(e.message.c_str());
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Double-click to copy line");
        ImGui::PopID();
        ImGui::PopStyleColor();
        ++shown;
    }

    if (shown == 0)
        ImGui::TextDisabled("(no messages match filter)");

    const int count = static_cast<int>(lines.size());
    if (m_logAutoScroll && count != m_logLastCount)
        ImGui::SetScrollHereY(1.0f);
    m_logLastCount = count;

    ImGui::EndChild();
    ImGui::End();
}

} // namespace meat


